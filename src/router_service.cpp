#include "router_service.hpp"
#include "logging.hpp"

#include <seastar/core/smp.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/metrics.hh>
#include <boost/range/irange.hpp>
#include <unordered_map>
#include <random>
#include <chrono>

namespace ranvier {

thread_local RadixTree local_tree;
thread_local std::unordered_map<BackendId, seastar::socket_address> local_backends;
thread_local std::vector<BackendId> local_backend_ids;

// The "Blacklist" for circuit breaker
thread_local std::unordered_set<BackendId> local_dead_backends;

// Thread-local counters for metrics
thread_local uint64_t stats_cache_hits = 0;
thread_local uint64_t stats_cache_misses = 0;

thread_local std::mt19937 rng([]() {
    std::random_device rd;
    auto time_seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return std::mt19937(rd() ^ time_seed);
}());

RouterService::RouterService() {
    _metrics.add_group("ranvier", {
        seastar::metrics::make_counter("router_cache_hits", stats_cache_hits,
            seastar::metrics::description("Total number of prefix cache hits")),
        seastar::metrics::make_counter("router_cache_misses", stats_cache_misses,
            seastar::metrics::description("Total number of prefix cache misses"))
    });
}

std::optional<BackendId> RouterService::lookup(const std::vector<int32_t>& tokens) {
    auto result = local_tree.lookup(tokens);

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
        return it->second;
    }
    return std::nullopt;
}

std::optional<BackendId> RouterService::get_random_backend() {
    if (local_backend_ids.empty()) {
        return std::nullopt;
    }

    // Simple retry loop to find a live backend
    for (int i = 0; i < 5; ++i) {
        std::uniform_int_distribution<size_t> dist(0, local_backend_ids.size() - 1);
        BackendId candidate = local_backend_ids[dist(rng)];

        if (!local_dead_backends.contains(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

seastar::future<> RouterService::learn_route_global(std::vector<int32_t> tokens, BackendId backend) {
    return seastar::do_with(std::move(tokens), [backend](std::vector<int32_t>& shared_tokens) {
        return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count), [backend, &shared_tokens] (unsigned shard_id) {
            return seastar::smp::submit_to(shard_id, [backend, tokens = shared_tokens] {
                local_tree.insert(tokens, backend);
                return seastar::make_ready_future<>();
            });
        });
    });
}

seastar::future<> RouterService::register_backend_global(BackendId id, seastar::socket_address addr) {
    return seastar::do_with(addr, [id](seastar::socket_address& shared_addr) {
        return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count), [id, &shared_addr] (unsigned shard_id) {
            return seastar::smp::submit_to(shard_id, [id, addr = shared_addr] {
                local_backends[id] = addr;

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
