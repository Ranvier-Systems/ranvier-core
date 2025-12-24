#pragma once

#include "radix_tree.hpp"
#include "config.hpp"

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <seastar/core/future.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/core/timer.hh>
#include <seastar/net/socket_defs.hh>

namespace ranvier {

class RouterService {
public:
    RouterService();
    explicit RouterService(const RoutingConfig& config);

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
    seastar::future<> learn_route_global(std::vector<int32_t> tokens, BackendId backend,
                                          const std::string& request_id = "");

    // Teach the system a new server (ID -> IP:Port) with optional weight and priority
    // Weight: relative load balancing weight (default 100, higher = more traffic)
    // Priority: priority group (default 0 = highest, lower priority backends used for fallback)
    seastar::future<> register_backend_global(BackendId id, seastar::socket_address addr,
                                               uint32_t weight = 100, uint32_t priority = 0);

    // Remove a backend from all shards
    seastar::future<> unregister_backend_global(BackendId id);

    // Get a backend using weighted random selection within the highest available priority group
    std::optional<BackendId> get_random_backend();

    // Get list of all IDs (For the Health Checker to iterate)
    std::vector<BackendId> get_all_backend_ids() const;

    // Circuit Breaker API
    seastar::future<> set_backend_status_global(BackendId id, bool is_alive);

    // Hot-reload: Update routing configuration on all shards
    seastar::future<> update_routing_config(const RoutingConfig& config);

private:
    // Thread-local metrics group
    // This holds the handle that keeps the metrics alive
    seastar::metrics::metric_groups _metrics;

    // Routing configuration (LRU parameters)
    RoutingConfig _config;

    // TTL cleanup timer (runs on shard 0, broadcasts to all shards)
    seastar::timer<> _ttl_timer;

    // Perform TTL cleanup on all shards
    void run_ttl_cleanup();
};

} // namespace ranvier
