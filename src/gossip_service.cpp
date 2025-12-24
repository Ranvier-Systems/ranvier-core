// Ranvier Core - Gossip Service Implementation
//
// UDP-based gossip protocol for distributed state synchronization

#include "gossip_service.hpp"

#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>
#include <seastar/net/api.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/ip.hh>
#include <seastar/net/udp.hh>

namespace ranvier {

GossipService::GossipService(const ClusterConfig& config)
    : _config(config),
    _receive_loop_future(seastar::make_ready_future<>()) {

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
    seastar::socket_address bind_addr(seastar::ipv4_addr("0.0.0.0", _config.gossip_port));

    try {
        // Synchronously create the channel.
        _channel = seastar::engine().net().make_bound_datagram_channel(bind_addr);
        _running = true;

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
    // Move the packet out and linearize it
    seastar::net::packet data = std::move(dgram.get_data());

    // This ensures data.fragments()[0] contains the ENTIRE packet
    data.linearize();

    // Now it is safe to access the full length from the first fragment
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data.fragments()[0].base);
    auto pkt = RouteAnnouncementPacket::deserialize(ptr, data.len());

    if (!pkt) {
        _packets_invalid++;
        return seastar::make_ready_future<>();
    }

    _packets_received++;

    // Ensure RouteAnnouncementPacket::deserialize creates OWNED data (like std::vector<int>)
    // and not views into the 'ptr' buffer.
    if (_route_learn_callback) {
        // We move pkt->tokens because the callback likely takes ownership
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
