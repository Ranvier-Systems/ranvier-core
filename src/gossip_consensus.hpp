// Ranvier Core - Gossip Consensus Module
//
// Manages cluster quorum and split-brain detection for the gossip protocol.
// Extracted from GossipService for better modularity and testability.
//
// Responsibilities:
// - Peer table management (liveness tracking)
// - Quorum state calculation and transitions
// - Split-brain detection with configurable thresholds
// - Resync mode for graceful recovery

#pragma once

#include "config.hpp"
#include "logging.hpp"
#include "types.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/timer.hh>
#include <seastar/net/socket_defs.hh>

namespace ranvier {

// Forward declaration for logger
inline seastar::logger& log_gossip_consensus() {
    static seastar::logger logger("ranvier.gossip.consensus");
    return logger;
}

// Cluster quorum state for split-brain detection
// Healthy: N/2+1 peers are reachable, full cluster operations allowed
// Degraded: Lost quorum, reject new route writes but serve existing routes
enum class QuorumState : uint8_t {
    HEALTHY = 1,   // Quorum maintained, full operations
    DEGRADED = 0,  // Quorum lost, read-only mode for routes
};

// Peer state tracking for liveness detection
// Clock parameter allows injecting a test clock for deterministic timing
template<typename Clock = seastar::lowres_clock>
struct BasicPeerState {
    typename Clock::time_point last_seen;
    bool is_alive = true;
    std::optional<BackendId> associated_backend;  // Track which backend this peer represents
};

// Backward-compatible alias: production code uses PeerState unchanged
using PeerState = BasicPeerState<>;

// Peer info for admin API
struct PeerInfo {
    std::string address;
    uint16_t port;
    bool is_alive;
    int64_t last_seen_ms;  // Milliseconds since last seen (relative time)
    std::optional<BackendId> associated_backend;
};

// Cluster state for admin API
struct ClusterState {
    std::string quorum_state;  // "HEALTHY" or "DEGRADED"
    size_t quorum_required;
    size_t peers_alive;
    size_t total_peers;
    size_t peers_recently_seen;
    bool is_draining;
    BackendId local_backend_id;
    std::vector<PeerInfo> peers;
};

// Callback type for pruning routes when a peer dies
using RoutePruneCallback = std::function<seastar::future<>(BackendId)>;

// GossipConsensus: Manages quorum and peer liveness for cluster state
//
// This class handles the consensus aspects of the gossip protocol:
// - Tracking which peers are alive based on heartbeats
// - Calculating quorum state (healthy vs degraded)
// - Triggering route pruning when peers are marked dead
//
// Threading: This class is shard-local. The peer table is only maintained
// on shard 0. Other shards can query quorum_state() but get stale values.
class GossipConsensus {
public:
    explicit GossipConsensus(const ClusterConfig& config);
    ~GossipConsensus() = default;

    // Non-copyable, non-movable
    GossipConsensus(const GossipConsensus&) = delete;
    GossipConsensus& operator=(const GossipConsensus&) = delete;

    // Lifecycle
    seastar::future<> start(const std::vector<seastar::socket_address>& initial_peers);
    seastar::future<> stop();

    // Peer management
    void update_peer_seen(const seastar::socket_address& peer);
    void associate_backend(const seastar::socket_address& peer, BackendId id);
    void add_peer(const seastar::socket_address& peer);
    void remove_peer(const seastar::socket_address& peer);

    // Update peer table with new list (used by DNS discovery)
    // Returns list of newly added peers
    std::vector<seastar::socket_address> update_peer_list(
        const std::vector<seastar::socket_address>& new_peers);

    // Quorum queries (lock-free, safe from any thread)
    QuorumState quorum_state() const { return _quorum_state; }
    bool has_quorum() const { return _quorum_state == QuorumState::HEALTHY; }
    bool is_degraded() const { return _quorum_state == QuorumState::DEGRADED; }

    // Fail-open mode: when enabled and quorum is lost, requests should be
    // routed randomly to healthy backends instead of being rejected.
    bool is_fail_open_mode() const {
        return _config.quorum_enabled &&
               _config.fail_open_on_quorum_loss &&
               is_degraded();
    }

    // Quorum metrics (lock-free)
    size_t quorum_required() const;
    size_t peers_alive_count() const { return _stats_cluster_peers_alive; }
    size_t total_peers_count() const { return _peer_table.size(); }
    size_t peers_recently_seen_count() const { return _stats_peers_recently_seen; }

    // Draining state
    bool is_draining() const { return _draining.load(std::memory_order_relaxed); }
    void set_draining(bool draining) { _draining.store(draining, std::memory_order_relaxed); }

    // Resync mode control
    bool is_resyncing() const { return _resyncing.load(std::memory_order_relaxed); }
    void start_resync();
    void end_resync();

    // Check if service is accepting new tasks
    bool is_accepting_tasks() const {
        return _running && !is_resyncing();
    }

    // Local backend ID
    void set_local_backend_id(BackendId id) { _local_backend_id = id; }
    BackendId local_backend_id() const { return _local_backend_id; }

    // Admin API
    ClusterState get_cluster_state() const;

    // Per-shard callback registration for route pruning.
    // Each shard registers its own local callback to avoid broadcasting
    // std::function across shards (anti-pattern Bug #3: cross-shard free).
    // Shard 0 broadcasts only the scalar BackendId; each shard invokes
    // its own locally-registered callback.
    static void register_local_prune_callback(RoutePruneCallback callback);
    static void clear_local_prune_callback();

    // Provide read access to peer table for protocol layer
    const std::unordered_map<seastar::socket_address, PeerState>& peer_table() const {
        return _peer_table;
    }

    // Metrics accessors for GossipService to expose
    uint64_t stats_quorum_state() const { return _stats_quorum_state; }
    uint64_t quorum_transitions() const { return _quorum_transitions; }
    uint64_t routes_rejected_degraded() const { return _routes_rejected_degraded; }
    uint64_t routes_rejected_incoming_degraded() const { return _routes_rejected_incoming_degraded; }
    uint64_t routes_allowed_fail_open() const { return _routes_allowed_fail_open; }
    uint64_t gossip_accepted_fail_open() const { return _gossip_accepted_fail_open; }

    // Increment metrics (called by protocol layer)
    void inc_routes_rejected_degraded() { ++_routes_rejected_degraded; }
    void inc_routes_rejected_incoming_degraded() { ++_routes_rejected_incoming_degraded; }
    void inc_routes_allowed_fail_open() { ++_routes_allowed_fail_open; }
    void inc_gossip_accepted_fail_open() { ++_gossip_accepted_fail_open; }

    // Access timer gate for shared timer safety
    seastar::gate& timer_gate() { return _timer_gate; }

private:
    const ClusterConfig& _config;
    bool _running = false;

    // Peer table (shard 0 only)
    std::unordered_map<seastar::socket_address, PeerState> _peer_table;

    // Local backend ID
    BackendId _local_backend_id = 0;

    // State flags
    std::atomic<bool> _draining{false};
    std::atomic<bool> _resyncing{false};

    // Quorum state
    QuorumState _quorum_state = QuorumState::HEALTHY;
    uint64_t _stats_quorum_state = 1;  // 1=healthy, 0=degraded (for Prometheus gauge)
    uint64_t _quorum_transitions = 0;
    uint64_t _routes_rejected_degraded = 0;
    uint64_t _routes_rejected_incoming_degraded = 0;
    uint64_t _routes_allowed_fail_open = 0;
    uint64_t _gossip_accepted_fail_open = 0;
    uint64_t _stats_cluster_peers_alive = 0;
    uint64_t _stats_peers_recently_seen = 0;
    bool _quorum_warning_active = false;

    // Timers
    seastar::timer<> _liveness_timer;
    seastar::gate _timer_gate;

    // Internal methods
    void check_liveness();
    void check_quorum();
    void update_quorum_state();
};

}  // namespace ranvier
