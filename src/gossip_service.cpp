// Ranvier Core - Gossip Service Implementation
//
// UDP-based gossip protocol for distributed state synchronization

#include "gossip_service.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
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
        seastar::metrics::make_counter("cluster_dedup_peers_overflow", _dedup_peers_overflow,
            seastar::metrics::description("Times dedup peer limit was reached, new peers not tracked (Rule #4: bounded containers)")),
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
            seastar::metrics::description("Total number of crypto operations offloaded to thread pool")),
        seastar::metrics::make_counter("cluster_crypto_batch_broadcasts", _crypto_batch_broadcasts,
            seastar::metrics::description("Total number of batched broadcast operations")),
        seastar::metrics::make_counter("cluster_crypto_stalls_avoided", _crypto_stalls_avoided,
            seastar::metrics::description("Total number of reactor stalls avoided via crypto offloading")),
        seastar::metrics::make_counter("cluster_crypto_handshakes_offloaded", _crypto_handshakes_offloaded,
            seastar::metrics::description("Total number of DTLS handshakes offloaded to thread pool")),
        // Split-brain detection / Quorum metrics
        seastar::metrics::make_gauge("cluster_quorum_state", _stats_quorum_state,
            seastar::metrics::description("Cluster quorum state: 1=healthy (quorum maintained), 0=degraded (quorum lost)")),
        seastar::metrics::make_counter("cluster_quorum_transitions", _quorum_transitions,
            seastar::metrics::description("Total number of quorum state transitions (healthy<->degraded)")),
        seastar::metrics::make_counter("cluster_routes_rejected_degraded", _routes_rejected_degraded,
            seastar::metrics::description("Total number of route broadcasts rejected due to degraded quorum state")),
        seastar::metrics::make_counter("cluster_routes_rejected_incoming_degraded", _routes_rejected_incoming_degraded,
            seastar::metrics::description("Total number of incoming routes rejected due to degraded quorum state")),
        seastar::metrics::make_gauge("cluster_peers_recently_seen", _stats_peers_recently_seen,
            seastar::metrics::description("Number of peers seen within quorum check window")),
        // DTLS lockdown metrics
        seastar::metrics::make_counter("cluster_dtls_lockdown_drops", _dtls_lockdown_drops,
            seastar::metrics::description("Total number of packets dropped due to mTLS lockdown enforcement")),
        // Node state notification metrics
        seastar::metrics::make_counter("cluster_node_state_sent", _node_state_sent,
            seastar::metrics::description("Total number of node state notifications sent (e.g., DRAINING)")),
        seastar::metrics::make_counter("cluster_node_state_received", _node_state_received,
            seastar::metrics::description("Total number of node state notifications received from peers"))
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

            // Initialize crypto offloader for adaptive offloading of expensive operations
            initialize_crypto_offloader();
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

            // Set up periodic discovery refresh with RAII timer safety
            _discovery_timer.set_callback([this] {
                // RAII Timer Safety: Acquire gate holder to prevent execution during shutdown.
                try {
                    [[maybe_unused]] auto timer_holder = _timer_gate.hold();
                } catch (const seastar::gate_closed_exception&) {
                    return;
                }

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

    // RAII Timer Safety: Close the timer gate FIRST to ensure no timer callbacks
    // can execute during or after shutdown. This waits for any in-flight timer
    // callbacks to complete before proceeding.
    //
    // Order is critical:
    //   1. Set _running = false
    //   2. Close _timer_gate (waits for in-flight callbacks)
    //   3. Cancel all timers (prevents future callbacks)
    //   4. Shutdown channel and other resources
    //
    // This guarantees no timer callback can access `this` after stop() returns.
    co_await _timer_gate.close();
    log_gossip.debug("Timer gate closed - all timer callbacks completed");

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

    // Stop crypto offloader first (before closing gates)
    if (_crypto_offloader) {
        // Sync stats from offloader before stopping
        sync_crypto_offloader_stats();
        _crypto_offloader->stop();
        log_gossip.debug("Crypto offloader stopped");
    }

    // Close the gossip task gate and wait for in-flight gossip tasks
    co_await _gossip_task_gate.close();
    log_gossip.debug("Gossip task gate closed");

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

void GossipService::set_node_state_callback(NodeStateCallback callback) {
    _node_state_callback = std::move(callback);
}

seastar::future<> GossipService::broadcast_node_state(NodeState state) {
    if (!_config.enabled || !_channel || _peer_addresses.empty()) {
        return seastar::make_ready_future<>();
    }

    // Mark this node as draining if applicable
    if (state == NodeState::DRAINING) {
        _draining.store(true, std::memory_order_relaxed);
        log_gossip.info("Broadcasting DRAINING state to {} peers (local_backend_id={})",
                       _peer_addresses.size(), _local_backend_id);
    }

    // Create the node state packet
    NodeStatePacket pkt;
    pkt.state = state;
    pkt.backend_id = _local_backend_id;
    auto serialized = pkt.serialize();

    // Broadcast to all peers
    // Use DTLS encryption if enabled
    if (_dtls_context && _dtls_context->is_enabled()) {
        return broadcast_encrypted(_peer_addresses, serialized).then([this] {
            ++_node_state_sent;
        });
    }

    // Plaintext mode: create an owned buffer for async send
    auto serialized_copy = std::make_shared<std::vector<uint8_t>>(std::move(serialized));
    return seastar::parallel_for_each(_peer_addresses, [this, serialized_copy](const seastar::socket_address& peer) {
        seastar::temporary_buffer<char> buf(serialized_copy->size());
        std::memcpy(buf.get_write(), serialized_copy->data(), serialized_copy->size());
        seastar::net::packet packet(std::move(buf));

        return _channel->send(peer, std::move(packet)).handle_exception([peer](auto ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                log_gossip.debug("Failed to send node state to peer {}: {}", peer, e.what());
            }
        });
    }).then([this] {
        ++_node_state_sent;
    });
}

seastar::future<> GossipService::broadcast_route(const std::vector<TokenId>& tokens, BackendId backend) {
    if (!_config.enabled || !_channel || _peer_addresses.empty()) {
        return seastar::make_ready_future<>();
    }

    // Gossip protection: reject new gossip tasks during shutdown or re-sync
    if (!is_accepting_tasks()) {
        log_gossip.debug("Route broadcast rejected: gossip service not accepting tasks. "
                        "tokens={}, backend={}", tokens.size(), backend);
        return seastar::make_ready_future<>();
    }

    // Try to enter the gossip task gate (non-blocking for graceful rejection)
    seastar::gate::holder gate_holder;
    try {
        gate_holder = _gossip_task_gate.hold();
    } catch (const seastar::gate_closed_exception&) {
        log_gossip.debug("Route broadcast rejected: gossip gate closed. tokens={}, backend={}",
                        tokens.size(), backend);
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
    // Capture gate_holder to keep it alive for the duration of the async operation
    return seastar::parallel_for_each(_peer_addresses, [this, tokens, backend, gate_holder = std::move(gate_holder)](const seastar::socket_address& peer) mutable {
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

    // Move the packet out and linearize it
    seastar::net::packet data = std::move(dgram.get_data());

    // This ensures data.fragments()[0] contains the ENTIRE packet
    data.linearize();

    // Now it is safe to access the full length from the first fragment
    const uint8_t* raw_ptr = reinterpret_cast<const uint8_t*>(data.fragments()[0].base);
    size_t raw_len = data.len();

    // DTLS Security Lockdown: when mTLS is enabled, drop any packet that
    // is not a DTLS handshake and does not belong to an active DTLS session
    if (should_drop_packet_mtls_lockdown(src_addr, raw_ptr, raw_len)) {
        return seastar::make_ready_future<>();
    }

    // Update liveness after security checks pass
    update_peer_liveness(src_addr);

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

    // Handle node state packets (e.g., DRAINING notifications)
    if (type == GossipPacketType::NODE_STATE) {
        auto state_pkt = NodeStatePacket::deserialize(ptr, len);
        if (!state_pkt) {
            _packets_invalid++;
            return seastar::make_ready_future<>();
        }

        ++_node_state_received;
        log_gossip.info("Received NODE_STATE packet from {}: backend={}, state={}",
                       src_addr, state_pkt->backend_id,
                       state_pkt->state == NodeState::DRAINING ? "DRAINING" : "ACTIVE");

        // Invoke the callback to handle the state change
        if (_node_state_callback) {
            return _node_state_callback(state_pkt->backend_id, state_pkt->state);
        }
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

    // Split-brain protection: reject incoming route propagation when quorum is lost
    // This prevents a partitioned node from accepting potentially stale routes
    if (_config.quorum_enabled && _config.reject_routes_on_quorum_loss && is_degraded()) {
        ++_routes_rejected_incoming_degraded;
        log_gossip.debug("Incoming route rejected: cluster in DEGRADED mode (no quorum). "
                        "peer={}, tokens={}, backend={}", src_addr, pkt->tokens.size(), pkt->backend_id);
        // Still send ACK to avoid sender retries (we're explicitly rejecting, not failing)
        if (_config.gossip_reliable_delivery) {
            return send_ack(src_addr, pkt->seq_num);
        }
        return seastar::make_ready_future<>();
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
    // RAII Timer Safety: Acquire gate holder to prevent execution during shutdown.
    // If the gate is closed (stop() in progress), exit safely without accessing state.
    try {
        [[maybe_unused]] auto timer_holder = _timer_gate.hold();
    } catch (const seastar::gate_closed_exception&) {
        return seastar::make_ready_future<>();
    }

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
    // RAII Timer Safety: Acquire gate holder to prevent execution during shutdown.
    // If the gate is closed (stop() in progress), exit safely without accessing state.
    try {
        [[maybe_unused]] auto timer_holder = _timer_gate.hold();
    } catch (const seastar::gate_closed_exception&) {
        return;
    }

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
        // Use check_quorum() for stricter recently-seen counting
        check_quorum();
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

void GossipService::check_quorum() {
    // Only run on shard 0 since it manages the peer table
    if (seastar::this_shard_id() != 0) {
        return;
    }

    auto now = seastar::lowres_clock::now();
    size_t recently_seen = 0;

    // Count peers seen within the quorum check window
    for (const auto& [addr, state] : _peer_table) {
        auto time_since_seen = std::chrono::duration_cast<std::chrono::seconds>(now - state.last_seen);
        if (time_since_seen <= _config.quorum_check_window) {
            ++recently_seen;
        }
    }

    _stats_peers_recently_seen = recently_seen;

    // Use recently seen count for quorum calculation (more strict than just alive)
    size_t total_nodes = _peer_table.size() + 1;  // +1 for self
    size_t recently_seen_nodes = recently_seen + 1;  // +1 for self (we're always "seen")
    size_t required = quorum_required();

    QuorumState new_state = (recently_seen_nodes >= required) ? QuorumState::HEALTHY : QuorumState::DEGRADED;

    // Handle state transitions
    if (new_state != _quorum_state) {
        ++_quorum_transitions;

        if (new_state == QuorumState::DEGRADED) {
            _quorum_warning_active = false;
            log_gossip.error("QUORUM LOST (check_quorum): Cluster entering DEGRADED mode. "
                            "Only {}/{} nodes recently seen within {}s window (need {} for quorum). "
                            "New route propagation will be rejected to prevent split-brain divergence.",
                            recently_seen_nodes, total_nodes, _config.quorum_check_window.count(), required);
        } else {
            log_gossip.info("QUORUM RESTORED (check_quorum): Cluster returning to HEALTHY mode. "
                           "{}/{} nodes recently seen within {}s window (need {} for quorum). "
                           "Route propagation re-enabled.",
                           recently_seen_nodes, total_nodes, _config.quorum_check_window.count(), required);
        }

        _quorum_state = new_state;
        _stats_quorum_state = (new_state == QuorumState::HEALTHY) ? 1 : 0;
    }

    // Check for warning threshold
    if (_config.quorum_warning_threshold > 0 && new_state == QuorumState::HEALTHY) {
        size_t margin = recently_seen_nodes - required;
        bool should_warn = margin <= _config.quorum_warning_threshold;

        if (should_warn && !_quorum_warning_active) {
            _quorum_warning_active = true;
            log_gossip.warn("QUORUM WARNING: Only {} node(s) above quorum threshold "
                           "(recently_seen={}, required={}, total={}). Cluster at risk of split-brain.",
                           margin, recently_seen_nodes, required, total_nodes);
        } else if (!should_warn && _quorum_warning_active) {
            _quorum_warning_active = false;
            log_gossip.info("QUORUM WARNING CLEARED: Cluster has sufficient margin "
                           "(recently_seen={}, required={}, total={}).",
                           recently_seen_nodes, required, total_nodes);
        }
    }
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

        // Track newly discovered peers for DTLS handshake initiation
        std::vector<seastar::socket_address> new_peers_for_handshake;

        for (const auto& addr : new_peer_addresses) {
            auto it = _peer_table.find(addr);
            if (it != _peer_table.end()) {
                // Preserve existing state (last_seen, is_alive, associated_backend)
                new_peer_table[addr] = it->second;
            } else {
                // New peer - initialize with current time
                new_peer_table[addr] = { now, true, std::nullopt };
                log_gossip.info("DNS discovery: new peer added: {}", addr);
                // Mark for DTLS handshake if DTLS is enabled
                if (_dtls_context && _dtls_context->is_enabled()) {
                    new_peers_for_handshake.push_back(addr);
                }
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

        // Auto-trigger DTLS handshakes for newly discovered peers BEFORE any routing data exchange
        // This ensures encrypted channels are established first when mTLS is enabled
        if (!new_peers_for_handshake.empty()) {
            log_gossip.info("Initiating DTLS handshakes with {} newly discovered peers", new_peers_for_handshake.size());
            for (const auto& peer : new_peers_for_handshake) {
                (void)initiate_peer_handshake(peer);
            }
        }

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

// Reliable delivery: check if a sequence number is a duplicate (SECURITY-CRITICAL)
//
// This function implements a sliding window for sequence number tracking that
// provides protection against replay attacks. The window MUST persist across
// resync events - clearing it would allow an attacker to replay old messages.
//
// Security properties:
// 1. Duplicate detection: Prevents processing the same message twice
// 2. Replay attack prevention: Old captured packets cannot be replayed
// 3. Window size limits memory usage while maintaining security
//
// The window size (gossip_dedup_window) should be large enough to cover
// the maximum expected in-flight messages during normal operation and
// any network partitions that might delay delivery.
bool GossipService::is_duplicate(const seastar::socket_address& peer, uint32_t seq_num) {
    // Check if we already track this peer
    auto set_it = _received_seq_sets.find(peer);
    auto window_it = _received_seq_windows.find(peer);

    if (set_it != _received_seq_sets.end()) {
        // Existing peer - check for duplicate
        auto& seq_set = set_it->second;
        auto& seq_window = window_it->second;

        if (seq_set.count(seq_num) > 0) {
            return true;
        }

        // Add to window
        seq_set.insert(seq_num);
        seq_window.push_back(seq_num);

        // Slide window if too large - evict oldest sequence numbers
        while (seq_window.size() > _config.gossip_dedup_window) {
            uint32_t oldest = seq_window.front();
            seq_window.pop_front();
            seq_set.erase(oldest);
        }

        return false;
    }

    // New peer - check bounds before adding (Rule #4: bounded containers)
    if (_received_seq_sets.size() >= MAX_DEDUP_PEERS) {
        ++_dedup_peers_overflow;
        // Cannot track new peer - treat as non-duplicate to allow processing
        // This is a security/availability tradeoff: we prioritize availability
        // over duplicate suppression when under peer flooding attack
        return false;
    }

    // Create new entry for this peer
    auto& seq_set = _received_seq_sets[peer];
    auto& seq_window = _received_seq_windows[peer];

    seq_set.insert(seq_num);
    seq_window.push_back(seq_num);

    return false;
}

// Reliable delivery: process pending retries
void GossipService::process_retries() {
    // RAII Timer Safety: Acquire gate holder to prevent execution during shutdown.
    // If the gate is closed (stop() in progress), exit safely without accessing state.
    try {
        [[maybe_unused]] auto timer_holder = _timer_gate.hold();
    } catch (const seastar::gate_closed_exception&) {
        return;
    }

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

    // Set up certificate reload timer if enabled with RAII timer safety
    if (_config.tls.cert_reload_interval.count() > 0) {
        log_gossip.info("Certificate hot-reload enabled: interval={}s",
                        _config.tls.cert_reload_interval.count());
        _cert_reload_timer.set_callback([this] {
            // RAII Timer Safety: Acquire gate holder to prevent execution during shutdown.
            try {
                [[maybe_unused]] auto timer_holder = _timer_gate.hold();
            } catch (const seastar::gate_closed_exception&) {
                return;
            }

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
        (void)initiate_peer_handshake(peer);
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

    // Use adaptive crypto offloading for encryption
    // The offloader decides whether to run inline or on thread pool based on
    // data size, operation type, and predicted latency
    auto peer_copy = peer;
    return encrypt_with_offloading(session, plaintext, peer).then([this, peer_copy](std::vector<uint8_t> encrypted) {
        if (encrypted.empty()) {
            return seastar::make_ready_future<>();
        }

        // Send encrypted data - check channel is still valid
        if (!_channel) {
            return seastar::make_ready_future<>();
        }

        seastar::temporary_buffer<char> buf(encrypted.size());
        std::memcpy(buf.get_write(), encrypted.data(), encrypted.size());
        seastar::net::packet packet(std::move(buf));
        return _channel->send(peer_copy, std::move(packet)).handle_exception([peer_copy](auto ep) {
            log_gossip.debug("Failed to send encrypted data to {}: {}", peer_copy, ep);
        });
    });
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
        if (!response.empty()) {
            send_packet_async(peer, response);
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
            if (!response.empty()) {
                send_packet_async(peer, response);
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
            // Check context is still valid (could be invalidated during shutdown)
            if (!_dtls_context || !_dtls_context->is_enabled()) {
                return;
            }

            // Pre-encrypt for all peers in the thread context with batch timing
            std::vector<std::pair<seastar::socket_address, std::vector<uint8_t>>> encrypted_packets;
            encrypted_packets.reserve(peers_copy->size());

            auto batch_start = std::chrono::steady_clock::now();

            for (const auto& peer : *peers_copy) {
                // Re-check context validity in loop (could be invalidated mid-loop)
                if (!_dtls_context) {
                    break;
                }

                auto* session = _dtls_context->get_or_create_session(peer, false);
                if (!session || !session->is_established()) {
                    continue;
                }

                // Direct encryption without per-op timing (batch timing covers this)
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
                // Check channel is still valid before each send
                if (!_channel) {
                    break;
                }

                send_packet_async(peer, encrypted);

                // Yield between sends to avoid monopolizing the reactor
                seastar::thread::yield();
            }

            log_gossip.trace("Batch broadcast completed: {} packets encrypted and sent", encrypted_packets.size());
        });
    }

    // For small peer counts, use parallel_for_each (existing behavior)
    // Copy plaintext to avoid dangling reference (parallel_for_each is async)
    auto plaintext_copy = std::make_shared<std::vector<uint8_t>>(plaintext);
    return seastar::parallel_for_each(peers, [this, plaintext_copy](const seastar::socket_address& peer) {
        return send_encrypted(peer, *plaintext_copy);
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
    // The gate allows graceful shutdown - stop() waits for gate.close() before resetting _dtls_context
    co_await seastar::parallel_for_each(_peer_addresses, [this](const seastar::socket_address& peer) -> seastar::future<> {
        // Use the gate to coordinate with shutdown
        try {
            [[maybe_unused]] auto holder = _handshake_gate.hold();
            co_await initiate_peer_handshake(peer);
        } catch (const seastar::gate_closed_exception&) {
            // Service is shutting down, skip this handshake
            log_gossip.debug("Handshake skipped during shutdown for peer {}", peer);
        }
    });

    log_gossip.info("Certificate reload handshakes initiated for all peers");
}

void GossipService::cleanup_dtls_sessions() {
    // RAII Timer Safety: Acquire gate holder to prevent execution during shutdown.
    // If the gate is closed (stop() in progress), exit safely without accessing state.
    try {
        [[maybe_unused]] auto timer_holder = _timer_gate.hold();
    } catch (const seastar::gate_closed_exception&) {
        return;
    }

    if (!_dtls_context) {
        return;
    }

    // Clean up sessions idle for more than 5 minutes
    _dtls_context->cleanup_idle_sessions(std::chrono::seconds(300));
}

//------------------------------------------------------------------------------
// Low-level Helper Methods
//------------------------------------------------------------------------------

void GossipService::send_packet_async(const seastar::socket_address& peer,
                                       const std::vector<uint8_t>& data) {
    if (!_channel || data.empty()) {
        return;
    }

    // Create an owned buffer to avoid use-after-free with async send
    // (temporary_buffer(ptr, len) creates a non-owning view)
    seastar::temporary_buffer<char> buf(data.size());
    std::memcpy(buf.get_write(), data.data(), data.size());
    seastar::net::packet packet(std::move(buf));

    (void)_channel->send(peer, std::move(packet)).handle_exception([peer](auto ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            log_gossip.debug("Failed to send packet to {}: {}", peer, e.what());
        }
    });
}

std::vector<uint8_t> GossipService::encrypt_with_timing(DtlsSession* session,
                                                         const uint8_t* data, size_t len,
                                                         const seastar::socket_address& peer) {
    std::vector<uint8_t> encrypted;

    if (!session || !session->is_established()) {
        return encrypted;  // Return empty vector on failure
    }

    auto start = std::chrono::steady_clock::now();
    auto result = session->encrypt(data, len, encrypted);
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();

    if (static_cast<uint64_t>(elapsed_us) > CRYPTO_STALL_WARNING_US) {
        ++_crypto_stall_warnings;
        log_gossip.warn("Crypto stall: encrypt took {}μs for {} bytes to peer {}",
                       elapsed_us, len, peer);
    }

    if (result != DtlsResult::SUCCESS) {
        log_gossip.debug("Encryption failed for peer {}: {}", peer, session->last_error());
        return {};  // Return empty vector on failure
    }

    ++_dtls_packets_encrypted;
    return encrypted;
}

seastar::future<> GossipService::initiate_peer_handshake(const seastar::socket_address& peer) {
    if (!_dtls_context || !_channel) {
        return seastar::make_ready_future<>();
    }

    auto* session = _dtls_context->get_or_create_session(peer, false);  // Client role
    if (!session) {
        return seastar::make_ready_future<>();
    }

    std::vector<uint8_t> handshake_data;
    auto result = session->initiate_handshake(handshake_data);

    if (result == DtlsResult::WANT_READ && !handshake_data.empty()) {
        ++_dtls_handshakes_started;
        send_packet_async(peer, handshake_data);
    }

    return seastar::make_ready_future<>();
}

//------------------------------------------------------------------------------
// Resync Methods for Graceful Recovery
//------------------------------------------------------------------------------

void GossipService::start_resync() {
    if (!_config.enabled) {
        return;
    }

    log_gossip.info("Starting gossip re-sync mode - rejecting new gossip tasks");
    _resyncing.store(true, std::memory_order_relaxed);

    // Clear pending ACKs to avoid stale retries during re-sync
    _pending_acks.clear();

    // SECURITY: DO NOT clear _received_seq_windows and _received_seq_sets!
    // These sliding windows MUST persist across resync events to prevent
    // "Replay Attacks" where an attacker re-sends old routing messages
    // to overwrite newer routes. If we cleared these windows, an attacker
    // could capture old sequence numbers and replay them after a resync
    // to inject stale/malicious routes into the routing table.
    //
    // The sequence numbers in the sliding window prevent this by ensuring
    // that even after a resync, previously-seen sequence numbers are still
    // rejected as duplicates.

    // Note: We don't close the gate here - we just set the flag
    // This allows in-flight tasks to complete while new ones are rejected
}

void GossipService::end_resync() {
    if (!_config.enabled) {
        return;
    }

    log_gossip.info("Ending gossip re-sync mode - resuming normal gossip operations");
    _resyncing.store(false, std::memory_order_relaxed);
}

//------------------------------------------------------------------------------
// DTLS Lockdown Helper Methods
//------------------------------------------------------------------------------

bool GossipService::is_dtls_handshake_packet(const uint8_t* data, size_t len) const {
    // DTLS record layer header format (DTLS 1.0/1.2, RFC 6347):
    //   - ContentType (1 byte)
    //   - Version (2 bytes): Major.Minor (0xFE 0xFF=DTLS 1.0, 0xFE 0xFD=DTLS 1.2)
    //   - Epoch (2 bytes)
    //   - Sequence number (6 bytes)
    //   - Length (2 bytes)
    //
    // We allow Alert packets because they may be sent during handshake
    // failures and must not be silently dropped.

    if (len < DTLS_RECORD_HEADER_SIZE) {
        return false;
    }

    uint8_t content_type = data[0];

    // Check for handshake-related content types
    bool is_handshake_type = (content_type == DTLS_CONTENT_CHANGE_CIPHER_SPEC ||
                              content_type == DTLS_CONTENT_ALERT ||
                              content_type == DTLS_CONTENT_HANDSHAKE);
    if (!is_handshake_type) {
        return false;
    }

    // Verify DTLS version marker (distinguishes DTLS from TLS)
    if (data[1] != DTLS_VERSION_MARKER) {
        return false;
    }

    return true;
}

bool GossipService::should_drop_packet_mtls_lockdown(const seastar::socket_address& peer,
                                                       const uint8_t* data, size_t len) {
    // If mTLS lockdown is not enabled, don't drop anything
    if (!_config.mtls_enabled) {
        return false;
    }

    // DTLS must be configured and enabled for mTLS to work
    if (!_dtls_context || !_dtls_context->is_enabled()) {
        // DTLS not initialized - this shouldn't happen with mtls_enabled
        // but if it does, we must drop all packets for security
        log_gossip.warn("mTLS lockdown active but DTLS not initialized - dropping packet from {}", peer);
        ++_dtls_lockdown_drops;
        return true;
    }

    // Allow DTLS handshake packets through (needed to establish sessions)
    if (is_dtls_handshake_packet(data, len)) {
        return false;
    }

    // Check if we have an established DTLS session for this peer
    auto* session = _dtls_context->get_or_create_session(peer, true);
    if (session && session->is_established()) {
        // Active session exists - packet is allowed (will be decrypted)
        return false;
    }

    // No established session and not a handshake packet - drop it
    log_gossip.debug("mTLS lockdown: dropping non-DTLS packet from {} (no established session)", peer);
    ++_dtls_lockdown_drops;
    return true;
}

//------------------------------------------------------------------------------
// Adaptive Crypto Offloading Implementation
//------------------------------------------------------------------------------

void GossipService::initialize_crypto_offloader() {
    if (!_config.tls.enabled) {
        return;
    }

    CryptoOffloaderConfig offloader_config;

    // Size threshold: offload operations on data larger than 1KB
    offloader_config.size_threshold_bytes = CRYPTO_OFFLOAD_BYTES_THRESHOLD;

    // Stall threshold: flag operations that exceed 500μs (matches Seastar's task quota)
    offloader_config.stall_threshold_us = 500;

    // Offload threshold: offload if predicted latency > 100μs
    offloader_config.offload_latency_threshold_us = CRYPTO_STALL_WARNING_US;

    // Queue depth: allow up to 1024 pending operations
    offloader_config.max_queue_depth = 1024;

    // Enable adaptive offloading
    offloader_config.enabled = true;

    // Always run symmetric operations inline for small packets
    offloader_config.symmetric_always_inline = true;

    // Always offload handshakes (they involve RSA/ECDH which is slow)
    offloader_config.handshake_always_offload = true;

    _crypto_offloader = std::make_unique<CryptoOffloader>(offloader_config);
    _crypto_offloader->start();

    // Register offloader metrics
    _crypto_offloader->register_metrics(_metrics);

    log_gossip.info("Crypto offloader initialized (seastar::async mode)");
    log_gossip.info("Adaptive offloading thresholds: size={}B, stall={}μs, offload={}μs, max_queue={}",
                    offloader_config.size_threshold_bytes,
                    offloader_config.stall_threshold_us,
                    offloader_config.offload_latency_threshold_us,
                    offloader_config.max_queue_depth);
}

seastar::future<std::vector<uint8_t>> GossipService::encrypt_with_offloading(
    DtlsSession* session,
    const std::vector<uint8_t>& plaintext,
    const seastar::socket_address& peer) {

    if (!session || !session->is_established()) {
        return seastar::make_ready_future<std::vector<uint8_t>>(std::vector<uint8_t>{});
    }

    // If crypto offloader is not available, fall back to inline encryption
    if (!_crypto_offloader || !_crypto_offloader->is_running()) {
        auto encrypted = encrypt_with_timing(session, plaintext.data(), plaintext.size(), peer);
        return seastar::make_ready_future<std::vector<uint8_t>>(std::move(encrypted));
    }

    // Determine operation type (symmetric since session is established)
    CryptoOpType op_type = CryptoOpType::SYMMETRIC_ENCRYPT;

    // Check if we should offload (based on size and type)
    if (!_crypto_offloader->should_offload(op_type, plaintext.size())) {
        // Run inline with timing
        auto encrypted = encrypt_with_timing(session, plaintext.data(), plaintext.size(), peer);
        return seastar::make_ready_future<std::vector<uint8_t>>(std::move(encrypted));
    }

    // Offload to thread pool
    ++_crypto_ops_offloaded;

    // Copy plaintext for thread safety
    auto plaintext_copy = std::make_shared<std::vector<uint8_t>>(plaintext);
    auto peer_copy = peer;

    // We can't pass session pointer directly to the thread pool since OpenSSL
    // is not thread-safe for the same SSL object. Instead, we need to re-lookup
    // the session inside the async context, similar to existing code.
    return _crypto_offloader->wrap_crypto_op(
        op_type,
        plaintext.size(),
        [this, peer_copy, plaintext_copy]() -> std::vector<uint8_t> {
            // Re-lookup session inside async block to avoid dangling pointer
            if (!_dtls_context || !_dtls_context->is_enabled()) {
                return {};
            }

            auto* session = _dtls_context->get_or_create_session(peer_copy, false);
            if (!session || !session->is_established()) {
                return {};
            }

            std::vector<uint8_t> encrypted;
            auto result = session->encrypt(plaintext_copy->data(), plaintext_copy->size(), encrypted);

            if (result != DtlsResult::SUCCESS) {
                log_gossip.debug("Offloaded encryption failed for peer {}", peer_copy);
                return {};
            }

            return encrypted;
        }
    ).then([this](std::vector<uint8_t> encrypted) {
        if (!encrypted.empty()) {
            ++_dtls_packets_encrypted;
        }
        return seastar::make_ready_future<std::vector<uint8_t>>(std::move(encrypted));
    });
}

seastar::future<DtlsResult> GossipService::handshake_with_offloading(
    DtlsSession* session,
    const uint8_t* data,
    size_t len,
    std::vector<uint8_t>& response) {

    if (!session) {
        return seastar::make_ready_future<DtlsResult>(DtlsResult::ERROR);
    }

    // If crypto offloader is not available, run handshake inline
    if (!_crypto_offloader || !_crypto_offloader->is_running()) {
        DtlsResult result;
        if (data && len > 0) {
            result = session->continue_handshake(data, len, response);
        } else {
            result = session->initiate_handshake(response);
        }
        return seastar::make_ready_future<DtlsResult>(result);
    }

    // Handshakes are always offloaded (RSA/ECDH operations are expensive)
    ++_crypto_ops_offloaded;
    ++_crypto_handshakes_offloaded;

    // Determine handshake type
    CryptoOpType op_type = (data && len > 0) ?
        CryptoOpType::HANDSHAKE_CONTINUE : CryptoOpType::HANDSHAKE_INITIATE;

    // Copy data for thread safety
    auto data_copy = (data && len > 0) ?
        std::make_shared<std::vector<uint8_t>>(data, data + len) :
        std::make_shared<std::vector<uint8_t>>();

    // Use shared_ptr for response to allow modification in thread
    auto response_holder = seastar::make_lw_shared<std::vector<uint8_t>>();

    // Note: We can't safely pass session pointer to thread pool since SSL is not thread-safe.
    // For handshakes, we need to run them on the reactor thread and just measure timing.
    // The actual offloading benefit comes from batching and not blocking on I/O.
    //
    // IMPORTANT: OpenSSL SSL objects are NOT thread-safe. Handshakes must run on the
    // same thread that owns the SSL object. We use wrap_crypto_op here mainly for
    // timing and statistics, but the actual work runs on reactor.
    //
    // For true thread-pool offloading of handshakes, we would need to redesign the
    // DTLS session management to be thread-safe or use a different approach.
    //
    // For now, we run handshakes inline but track them for stall detection.
    auto start = std::chrono::steady_clock::now();

    DtlsResult result;
    if (data && len > 0) {
        result = session->continue_handshake(data, len, response);
    } else {
        result = session->initiate_handshake(response);
    }

    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();

    // Track stalls from handshakes
    if (static_cast<uint64_t>(elapsed_us) > CRYPTO_STALL_WARNING_US) {
        ++_crypto_stall_warnings;
        log_gossip.warn("Handshake exceeded stall threshold: {}μs (type: {})",
                       elapsed_us, op_type == CryptoOpType::HANDSHAKE_INITIATE ? "initiate" : "continue");
    }

    return seastar::make_ready_future<DtlsResult>(result);
}

void GossipService::sync_crypto_offloader_stats() {
    if (!_crypto_offloader) {
        return;
    }

    auto stats = _crypto_offloader->get_stats();

    // Sync relevant stats to local metrics
    // These add to existing metrics rather than replace
    _crypto_stalls_avoided += stats.stalls_avoided;
    _crypto_stall_warnings += stats.stall_warnings;
}

GossipService::ClusterState GossipService::get_cluster_state() const {
    ClusterState state;

    state.quorum_state = (_quorum_state == QuorumState::HEALTHY) ? "HEALTHY" : "DEGRADED";
    state.quorum_required = quorum_required();
    state.peers_alive = _stats_cluster_peers_alive;
    state.total_peers = _peer_table.size();
    state.peers_recently_seen = _stats_peers_recently_seen;
    state.is_draining = _draining.load(std::memory_order_relaxed);
    state.local_backend_id = _local_backend_id;

    // Collect peer information
    for (const auto& [addr, peer_state] : _peer_table) {
        PeerInfo info;

        // Extract address and port from socket_address
        std::ostringstream oss;
        oss << addr;
        std::string addr_str = oss.str();

        // Parse address:port format
        // Handle both IPv4 (192.168.1.1:8080) and IPv6 ([::1]:8080) formats
        if (!addr_str.empty() && addr_str.back() >= '0' && addr_str.back() <= '9') {
            auto colon_pos = addr_str.find_last_of(':');
            if (colon_pos != std::string::npos && colon_pos > 0) {
                // Verify this is the port separator, not part of IPv6 address
                bool is_port_separator = (addr_str[colon_pos - 1] == ']') ||
                                         (addr_str.find('[') == std::string::npos);
                if (is_port_separator) {
                    info.address = addr_str.substr(0, colon_pos);
                    try {
                        info.port = static_cast<uint16_t>(std::stoi(addr_str.substr(colon_pos + 1)));
                    } catch (const std::exception&) {
                        info.port = 0;
                    }
                } else {
                    info.address = addr_str;
                    info.port = 0;
                }
            } else {
                info.address = addr_str;
                info.port = 0;
            }
        } else {
            info.address = addr_str;
            info.port = 0;
        }

        info.is_alive = peer_state.is_alive;
        info.last_seen_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            peer_state.last_seen.time_since_epoch()).count();
        info.associated_backend = peer_state.associated_backend;

        state.peers.push_back(std::move(info));
    }

    return state;
}

}  // namespace ranvier
