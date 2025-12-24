#include "router_service.hpp"
#include "logging.hpp"

#include <seastar/core/smp.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/metrics.hh>
#include <boost/range/irange.hpp>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <random>
#include <chrono>

namespace ranvier {

// Backend info including weight and priority
struct BackendInfo {
    seastar::socket_address addr;
    uint32_t weight = 100;
    uint32_t priority = 0;
};

// Thread-local RadixTree pointer (initialized per-shard with config)
thread_local std::unique_ptr<RadixTree> local_tree;
thread_local std::unordered_map<BackendId, BackendInfo> local_backends;
thread_local std::vector<BackendId> local_backend_ids;

// The "Blacklist" for circuit breaker
thread_local std::unordered_set<BackendId> local_dead_backends;

// Thread-local counters for metrics
thread_local uint64_t stats_cache_hits = 0;
thread_local uint64_t stats_cache_misses = 0;
thread_local uint64_t stats_routes_evicted = 0;
thread_local uint64_t stats_routes_expired = 0;

// Thread-local routing configuration (set during shard initialization)
thread_local size_t local_max_routes = 100000;
thread_local std::chrono::seconds local_ttl_seconds{3600};

thread_local std::mt19937 rng([]() {
    std::random_device rd;
    auto time_seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return std::mt19937(rd() ^ time_seed);
}());

RouterService::RouterService() : RouterService(RoutingConfig{}) {}

RouterService::RouterService(const RoutingConfig& config) : _config(config) {
    // Initialize shard 0's local_tree immediately
    local_tree = std::make_unique<RadixTree>(config.block_alignment);
    local_max_routes = config.max_routes;
    local_ttl_seconds = config.ttl_seconds;

    _metrics.add_group("ranvier", {
        seastar::metrics::make_counter("router_cache_hits", stats_cache_hits,
            seastar::metrics::description("Total number of prefix cache hits")),
        seastar::metrics::make_counter("router_cache_misses", stats_cache_misses,
            seastar::metrics::description("Total number of prefix cache misses")),
        seastar::metrics::make_counter("router_routes_evicted", stats_routes_evicted,
            seastar::metrics::description("Total number of routes evicted due to capacity limits")),
        seastar::metrics::make_counter("router_routes_expired", stats_routes_expired,
            seastar::metrics::description("Total number of routes expired due to TTL"))
    });
}

seastar::future<> RouterService::initialize_shards() {
    // Initialize RadixTree on all other shards with the config from shard 0
    uint32_t block_alignment = _config.block_alignment;
    size_t max_routes = _config.max_routes;
    auto ttl_seconds = _config.ttl_seconds;

    return seastar::parallel_for_each(boost::irange(1u, seastar::smp::count),
        [block_alignment, max_routes, ttl_seconds](unsigned shard_id) {
            return seastar::smp::submit_to(shard_id, [block_alignment, max_routes, ttl_seconds] {
                local_tree = std::make_unique<RadixTree>(block_alignment);
                local_max_routes = max_routes;
                local_ttl_seconds = ttl_seconds;
                return seastar::make_ready_future<>();
            });
        });
}

void RouterService::start_ttl_timer() {
    // Set up the timer to fire every 60 seconds
    _ttl_timer.set_callback([this] { run_ttl_cleanup(); });
    _ttl_timer.arm_periodic(std::chrono::seconds(60));
    log_main.info("TTL cleanup timer started (interval: 60s, TTL: {}s)", _config.ttl_seconds.count());
}

void RouterService::stop_ttl_timer() {
    _ttl_timer.cancel();
    log_main.info("TTL cleanup timer stopped");
}

void RouterService::run_ttl_cleanup() {
    auto cutoff = std::chrono::steady_clock::now() - local_ttl_seconds;

    // Run cleanup on all shards
    (void)seastar::parallel_for_each(boost::irange(0u, seastar::smp::count), [cutoff](unsigned shard_id) {
        return seastar::smp::submit_to(shard_id, [cutoff] {
            if (local_tree) {
                size_t removed = local_tree->remove_expired(cutoff);
                if (removed > 0) {
                    stats_routes_expired += removed;
                    log_main.debug("Shard {}: Expired {} routes", seastar::this_shard_id(), removed);
                }
            }
            return seastar::make_ready_future<>();
        });
    });
}

std::optional<BackendId> RouterService::lookup(const std::vector<int32_t>& tokens) {
    if (!local_tree) return std::nullopt;
    auto result = local_tree->lookup(tokens);

    // Circuit breaker: if cache hit points to dead backend, treat as miss
    if (result.has_value()) {
        if (local_dead_backends.contains(result.value())) {
            stats_cache_misses++;
            return std::nullopt;
        }
        stats_cache_hits++;
    } else {
        stats_cache_misses++;
    }

    return result;
}

std::optional<seastar::socket_address> RouterService::get_backend_address(BackendId id) {
    auto it = local_backends.find(id);
    if (it != local_backends.end()) {
        return it->second.addr;
    }
    return std::nullopt;
}

std::optional<BackendId> RouterService::get_random_backend() {
    if (local_backend_ids.empty()) {
        return std::nullopt;
    }

    // Collect live backends grouped by priority
    // Priority 0 = highest, backends with lower priority number are tried first
    std::map<uint32_t, std::vector<std::pair<BackendId, uint32_t>>> priority_groups;

    for (BackendId id : local_backend_ids) {
        if (local_dead_backends.contains(id)) {
            continue;  // Skip dead backends
        }
        auto it = local_backends.find(id);
        if (it == local_backends.end()) {
            continue;
        }
        const auto& info = it->second;
        if (info.weight > 0) {
            priority_groups[info.priority].emplace_back(id, info.weight);
        }
    }

    if (priority_groups.empty()) {
        return std::nullopt;  // No live backends available
    }

    // Get the highest priority group (lowest priority number)
    const auto& candidates = priority_groups.begin()->second;

    // Calculate total weight
    uint64_t total_weight = 0;
    for (const auto& [id, weight] : candidates) {
        total_weight += weight;
    }

    if (total_weight == 0) {
        return std::nullopt;
    }

    // Weighted random selection
    std::uniform_int_distribution<uint64_t> dist(0, total_weight - 1);
    uint64_t roll = dist(rng);

    uint64_t cumulative = 0;
    for (const auto& [id, weight] : candidates) {
        cumulative += weight;
        if (roll < cumulative) {
            return id;
        }
    }

    // Fallback (shouldn't reach here)
    return candidates.back().first;
}

seastar::future<> RouterService::learn_route_global(std::vector<int32_t> tokens, BackendId backend) {
    return seastar::do_with(std::move(tokens), [backend](std::vector<int32_t>& shared_tokens) {
        return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count), [backend, &shared_tokens] (unsigned shard_id) {
            return seastar::smp::submit_to(shard_id, [backend, tokens = shared_tokens] {
                if (!local_tree) return seastar::make_ready_future<>();

                // LRU eviction: if at capacity, evict oldest routes first
                if (local_max_routes > 0) {
                    while (local_tree->route_count() >= local_max_routes) {
                        if (local_tree->evict_oldest()) {
                            stats_routes_evicted++;
                        } else {
                            break;  // No more routes to evict
                        }
                    }
                }

                local_tree->insert(tokens, backend);
                return seastar::make_ready_future<>();
            });
        });
    });
}

seastar::future<> RouterService::register_backend_global(BackendId id, seastar::socket_address addr,
                                                          uint32_t weight, uint32_t priority) {
    return seastar::do_with(addr, weight, priority, [id](seastar::socket_address& shared_addr,
                                                          uint32_t& shared_weight,
                                                          uint32_t& shared_priority) {
        return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count),
            [id, &shared_addr, &shared_weight, &shared_priority] (unsigned shard_id) {
            return seastar::smp::submit_to(shard_id, [id, addr = shared_addr,
                                                       weight = shared_weight,
                                                       priority = shared_priority] {
                local_backends[id] = BackendInfo{addr, weight, priority};

                // Update vector (check for duplicates)
                bool exists = false;
                for (auto existing : local_backend_ids) {
                    if (existing == id) { exists = true; break; }
                }
                if (!exists) {
                    local_backend_ids.push_back(id);
                }

                return seastar::make_ready_future<>();
            });
        });
    });
}

seastar::future<> RouterService::unregister_backend_global(BackendId id) {
    return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count), [id] (unsigned shard_id) {
        return seastar::smp::submit_to(shard_id, [id] {
            // Remove from backends map
            local_backends.erase(id);

            // Remove from backend IDs vector
            auto it = std::find(local_backend_ids.begin(), local_backend_ids.end(), id);
            if (it != local_backend_ids.end()) {
                local_backend_ids.erase(it);
            }

            // Also remove from dead backends set if present
            local_dead_backends.erase(id);

            return seastar::make_ready_future<>();
        });
    });
}

std::vector<BackendId> RouterService::get_all_backend_ids() const {
    return local_backend_ids;
}

seastar::future<> RouterService::set_backend_status_global(BackendId id, bool is_alive) {
    // Check local state (Core 0) to deduplicate logs
    bool is_currently_marked_dead = local_dead_backends.contains(id);

    // No state change needed
    if (is_alive != is_currently_marked_dead) {
        return seastar::make_ready_future<>();
    }

    // State change detected - log it
    if (is_alive) {
        log_health.info("Backend {} is UP (Recovered)", id);
    } else {
        log_health.warn("Backend {} is DOWN (Quarantined)", id);
    }

    // Broadcast to all cores
    return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count), [id, is_alive] (unsigned shard_id) {
        return seastar::smp::submit_to(shard_id, [id, is_alive] {
            if (is_alive) {
                local_dead_backends.erase(id);
            } else {
                local_dead_backends.insert(id);
            }
            return seastar::make_ready_future<>();
        });
    });
}

} // namespace ranvier
