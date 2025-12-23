#pragma once

#include "radix_tree.hpp"

#include <optional>
#include <unordered_set>
#include <vector>

#include <seastar/core/future.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/net/socket_defs.hh>

namespace ranvier {

class RouterService {
public:
    RouterService();

    // 1. DATA PLANE (Fast Lookups)
    // Find which Backend ID owns this prefix
    std::optional<BackendId> lookup(const std::vector<int32_t>& tokens);

    // Resolve ID -> IP:Port
    std::optional<seastar::socket_address> get_backend_address(BackendId id);

    // 2. CONTROL PLANE (Async Broadcasts)
    // Teach the tree a new prefix (Prefix -> ID)
    seastar::future<> learn_route_global(std::vector<int32_t> tokens, BackendId backend);

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

private:
    // Thread-local metrics group
    // This holds the handle that keeps the metrics alive
    seastar::metrics::metric_groups _metrics;
};

} // namespace ranvier
