// Ranvier Core - Gossip Service Implementation
//
// UDP-based gossip protocol for distributed state synchronization

#include "gossip_service.hpp"

#include <algorithm>
#include <unordered_set>

#include <boost/range/irange.hpp>

#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/smp.hh>
#include <seastar/net/api.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/ip.hh>
#include <seastar/net/udp.hh>

namespace ranvier {

GossipService::GossipService(const ClusterConfig& config)
    : _config(config),
    _receive_loop_future(seastar::make_ready_future<>()),
    _discovery_future(seastar::make_ready_future<>()) {

    // Parse peer addresses at construction
    for (const auto& peer : _config.peers) {
        auto addr = parse_peer_address(peer);
        if (addr) {
            _peer_addresses.push_back(*addr);
            log_gossip.debug("Added peer: {}", peer);
        } else {
            log_gossip.warn("Failed to parse peer address: {}", peer);
        }
    }

    // Register metrics
    _metrics.add_group("ranvier", {
        seastar::metrics::make_counter("router_cluster_sync_sent", _packets_sent,
            seastar::metrics::description("Total number of gossip packets sent to cluster peers")),
        seastar::metrics::make_counter("router_cluster_sync_received", _packets_received,
            seastar::metrics::description("Total number of gossip packets received from cluster peers")),
        seastar::metrics::make_counter("router_cluster_sync_invalid", _packets_invalid,
            seastar::metrics::description("Total number of invalid gossip packets received")),
        seastar::metrics::make_counter("router_cluster_sync_untrusted", _packets_untrusted,
            seastar::metrics::description("Total number of gossip packets received from unknown peers")),
        seastar::metrics::make_gauge("cluster_peers_alive", _stats_cluster_peers_alive,
            seastar::metrics::description("Number of cluster peers currently alive")),
        seastar::metrics::make_counter("cluster_dns_discovery_success", _dns_discovery_success,
            seastar::metrics::description("Total number of successful DNS peer discovery operations")),
        seastar::metrics::make_counter("cluster_dns_discovery_failure", _dns_discovery_failure,
            seastar::metrics::description("Total number of failed DNS peer discovery operations"))
    });
}

seastar::future<> GossipService::start() {
    if (!_config.enabled) {
        log_gossip.info("Gossip service disabled");
        return seastar::make_ready_future<>();
    }

    if (_peer_addresses.empty()) {
        log_gossip.warn("Gossip enabled but no valid peers configured");
    }

    // Mark as running on ALL shards so stop() logic remains consistent
    _running = true;

    // Only Shard 0 manages the physical hardware/UDP port
    if (seastar::this_shard_id() != 0) {
        log_gossip.debug("Gossip service started on worker shard {}", seastar::this_shard_id());
        return seastar::make_ready_future<>();
    }

    log_gossip.info("Starting gossip service on port {}", _config.gossip_port);
    log_gossip.info("Configured peers: {}", _peer_addresses.size());
    seastar::socket_address bind_addr(seastar::ipv4_addr("0.0.0.0", _config.gossip_port));

    try {
        // Synchronously create the channel.
        _channel = seastar::engine().net().make_bound_datagram_channel(bind_addr);
        _my_address = bind_addr;

        log_gossip.info("Gossip UDP channel opened on port {}", _config.gossip_port);

        // Start receive loop in background (fire-and-forget).
        // It returns a future, but we don't wait for it to 'finish' because it runs for the lifetime of the service.
        _receive_loop_future = receive_loop().handle_exception([](auto ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                log_gossip.error("Gossip receive loop error: {}", e.what());
            }
        });

        // Trigger periodic heartbeats using configured interval
        _heartbeat_timer.set_callback([this] {
            (void)broadcast_heartbeat();
        });
        _heartbeat_timer.arm_periodic(_config.gossip_heartbeat_interval);

        // Check for peer timeouts at the heartbeat interval
        _liveness_timer.set_callback([this] { check_liveness(); });
        _liveness_timer.arm_periodic(_config.gossip_heartbeat_interval);

        for (const auto& addr : _peer_addresses) {
            _peer_table[addr] = { seastar::lowres_clock::now(), true };
        }

        // Set up DNS discovery if configured
        if (_config.discovery_type != DiscoveryType::STATIC && !_config.discovery_dns_name.empty()) {
            _discovery_enabled = true;
            log_gossip.info("DNS peer discovery enabled: type={}, dns_name={}, refresh_interval={}s",
                            _config.discovery_type == DiscoveryType::SRV ? "SRV" : "A",
                            _config.discovery_dns_name,
                            _config.discovery_refresh_interval.count());

            // Perform initial discovery
            _discovery_future = refresh_peers().handle_exception([](auto ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    log_gossip.error("Initial DNS discovery failed: {}", e.what());
                }
            });

            // Set up periodic discovery refresh
            _discovery_timer.set_callback([this] {
                if (_discovery_enabled && _running) {
                    (void)refresh_peers().handle_exception([](auto ep) {
                        try {
                            std::rethrow_exception(ep);
                        } catch (const std::exception& e) {
                            log_gossip.warn("Periodic DNS discovery failed: {}", e.what());
                        }
                    });
                }
            });
            _discovery_timer.arm_periodic(_config.discovery_refresh_interval);
        }

        // Return a ready future to signal the service has 'started' successfully.
        return seastar::make_ready_future<>();
    } catch (...) {
        // If bind fails (e.g. EADDRINUSE), convert the exception to a failed future.
        return seastar::make_exception_future<>(std::current_exception());
    }
}

#include <seastar/core/coroutine.hh>

seastar::future<> GossipService::stop() {
    if (!_running) {
        co_return;
    }

    log_gossip.info("Stopping gossip service");
    _running = false;

    // Important: Shutdown the channel first.
    // This wakes up the pending _channel->receive() call with an exception.
    if (_channel) {
        // Stop incoming traffic (unblocks receive_loop)
        // This is the "alarm clock" that wakes up the receive() call
        _channel->shutdown_input();

        // Stop outgoing traffic
        _channel->shutdown_output();
    }

    // Move the future so we can await it
    try {
        co_await std::move(_receive_loop_future);
    } catch (...) {
        // We expect an exception here (ECONNABORTED) because we shut down the channel
        log_gossip.debug("Gossip loop exited");
    }

    // Cleanup to release the port back to the system.
    _channel = std::nullopt;

    // Stop the heartbeat generator
    _heartbeat_timer.cancel();

    // Stop the liveness checker
    _liveness_timer.cancel();

    // Stop DNS discovery
    _discovery_enabled = false;
    _discovery_timer.cancel();

    // Wait for any in-flight discovery to complete
    try {
        co_await std::move(_discovery_future);
    } catch (...) {
        log_gossip.debug("Discovery future completed with exception during shutdown");
    }

    // Close DNS resolver
    try {
        co_await _dns_resolver.close();
    } catch (...) {
        log_gossip.debug("DNS resolver closed with exception during shutdown");
    }

    co_return;
}

void GossipService::set_route_learn_callback(RouteLearnCallback callback) {
    _route_learn_callback = std::move(callback);
}

seastar::future<> GossipService::broadcast_route(const std::vector<TokenId>& tokens, BackendId backend) {
    if (!_config.enabled || !_channel || _peer_addresses.empty()) {
        return seastar::make_ready_future<>();
    }

    RouteAnnouncementPacket pkt;
    pkt.backend_id = backend;
    pkt.tokens = tokens;
    pkt.token_count = static_cast<uint16_t>(std::min(tokens.size(),
                                                      static_cast<size_t>(RouteAnnouncementPacket::MAX_TOKENS)));

    auto serialized = pkt.serialize();

    // Create the base packet once.
    seastar::net::packet base_packet(seastar::temporary_buffer<char>(
        reinterpret_cast<const char*>(serialized.data()), serialized.size()));

    log_gossip.debug("Broadcasting route: {} tokens -> backend {} to {} peers",
                     tokens.size(), backend, _peer_addresses.size());

    // We do NOT capture base_packet by value if the copy constructor is deleted.
    // Instead, we use the shared-pointer-like behavior of Seastar packets.
    return seastar::parallel_for_each(_peer_addresses, [this, p = base_packet.share()](const seastar::socket_address& peer) mutable {
        // Basic loopback prevention: Don't send gossip to ourselves
        // Note: You may need to store 'bind_addr' as a member variable during start()
        if (peer == _my_address) {
            return seastar::make_ready_future<>();
        }

        // .share() increments the internal reference count.
        // We move the shared instance into the send call.
        return _channel->send(peer, p.share()).then([this] {
            _packets_sent++;
        }).handle_exception([peer](auto ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                log_gossip.debug("Failed to send to peer {}: {}", peer, e.what());
            }
        });
    });
}

seastar::future<> GossipService::receive_loop() {
    try {
        while (_running) {
            auto dgram = co_await _channel->receive();
            co_await handle_packet(std::move(dgram));
        }
    } catch (const std::exception& e) {
        if (_running) {
            log_gossip.error("Receive loop fatal error: {}", e.what());
        }
    }
    co_return;
}

seastar::future<> GossipService::handle_packet(seastar::net::udp_datagram&& dgram) {
    auto src_addr = dgram.get_src();

    // Verify source is a known peer before any processing
    auto it = std::find(_peer_addresses.begin(), _peer_addresses.end(), src_addr);
    if (it == _peer_addresses.end()) {
        ++_packets_untrusted;
        return seastar::make_ready_future<>();
    }

    update_peer_liveness(src_addr);

    // Move the packet out and linearize it
    seastar::net::packet data = std::move(dgram.get_data());

    // This ensures data.fragments()[0] contains the ENTIRE packet
    data.linearize();

    // Now it is safe to access the full length from the first fragment
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data.fragments()[0].base);

    // Identify packet type
    GossipPacketType type = static_cast<GossipPacketType>(ptr[0]);

    if (type == GossipPacketType::HEARTBEAT) {
        // Change get_src_address() to get_src()
        log_gossip.debug("Received heartbeat from {}", dgram.get_src());

        // Logic: You would typically update a 'last_seen' timestamp for this peer
        // in a peer-management table here.
        return seastar::make_ready_future<>();
    }

    auto pkt = RouteAnnouncementPacket::deserialize(ptr, data.len());
    if (!pkt) {
        _packets_invalid++;
        return seastar::make_ready_future<>();
    }

    // Map this peer address to the backend ID it just announced
    if (seastar::this_shard_id() == 0) {
        auto it = _peer_table.find(src_addr);
        if (it != _peer_table.end()) {
            it->second.associated_backend = pkt->backend_id;
        }
    }

    ++_packets_received;

    if (_route_learn_callback) {
        auto shared_tokens = std::make_shared<std::vector<TokenId>>(std::move(pkt->tokens));
        auto b_id = pkt->backend_id;
        // Copy the callback to avoid cross-shard memory access.
        // The callback captures RouterService 'this', but learn_route_remote()
        // only accesses thread_local data, making it safe to call from any shard.
        auto callback = _route_learn_callback;

        // Use an integer range from 0 to seastar::smp::count
        return seastar::parallel_for_each(
                boost::irange<unsigned>(0, seastar::smp::count),
                [callback, shared_tokens, b_id](unsigned shard_id) {
                return seastar::smp::submit_to(shard_id, [callback, shared_tokens, b_id] {
                        return callback(*shared_tokens, b_id);
                        });
                });
    }

    return seastar::make_ready_future<>();
}

seastar::future<> GossipService::broadcast_heartbeat() {
    if (!_channel || _peer_addresses.empty()) {
        return seastar::make_ready_future<>();
    }

    // Prepare a small 2-byte heartbeat (Type + Version)
    std::vector<uint8_t> payload = {
        static_cast<uint8_t>(GossipPacketType::HEARTBEAT),
        RouteAnnouncementPacket::PROTOCOL_VERSION
    };

    seastar::net::packet pb(seastar::temporary_buffer<char>(
        reinterpret_cast<const char*>(payload.data()), payload.size()));

    return seastar::parallel_for_each(_peer_addresses, [this, p = pb.share()](const seastar::socket_address& peer) mutable {
        if (peer == _my_address) {
            return seastar::make_ready_future<>();
        }

        // Send and ignore individual results
        return _channel->send(peer, p.share()).discard_result();
    });
}

void GossipService::update_peer_liveness(const seastar::socket_address& addr) {
    auto it = _peer_table.find(addr);
    if (it != _peer_table.end()) {
        it->second.last_seen = seastar::lowres_clock::now();
        if (!it->second.is_alive) {
            log_gossip.info("Peer {} recovered", addr);
            it->second.is_alive = true;
        }
    }
}

void GossipService::check_liveness() {
    auto now = seastar::lowres_clock::now();
    uint64_t alive_count = 0;

    for (auto& [addr, state] : _peer_table) {
        if (state.is_alive && (now - state.last_seen) > _config.gossip_peer_timeout) {
            state.is_alive = false;
            log_gossip.warn("Peer marked dead: socket_address={}", addr);

            if (state.associated_backend && _route_prune_callback) {
                BackendId b_id = *state.associated_backend;
                // Copy the callback to avoid cross-shard memory access
                auto callback = _route_prune_callback;

                // Broadcast the prune command to ALL shards
                (void)seastar::parallel_for_each(boost::irange<unsigned>(0, seastar::smp::count),
                    [callback, b_id](unsigned shard_id) {
                        return seastar::smp::submit_to(shard_id, [callback, b_id] {
                            return callback(b_id);
                        });
                    });
            }
        }

        if (state.is_alive) {
            ++alive_count;
        }
    }

    _stats_cluster_peers_alive = alive_count;
}

std::optional<seastar::socket_address> GossipService::parse_peer_address(const std::string& peer) {
    // Expected format: "IP:Port" or "hostname:Port"
    auto colon_pos = peer.rfind(':');
    if (colon_pos == std::string::npos || colon_pos == 0 || colon_pos == peer.length() - 1) {
        return std::nullopt;
    }

    std::string host = peer.substr(0, colon_pos);
    std::string port_str = peer.substr(colon_pos + 1);

    try {
        uint16_t port = static_cast<uint16_t>(std::stoi(port_str));

        // Try to parse as IPv4 address first
        seastar::net::inet_address addr(host);
        return seastar::socket_address(addr, port);
    } catch (const std::exception& e) {
        log_gossip.debug("Failed to parse peer '{}': {}", peer, e.what());
        return std::nullopt;
    }
}

seastar::future<> GossipService::refresh_peers() {
    if (!_discovery_enabled || seastar::this_shard_id() != 0) {
        co_return;
    }

    log_gossip.debug("Refreshing peers from DNS: {}", _config.discovery_dns_name);

    try {
        std::vector<seastar::socket_address> discovered_addresses;

        if (_config.discovery_type == DiscoveryType::SRV) {
            // Query SRV records - returns service records with priority, weight, port, and target
            auto srv_records = co_await _dns_resolver.get_srv_records(
                seastar::net::dns_resolver::srv_proto::udp,
                "_gossip",
                _config.discovery_dns_name);

            for (const auto& srv : srv_records) {
                // Resolve each SRV target to IP addresses
                try {
                    // Use get_host_by_name to get all addresses for the target
                    auto host_entry = co_await _dns_resolver.get_host_by_name(srv.target);
                    for (const auto& addr : host_entry.addr_list) {
                        discovered_addresses.emplace_back(addr, srv.port);
                        log_gossip.debug("DNS SRV discovered peer: {}:{}", addr, srv.port);
                    }
                } catch (const std::exception& e) {
                    log_gossip.warn("Failed to resolve SRV target {}: {}", srv.target, e.what());
                }
            }
        } else if (_config.discovery_type == DiscoveryType::A) {
            // Query A/AAAA records - use default gossip port
            // Use get_host_by_name to get all addresses
            auto host_entry = co_await _dns_resolver.get_host_by_name(_config.discovery_dns_name);

            for (const auto& addr : host_entry.addr_list) {
                discovered_addresses.emplace_back(addr, _config.gossip_port);
                log_gossip.debug("DNS A discovered peer: {}:{}", addr, _config.gossip_port);
            }
        }

        if (discovered_addresses.empty()) {
            log_gossip.debug("DNS discovery returned no addresses");
            co_return;
        }

        // Merge discovered addresses with existing peer list
        // Use a set for deduplication
        std::unordered_set<seastar::socket_address> new_peer_set;

        // Keep static peers (from config)
        for (const auto& peer : _config.peers) {
            auto addr = parse_peer_address(peer);
            if (addr) {
                new_peer_set.insert(*addr);
            }
        }

        // Add discovered peers
        for (const auto& addr : discovered_addresses) {
            new_peer_set.insert(addr);
        }

        // Build new peer address list
        std::vector<seastar::socket_address> new_peer_addresses(new_peer_set.begin(), new_peer_set.end());

        // Graceful merge: preserve existing PeerState for peers that are still present
        auto now = seastar::lowres_clock::now();
        std::unordered_map<seastar::socket_address, PeerState> new_peer_table;

        for (const auto& addr : new_peer_addresses) {
            auto it = _peer_table.find(addr);
            if (it != _peer_table.end()) {
                // Preserve existing state (last_seen, is_alive, associated_backend)
                new_peer_table[addr] = it->second;
            } else {
                // New peer - initialize with current time
                new_peer_table[addr] = { now, true, std::nullopt };
                log_gossip.info("DNS discovery: new peer added: {}", addr);
            }
        }

        // Log peers that were removed
        for (const auto& [addr, state] : _peer_table) {
            if (new_peer_table.find(addr) == new_peer_table.end()) {
                log_gossip.info("DNS discovery: peer removed: {}", addr);

                // Prune routes for removed peers if they had an associated backend
                if (state.associated_backend && _route_prune_callback) {
                    BackendId b_id = *state.associated_backend;
                    // Copy the callback to avoid cross-shard memory access
                    auto callback = _route_prune_callback;
                    (void)seastar::parallel_for_each(
                        boost::irange<unsigned>(0, seastar::smp::count),
                        [callback, b_id](unsigned shard_id) {
                            return seastar::smp::submit_to(shard_id, [callback, b_id] {
                                return callback(b_id);
                            });
                        });
                }
            }
        }

        // Update the peer structures
        _peer_addresses = std::move(new_peer_addresses);
        _peer_table = std::move(new_peer_table);

        // Update alive count metric
        uint64_t alive_count = 0;
        for (const auto& [addr, state] : _peer_table) {
            if (state.is_alive) {
                ++alive_count;
            }
        }
        _stats_cluster_peers_alive = alive_count;

        log_gossip.info("DNS discovery complete: {} peers ({} from DNS)",
                        _peer_addresses.size(), discovered_addresses.size());

        ++_dns_discovery_success;

    } catch (const std::exception& e) {
        log_gossip.warn("DNS discovery failed: {}", e.what());
        ++_dns_discovery_failure;
    }

    co_return;
}

}  // namespace ranvier
