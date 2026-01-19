// Ranvier Core - Gossip Protocol Module Implementation
//
// Manages message handling, reliable delivery, and DNS discovery.

#include "gossip_protocol.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

#include <boost/range/irange.hpp>
#include <seastar/core/coroutine.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/when_all.hh>
#include <seastar/net/inet_address.hh>

namespace ranvier {

//------------------------------------------------------------------------------
// Packet Serialization
//------------------------------------------------------------------------------

std::vector<uint8_t> RouteAnnouncementPacket::serialize() const {
    std::vector<uint8_t> buffer;
    buffer.reserve(HEADER_SIZE + tokens.size() * sizeof(TokenId));

    buffer.push_back(static_cast<uint8_t>(type));
    buffer.push_back(version);

    // Sequence number (big-endian)
    buffer.push_back((seq_num >> 24) & 0xFF);
    buffer.push_back((seq_num >> 16) & 0xFF);
    buffer.push_back((seq_num >> 8) & 0xFF);
    buffer.push_back(seq_num & 0xFF);

    // Backend ID (big-endian)
    buffer.push_back((backend_id >> 24) & 0xFF);
    buffer.push_back((backend_id >> 16) & 0xFF);
    buffer.push_back((backend_id >> 8) & 0xFF);
    buffer.push_back(backend_id & 0xFF);

    // Token count (big-endian)
    uint16_t count = static_cast<uint16_t>(std::min(tokens.size(), static_cast<size_t>(MAX_TOKENS)));
    buffer.push_back((count >> 8) & 0xFF);
    buffer.push_back(count & 0xFF);

    // Tokens (big-endian)
    for (size_t i = 0; i < count; ++i) {
        TokenId t = tokens[i];
        buffer.push_back((t >> 24) & 0xFF);
        buffer.push_back((t >> 16) & 0xFF);
        buffer.push_back((t >> 8) & 0xFF);
        buffer.push_back(t & 0xFF);
    }

    return buffer;
}

std::optional<RouteAnnouncementPacket> RouteAnnouncementPacket::deserialize(const uint8_t* data, size_t len) {
    if (len < HEADER_SIZE) {
        return std::nullopt;
    }

    RouteAnnouncementPacket pkt;
    pkt.type = static_cast<GossipPacketType>(data[0]);
    pkt.version = data[1];

    if (pkt.type != GossipPacketType::ROUTE_ANNOUNCEMENT || pkt.version != PROTOCOL_VERSION) {
        return std::nullopt;
    }

    pkt.seq_num = (static_cast<uint32_t>(data[2]) << 24) |
                  (static_cast<uint32_t>(data[3]) << 16) |
                  (static_cast<uint32_t>(data[4]) << 8) |
                  static_cast<uint32_t>(data[5]);

    pkt.backend_id = (static_cast<BackendId>(data[6]) << 24) |
                     (static_cast<BackendId>(data[7]) << 16) |
                     (static_cast<BackendId>(data[8]) << 8) |
                     static_cast<BackendId>(data[9]);

    pkt.token_count = (static_cast<uint16_t>(data[10]) << 8) | static_cast<uint16_t>(data[11]);

    size_t expected_size = HEADER_SIZE + pkt.token_count * sizeof(TokenId);
    if (len != expected_size || pkt.token_count > MAX_TOKENS) {
        return std::nullopt;
    }

    pkt.tokens.reserve(pkt.token_count);
    for (size_t i = 0; i < pkt.token_count; ++i) {
        size_t offset = HEADER_SIZE + i * sizeof(TokenId);
        TokenId t = (static_cast<TokenId>(data[offset]) << 24) |
                    (static_cast<TokenId>(data[offset + 1]) << 16) |
                    (static_cast<TokenId>(data[offset + 2]) << 8) |
                    static_cast<TokenId>(data[offset + 3]);
        pkt.tokens.push_back(t);
    }

    return pkt;
}

std::vector<uint8_t> RouteAckPacket::serialize() const {
    std::vector<uint8_t> buffer;
    buffer.reserve(PACKET_SIZE);

    buffer.push_back(static_cast<uint8_t>(type));
    buffer.push_back(version);

    buffer.push_back((seq_num >> 24) & 0xFF);
    buffer.push_back((seq_num >> 16) & 0xFF);
    buffer.push_back((seq_num >> 8) & 0xFF);
    buffer.push_back(seq_num & 0xFF);

    return buffer;
}

std::optional<RouteAckPacket> RouteAckPacket::deserialize(const uint8_t* data, size_t len) {
    if (len != PACKET_SIZE) {
        return std::nullopt;
    }

    RouteAckPacket pkt;
    pkt.type = static_cast<GossipPacketType>(data[0]);
    pkt.version = data[1];

    if (pkt.type != GossipPacketType::ROUTE_ACK || pkt.version != PROTOCOL_VERSION) {
        return std::nullopt;
    }

    pkt.seq_num = (static_cast<uint32_t>(data[2]) << 24) |
                  (static_cast<uint32_t>(data[3]) << 16) |
                  (static_cast<uint32_t>(data[4]) << 8) |
                  static_cast<uint32_t>(data[5]);

    return pkt;
}

std::vector<uint8_t> NodeStatePacket::serialize() const {
    std::vector<uint8_t> buffer;
    buffer.reserve(PACKET_SIZE);

    buffer.push_back(static_cast<uint8_t>(type));
    buffer.push_back(version);
    buffer.push_back(static_cast<uint8_t>(state));

    buffer.push_back((backend_id >> 24) & 0xFF);
    buffer.push_back((backend_id >> 16) & 0xFF);
    buffer.push_back((backend_id >> 8) & 0xFF);
    buffer.push_back(backend_id & 0xFF);

    return buffer;
}

std::optional<NodeStatePacket> NodeStatePacket::deserialize(const uint8_t* data, size_t len) {
    if (len != PACKET_SIZE) {
        return std::nullopt;
    }

    NodeStatePacket pkt;
    pkt.type = static_cast<GossipPacketType>(data[0]);
    pkt.version = data[1];

    if (pkt.type != GossipPacketType::NODE_STATE || pkt.version != PROTOCOL_VERSION) {
        return std::nullopt;
    }

    pkt.state = static_cast<NodeState>(data[2]);

    pkt.backend_id = (static_cast<BackendId>(data[3]) << 24) |
                     (static_cast<BackendId>(data[4]) << 16) |
                     (static_cast<BackendId>(data[5]) << 8) |
                     static_cast<BackendId>(data[6]);

    return pkt;
}

//------------------------------------------------------------------------------
// GossipProtocol Implementation
//------------------------------------------------------------------------------

GossipProtocol::GossipProtocol(const ClusterConfig& config)
    : _config(config),
      _discovery_future(seastar::make_ready_future<>()) {
}

seastar::future<> GossipProtocol::start(GossipTransport& transport, GossipConsensus& consensus,
                                         std::vector<seastar::socket_address>& peer_addresses) {
    _transport = &transport;
    _consensus = &consensus;
    _peer_addresses = &peer_addresses;
    _running = true;

    // Only shard 0 manages protocol logic
    if (seastar::this_shard_id() != 0) {
        co_return;
    }

    // Set up heartbeat timer with RAII timer safety
    _heartbeat_timer.set_callback([this] {
        (void)broadcast_heartbeat();
    });
    _heartbeat_timer.arm_periodic(_config.gossip_heartbeat_interval);

    // Set up reliable delivery retry timer
    if (_config.gossip_reliable_delivery) {
        log_gossip_protocol().info("Reliable delivery enabled: ack_timeout={}ms, max_retries={}, dedup_window={}",
                                   _config.gossip_ack_timeout.count(),
                                   _config.gossip_max_retries,
                                   _config.gossip_dedup_window);
        _retry_timer.set_callback([this] { process_retries(); });
        auto retry_check_interval = std::max(
            std::chrono::milliseconds(10),
            _config.gossip_ack_timeout / 2);
        _retry_timer.arm_periodic(retry_check_interval);
    }

    // Set up DNS discovery if configured
    if (_config.discovery_type != DiscoveryType::STATIC && !_config.discovery_dns_name.empty()) {
        _discovery_enabled = true;
        log_gossip_protocol().info("DNS peer discovery enabled: type={}, dns_name={}, refresh_interval={}s",
                                   _config.discovery_type == DiscoveryType::SRV ? "SRV" : "A",
                                   _config.discovery_dns_name,
                                   _config.discovery_refresh_interval.count());

        // Perform initial discovery
        _discovery_future = refresh_peers().handle_exception([](auto ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                log_gossip_protocol().error("Initial DNS discovery failed: {}", e.what());
            }
        });

        // Set up periodic discovery refresh with RAII timer safety
        _discovery_timer.set_callback([this] {
            try {
                [[maybe_unused]] auto timer_holder = _transport->timer_gate().hold();
            } catch (const seastar::gate_closed_exception&) {
                return;
            }

            if (_discovery_enabled && _running) {
                (void)refresh_peers().handle_exception([](auto ep) {
                    try {
                        std::rethrow_exception(ep);
                    } catch (const std::exception& e) {
                        log_gossip_protocol().warn("Periodic DNS discovery failed: {}", e.what());
                    }
                });
            }
        });
        _discovery_timer.arm_periodic(_config.discovery_refresh_interval);
    }

    log_gossip_protocol().info("Protocol module started");
    co_return;
}

seastar::future<> GossipProtocol::stop() {
    if (!_running) {
        co_return;
    }

    log_gossip_protocol().info("Stopping protocol module");
    _running = false;

    // Cancel timers
    _heartbeat_timer.cancel();
    _retry_timer.cancel();
    _discovery_timer.cancel();
    _discovery_enabled = false;

    // Clear pending ACKs
    _pending_acks.clear();
    _stats_pending_acks_count = 0;

    // Wait for discovery to complete
    try {
        co_await std::move(_discovery_future);
    } catch (...) {
        log_gossip_protocol().debug("Discovery future completed with exception during shutdown");
    }

    // Close DNS resolver
    try {
        co_await _dns_resolver.close();
    } catch (...) {
        log_gossip_protocol().debug("DNS resolver closed with exception during shutdown");
    }
}

seastar::future<> GossipProtocol::broadcast_route(const std::vector<TokenId>& tokens, BackendId backend) {
    if (!_config.enabled || !_transport || !_transport->is_ready() || _peer_addresses->empty()) {
        return seastar::make_ready_future<>();
    }

    // Gossip protection: reject during shutdown or re-sync
    if (_consensus && !_consensus->is_accepting_tasks()) {
        log_gossip_protocol().debug("Route broadcast rejected: gossip service not accepting tasks. "
                                    "tokens={}, backend={}", tokens.size(), backend);
        return seastar::make_ready_future<>();
    }

    // Split-brain protection
    if (_consensus && _config.quorum_enabled && _consensus->is_degraded()) {
        if (_config.fail_open_on_quorum_loss) {
            _consensus->inc_routes_allowed_fail_open();
            log_gossip_protocol().debug("Route broadcast allowed (fail-open mode): cluster DEGRADED but "
                                        "fail_open_on_quorum_loss=true. tokens={}, backend={}",
                                        tokens.size(), backend);
        } else if (_config.reject_routes_on_quorum_loss) {
            _consensus->inc_routes_rejected_degraded();
            log_gossip_protocol().debug("Route broadcast rejected: cluster in DEGRADED mode (no quorum). "
                                        "tokens={}, backend={}", tokens.size(), backend);
            return seastar::make_ready_future<>();
        }
    }

    log_gossip_protocol().debug("Broadcasting route: {} tokens -> backend {} to {} peers",
                                tokens.size(), backend, _peer_addresses->size());

    // Send to each peer with per-peer sequence numbers
    return seastar::parallel_for_each(*_peer_addresses, [this, tokens, backend](const seastar::socket_address& peer) mutable {
        RouteAnnouncementPacket pkt;
        pkt.backend_id = backend;
        // Force local allocation for tokens (Rule #14: cross-shard heap memory)
        pkt.tokens = std::vector<TokenId>(tokens.begin(), tokens.end());
        pkt.token_count = static_cast<uint16_t>(std::min(tokens.size(),
                                                          static_cast<size_t>(RouteAnnouncementPacket::MAX_TOKENS)));

        if (_config.gossip_reliable_delivery) {
            pkt.seq_num = next_seq_num(peer);
        }

        auto serialized = pkt.serialize();

        // Track pending ACK if reliable delivery enabled
        if (_config.gossip_reliable_delivery) {
            if (_stats_pending_acks_count >= MAX_PENDING_ACKS) {
                ++_pending_acks_overflow;
                log_gossip_protocol().warn("Pending acks limit reached ({}), not tracking ACK for peer={}, seq_num={}",
                                           MAX_PENDING_ACKS, peer, pkt.seq_num);
            } else {
                PendingAck pending;
                pending.seq_num = pkt.seq_num;
                pending.serialized_packet = serialized;
                pending.next_retry = seastar::lowres_clock::now() + _config.gossip_ack_timeout;
                pending.retry_count = 0;
                _pending_acks[peer][pkt.seq_num] = std::move(pending);
                ++_stats_pending_acks_count;
                log_gossip_protocol().trace("Tracking pending ACK: peer={}, seq_num={}", peer, pkt.seq_num);
            }
        }

        return _transport->send(peer, serialized).then([this] {
            ++_packets_sent;
        }).handle_exception([peer](auto ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                log_gossip_protocol().debug("Failed to send to peer {}: {}", peer, e.what());
            }
        });
    });
}

seastar::future<> GossipProtocol::broadcast_node_state(NodeState state, BackendId local_backend_id) {
    if (!_config.enabled || !_transport || !_transport->is_ready() || _peer_addresses->empty()) {
        return seastar::make_ready_future<>();
    }

    if (state == NodeState::DRAINING) {
        log_gossip_protocol().info("Broadcasting DRAINING state to {} peers (local_backend_id={})",
                                   _peer_addresses->size(), local_backend_id);
    }

    NodeStatePacket pkt;
    pkt.state = state;
    pkt.backend_id = local_backend_id;
    auto serialized = pkt.serialize();

    return _transport->broadcast(*_peer_addresses, serialized).then([this] {
        ++_node_state_sent;
    });
}

seastar::future<> GossipProtocol::broadcast_heartbeat() {
    // RAII Timer Safety
    try {
        [[maybe_unused]] auto timer_holder = _transport->timer_gate().hold();
    } catch (const seastar::gate_closed_exception&) {
        return seastar::make_ready_future<>();
    }

    if (!_transport || !_transport->is_ready() || _peer_addresses->empty()) {
        return seastar::make_ready_future<>();
    }

    std::vector<uint8_t> heartbeat_data = {
        static_cast<uint8_t>(GossipPacketType::HEARTBEAT),
        static_cast<uint8_t>(RouteAnnouncementPacket::PROTOCOL_VERSION)
    };

    return _transport->broadcast(*_peer_addresses, heartbeat_data);
}

seastar::future<> GossipProtocol::handle_packet(seastar::net::udp_datagram&& dgram) {
    auto src_addr = dgram.get_src();

    // Verify source is a known peer
    auto it = std::find(_peer_addresses->begin(), _peer_addresses->end(), src_addr);
    if (it == _peer_addresses->end()) {
        ++_packets_untrusted;
        return seastar::make_ready_future<>();
    }

    // Linearize packet data
    seastar::net::packet data = std::move(dgram.get_data());
    data.linearize();

    const uint8_t* raw_ptr = reinterpret_cast<const uint8_t*>(data.fragments()[0].base);
    size_t raw_len = data.len();

    // mTLS lockdown check
    if (_transport->should_drop_mtls_lockdown(src_addr, raw_ptr, raw_len)) {
        return seastar::make_ready_future<>();
    }

    // Update peer liveness
    if (_consensus) {
        _consensus->update_peer_seen(src_addr);
    }

    // Decrypt if needed
    const uint8_t* ptr = raw_ptr;
    size_t len = raw_len;
    std::optional<std::vector<uint8_t>> decrypted;

    if (_transport->is_dtls_enabled()) {
        decrypted = _transport->decrypt(src_addr, raw_ptr, raw_len);
        if (!decrypted) {
            return seastar::make_ready_future<>();
        }
        ptr = decrypted->data();
        len = decrypted->size();
    }

    // Identify packet type
    if (len < 1) {
        ++_packets_invalid;
        return seastar::make_ready_future<>();
    }

    GossipPacketType type = static_cast<GossipPacketType>(ptr[0]);

    if (type == GossipPacketType::HEARTBEAT) {
        log_gossip_protocol().debug("Received heartbeat from {}", src_addr);
        return seastar::make_ready_future<>();
    }

    // Handle node state packets
    if (type == GossipPacketType::NODE_STATE) {
        auto state_pkt = NodeStatePacket::deserialize(ptr, len);
        if (!state_pkt) {
            ++_packets_invalid;
            return seastar::make_ready_future<>();
        }

        ++_node_state_received;
        log_gossip_protocol().info("Received NODE_STATE packet from {}: backend={}, state={}",
                                   src_addr, state_pkt->backend_id,
                                   state_pkt->state == NodeState::DRAINING ? "DRAINING" : "ACTIVE");

        if (_node_state_callback) {
            return _node_state_callback(state_pkt->backend_id, state_pkt->state);
        }
        return seastar::make_ready_future<>();
    }

    // Handle ACK packets
    if (type == GossipPacketType::ROUTE_ACK) {
        auto ack_pkt = RouteAckPacket::deserialize(ptr, len);
        if (!ack_pkt) {
            ++_packets_invalid;
            return seastar::make_ready_future<>();
        }

        handle_ack(src_addr, ack_pkt->seq_num);
        return seastar::make_ready_future<>();
    }

    // Handle route announcements
    auto pkt = RouteAnnouncementPacket::deserialize(ptr, len);
    if (!pkt) {
        ++_packets_invalid;
        return seastar::make_ready_future<>();
    }

    // Check for duplicate
    if (_config.gossip_reliable_delivery) {
        if (is_duplicate(src_addr, pkt->seq_num)) {
            log_gossip_protocol().trace("Duplicate route announcement suppressed: peer={}, seq_num={}", src_addr, pkt->seq_num);
            ++_duplicates_suppressed;
            return send_ack(src_addr, pkt->seq_num);
        }
    }

    // Split-brain protection for incoming routes
    if (_consensus && _config.quorum_enabled && _consensus->is_degraded()) {
        if (_config.accept_gossip_on_quorum_loss) {
            _consensus->inc_gossip_accepted_fail_open();
            log_gossip_protocol().debug("Incoming route accepted (fail-open mode): cluster DEGRADED but "
                                        "accept_gossip_on_quorum_loss=true. peer={}, tokens={}, backend={}",
                                        src_addr, pkt->tokens.size(), pkt->backend_id);
        } else if (_config.reject_routes_on_quorum_loss) {
            _consensus->inc_routes_rejected_incoming_degraded();
            log_gossip_protocol().debug("Incoming route rejected: cluster in DEGRADED mode (no quorum). "
                                        "peer={}, tokens={}, backend={}", src_addr, pkt->tokens.size(), pkt->backend_id);
            if (_config.gossip_reliable_delivery) {
                return send_ack(src_addr, pkt->seq_num);
            }
            return seastar::make_ready_future<>();
        }
    }

    // Associate peer with backend
    if (_consensus && seastar::this_shard_id() == 0) {
        _consensus->associate_backend(src_addr, pkt->backend_id);
    }

    ++_packets_received;

    // Send ACK if reliable delivery enabled
    seastar::future<> ack_future = seastar::make_ready_future<>();
    if (_config.gossip_reliable_delivery) {
        ack_future = send_ack(src_addr, pkt->seq_num);
    }

    if (_route_learn_callback) {
        // Cross-shard dispatch with proper memory handling (Rule #14)
        // CRITICAL: Cannot use std::shared_ptr across shards - the destructor would run
        // on the wrong shard when the last copy is destroyed, causing SIGSEGV.
        // Instead, use seastar::do_with to keep the vector alive on shard 0, and pass
        // a raw pointer for reading only. Each shard forces local allocation.
        auto b_id = pkt->backend_id;
        auto callback = _route_learn_callback;

        auto learn_future = seastar::do_with(
            std::vector<TokenId>(pkt->tokens),  // Owned by do_with, destroyed on shard 0
            [callback, b_id](const std::vector<TokenId>& tokens_on_shard0) {
                // Raw pointer to shard 0's data - safe to read from other shards
                // while do_with keeps it alive
                const std::vector<TokenId>* tokens_ptr = &tokens_on_shard0;

                return seastar::parallel_for_each(
                    boost::irange<unsigned>(0, seastar::smp::count),
                    [callback, tokens_ptr, b_id](unsigned shard_id) {
                        return seastar::smp::submit_to(shard_id, [callback, tokens_ptr, b_id] {
                            // Force local allocation on THIS shard (Rule #14)
                            // This creates a new vector with memory from this shard's allocator
                            auto local_tokens = std::vector<TokenId>(tokens_ptr->begin(), tokens_ptr->end());
                            return callback(std::move(local_tokens), b_id);
                        });
                    });
            });

        return seastar::when_all_succeed(std::move(ack_future), std::move(learn_future)).discard_result();
    }

    return ack_future;
}

seastar::future<> GossipProtocol::refresh_peers() {
    if (!_discovery_enabled || seastar::this_shard_id() != 0) {
        co_return;
    }

    log_gossip_protocol().debug("Refreshing peers from DNS: {}", _config.discovery_dns_name);

    try {
        std::vector<seastar::socket_address> discovered_addresses;

        if (_config.discovery_type == DiscoveryType::SRV) {
            auto srv_records = co_await _dns_resolver.get_srv_records(
                seastar::net::dns_resolver::srv_proto::udp,
                "_gossip",
                _config.discovery_dns_name);

            for (const auto& srv : srv_records) {
                try {
                    auto host_entry = co_await _dns_resolver.get_host_by_name(srv.target);
                    for (const auto& addr : host_entry.addr_list) {
                        discovered_addresses.emplace_back(addr, srv.port);
                        log_gossip_protocol().debug("DNS SRV discovered peer: {}:{}", addr, srv.port);
                    }
                } catch (const std::exception& e) {
                    log_gossip_protocol().warn("Failed to resolve SRV target {}: {}", srv.target, e.what());
                }
            }
        } else if (_config.discovery_type == DiscoveryType::A) {
            auto host_entry = co_await _dns_resolver.get_host_by_name(_config.discovery_dns_name);

            for (const auto& addr : host_entry.addr_list) {
                discovered_addresses.emplace_back(addr, _config.gossip_port);
                log_gossip_protocol().debug("DNS A discovered peer: {}:{}", addr, _config.gossip_port);
            }
        }

        if (discovered_addresses.empty()) {
            log_gossip_protocol().debug("DNS discovery returned no addresses");
            co_return;
        }

        // Merge with static peers
        std::unordered_set<seastar::socket_address> new_peer_set;

        for (const auto& peer : _config.peers) {
            auto addr = parse_peer_address(peer);
            if (addr) {
                new_peer_set.insert(*addr);
            }
        }

        for (const auto& addr : discovered_addresses) {
            new_peer_set.insert(addr);
        }

        std::vector<seastar::socket_address> new_peer_addresses(new_peer_set.begin(), new_peer_set.end());

        // Update consensus peer table and get newly added peers
        std::vector<seastar::socket_address> new_peers_for_handshake;
        if (_consensus) {
            new_peers_for_handshake = _consensus->update_peer_list(new_peer_addresses);
        }

        // Clean up protocol state for removed peers
        for (const auto& peer : *_peer_addresses) {
            if (new_peer_set.find(peer) == new_peer_set.end()) {
                cleanup_peer_state(peer);
            }
        }

        // Update peer addresses
        *_peer_addresses = std::move(new_peer_addresses);

        log_gossip_protocol().info("DNS discovery complete: {} peers ({} from DNS)",
                                   _peer_addresses->size(), discovered_addresses.size());

        ++_dns_discovery_success;

        // Initiate DTLS handshakes for new peers
        if (_transport && _transport->is_dtls_enabled() && !new_peers_for_handshake.empty()) {
            log_gossip_protocol().info("Initiating DTLS handshakes with {} newly discovered peers",
                                       new_peers_for_handshake.size());
            for (const auto& peer : new_peers_for_handshake) {
                (void)_transport->initiate_handshake(peer);
            }
        }

    } catch (const std::exception& e) {
        log_gossip_protocol().warn("DNS discovery failed: {}", e.what());
        ++_dns_discovery_failure;
    }

    co_return;
}

seastar::future<> GossipProtocol::send_ack(const seastar::socket_address& peer, uint32_t seq_num) {
    if (!_transport || !_transport->is_ready()) {
        return seastar::make_ready_future<>();
    }

    RouteAckPacket ack;
    ack.seq_num = seq_num;
    auto serialized = ack.serialize();

    log_gossip_protocol().trace("Sending ACK: peer={}, seq_num={}", peer, seq_num);

    return _transport->send(peer, serialized).then([this] {
        ++_acks_sent;
    }).handle_exception([peer, seq_num](auto ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            log_gossip_protocol().debug("Failed to send ACK to peer {}: {}", peer, e.what());
        }
    });
}

void GossipProtocol::handle_ack(const seastar::socket_address& peer, uint32_t seq_num) {
    auto peer_it = _pending_acks.find(peer);
    if (peer_it == _pending_acks.end()) {
        log_gossip_protocol().trace("Received ACK for unknown peer: peer={}, seq_num={}", peer, seq_num);
        return;
    }

    auto& pending_map = peer_it->second;
    auto ack_it = pending_map.find(seq_num);
    if (ack_it == pending_map.end()) {
        log_gossip_protocol().trace("Received ACK for unknown seq_num: peer={}, seq_num={}", peer, seq_num);
        return;
    }

    log_gossip_protocol().trace("Received ACK: peer={}, seq_num={}", peer, seq_num);
    pending_map.erase(ack_it);
    ++_acks_received;
    --_stats_pending_acks_count;

    if (pending_map.empty()) {
        _pending_acks.erase(peer_it);
    }
}

bool GossipProtocol::is_duplicate(const seastar::socket_address& peer, uint32_t seq_num) {
    auto set_it = _received_seq_sets.find(peer);
    auto window_it = _received_seq_windows.find(peer);

    if (set_it != _received_seq_sets.end()) {
        auto& seq_set = set_it->second;
        auto& seq_window = window_it->second;

        if (seq_set.count(seq_num) > 0) {
            return true;
        }

        seq_set.insert(seq_num);
        seq_window.push_back(seq_num);

        // Slide window if too large
        while (seq_window.size() > _config.gossip_dedup_window) {
            uint32_t oldest = seq_window.front();
            seq_window.pop_front();
            seq_set.erase(oldest);
        }

        return false;
    }

    // New peer - check bounds (Rule #4)
    if (_received_seq_sets.size() >= MAX_DEDUP_PEERS) {
        ++_dedup_peers_overflow;
        return false;
    }

    auto& seq_set = _received_seq_sets[peer];
    auto& seq_window = _received_seq_windows[peer];

    seq_set.insert(seq_num);
    seq_window.push_back(seq_num);

    return false;
}

void GossipProtocol::process_retries() {
    // RAII Timer Safety
    try {
        [[maybe_unused]] auto timer_holder = _transport->timer_gate().hold();
    } catch (const seastar::gate_closed_exception&) {
        return;
    }

    if (!_transport || !_transport->is_ready() || !_running) {
        return;
    }

    auto now = seastar::lowres_clock::now();

    for (auto& [peer, pending_map] : _pending_acks) {
        std::vector<uint32_t> to_retry;
        std::vector<uint32_t> to_remove;

        for (auto& [seq_num, pending] : pending_map) {
            if (now >= pending.next_retry) {
                if (pending.retry_count >= _config.gossip_max_retries) {
                    to_remove.push_back(seq_num);
                    ++_max_retries_exceeded;
                    log_gossip_protocol().warn("Max retries exceeded for route announcement: peer={}, seq_num={}", peer, seq_num);
                } else {
                    to_retry.push_back(seq_num);
                }
            }
        }

        for (uint32_t seq_num : to_retry) {
            auto& pending = pending_map[seq_num];
            pending.retry_count++;

            auto backoff = calculate_backoff(pending.retry_count);
            pending.next_retry = now + backoff;

            log_gossip_protocol().debug("Retrying route announcement: peer={}, seq_num={}, retry={}/{}",
                                        peer, seq_num, pending.retry_count, _config.gossip_max_retries);

            (void)_transport->send(peer, pending.serialized_packet).then([this] {
                ++_retries_sent;
            }).handle_exception([peer, seq_num](auto ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    log_gossip_protocol().debug("Failed to retry to peer {}: {}", peer, e.what());
                }
            });
        }

        for (uint32_t seq_num : to_remove) {
            pending_map.erase(seq_num);
            --_stats_pending_acks_count;
        }
    }

    // Clean up empty peer entries
    for (auto it = _pending_acks.begin(); it != _pending_acks.end(); ) {
        if (it->second.empty()) {
            it = _pending_acks.erase(it);
        } else {
            ++it;
        }
    }
}

uint32_t GossipProtocol::next_seq_num(const seastar::socket_address& peer) {
    auto& counter = _peer_seq_counters[peer];
    return ++counter;
}

std::chrono::milliseconds GossipProtocol::calculate_backoff(uint32_t retry_count) const {
    auto base = _config.gossip_ack_timeout;
    auto multiplier = static_cast<uint32_t>(1) << std::min(retry_count, 3u);
    return std::min(base * multiplier, base * 8);
}

void GossipProtocol::cleanup_peer_state(const seastar::socket_address& peer) {
    // Clean up reliable delivery state
    auto pa_it = _pending_acks.find(peer);
    if (pa_it != _pending_acks.end()) {
        _stats_pending_acks_count -= pa_it->second.size();
        _pending_acks.erase(pa_it);
    }
    _peer_seq_counters.erase(peer);
    _received_seq_windows.erase(peer);
    _received_seq_sets.erase(peer);
}

void GossipProtocol::clear_pending_acks() {
    _pending_acks.clear();
    _stats_pending_acks_count = 0;
}

std::optional<seastar::socket_address> GossipProtocol::parse_peer_address(const std::string& peer) {
    auto colon_pos = peer.rfind(':');
    if (colon_pos == std::string::npos || colon_pos == 0 || colon_pos == peer.length() - 1) {
        return std::nullopt;
    }

    std::string host = peer.substr(0, colon_pos);
    std::string port_str = peer.substr(colon_pos + 1);

    try {
        uint16_t port = static_cast<uint16_t>(std::stoi(port_str));
        seastar::net::inet_address addr(host);
        return seastar::socket_address(addr, port);
    } catch (const std::exception& e) {
        log_gossip_protocol().debug("Failed to parse peer '{}': {}", peer, e.what());
        return std::nullopt;
    }
}

}  // namespace ranvier
