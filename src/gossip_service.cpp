// Ranvier Core - Gossip Service Implementation
//
// UDP-based gossip protocol for distributed state synchronization

#include "gossip_service.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

#include <boost/range/irange.hpp>

#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/when_all.hh>
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
            seastar::metrics::description("Total number of failed DNS peer discovery operations")),
        // Reliable delivery metrics
        seastar::metrics::make_counter("cluster_acks_sent", _acks_sent,
            seastar::metrics::description("Total number of route ACKs sent")),
        seastar::metrics::make_counter("cluster_acks_received", _acks_received,
            seastar::metrics::description("Total number of route ACKs received")),
        seastar::metrics::make_counter("cluster_retries_sent", _retries_sent,
            seastar::metrics::description("Total number of route announcement retries sent")),
        seastar::metrics::make_counter("cluster_duplicates_suppressed", _duplicates_suppressed,
            seastar::metrics::description("Total number of duplicate route announcements suppressed")),
        seastar::metrics::make_counter("cluster_max_retries_exceeded", _max_retries_exceeded,
            seastar::metrics::description("Total number of route announcements that exceeded max retries")),
        // DTLS encryption metrics
        seastar::metrics::make_counter("cluster_dtls_handshakes_started", _dtls_handshakes_started,
            seastar::metrics::description("Total number of DTLS handshakes initiated")),
        seastar::metrics::make_counter("cluster_dtls_handshakes_completed", _dtls_handshakes_completed,
            seastar::metrics::description("Total number of DTLS handshakes completed successfully")),
        seastar::metrics::make_counter("cluster_dtls_handshakes_failed", _dtls_handshakes_failed,
            seastar::metrics::description("Total number of DTLS handshakes that failed")),
        seastar::metrics::make_counter("cluster_dtls_packets_encrypted", _dtls_packets_encrypted,
            seastar::metrics::description("Total number of packets encrypted with DTLS")),
        seastar::metrics::make_counter("cluster_dtls_packets_decrypted", _dtls_packets_decrypted,
            seastar::metrics::description("Total number of packets decrypted with DTLS")),
        seastar::metrics::make_counter("cluster_dtls_cert_reloads", _dtls_cert_reloads,
            seastar::metrics::description("Total number of certificate hot-reloads")),
        // Crypto stall watchdog metrics
        seastar::metrics::make_counter("cluster_crypto_stall_warnings", _crypto_stall_warnings,
            seastar::metrics::description("Total number of crypto operations exceeding stall threshold")),
        seastar::metrics::make_counter("cluster_crypto_ops_offloaded", _crypto_ops_offloaded,
            seastar::metrics::description("Total number of crypto operations offloaded to background thread")),
        seastar::metrics::make_counter("cluster_crypto_batch_broadcasts", _crypto_batch_broadcasts,
            seastar::metrics::description("Total number of batched broadcast operations")),
        // Split-brain detection / Quorum metrics
        seastar::metrics::make_gauge("cluster_quorum_state", _stats_quorum_state,
            seastar::metrics::description("Cluster quorum state: 1=healthy (quorum maintained), 0=degraded (quorum lost)")),
        seastar::metrics::make_counter("cluster_quorum_transitions", _quorum_transitions,
            seastar::metrics::description("Total number of quorum state transitions (healthy<->degraded)")),
        seastar::metrics::make_counter("cluster_routes_rejected_degraded", _routes_rejected_degraded,
            seastar::metrics::description("Total number of route broadcasts rejected due to degraded quorum state"))
    });
}

seastar::future<> GossipService::start() {
    if (!_config.enabled) {
        log_gossip.info("Gossip service disabled");
        co_return;
    }

    if (_peer_addresses.empty()) {
        log_gossip.warn("Gossip enabled but no valid peers configured");
    }

    // Mark as running on ALL shards so stop() logic remains consistent
    _running = true;

    // Only Shard 0 manages the physical hardware/UDP port
    if (seastar::this_shard_id() != 0) {
        log_gossip.debug("Gossip service started on worker shard {}", seastar::this_shard_id());
        co_return;
    }

    log_gossip.info("Starting gossip service on port {}", _config.gossip_port);
    log_gossip.info("Configured peers: {}", _peer_addresses.size());
    seastar::socket_address bind_addr(seastar::ipv4_addr("0.0.0.0", _config.gossip_port));

    try {
        // Synchronously create the channel.
        _channel = seastar::engine().net().make_bound_datagram_channel(bind_addr);

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

        // Initialize alive count (all peers start alive)
        _stats_cluster_peers_alive = _peer_table.size();

        // Calculate initial quorum state
        // At startup, all configured peers are assumed alive
        if (_config.quorum_enabled) {
            size_t total_nodes = _peer_table.size() + 1;  // +1 for self
            size_t required = quorum_required();  // Already capped at total_nodes
            size_t alive_nodes = _stats_cluster_peers_alive + 1;  // +1 for self

            if (alive_nodes >= required) {
                _quorum_state = QuorumState::HEALTHY;
                _stats_quorum_state = 1;
                log_gossip.info("Quorum initialized: HEALTHY (alive={}, required={}, total={})",
                               alive_nodes, required, total_nodes);
            } else {
                // This can happen if no peers are configured with high threshold
                _quorum_state = QuorumState::DEGRADED;
                _stats_quorum_state = 0;
                log_gossip.warn("Quorum initialized: DEGRADED - insufficient peers "
                               "(alive={}, required={}, total={})",
                               alive_nodes, required, total_nodes);
            }
        }

        // Initialize DTLS if enabled
        if (_config.tls.enabled) {
            co_await initialize_dtls();
        } else {
            log_gossip.warn("Gossip TLS is DISABLED - cluster traffic is unencrypted!");
            log_gossip.warn("This is a SECURITY RISK in production. Enable cluster.tls for encrypted gossip.");
        }

        // Set up reliable delivery retry timer if enabled
        if (_config.gossip_reliable_delivery) {
            log_gossip.info("Reliable delivery enabled: ack_timeout={}ms, max_retries={}, dedup_window={}",
                            _config.gossip_ack_timeout.count(),
                            _config.gossip_max_retries,
                            _config.gossip_dedup_window);
            _retry_timer.set_callback([this] { process_retries(); });
            // Check retries at half the ack timeout interval for responsiveness
            auto retry_check_interval = std::max(
                std::chrono::milliseconds(10),
                _config.gossip_ack_timeout / 2);
            _retry_timer.arm_periodic(retry_check_interval);
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

        // Service started successfully.
        co_return;
    } catch (...) {
        // If bind fails (e.g. EADDRINUSE), rethrow to fail the coroutine
        throw;
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

    // Stop reliable delivery retry timer
    _retry_timer.cancel();

    // Clear pending ACKs (no point in retrying during shutdown)
    _pending_acks.clear();

    // Stop DNS discovery
    _discovery_enabled = false;
    _discovery_timer.cancel();

    // Stop DTLS timers
    _cert_reload_timer.cancel();
    _dtls_session_cleanup_timer.cancel();

    // Close the handshake gate and wait for in-flight handshakes to complete
    co_await _handshake_gate.close();

    // Clean up DTLS context
    if (_dtls_context) {
        _dtls_context.reset();
        log_gossip.debug("DTLS context released");
    }

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

    // Split-brain protection: reject new route writes when quorum is lost
    if (_config.quorum_enabled && _config.reject_routes_on_quorum_loss && is_degraded()) {
        ++_routes_rejected_degraded;
        log_gossip.debug("Route broadcast rejected: cluster in DEGRADED mode (no quorum). "
                        "tokens={}, backend={}", tokens.size(), backend);
        return seastar::make_ready_future<>();
    }

    log_gossip.debug("Broadcasting route: {} tokens -> backend {} to {} peers",
                     tokens.size(), backend, _peer_addresses.size());

    // Send to each peer with per-peer sequence numbers
    // Note: Operators should ensure the peer list does not include the node's own address.
    return seastar::parallel_for_each(_peer_addresses, [this, tokens, backend](const seastar::socket_address& peer) mutable {
        // Create packet with per-peer sequence number
        RouteAnnouncementPacket pkt;
        pkt.backend_id = backend;
        pkt.tokens = tokens;
        pkt.token_count = static_cast<uint16_t>(std::min(tokens.size(),
                                                          static_cast<size_t>(RouteAnnouncementPacket::MAX_TOKENS)));

        // Assign sequence number if reliable delivery is enabled
        if (_config.gossip_reliable_delivery) {
            pkt.seq_num = next_seq_num(peer);
        }

        auto serialized = pkt.serialize();

        // Track pending ACK if reliable delivery is enabled
        if (_config.gossip_reliable_delivery) {
            PendingAck pending;
            pending.seq_num = pkt.seq_num;
            pending.serialized_packet = serialized;
            pending.next_retry = seastar::lowres_clock::now() + _config.gossip_ack_timeout;
            pending.retry_count = 0;
            _pending_acks[peer][pkt.seq_num] = std::move(pending);
            log_gossip.trace("Tracking pending ACK: peer={}, seq_num={}", peer, pkt.seq_num);
        }

        // Use DTLS encryption if enabled
        if (_dtls_context && _dtls_context->is_enabled()) {
            return send_encrypted(peer, serialized).then([this] {
                _packets_sent++;
            }).handle_exception([peer](auto ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    log_gossip.debug("Failed to send encrypted to peer {}: {}", peer, e.what());
                }
            });
        }

        // Plaintext mode: create an owned buffer by copying the data
        // Note: temporary_buffer(ptr, len) creates a non-owning view, which would cause
        // use-after-free since 'serialized' goes out of scope before async send completes
        seastar::temporary_buffer<char> buf(serialized.size());
        std::memcpy(buf.get_write(), serialized.data(), serialized.size());
        seastar::net::packet packet(std::move(buf));

        return _channel->send(peer, std::move(packet)).then([this] {
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
    const uint8_t* raw_ptr = reinterpret_cast<const uint8_t*>(data.fragments()[0].base);
    size_t raw_len = data.len();

    // Decrypt if DTLS is enabled
    const uint8_t* ptr = raw_ptr;
    size_t len = raw_len;
    std::optional<std::vector<uint8_t>> decrypted;

    if (_dtls_context && _dtls_context->is_enabled()) {
        decrypted = decrypt_packet(src_addr, raw_ptr, raw_len);
        if (!decrypted) {
            // Either handshake data (no application data) or decryption failed
            // Handshake responses are sent inside decrypt_packet
            return seastar::make_ready_future<>();
        }
        ptr = decrypted->data();
        len = decrypted->size();
    }

    // Identify packet type (need at least 1 byte)
    if (len < 1) {
        _packets_invalid++;
        return seastar::make_ready_future<>();
    }

    GossipPacketType type = static_cast<GossipPacketType>(ptr[0]);

    if (type == GossipPacketType::HEARTBEAT) {
        log_gossip.debug("Received heartbeat from {}", dgram.get_src());
        return seastar::make_ready_future<>();
    }

    // Handle ACK packets
    if (type == GossipPacketType::ROUTE_ACK) {
        auto ack_pkt = RouteAckPacket::deserialize(ptr, len);
        if (!ack_pkt) {
            _packets_invalid++;
            return seastar::make_ready_future<>();
        }

        handle_ack(src_addr, ack_pkt->seq_num);
        return seastar::make_ready_future<>();
    }

    // Handle route announcements
    auto pkt = RouteAnnouncementPacket::deserialize(ptr, len);
    if (!pkt) {
        _packets_invalid++;
        return seastar::make_ready_future<>();
    }

    // Check for duplicate if reliable delivery is enabled
    if (_config.gossip_reliable_delivery) {
        if (is_duplicate(src_addr, pkt->seq_num)) {
            log_gossip.trace("Duplicate route announcement suppressed: peer={}, seq_num={}", src_addr, pkt->seq_num);
            ++_duplicates_suppressed;
            // Still send ACK for duplicate (sender may not have received our previous ACK)
            return send_ack(src_addr, pkt->seq_num);
        }
    }

    // Map this peer address to the backend ID it just announced
    if (seastar::this_shard_id() == 0) {
        auto peer_it = _peer_table.find(src_addr);
        if (peer_it != _peer_table.end()) {
            peer_it->second.associated_backend = pkt->backend_id;
        }
    }

    ++_packets_received;

    // Send ACK if reliable delivery is enabled
    seastar::future<> ack_future = seastar::make_ready_future<>();
    if (_config.gossip_reliable_delivery) {
        ack_future = send_ack(src_addr, pkt->seq_num);
    }

    if (_route_learn_callback) {
        auto shared_tokens = std::make_shared<std::vector<TokenId>>(std::move(pkt->tokens));
        auto b_id = pkt->backend_id;
        // Copy the callback to avoid cross-shard memory access.
        // The callback captures RouterService 'this', but learn_route_remote()
        // only accesses thread_local data, making it safe to call from any shard.
        auto callback = _route_learn_callback;

        // Use an integer range from 0 to seastar::smp::count
        auto learn_future = seastar::parallel_for_each(
                boost::irange<unsigned>(0, seastar::smp::count),
                [callback, shared_tokens, b_id](unsigned shard_id) {
                return seastar::smp::submit_to(shard_id, [callback, shared_tokens, b_id] {
                        return callback(*shared_tokens, b_id);
                        });
                });

        return seastar::when_all_succeed(std::move(ack_future), std::move(learn_future)).discard_result();
    }

    return ack_future;
}

seastar::future<> GossipService::broadcast_heartbeat() {
    if (!_channel || _peer_addresses.empty()) {
        return seastar::make_ready_future<>();
    }

    // Prepare a small 2-byte heartbeat (Type + Version)
    std::vector<uint8_t> heartbeat_data = {
        static_cast<uint8_t>(GossipPacketType::HEARTBEAT),
        static_cast<uint8_t>(RouteAnnouncementPacket::PROTOCOL_VERSION)
    };

    // Use DTLS encryption if enabled - use broadcast_encrypted for efficiency
    if (_dtls_context && _dtls_context->is_enabled()) {
        return broadcast_encrypted(_peer_addresses, heartbeat_data);
    }

    // Plaintext mode: create an owned buffer to avoid use-after-free with async sends
    seastar::temporary_buffer<char> buf(2);
    buf.get_write()[0] = static_cast<char>(GossipPacketType::HEARTBEAT);
    buf.get_write()[1] = static_cast<char>(RouteAnnouncementPacket::PROTOCOL_VERSION);
    seastar::net::packet pb(std::move(buf));

    return seastar::parallel_for_each(_peer_addresses, [this, p = pb.share()](const seastar::socket_address& peer) mutable {
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

    // Update quorum state after liveness check
    if (_config.quorum_enabled) {
        update_quorum_state();
    }
}

size_t GossipService::quorum_required() const {
    // Quorum = floor(N * threshold) + 1 where N is total peers (including self)
    // For standard majority (threshold=0.5), this gives N/2+1
    // Example: 5 nodes (4 peers + self) with threshold=0.5 -> floor(5*0.5)+1 = 3 required
    size_t total_nodes = _peer_table.size() + 1;  // +1 for self
    size_t required = static_cast<size_t>(std::floor(total_nodes * _config.quorum_threshold)) + 1;

    // Cap at total_nodes to prevent impossible quorum requirements
    // (e.g., threshold=1.0 would give N+1 which exceeds cluster size)
    return std::min(required, total_nodes);
}

void GossipService::update_quorum_state() {
    // Only run on shard 0 since it manages the peer table
    if (seastar::this_shard_id() != 0) {
        return;
    }

    size_t total_nodes = _peer_table.size() + 1;  // +1 for self
    size_t alive_nodes = _stats_cluster_peers_alive + 1;  // +1 for self (we're always alive)
    size_t required = quorum_required();  // Already capped at total_nodes

    QuorumState new_state = (alive_nodes >= required) ? QuorumState::HEALTHY : QuorumState::DEGRADED;

    // Handle state transitions first (log before warning for clearer message ordering)
    if (new_state != _quorum_state) {
        ++_quorum_transitions;

        if (new_state == QuorumState::DEGRADED) {
            // Entering degraded mode - reset warning flag
            _quorum_warning_active = false;
            log_gossip.error("QUORUM LOST: Cluster entering DEGRADED mode. "
                            "Only {}/{} nodes reachable (need {} for quorum). "
                            "New route writes will be rejected to prevent split-brain divergence.",
                            alive_nodes, total_nodes, required);
        } else {
            log_gossip.info("QUORUM RESTORED: Cluster returning to HEALTHY mode. "
                           "{}/{} nodes reachable (need {} for quorum). "
                           "Route writes re-enabled.",
                           alive_nodes, total_nodes, required);
        }

        _quorum_state = new_state;
        _stats_quorum_state = (new_state == QuorumState::HEALTHY) ? 1 : 0;
    }

    // Check for warning threshold: approaching quorum loss
    // Rate-limited: only log when entering/exiting warning zone, not every check
    // Done after state transition so "QUORUM RESTORED" logs before "WARNING" if applicable
    if (_config.quorum_warning_threshold > 0 && new_state == QuorumState::HEALTHY) {
        size_t margin = alive_nodes - required;
        bool should_warn = margin <= _config.quorum_warning_threshold;

        if (should_warn && !_quorum_warning_active) {
            // Entering warning zone
            _quorum_warning_active = true;
            log_gossip.warn("QUORUM WARNING: Only {} node(s) above quorum threshold "
                           "(alive={}, required={}, total={}). Cluster at risk of split-brain.",
                           margin, alive_nodes, required, total_nodes);
        } else if (!should_warn && _quorum_warning_active) {
            // Exiting warning zone (recovered)
            _quorum_warning_active = false;
            log_gossip.info("QUORUM WARNING CLEARED: Cluster has sufficient margin "
                           "(alive={}, required={}, total={}).",
                           alive_nodes, required, total_nodes);
        }
    }
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

        // Log peers that were removed and clean up their state
        for (const auto& [addr, state] : _peer_table) {
            if (new_peer_table.find(addr) == new_peer_table.end()) {
                log_gossip.info("DNS discovery: peer removed: {}", addr);

                // Clean up reliable delivery state for removed peer
                _pending_acks.erase(addr);
                _peer_seq_counters.erase(addr);
                _received_seq_windows.erase(addr);
                _received_seq_sets.erase(addr);

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

// Reliable delivery: send an ACK for a received packet
seastar::future<> GossipService::send_ack(const seastar::socket_address& peer, uint32_t seq_num) {
    if (!_channel) {
        return seastar::make_ready_future<>();
    }

    RouteAckPacket ack;
    ack.seq_num = seq_num;
    auto serialized = ack.serialize();

    log_gossip.trace("Sending ACK: peer={}, seq_num={}", peer, seq_num);

    // Use DTLS encryption if enabled
    if (_dtls_context && _dtls_context->is_enabled()) {
        return send_encrypted(peer, serialized).then([this] {
            ++_acks_sent;
        }).handle_exception([peer, seq_num](auto ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                log_gossip.debug("Failed to send encrypted ACK to peer {}: {}", peer, e.what());
            }
        });
    }

    // Plaintext mode: create an owned buffer to avoid use-after-free with async send
    seastar::temporary_buffer<char> buf(serialized.size());
    std::memcpy(buf.get_write(), serialized.data(), serialized.size());
    seastar::net::packet packet(std::move(buf));

    return _channel->send(peer, std::move(packet)).then([this] {
        ++_acks_sent;
    }).handle_exception([peer, seq_num](auto ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            log_gossip.debug("Failed to send ACK to peer {}: {}", peer, e.what());
        }
    });
}

// Reliable delivery: handle a received ACK
void GossipService::handle_ack(const seastar::socket_address& peer, uint32_t seq_num) {
    auto peer_it = _pending_acks.find(peer);
    if (peer_it == _pending_acks.end()) {
        log_gossip.trace("Received ACK for unknown peer: peer={}, seq_num={}", peer, seq_num);
        return;
    }

    auto& pending_map = peer_it->second;
    auto ack_it = pending_map.find(seq_num);
    if (ack_it == pending_map.end()) {
        log_gossip.trace("Received ACK for unknown seq_num: peer={}, seq_num={}", peer, seq_num);
        return;
    }

    log_gossip.trace("Received ACK: peer={}, seq_num={}", peer, seq_num);
    pending_map.erase(ack_it);
    ++_acks_received;

    // Clean up empty peer entry
    if (pending_map.empty()) {
        _pending_acks.erase(peer_it);
    }
}

// Reliable delivery: check if a sequence number is a duplicate
bool GossipService::is_duplicate(const seastar::socket_address& peer, uint32_t seq_num) {
    auto& seq_set = _received_seq_sets[peer];
    auto& seq_window = _received_seq_windows[peer];

    // Check if we've seen this sequence number
    if (seq_set.count(seq_num) > 0) {
        return true;
    }

    // Add to window
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

// Reliable delivery: process pending retries
void GossipService::process_retries() {
    if (!_channel || !_running) {
        return;
    }

    auto now = seastar::lowres_clock::now();

    for (auto& [peer, pending_map] : _pending_acks) {
        // Collect entries to retry or remove (can't modify while iterating)
        std::vector<uint32_t> to_retry;
        std::vector<uint32_t> to_remove;

        for (auto& [seq_num, pending] : pending_map) {
            if (now >= pending.next_retry) {
                if (pending.retry_count >= _config.gossip_max_retries) {
                    // Max retries exceeded, give up
                    to_remove.push_back(seq_num);
                    ++_max_retries_exceeded;
                    log_gossip.warn("Max retries exceeded for route announcement: peer={}, seq_num={}", peer, seq_num);
                } else {
                    to_retry.push_back(seq_num);
                }
            }
        }

        // Process retries
        for (uint32_t seq_num : to_retry) {
            auto& pending = pending_map[seq_num];
            pending.retry_count++;

            // Calculate next retry time with exponential backoff
            auto backoff = calculate_backoff(pending.retry_count);
            pending.next_retry = now + backoff;

            log_gossip.debug("Retrying route announcement: peer={}, seq_num={}, retry={}/{}",
                           peer, seq_num, pending.retry_count, _config.gossip_max_retries);

            // Use DTLS encryption if enabled
            if (_dtls_context && _dtls_context->is_enabled()) {
                (void)send_encrypted(peer, pending.serialized_packet).then([this] {
                    ++_retries_sent;
                }).handle_exception([peer, seq_num](auto ep) {
                    try {
                        std::rethrow_exception(ep);
                    } catch (const std::exception& e) {
                        log_gossip.debug("Failed to retry encrypted to peer {}: {}", peer, e.what());
                    }
                });
            } else {
                // Plaintext mode: resend the packet - create owned buffer for safety with async send
                seastar::temporary_buffer<char> buf(pending.serialized_packet.size());
                std::memcpy(buf.get_write(), pending.serialized_packet.data(), pending.serialized_packet.size());
                seastar::net::packet packet(std::move(buf));

                (void)_channel->send(peer, std::move(packet)).then([this] {
                    ++_retries_sent;
                }).handle_exception([peer, seq_num](auto ep) {
                    try {
                        std::rethrow_exception(ep);
                    } catch (const std::exception& e) {
                        log_gossip.debug("Failed to retry to peer {}: {}", peer, e.what());
                    }
                });
            }
        }

        // Remove entries that exceeded max retries
        for (uint32_t seq_num : to_remove) {
            pending_map.erase(seq_num);
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

// Reliable delivery: get next sequence number for a peer
uint32_t GossipService::next_seq_num(const seastar::socket_address& peer) {
    auto& counter = _peer_seq_counters[peer];
    return ++counter;  // Pre-increment so we start at 1, not 0
}

// Reliable delivery: calculate backoff delay for retry
std::chrono::milliseconds GossipService::calculate_backoff(uint32_t retry_count) const {
    // Exponential backoff: 100ms, 200ms, 400ms, ...
    // Capped at 8x the base timeout
    auto base = _config.gossip_ack_timeout;
    auto multiplier = static_cast<uint32_t>(1) << std::min(retry_count, 3u);  // 1, 2, 4, 8
    return std::min(base * multiplier, base * 8);
}

//------------------------------------------------------------------------------
// DTLS Helper Methods
//------------------------------------------------------------------------------

seastar::future<> GossipService::initialize_dtls() {
    log_gossip.info("Initializing DTLS for gossip encryption");

    _dtls_context = std::make_unique<DtlsContext>(_config.tls);

    auto err = _dtls_context->initialize();
    if (err) {
        log_gossip.error("Failed to initialize DTLS: {}", *err);
        if (!_config.tls.allow_plaintext_fallback) {
            throw std::runtime_error("DTLS initialization failed: " + *err);
        }
        log_gossip.warn("Falling back to plaintext mode (allow_plaintext_fallback=true)");
        _dtls_context.reset();
        co_return;
    }

    log_gossip.info("DTLS initialized successfully");
    log_gossip.info("DTLS peer verification: {}", _config.tls.verify_peer ? "enabled (mTLS)" : "disabled");

    // Set up certificate reload timer if enabled
    if (_config.tls.cert_reload_interval.count() > 0) {
        log_gossip.info("Certificate hot-reload enabled: interval={}s",
                        _config.tls.cert_reload_interval.count());
        _cert_reload_timer.set_callback([this] {
            // Fire-and-forget the async cert reload check
            (void)check_cert_reload().handle_exception([](auto ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    log_gossip.error("Certificate reload check failed: {}", e.what());
                }
            });
        });
        _cert_reload_timer.arm_periodic(_config.tls.cert_reload_interval);
    }

    // Set up DTLS session cleanup timer (cleanup idle sessions every minute)
    _dtls_session_cleanup_timer.set_callback([this] { cleanup_dtls_sessions(); });
    _dtls_session_cleanup_timer.arm_periodic(std::chrono::seconds(60));

    // Initiate handshakes with all configured peers
    for (const auto& peer : _peer_addresses) {
        auto* session = _dtls_context->get_or_create_session(peer, false);  // Client role
        if (session) {
            std::vector<uint8_t> handshake_data;
            auto result = session->initiate_handshake(handshake_data);
            if (result == DtlsResult::WANT_READ && !handshake_data.empty()) {
                ++_dtls_handshakes_started;
                // Send ClientHello to peer
                seastar::temporary_buffer<char> buf(handshake_data.size());
                std::memcpy(buf.get_write(), handshake_data.data(), handshake_data.size());
                seastar::net::packet packet(std::move(buf));
                (void)_channel->send(peer, std::move(packet)).handle_exception([peer](auto ep) {
                    log_gossip.debug("Failed to send DTLS handshake to {}: {}", peer, ep);
                });
            }
        }
    }

    co_return;
}

seastar::future<> GossipService::send_encrypted(const seastar::socket_address& peer,
                                                 const std::vector<uint8_t>& plaintext) {
    if (!_dtls_context || !_dtls_context->is_enabled()) {
        // DTLS not enabled, send plaintext
        seastar::temporary_buffer<char> buf(plaintext.size());
        std::memcpy(buf.get_write(), plaintext.data(), plaintext.size());
        seastar::net::packet packet(std::move(buf));
        return _channel->send(peer, std::move(packet));
    }

    auto* session = _dtls_context->get_or_create_session(peer, false);
    if (!session) {
        log_gossip.debug("Failed to get DTLS session for peer {}", peer);
        return seastar::make_ready_future<>();
    }

    // Check if handshake is complete
    if (!session->is_established()) {
        // Handshake not complete - queue data or wait
        log_gossip.trace("DTLS handshake not complete for peer {}, cannot send yet", peer);
        return seastar::make_ready_future<>();
    }

    // For large packets, offload encryption to seastar::thread to avoid reactor stalls
    if (plaintext.size() > CRYPTO_OFFLOAD_BYTES_THRESHOLD) {
        ++_crypto_ops_offloaded;

        // Copy plaintext for the thread context (plaintext ref may not survive the async boundary)
        auto plaintext_copy = std::make_shared<std::vector<uint8_t>>(plaintext);

        return seastar::async([this, peer, plaintext_copy, session]() {
            // Run encryption in the thread context
            std::vector<uint8_t> encrypted;
            auto start = std::chrono::steady_clock::now();
            auto result = session->encrypt(plaintext_copy->data(), plaintext_copy->size(), encrypted);
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (static_cast<uint64_t>(elapsed_us) > CRYPTO_STALL_WARNING_US) {
                ++_crypto_stall_warnings;
                log_gossip.warn("Crypto stall (offloaded): encrypt took {}μs for {} bytes to peer {}",
                               elapsed_us, plaintext_copy->size(), peer);
            }

            if (result != DtlsResult::SUCCESS || encrypted.empty()) {
                log_gossip.debug("Failed to encrypt data for peer {}", peer);
                return;
            }

            ++_dtls_packets_encrypted;

            // Send encrypted data (back on reactor context)
            seastar::temporary_buffer<char> buf(encrypted.size());
            std::memcpy(buf.get_write(), encrypted.data(), encrypted.size());
            seastar::net::packet packet(std::move(buf));
            (void)_channel->send(peer, std::move(packet)).handle_exception([peer](auto ep) {
                log_gossip.debug("Failed to send encrypted data to {}: {}", peer, ep);
            });
        });
    }

    // Small packets: encrypt inline with stall watchdog
    std::vector<uint8_t> encrypted;
    auto encrypt_result = with_stall_watchdog("encrypt", [&]() {
        return session->encrypt(plaintext.data(), plaintext.size(), encrypted);
    });

    if (encrypt_result != DtlsResult::SUCCESS || encrypted.empty()) {
        log_gossip.debug("Failed to encrypt data for peer {}", peer);
        return seastar::make_ready_future<>();
    }

    ++_dtls_packets_encrypted;

    // Send encrypted data
    seastar::temporary_buffer<char> buf(encrypted.size());
    std::memcpy(buf.get_write(), encrypted.data(), encrypted.size());
    seastar::net::packet packet(std::move(buf));
    return _channel->send(peer, std::move(packet));
}

std::optional<std::vector<uint8_t>> GossipService::decrypt_packet(const seastar::socket_address& peer,
                                                                   const uint8_t* data, size_t len) {
    if (!_dtls_context || !_dtls_context->is_enabled()) {
        // DTLS not enabled, return data as-is
        return std::vector<uint8_t>(data, data + len);
    }

    auto* session = _dtls_context->get_or_create_session(peer, true);  // Server role (received first)
    if (!session) {
        log_gossip.debug("Failed to get DTLS session for peer {}", peer);
        return std::nullopt;
    }

    // If not established, this is handshake data
    if (!session->is_established()) {
        std::vector<uint8_t> response;
        auto result = session->continue_handshake(data, len, response);

        if (result == DtlsResult::SUCCESS) {
            ++_dtls_handshakes_completed;
            log_gossip.info("DTLS handshake completed with peer {}", peer);
        } else if (result == DtlsResult::ERROR) {
            ++_dtls_handshakes_failed;
            log_gossip.error("DTLS handshake failed with peer {}: {}", peer, session->last_error());
            _dtls_context->remove_session(peer);
            return std::nullopt;
        }

        // Send handshake response if any
        if (!response.empty() && _channel) {
            seastar::temporary_buffer<char> buf(response.size());
            std::memcpy(buf.get_write(), response.data(), response.size());
            seastar::net::packet packet(std::move(buf));
            (void)_channel->send(peer, std::move(packet)).handle_exception([peer](auto ep) {
                log_gossip.debug("Failed to send DTLS handshake response to {}: {}", peer, ep);
            });
        }

        // Handshake data processed, no application data to return
        return std::nullopt;
    }

    // Session established, decrypt the data with stall watchdog
    std::vector<uint8_t> decrypted;
    auto start = std::chrono::steady_clock::now();
    auto result = session->decrypt(data, len, decrypted);
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();

    if (static_cast<uint64_t>(elapsed_us) > CRYPTO_STALL_WARNING_US) {
        ++_crypto_stall_warnings;
        log_gossip.warn("Crypto stall detected: decrypt took {}μs for {} bytes from peer {}",
                       elapsed_us, len, peer);
    }

    if (result != DtlsResult::SUCCESS) {
        if (result == DtlsResult::WANT_READ) {
            // Might be more handshake data, process it
            std::vector<uint8_t> response;
            session->continue_handshake(data, len, response);
            if (!response.empty() && _channel) {
                seastar::temporary_buffer<char> buf(response.size());
                std::memcpy(buf.get_write(), response.data(), response.size());
                seastar::net::packet packet(std::move(buf));
                (void)_channel->send(peer, std::move(packet));
            }
        }
        return std::nullopt;
    }

    ++_dtls_packets_decrypted;
    return decrypted;
}

seastar::future<> GossipService::handle_dtls_handshake(const seastar::socket_address& peer,
                                                        const uint8_t* data, size_t len) {
    // This is called when we receive potential handshake data
    // The decrypt_packet function handles handshake internally
    auto result = decrypt_packet(peer, data, len);
    // If result has value, it's application data that was decrypted after handshake
    // We ignore it here since handle_packet will call decrypt_packet again
    return seastar::make_ready_future<>();
}

// Parallel broadcast: batch encrypt and send to multiple peers
// Uses seastar::thread when peer count exceeds threshold to avoid reactor stalls
seastar::future<> GossipService::broadcast_encrypted(const std::vector<seastar::socket_address>& peers,
                                                      const std::vector<uint8_t>& plaintext) {
    if (!_dtls_context || !_dtls_context->is_enabled() || peers.empty() || !_channel) {
        return seastar::make_ready_future<>();
    }

    // For high fan-out broadcasts, use seastar::thread to batch the crypto work
    if (peers.size() > CRYPTO_OFFLOAD_PEER_THRESHOLD) {
        ++_crypto_batch_broadcasts;
        ++_crypto_ops_offloaded;

        // Copy data for thread safety
        auto plaintext_copy = std::make_shared<std::vector<uint8_t>>(plaintext);
        auto peers_copy = std::make_shared<std::vector<seastar::socket_address>>(peers);

        return seastar::async([this, plaintext_copy, peers_copy]() {
            // Pre-encrypt for all peers in the thread context with batch timing
            std::vector<std::pair<seastar::socket_address, std::vector<uint8_t>>> encrypted_packets;
            encrypted_packets.reserve(peers_copy->size());

            auto batch_start = std::chrono::steady_clock::now();

            for (const auto& peer : *peers_copy) {
                auto* session = _dtls_context->get_or_create_session(peer, false);
                if (!session || !session->is_established()) {
                    continue;
                }

                std::vector<uint8_t> encrypted;
                auto result = session->encrypt(plaintext_copy->data(), plaintext_copy->size(), encrypted);
                if (result == DtlsResult::SUCCESS && !encrypted.empty()) {
                    ++_dtls_packets_encrypted;
                    encrypted_packets.emplace_back(peer, std::move(encrypted));
                }
            }

            auto batch_elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - batch_start).count();

            // Log if the batch operation stalled (use higher threshold for batched ops)
            // Threshold scales with peer count: 100μs per peer
            uint64_t batch_threshold = CRYPTO_STALL_WARNING_US * peers_copy->size();
            if (static_cast<uint64_t>(batch_elapsed_us) > batch_threshold) {
                ++_crypto_stall_warnings;
                log_gossip.warn("Crypto batch stall: encrypted {} peers in {}μs (threshold: {}μs)",
                               encrypted_packets.size(), batch_elapsed_us, batch_threshold);
            }

            // Now send all encrypted packets (yields back to reactor between sends)
            for (auto& [peer, encrypted] : encrypted_packets) {
                seastar::temporary_buffer<char> buf(encrypted.size());
                std::memcpy(buf.get_write(), encrypted.data(), encrypted.size());
                seastar::net::packet packet(std::move(buf));
                (void)_channel->send(peer, std::move(packet)).handle_exception([peer](auto ep) {
                    log_gossip.debug("Failed to send batched encrypted to {}: {}", peer, ep);
                });

                // Yield between sends to avoid monopolizing the reactor
                seastar::thread::yield();
            }

            log_gossip.trace("Batch broadcast completed: {} packets encrypted and sent", encrypted_packets.size());
        });
    }

    // For small peer counts, use parallel_for_each (existing behavior)
    return seastar::parallel_for_each(peers, [this, &plaintext](const seastar::socket_address& peer) {
        return send_encrypted(peer, plaintext);
    });
}

// Async check and reload certificates with parallel handshake initiation
seastar::future<> GossipService::check_cert_reload() {
    if (!_dtls_context) {
        co_return;
    }

    if (!_dtls_context->check_and_reload_certs()) {
        co_return;  // No reload needed
    }

    ++_dtls_cert_reloads;
    log_gossip.info("Certificates reloaded successfully, re-initiating handshakes with {} peers",
                    _peer_addresses.size());

    // Re-initiate handshakes with all peers using parallel_for_each with gating
    // The gate limits the number of concurrent handshakes to prevent SMP storms
    co_await seastar::parallel_for_each(_peer_addresses, [this](const seastar::socket_address& peer) -> seastar::future<> {
        // Use the gate to limit concurrent handshakes
        try {
            auto holder = _handshake_gate.hold();

            auto* session = _dtls_context->get_or_create_session(peer, false);
            if (!session) {
                co_return;
            }

            std::vector<uint8_t> handshake_data;
            auto result = session->initiate_handshake(handshake_data);
            if (result == DtlsResult::WANT_READ && !handshake_data.empty() && _channel) {
                ++_dtls_handshakes_started;
                seastar::temporary_buffer<char> buf(handshake_data.size());
                std::memcpy(buf.get_write(), handshake_data.data(), handshake_data.size());
                seastar::net::packet packet(std::move(buf));
                co_await _channel->send(peer, std::move(packet)).handle_exception([peer](auto ep) {
                    log_gossip.debug("Failed to send handshake to {}: {}", peer, ep);
                });
            }
        } catch (const seastar::gate_closed_exception&) {
            // Service is shutting down, skip this handshake
            log_gossip.debug("Handshake skipped during shutdown for peer {}", peer);
        }
    });

    log_gossip.info("Certificate reload handshakes initiated for all peers");
}

void GossipService::cleanup_dtls_sessions() {
    if (!_dtls_context) {
        return;
    }

    // Clean up sessions idle for more than 5 minutes
    _dtls_context->cleanup_idle_sessions(std::chrono::seconds(300));
}

}  // namespace ranvier
