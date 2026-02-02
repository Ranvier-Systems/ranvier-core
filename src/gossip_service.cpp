// Ranvier Core - Gossip Service Implementation (Refactored Orchestrator)
//
// Thin orchestrator (~350 LOC) that coordinates three extracted modules:
// - GossipConsensus: Peer table, quorum state, split-brain detection
// - GossipTransport: UDP channel, DTLS encryption, crypto offloading
// - GossipProtocol: Message handling, reliable delivery, DNS discovery

#include "gossip_service.hpp"

#include <seastar/core/coroutine.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/smp.hh>
#include <seastar/net/inet_address.hh>

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

    // Create module instances
    _consensus = std::make_unique<GossipConsensus>(_config);
    _transport = std::make_unique<GossipTransport>(_config);
    _protocol = std::make_unique<GossipProtocol>(_config);

    // Register metrics
    register_metrics();
}

void GossipService::register_metrics() {
    // Register operational metrics (always enabled)
    // These ~23 metrics are essential for production monitoring
    _metrics.add_group("ranvier", {
        // Protocol metrics (packet counts)
        seastar::metrics::make_counter("router_cluster_sync_sent",
            [this] { return _protocol->packets_sent(); },
            seastar::metrics::description("Total number of gossip packets sent to cluster peers")),
        seastar::metrics::make_counter("router_cluster_sync_received",
            [this] { return _protocol->packets_received(); },
            seastar::metrics::description("Total number of gossip packets received from cluster peers")),
        seastar::metrics::make_counter("router_cluster_sync_invalid",
            [this] { return _protocol->packets_invalid(); },
            seastar::metrics::description("Total number of invalid gossip packets received")),
        seastar::metrics::make_counter("router_cluster_sync_untrusted",
            [this] { return _protocol->packets_untrusted(); },
            seastar::metrics::description("Total number of gossip packets received from unknown peers")),

        // Consensus metrics (peer/quorum)
        seastar::metrics::make_gauge("cluster_peers_alive",
            [this] { return _consensus->peers_alive_count(); },
            seastar::metrics::description("Number of cluster peers currently alive")),
        seastar::metrics::make_gauge("cluster_quorum_state",
            [this] { return _consensus->stats_quorum_state(); },
            seastar::metrics::description("Cluster quorum state: 1=healthy, 0=degraded")),
        seastar::metrics::make_counter("cluster_quorum_transitions",
            [this] { return _consensus->quorum_transitions(); },
            seastar::metrics::description("Total number of quorum state transitions")),
        seastar::metrics::make_counter("cluster_routes_rejected_degraded",
            [this] { return _consensus->routes_rejected_degraded(); },
            seastar::metrics::description("Total number of route broadcasts rejected due to degraded quorum")),
        seastar::metrics::make_counter("cluster_routes_rejected_incoming_degraded",
            [this] { return _consensus->routes_rejected_incoming_degraded(); },
            seastar::metrics::description("Total number of incoming routes rejected due to degraded quorum")),
        seastar::metrics::make_gauge("cluster_peers_recently_seen",
            [this] { return _consensus->peers_recently_seen_count(); },
            seastar::metrics::description("Number of peers seen within quorum check window")),
        seastar::metrics::make_counter("cluster_routes_allowed_fail_open",
            [this] { return _consensus->routes_allowed_fail_open(); },
            seastar::metrics::description("Total number of routes allowed due to fail-open mode")),
        seastar::metrics::make_counter("cluster_gossip_accepted_fail_open",
            [this] { return _consensus->gossip_accepted_fail_open(); },
            seastar::metrics::description("Total number of incoming gossip accepted due to fail-open mode")),

        // Protocol metrics (reliable delivery)
        seastar::metrics::make_counter("cluster_dns_discovery_success",
            [this] { return _protocol->dns_discovery_success(); },
            seastar::metrics::description("Total number of successful DNS peer discovery operations")),
        seastar::metrics::make_counter("cluster_dns_discovery_failure",
            [this] { return _protocol->dns_discovery_failure(); },
            seastar::metrics::description("Total number of failed DNS peer discovery operations")),
        seastar::metrics::make_counter("cluster_acks_sent",
            [this] { return _protocol->acks_sent(); },
            seastar::metrics::description("Total number of route ACKs sent")),
        seastar::metrics::make_counter("cluster_acks_received",
            [this] { return _protocol->acks_received(); },
            seastar::metrics::description("Total number of route ACKs received")),
        seastar::metrics::make_counter("cluster_retries_sent",
            [this] { return _protocol->retries_sent(); },
            seastar::metrics::description("Total number of route announcement retries sent")),
        seastar::metrics::make_counter("cluster_duplicates_suppressed",
            [this] { return _protocol->duplicates_suppressed(); },
            seastar::metrics::description("Total number of duplicate route announcements suppressed")),
        seastar::metrics::make_counter("cluster_max_retries_exceeded",
            [this] { return _protocol->max_retries_exceeded(); },
            seastar::metrics::description("Total number of route announcements that exceeded max retries")),
        seastar::metrics::make_gauge("cluster_pending_acks_count",
            [this] { return _protocol->pending_acks_count(); },
            seastar::metrics::description("Current number of pending ACKs awaiting response")),
        seastar::metrics::make_counter("cluster_node_state_sent",
            [this] { return _protocol->node_state_sent(); },
            seastar::metrics::description("Total number of node state notifications sent")),
        seastar::metrics::make_counter("cluster_node_state_received",
            [this] { return _protocol->node_state_received(); },
            seastar::metrics::description("Total number of node state notifications received")),

        // Transport metrics (DTLS)
        seastar::metrics::make_counter("cluster_dtls_handshakes_started",
            [this] { return _transport->dtls_handshakes_started(); },
            seastar::metrics::description("Total number of DTLS handshakes initiated")),
        seastar::metrics::make_counter("cluster_dtls_handshakes_completed",
            [this] { return _transport->dtls_handshakes_completed(); },
            seastar::metrics::description("Total number of DTLS handshakes completed")),
        seastar::metrics::make_counter("cluster_dtls_handshakes_failed",
            [this] { return _transport->dtls_handshakes_failed(); },
            seastar::metrics::description("Total number of DTLS handshakes failed")),
        seastar::metrics::make_counter("cluster_dtls_packets_encrypted",
            [this] { return _transport->dtls_packets_encrypted(); },
            seastar::metrics::description("Total number of packets encrypted with DTLS")),
        seastar::metrics::make_counter("cluster_dtls_packets_decrypted",
            [this] { return _transport->dtls_packets_decrypted(); },
            seastar::metrics::description("Total number of packets decrypted with DTLS")),
        seastar::metrics::make_counter("cluster_dtls_lockdown_drops",
            [this] { return _transport->dtls_lockdown_drops(); },
            seastar::metrics::description("Total packets dropped due to mTLS lockdown"))
    });

#ifdef RANVIER_DEBUG_METRICS
    // Debug metrics (optional, enabled via -DRANVIER_DEBUG_METRICS=ON)
    // These 8 metrics add scrape overhead and are primarily useful for development.
    // See BACKLOG.md Section 8.5 for rationale.
    _metrics.add_group("ranvier", {
        // Crypto offload debugging
        seastar::metrics::make_counter("cluster_crypto_stall_warnings",
            [this] { return _transport->crypto_stall_warnings(); },
            seastar::metrics::description("Total crypto operations exceeding stall threshold")),
        seastar::metrics::make_counter("cluster_crypto_ops_offloaded",
            [this] { return _transport->crypto_ops_offloaded(); },
            seastar::metrics::description("Total crypto operations offloaded to thread pool")),
        seastar::metrics::make_counter("cluster_crypto_batch_broadcasts",
            [this] { return _transport->crypto_batch_broadcasts(); },
            seastar::metrics::description("Total number of batched broadcast operations")),
        seastar::metrics::make_counter("cluster_crypto_stalls_avoided",
            [this] { return _transport->crypto_stalls_avoided(); },
            seastar::metrics::description("Total reactor stalls avoided via crypto offloading")),
        seastar::metrics::make_counter("cluster_crypto_handshakes_offloaded",
            [this] { return _transport->crypto_handshakes_offloaded(); },
            seastar::metrics::description("Total DTLS handshakes offloaded to thread pool")),
        seastar::metrics::make_counter("cluster_dtls_cert_reloads",
            [this] { return _transport->dtls_cert_reloads(); },
            seastar::metrics::description("Total number of certificate hot-reloads")),

        // Overflow counters (Rule #4 debugging)
        seastar::metrics::make_counter("cluster_dedup_peers_overflow",
            [this] { return _protocol->dedup_peers_overflow(); },
            seastar::metrics::description("Times dedup peer limit was reached (Rule #4)")),
        seastar::metrics::make_counter("cluster_pending_acks_overflow",
            [this] { return _protocol->pending_acks_overflow(); },
            seastar::metrics::description("Times pending acks limit was reached (Rule #4)"))
    });
#endif
}

seastar::future<> GossipService::start() {
    if (!_config.enabled) {
        log_gossip.info("Gossip service disabled");
        co_return;
    }

    if (_peer_addresses.empty()) {
        log_gossip.warn("Gossip enabled but no valid peers configured");
    }

    _running = true;

    // Only Shard 0 manages the modules
    if (seastar::this_shard_id() != 0) {
        log_gossip.debug("Gossip service started on worker shard {}", seastar::this_shard_id());
        co_return;
    }

    log_gossip.info("Starting gossip service on port {}", _config.gossip_port);
    log_gossip.info("Configured peers: {}", _peer_addresses.size());

    try {
        // Start transport layer (UDP/DTLS)
        co_await _transport->start(_config.gossip_port);

        // Start consensus layer (peer table, quorum)
        co_await _consensus->start(_peer_addresses);

        // Start protocol layer (message handling, reliable delivery)
        co_await _protocol->start(*_transport, *_consensus, _peer_addresses);

        // Initiate DTLS handshakes with all configured peers
        if (_transport->is_dtls_enabled()) {
            log_gossip.info("Initiating DTLS handshakes with {} peers", _peer_addresses.size());
            for (const auto& peer : _peer_addresses) {
                (void)_transport->initiate_handshake(peer);
            }
        }

        // Start receive loop in background
        _receive_loop_future = receive_loop().handle_exception([](auto ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                log_gossip.error("Gossip receive loop error: {}", e.what());
            }
        });

        log_gossip.info("Gossip service started successfully");
    } catch (...) {
        throw;
    }
}

seastar::future<> GossipService::stop() {
    if (!_running) {
        co_return;
    }

    log_gossip.info("Stopping gossip service");
    _running = false;

    // Rule #6: Deregister metrics FIRST
    _metrics.clear();

    // Only shard 0 has modules to stop
    if (seastar::this_shard_id() == 0) {
        // Stop protocol layer first (timers, DNS)
        co_await _protocol->stop();
        log_gossip.debug("Protocol module stopped");

        // Shutdown transport channel (wakes up receive loop)
        auto& channel = _transport->channel();
        if (channel) {
            channel->shutdown_input();
            channel->shutdown_output();
        }

        // Wait for receive loop to exit
        try {
            co_await std::move(_receive_loop_future);
        } catch (...) {
            log_gossip.debug("Gossip loop exited");
        }

        // Stop transport layer
        co_await _transport->stop();
        log_gossip.debug("Transport module stopped");

        // Stop consensus layer
        co_await _consensus->stop();
        log_gossip.debug("Consensus module stopped");
    }

    // Close gossip task gate
    co_await _gossip_task_gate.close();
    log_gossip.debug("Gossip task gate closed");

    co_return;
}

seastar::future<> GossipService::receive_loop() {
    auto& channel = _transport->channel();

    try {
        while (_running && channel) {
            auto dgram = co_await channel->receive();
            co_await _protocol->handle_packet(std::move(dgram));
        }
    } catch (const std::exception& e) {
        if (_running) {
            log_gossip.error("Receive loop fatal error: {}", e.what());
        }
    }
    co_return;
}

void GossipService::set_route_learn_callback(RouteLearnCallback callback) {
    if (_protocol) {
        _protocol->set_route_learn_callback(std::move(callback));
    }
}

void GossipService::set_node_state_callback(NodeStateCallback callback) {
    if (_protocol) {
        _protocol->set_node_state_callback(std::move(callback));
    }
}

void GossipService::set_local_backend_id(BackendId id) {
    if (_consensus) {
        _consensus->set_local_backend_id(id);
    }
}

BackendId GossipService::local_backend_id() const {
    return _consensus ? _consensus->local_backend_id() : 0;
}

seastar::future<> GossipService::broadcast_node_state(NodeState state) {
    if (!_config.enabled || !_protocol || _peer_addresses.empty()) {
        return seastar::make_ready_future<>();
    }

    // Mark draining state in consensus
    if (state == NodeState::DRAINING && _consensus) {
        _consensus->set_draining(true);
    }

    return _protocol->broadcast_node_state(state, local_backend_id());
}

bool GossipService::is_draining() const {
    return _consensus ? _consensus->is_draining() : false;
}

seastar::future<> GossipService::broadcast_route(const std::vector<TokenId>& tokens, BackendId backend) {
    if (!_config.enabled || !_protocol || _peer_addresses.empty()) {
        return seastar::make_ready_future<>();
    }

    // Gossip protection: reject during shutdown or re-sync
    if (!is_accepting_tasks()) {
        log_gossip.debug("Route broadcast rejected: gossip service not accepting tasks");
        return seastar::make_ready_future<>();
    }

    // Try to enter the gossip task gate
    seastar::gate::holder gate_holder;
    try {
        gate_holder = _gossip_task_gate.hold();
    } catch (const seastar::gate_closed_exception&) {
        log_gossip.debug("Route broadcast rejected: gossip gate closed");
        return seastar::make_ready_future<>();
    }

    // Capture gate holder to keep it alive for async operation
    return _protocol->broadcast_route(tokens, backend).finally([gate_holder = std::move(gate_holder)] {});
}

QuorumState GossipService::quorum_state() const {
    return _consensus ? _consensus->quorum_state() : QuorumState::HEALTHY;
}

bool GossipService::has_quorum() const {
    return _consensus ? _consensus->has_quorum() : true;
}

bool GossipService::is_degraded() const {
    return _consensus ? _consensus->is_degraded() : false;
}

bool GossipService::is_fail_open_mode() const {
    return _consensus ? _consensus->is_fail_open_mode() : false;
}

size_t GossipService::quorum_required() const {
    return _consensus ? _consensus->quorum_required() : 1;
}

size_t GossipService::peers_alive_count() const {
    return _consensus ? _consensus->peers_alive_count() : 0;
}

size_t GossipService::total_peers_count() const {
    return _consensus ? _consensus->total_peers_count() : 0;
}

size_t GossipService::peers_recently_seen_count() const {
    return _consensus ? _consensus->peers_recently_seen_count() : 0;
}

ClusterState GossipService::get_cluster_state() const {
    if (_consensus) {
        return _consensus->get_cluster_state();
    }

    // Return empty state if consensus not initialized
    ClusterState state;
    state.quorum_state = "UNKNOWN";
    state.quorum_required = 0;
    state.peers_alive = 0;
    state.total_peers = 0;
    state.peers_recently_seen = 0;
    state.is_draining = false;
    state.local_backend_id = 0;
    return state;
}

void GossipService::set_route_prune_callback(RoutePruneCallback callback) {
    if (_consensus) {
        _consensus->set_prune_callback(std::move(callback));
    }
}

bool GossipService::is_accepting_tasks() const {
    return _running && _consensus && _consensus->is_accepting_tasks();
}

void GossipService::start_resync() {
    if (_consensus) {
        _consensus->start_resync();
    }
    if (_protocol) {
        _protocol->clear_pending_acks();
    }
}

void GossipService::end_resync() {
    if (_consensus) {
        _consensus->end_resync();
    }
}

std::optional<seastar::socket_address> GossipService::parse_peer_address(const std::string& peer) {
    return GossipProtocol::parse_peer_address(peer);
}

}  // namespace ranvier
