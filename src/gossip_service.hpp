// Ranvier Core - Gossip Service for Distributed State Synchronization
//
// Implements a simple gossip protocol for cluster state sync:
// - UDP-based route announcements between cluster nodes
// - Stateless packet format for route propagation
// - Thread-local service following Seastar's shared-nothing model
// - Optional DTLS encryption for secure cluster communication (mTLS)
//
// Architecture (Refactored State Machine):
// GossipService is a thin orchestrator (~350 LOC) that coordinates three modules:
// - GossipConsensus: Peer table, quorum state, split-brain detection (~400 LOC)
// - GossipTransport: UDP channel, DTLS encryption, crypto offloading (~550 LOC)
// - GossipProtocol: Message handling, reliable delivery, DNS discovery (~750 LOC)

#pragma once

#include "chassis_config_schema.hpp"
#include "gossip_consensus.hpp"
#include "gossip_protocol.hpp"
#include "gossip_transport.hpp"
#include "logging.hpp"
#include "types.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/metrics.hh>
#include <seastar/net/socket_defs.hh>

namespace ranvier {

// Gossip logger
inline seastar::logger log_gossip("ranvier.gossip");

// Re-export types from modules for backward compatibility with existing code
// These were previously defined in this file but are now in gossip_protocol.hpp
// and gossip_consensus.hpp

// GossipService: Thread-local UDP gossip for cluster state sync
// Runs on shard 0 only (broadcasts received routes to all shards via RouterService)
//
// This class is a thin orchestrator that coordinates three modules:
// - GossipConsensus: Manages peer table and quorum state
// - GossipTransport: Handles UDP/DTLS communication
// - GossipProtocol: Handles message parsing and reliable delivery
class GossipService {
public:
    explicit GossipService(const ClusterConfig& config);

    // Initialize the UDP channel (must be called after Seastar starts)
    seastar::future<> start();

    // Stop the gossip service
    seastar::future<> stop();

    // Set callback for handling received route announcements
    void set_route_learn_callback(RouteLearnCallback callback);

    // Set callback for handling node state changes (e.g., DRAINING notifications)
    void set_node_state_callback(NodeStateCallback callback);

    // Set the local backend ID that this node represents
    // Used when broadcasting node state changes (e.g., DRAINING on shutdown)
    void set_local_backend_id(BackendId id);

    // Get the local backend ID
    BackendId local_backend_id() const;

    // Broadcast a node state change to all peers
    // Call with NodeState::DRAINING on SIGTERM to notify peers to stop sending traffic
    seastar::future<> broadcast_node_state(NodeState state);

    // Check if this node is in draining state
    bool is_draining() const;

    // Broadcast a route announcement to all peers
    // Called by RouterService when a new route is learned locally
    seastar::future<> broadcast_route(const std::vector<TokenId>& tokens, BackendId backend);

    // Check if gossip is enabled
    bool is_enabled() const { return _config.enabled; }

    // Quorum state accessors for split-brain detection
    // NOTE: These accessors only return valid data on shard 0.
    // On other shards, they return initial/stale values.
    // Use submit_to(0, ...) if you need to query from another shard.
    QuorumState quorum_state() const;
    bool has_quorum() const;
    bool is_degraded() const;

    // Fail-open mode: when enabled and quorum is lost, requests should be
    // routed randomly to healthy backends instead of being rejected.
    // RouterService queries this to decide routing behavior during split-brain.
    bool is_fail_open_mode() const;

    // Get quorum status for external queries (e.g., health checks, metrics)
    // NOTE: Only valid on shard 0 where peer table is maintained.
    size_t quorum_required() const;
    size_t peers_alive_count() const;
    size_t total_peers_count() const;
    size_t peers_recently_seen_count() const;

    // ==========================================================================
    // Admin API - Cluster State Inspection
    // ==========================================================================

    // Re-export types from GossipConsensus for backward compatibility
    using PeerInfo = ranvier::PeerInfo;
    using ClusterState = ranvier::ClusterState;

    // Get current cluster state for admin inspection
    ClusterState get_cluster_state() const;

    // Callback type for pruning routes
    using RoutePruneCallback = std::function<seastar::future<>(BackendId)>;
    void set_route_prune_callback(RoutePruneCallback callback);

    // Gossip protection: check if service is accepting new tasks
    // Returns false during shutdown or re-sync
    bool is_accepting_tasks() const;

    // Start re-sync mode: rejects new gossip tasks while flushing existing ones
    // Call when recovering from a network partition or cluster split
    void start_resync();

    // End re-sync mode: resume normal gossip operations
    void end_resync();

private:
    ClusterConfig _config;
    bool _running = false;

    // Module instances (owned by orchestrator)
    std::unique_ptr<GossipConsensus> _consensus;
    std::unique_ptr<GossipTransport> _transport;
    std::unique_ptr<GossipProtocol> _protocol;

    // Parsed peer addresses (owned by orchestrator, shared with modules)
    std::vector<seastar::socket_address> _peer_addresses;

    // Receive loop future
    seastar::future<> _receive_loop_future;

    // Seastar metrics registration
    seastar::metrics::metric_groups _metrics;

    // Gate for gossip tasks - ensures no new tasks during shutdown
    seastar::gate _gossip_task_gate;

    // Receive loop (runs continuously while service is active)
    seastar::future<> receive_loop();

    // Register Prometheus metrics
    void register_metrics();

    // Parse peer address string "IP:Port" to socket_address
    static std::optional<seastar::socket_address> parse_peer_address(const std::string& peer);
};

}  // namespace ranvier
