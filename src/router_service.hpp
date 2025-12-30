#pragma once

#include "radix_tree.hpp"
#include "config.hpp"

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

// Forward declaration
class GossipService;

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

    // 1. DATA PLANE (Fast Lookups)
    // Find which Backend ID owns this prefix
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
    seastar::future<> start_gossip();

    // Stop the gossip service (call before shutdown)
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

    // Get a backend using prefix-affinity routing (consistent hashing on prefix tokens)
    // Routes requests with the same prefix to the same backend for KV cache reuse
    std::optional<BackendId> get_backend_for_prefix(const std::vector<int32_t>& tokens,
                                                     const std::string& request_id = "");

    // Get list of all IDs (For the Health Checker to iterate)
    std::vector<BackendId> get_all_backend_ids() const;

    // Circuit Breaker API
    seastar::future<> set_backend_status_global(BackendId id, bool is_alive);

    // Hot-reload: Update routing configuration on all shards
    seastar::future<> update_routing_config(const RoutingConfig& config);

    // Callback type for pool cleanup when a backend is removed
    using PoolCleanupCallback = std::function<void(seastar::socket_address)>;

    // Set callback to be invoked when a backend is fully removed (for pool cleanup)
    void set_pool_cleanup_callback(PoolCleanupCallback callback);

    // Start the draining reaper timer (call after Seastar is initialized)
    void start_draining_reaper();

    // Stop the draining reaper timer (call before shutdown)
    void stop_draining_reaper();

    seastar::future<> remove_routes_for_backend(BackendId b_id);

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

    // Callback for pool cleanup when a backend is fully removed
    PoolCleanupCallback _pool_cleanup_callback;

    // Perform TTL cleanup on all shards
    void run_ttl_cleanup();

    // Check for backends that have been draining long enough and fully remove them
    void run_draining_reaper();
};

} // namespace ranvier
