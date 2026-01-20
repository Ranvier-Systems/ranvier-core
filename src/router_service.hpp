#pragma once

#include "radix_tree.hpp"
#include "config.hpp"
#include "gossip_service.hpp"  // For NodeState

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <seastar/core/future.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/core/timer.hh>
#include <seastar/net/socket_defs.hh>

namespace ranvier {

// ============================================================================
// Route Batching Types
// ============================================================================
// These types support batching of remote route updates to prevent "SMP storms"
// when receiving high volumes of route announcements via cluster gossip.

// A pending remote route waiting to be batched and broadcast to all shards
struct PendingRemoteRoute {
    std::vector<int32_t> tokens;
    BackendId backend;
};

// Configuration constants for route batching behavior
struct RouteBatchConfig {
    // Maximum routes to buffer before forcing an immediate flush
    static constexpr size_t MAX_BATCH_SIZE = 100;

    // Hard upper limit on buffer size to prevent OOM from gossip flooding
    // When exceeded, oldest routes are dropped to make room for new ones
    static constexpr size_t MAX_BUFFER_SIZE = 10000;

    // Number of routes to drop at once when buffer overflows
    // Batching drops amortizes the O(n) vector erase cost across multiple inserts
    // Drop count chosen to clear ~10% of buffer, reducing drop frequency while preserving recency
    static constexpr size_t OVERFLOW_DROP_COUNT = 1000;

    // Timer interval for periodic flushes (ensures bounded latency)
    static constexpr std::chrono::milliseconds FLUSH_INTERVAL{10};
};

// Forward declaration
class GossipService;

// ============================================================================
// Unified Route Result
// ============================================================================
// Encapsulates all routing decisions in a single return type.
// This enables HttpController to make one call and handle one error path,
// moving routing mode logic into RouterService where it belongs.

struct RouteResult {
    std::optional<BackendId> backend_id;  // Selected backend (nullopt if routing failed)
    std::string routing_mode;             // "prefix", "hash", or "random"
    bool cache_hit = false;               // True if route was found in cache (ART or prefix affinity)
    std::string error_message;            // Non-empty if backend_id is nullopt
};

class RouterService {
public:
    RouterService();
    explicit RouterService(const RoutingConfig& config);
    RouterService(const RoutingConfig& routing_config, const ClusterConfig& cluster_config);

    // Initialize all shards with the routing config (must be called on shard 0)
    seastar::future<> initialize_shards();

    // Start the TTL cleanup timer (call after Seastar is initialized)
    void start_ttl_timer();

    // Stop the TTL cleanup timer (call before shutdown)
    void stop_ttl_timer();

    // ==========================================================================
    // Lifecycle Management
    // ==========================================================================
    //
    // RouterService registers metrics lambdas that may capture 'this' or access
    // thread-local state (NodeSlab). To prevent use-after-free when Prometheus
    // scrapes during/after shutdown:
    //
    // 1. stop() MUST be called before RouterService destruction
    // 2. Metrics are deregistered FIRST, before any other cleanup
    // 3. Only then are timers cancelled and gossip stopped
    //
    // Destruction order: metrics -> timers -> gossip -> destructor
    //
    seastar::future<> stop();

    // 1. DATA PLANE (Fast Lookups)

    // Unified routing entry point - encapsulates all routing mode logic
    // Returns RouteResult with backend_id, routing_mode, cache_hit, and error_message
    // This is the primary method HttpController should call for routing decisions.
    // request_id: Optional request ID for tracing (empty string if not tracing)
    RouteResult route_request(const std::vector<int32_t>& tokens,
                              const std::string& request_id = "");

    // Find which Backend ID owns this prefix (radix tree lookup)
    // request_id: Optional request ID for tracing (empty string if not tracing)
    std::optional<BackendId> lookup(const std::vector<int32_t>& tokens,
                                     const std::string& request_id = "");

    // Resolve ID -> IP:Port
    std::optional<seastar::socket_address> get_backend_address(BackendId id);

    // 2. CONTROL PLANE (Async Broadcasts)
    // Teach the tree a new prefix (Prefix -> ID) with LRU eviction
    // request_id: Optional request ID for tracing (empty string if not tracing)
    // Also broadcasts to cluster peers if gossip is enabled
    seastar::future<> learn_route_global(std::vector<int32_t> tokens, BackendId backend,
                                          const std::string& request_id = "");

    // Learn a route from a remote cluster peer (marks as REMOTE origin)
    // REMOTE routes can be evicted more aggressively than LOCAL routes
    seastar::future<> learn_route_remote(std::vector<int32_t> tokens, BackendId backend);

    // Start the gossip service (call after Seastar is initialized)
    // Also starts the route batch flush timer for remote route updates
    seastar::future<> start_gossip();

    // Stop the gossip service (call before shutdown)
    // Ensures all pending route batches are flushed before stopping
    seastar::future<> stop_gossip();

    // Teach the system a new server (ID -> IP:Port) with optional weight and priority
    // Weight: relative load balancing weight (default 100, higher = more traffic)
    // Priority: priority group (default 0 = highest, lower priority backends used for fallback)
    seastar::future<> register_backend_global(BackendId id, seastar::socket_address addr,
                                               uint32_t weight = 100, uint32_t priority = 0);

    // Remove a backend from all shards
    seastar::future<> unregister_backend_global(BackendId id);

    // Start draining a backend (stops new requests, allows existing cache hits)
    // After backend_drain_timeout, the backend will be fully removed
    seastar::future<> drain_backend_global(BackendId id);

    // Get a backend using weighted random selection within the highest available priority group
    std::optional<BackendId> get_random_backend();

    // Get a backend using prefix-affinity routing (ART + consistent hash fallback)
    // Routes requests with the same prefix to the same backend for KV cache reuse
    std::optional<BackendId> get_backend_for_prefix(const std::vector<int32_t>& tokens,
                                                     const std::string& request_id = "");

    // Get a backend using consistent hash only (no ART, no learning)
    // Used to measure baseline hash performance vs ART
    std::optional<BackendId> get_backend_by_hash(const std::vector<int32_t>& tokens,
                                                  const std::string& request_id = "");

    // Get list of all IDs (For the Health Checker to iterate)
    std::vector<BackendId> get_all_backend_ids() const;

    // ==========================================================================
    // Admin API - State Inspection
    // ==========================================================================

    // Backend info for admin API
    struct BackendState {
        BackendId id;
        std::string address;
        uint16_t port;
        uint32_t weight;
        uint32_t priority;
        bool is_draining;
        bool is_dead;
        int64_t drain_start_ms;  // 0 if not draining
    };

    // Get all backend states for admin inspection
    std::vector<BackendState> get_all_backend_states() const;

    // Get tree dump for admin inspection (local shard only)
    RadixTree::DumpNode get_tree_dump() const;

    // Get tree dump with prefix filter (local shard only)
    std::optional<RadixTree::DumpNode> get_tree_dump_with_prefix(const std::vector<TokenId>& prefix) const;

    // Circuit Breaker API
    seastar::future<> set_backend_status_global(BackendId id, bool is_alive);

    // Hot-reload: Update routing configuration on all shards
    seastar::future<> update_routing_config(const RoutingConfig& config);

    // Callback type for pool cleanup when a backend is removed
    using PoolCleanupCallback = std::function<void(seastar::socket_address)>;

    // Set callback to be invoked when a backend is fully removed (for pool cleanup)
    void set_pool_cleanup_callback(PoolCleanupCallback callback);

    // Callback type for circuit breaker cleanup when a backend is unregistered
    // Called on each shard during unregister_backend_global() to clean up circuit entries
    using CircuitCleanupCallback = std::function<void(BackendId)>;

    // Set shard-local callback for circuit cleanup (must be called on each shard)
    // The callback is stored in thread-local state and invoked during backend unregistration
    static void set_circuit_cleanup_callback(CircuitCleanupCallback callback);

    // Start the draining reaper timer (call after Seastar is initialized)
    void start_draining_reaper();

    // Stop the draining reaper timer (call before shutdown)
    void stop_draining_reaper();

    // Static: Only uses thread_local g_shard_state, safe to call from any shard
    static seastar::future<> remove_routes_for_backend(BackendId b_id);

    // Handle node state change notifications from cluster peers
    // When a peer broadcasts DRAINING, this sets their backend weight to 0
    // Static: Only uses thread_local g_shard_state, safe to call from any shard
    static seastar::future<> handle_node_state_change(BackendId backend, NodeState state);

    // Get the gossip service (for broadcasting node state on shutdown)
    GossipService* gossip_service() { return _gossip.get(); }

    // ==========================================================================
    // Testing Support
    // ==========================================================================
    // Reset shard-local state for unit testing. This clears all per-shard state
    // including backends, routes, and statistics. If a config is provided, the
    // state will be reinitialized with that configuration.
    //
    // IMPORTANT: Only call this in test code, not in production.
    static void reset_shard_state_for_testing(const RoutingConfig* cfg = nullptr);

private:
    // Thread-local metrics group
    // This holds the handle that keeps the metrics alive
    seastar::metrics::metric_groups _metrics;

    // Routing configuration (LRU parameters)
    RoutingConfig _config;

    // Cluster configuration for distributed mode
    ClusterConfig _cluster_config;

    // Gossip service for cluster state sync (only on shard 0)
    std::unique_ptr<GossipService> _gossip;

    // TTL cleanup timer (runs on shard 0, broadcasts to all shards)
    seastar::timer<> _ttl_timer;

    // Draining reaper timer (runs on shard 0, checks for expired draining backends)
    seastar::timer<> _draining_reaper_timer;

    // Batch flush timer for remote routes (runs on shard 0)
    seastar::timer<> _batch_flush_timer;

    // Buffer for pending remote routes (shard 0 only)
    // Routes are accumulated here and broadcast in batches to reduce SMP message traffic
    std::vector<PendingRemoteRoute> _pending_remote_routes;

    // Counter for routes dropped due to buffer overflow (for metrics and rate-limited logging)
    uint64_t _routes_dropped_overflow = 0;

    // Callback for pool cleanup when a backend is fully removed
    PoolCleanupCallback _pool_cleanup_callback;

    // Perform TTL cleanup on all shards
    void run_ttl_cleanup();

    // Check for backends that have been draining long enough and fully remove them
    void run_draining_reaper();

    // ---- Route Batching (private implementation) ----

    // Start the periodic timer that flushes pending route batches
    void start_batch_flush_timer();

    // Flush all pending remote routes to all shards
    // Returns a future that completes when all shards have processed the batch
    seastar::future<> flush_route_batch();
};

} // namespace ranvier
