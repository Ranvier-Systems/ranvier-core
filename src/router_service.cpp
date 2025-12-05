#include "router_service.hpp"

#if 0
#include <seastar/core/metrics.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/do_with.hh>
#include <boost/range/irange.hpp>
#include <unordered_map>
#include <random>
#include <chrono>

namespace ranvier {

thread_local RadixTree local_tree;

// Primary Storage (Map)
thread_local std::unordered_map<BackendId, seastar::socket_address> local_backends;
// Secondary Index (Vector) for O(1) Randomness
thread_local std::vector<BackendId> local_backend_ids;

// Internal counters (Thread Local)
// // We use uint64_t because Seastar metrics read from these directly
thread_local uint64_t stats_cache_hits = 0;
thread_local uint64_t stats_cache_misses = 0;

// Improved Seeder: Mix RandomDevice with Time to ensure Docker doesn't give same seed
thread_local std::mt19937 rng([]() {
std::random_device rd;
auto time_seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
return std::mt19937(rd() ^ time_seed);
}());

std::optional<BackendId> RouterService::lookup(const std::vector<int32_t>& tokens) {
    auto result = local_tree.lookup(tokens);

    // Increment counters
    if (result.has_value()) {
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
    if (local_backend_ids.empty()) return std::nullopt;

    // O(1) Random Selection from Vector
    // This removes any "Map Bucket" bias
    std::uniform_int_distribution<size_t> dist(0, local_backend_ids.size() - 1);
    size_t random_index = dist(rng);

    return local_backend_ids[random_index];
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
                // Update Map
                local_backends[id] = addr;

                // Update Vector (Check for duplicates first)
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

} // namespace ranvier
#endif





#include <seastar/core/smp.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/metrics.hh> // Required for metrics
#include <boost/range/irange.hpp>
#include <unordered_map>
#include <random>
#include <chrono>

namespace ranvier {

thread_local RadixTree local_tree;
thread_local std::unordered_map<BackendId, seastar::socket_address> local_backends;
thread_local std::vector<BackendId> local_backend_ids;

// The "Blacklist"
thread_local std::unordered_set<BackendId> local_dead_backends;

// 1. Define the Thread-Local Counters
thread_local uint64_t stats_cache_hits = 0;
thread_local uint64_t stats_cache_misses = 0;

thread_local std::mt19937 rng([]() {
    std::random_device rd;
    auto time_seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return std::mt19937(rd() ^ time_seed);
}());

// 2. Implement the Constructor (This was missing!)
RouterService::RouterService() {
    _metrics.add_group("ranvier", {
        seastar::metrics::make_counter("router_cache_hits", stats_cache_hits,
            seastar::metrics::description("Total number of prefix cache hits")),
        seastar::metrics::make_counter("router_cache_misses", stats_cache_misses,
            seastar::metrics::description("Total number of prefix cache misses"))
    });
}

// 3. Update lookup to increment counters
std::optional<BackendId> RouterService::lookup(const std::vector<int32_t>& tokens) {
    auto result = local_tree.lookup(tokens);

    // CIRCUIT BREAKER LOGIC:
    // If the cache hit points to a dead node, treat it as a miss.
    if (result.has_value()) {
        if (local_dead_backends.contains(result.value())) {
            stats_cache_misses++; // Technically a miss because we can't use it
            return std::nullopt;
        }
        stats_cache_hits++;
    } else {
        stats_cache_misses++;
    }

    return result;
}

std::optional<seastar::socket_address> RouterService::get_backend_address(BackendId id) {
    // We need to resolve the address even if it's dead, so the HealthService can ping it.
    // If it's dead, pretend it doesn't exist
    //if (local_dead_backends.contains(id)) {
        //return std::nullopt;
    //}

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

    // Simple retry loop to find a live node
    // In production, you'd maintain a separate "live_backends" vector for O(1)
    for (int i = 0; i < 5; ++i) { // Try 5 times to find a live one
        std::uniform_int_distribution<size_t> dist(0, local_backend_ids.size() - 1);
        BackendId candidate = local_backend_ids[dist(rng)];

        if (!local_dead_backends.contains(candidate)) {
            return candidate;
        }
    }
    return std::nullopt; // All nodes might be dead
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

                // Update Vector (Check for duplicates)
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
    // 1. Check Local State (Core 0) to dedup
    // local_dead_backends contains the ID if it is currently marked DOWN.
    bool is_currently_marked_dead = local_dead_backends.contains(id);

    // If the backend is Alive and NOT marked dead -> It's already healthy. Do nothing.
    // If the backend is Dead and IS marked dead -> It's already dead. Do nothing.
    if (is_alive != is_currently_marked_dead) {
        return seastar::make_ready_future<>();
    }

    // 2. State Change Detected! Log it.
    if (is_alive) {
        std::cout << "[CircuitBreaker] Backend " << id << " is UP 🟢 (Recovered)\n";
    } else {
        std::cout << "[CircuitBreaker] Backend " << id << " is DOWN 🔴 (Quarantined)\n";
    }

    // 3. Broadcast to all cores
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
