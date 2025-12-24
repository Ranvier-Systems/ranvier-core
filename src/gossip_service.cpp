// Ranvier Core - Gossip Service Implementation
//
// UDP-based gossip protocol for distributed state synchronization

#include "gossip_service.hpp"

#include <seastar/core/metrics.hh>
#include <seastar/core/sleep.hh>
#include <seastar/net/inet_address.hh>

namespace ranvier {

GossipService::GossipService(const ClusterConfig& config)
    : _config(config) {

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
            seastar::metrics::description("Total number of invalid gossip packets received"))
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

    log_gossip.info("Starting gossip service on port {}", _config.gossip_port);
    log_gossip.info("Configured peers: {}", _peer_addresses.size());

    // Create UDP channel bound to gossip port
    seastar::socket_address bind_addr(seastar::ipv4_addr("0.0.0.0", _config.gossip_port));

    return seastar::net::make_udp_channel(bind_addr).then([this](seastar::net::udp_channel channel) {
        _channel = std::move(channel);
        _running = true;

        log_gossip.info("Gossip UDP channel opened on port {}", _config.gossip_port);

        // Start receive loop in background (fire-and-forget)
        (void)receive_loop().handle_exception([](auto ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                log_gossip.error("Gossip receive loop error: {}", e.what());
            }
        });

        return seastar::make_ready_future<>();
    }).handle_exception([this](auto ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            log_gossip.error("Failed to start gossip service: {}", e.what());
        }
        return seastar::make_ready_future<>();
    });
}

seastar::future<> GossipService::stop() {
    if (!_running) {
        return seastar::make_ready_future<>();
    }

    log_gossip.info("Stopping gossip service");
    _running = false;

    if (_channel) {
        _channel->shutdown_input();
        _channel->shutdown_output();
        _channel = std::nullopt;
    }

    return seastar::make_ready_future<>();
}

void GossipService::set_route_learn_callback(RouteLearnCallback callback) {
    _route_learn_callback = std::move(callback);
}

seastar::future<> GossipService::broadcast_route(const std::vector<TokenId>& tokens, BackendId backend) {
    if (!_config.enabled || !_channel || _peer_addresses.empty()) {
        return seastar::make_ready_future<>();
    }

    // Build the announcement packet
    RouteAnnouncementPacket pkt;
    pkt.backend_id = backend;
    pkt.tokens = tokens;
    pkt.token_count = static_cast<uint16_t>(std::min(tokens.size(),
                                                      static_cast<size_t>(RouteAnnouncementPacket::MAX_TOKENS)));

    auto serialized = pkt.serialize();

    log_gossip.debug("Broadcasting route: {} tokens -> backend {} to {} peers",
                     tokens.size(), backend, _peer_addresses.size());

    // Send to all peers in parallel
    return seastar::parallel_for_each(_peer_addresses, [this, serialized](const seastar::socket_address& peer) {
        // Create a temporary buffer for this send
        auto frag = seastar::temporary_buffer<char>(serialized.size());
        std::memcpy(frag.get_write(), serialized.data(), serialized.size());

        return _channel->send(peer, seastar::net::packet(std::move(frag))).then([this] {
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
    return seastar::do_until([this] { return !_running; }, [this] {
        if (!_channel) {
            return seastar::sleep(std::chrono::milliseconds(100));
        }

        return _channel->receive().then([this](seastar::net::udp_datagram dgram) {
            return handle_packet(std::move(dgram));
        }).handle_exception([this](auto ep) {
            if (_running) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    log_gossip.debug("Receive error: {}", e.what());
                }
            }
            return seastar::make_ready_future<>();
        });
    });
}

seastar::future<> GossipService::handle_packet(seastar::net::udp_datagram&& dgram) {
    auto& data = dgram.get_data();

    // Parse the packet
    auto pkt = RouteAnnouncementPacket::deserialize(
        reinterpret_cast<const uint8_t*>(data.get()), data.len());

    if (!pkt) {
        _packets_invalid++;
        log_gossip.debug("Received invalid gossip packet from {}", dgram.get_src());
        return seastar::make_ready_future<>();
    }

    _packets_received++;

    log_gossip.debug("Received route announcement: {} tokens -> backend {} from {}",
                     pkt->tokens.size(), pkt->backend_id, dgram.get_src());

    // Call the route learn callback if set
    if (_route_learn_callback) {
        return _route_learn_callback(std::move(pkt->tokens), pkt->backend_id);
    }

    return seastar::make_ready_future<>();
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

}  // namespace ranvier
