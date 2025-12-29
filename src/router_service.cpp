#include "router_service.hpp"
#include "gossip_service.hpp"
#include "logging.hpp"
#include "metrics_service.hpp"

#include <seastar/core/smp.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/when_all.hh>
#include <boost/range/irange.hpp>
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <algorithm>
#include <map>
#include <random>
#include <chrono>

namespace ranvier {

// Backend info including weight, priority, and draining state
struct BackendInfo {
    seastar::socket_address addr;
    uint32_t weight = 100;
    uint32_t priority = 0;
    bool is_draining = false;
    std::chrono::steady_clock::time_point drain_start_time;
};

// Thread-local RadixTree pointer (initialized per-shard with config)
thread_local std::unique_ptr<RadixTree> local_tree;
// Using absl::flat_hash_map for SIMD-accelerated lookups and better cache locality
// These remain shard-local to maintain Ranvier's lock-free architecture
thread_local absl::flat_hash_map<BackendId, BackendInfo> local_backends;
thread_local std::vector<BackendId> local_backend_ids;

// The "Blacklist" for circuit breaker
// Using absl::flat_hash_set for O(1) SIMD-accelerated membership checks
thread_local absl::flat_hash_set<BackendId> local_dead_backends;

// Thread-local counters for metrics
thread_local uint64_t stats_cache_hits = 0;
thread_local uint64_t stats_cache_misses = 0;
thread_local uint64_t stats_routes_evicted = 0;
thread_local uint64_t stats_routes_expired = 0;
thread_local uint64_t stats_cluster_routes_pruned = 0;

// Thread-local routing configuration (set during shard initialization)
thread_local size_t local_max_routes = 100000;
thread_local std::chrono::seconds local_ttl_seconds{3600};
thread_local std::chrono::seconds local_backend_drain_timeout{60};
thread_local bool local_prefix_affinity_enabled = true;
thread_local size_t local_prefix_token_length = 128;
thread_local uint32_t local_block_alignment = 16;

// Thread-local counter for prefix affinity routing
thread_local uint64_t stats_prefix_affinity_routes = 0;

// FNV-1a hash constants for 64-bit
constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;

// Hash function for prefix tokens using FNV-1a
// Aligns to block_alignment boundary for vLLM compatibility
inline uint64_t hash_prefix(const int32_t* tokens, size_t count, uint32_t block_alignment) {
    // Align to block_alignment boundary
    size_t aligned_len = (count / block_alignment) * block_alignment;
    if (aligned_len == 0) aligned_len = count;

    uint64_t hash = FNV_OFFSET_BASIS;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(tokens);
    size_t byte_len = aligned_len * sizeof(int32_t);

    for (size_t i = 0; i < byte_len; ++i) {
        hash ^= data[i];
        hash *= FNV_PRIME;
    }

    return hash;
}

thread_local std::mt19937 rng([]() {
    std::random_device rd;
    auto time_seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return std::mt19937(rd() ^ time_seed);
}());

RouterService::RouterService() : RouterService(RoutingConfig{}) {}

RouterService::RouterService(const RoutingConfig& config)
    : RouterService(config, ClusterConfig{}) {}

RouterService::RouterService(const RoutingConfig& routing_config, const ClusterConfig& cluster_config)
    : _config(routing_config), _cluster_config(cluster_config) {
    // Initialize shard 0's local_tree immediately
    local_tree = std::make_unique<RadixTree>(routing_config.block_alignment);
    local_max_routes = routing_config.max_routes;
    local_ttl_seconds = routing_config.ttl_seconds;
    local_backend_drain_timeout = routing_config.backend_drain_timeout;
    local_prefix_affinity_enabled = routing_config.prefix_affinity_enabled;
    local_prefix_token_length = routing_config.prefix_token_length;
    local_block_alignment = routing_config.block_alignment;

    // Create GossipService if cluster mode is enabled
    if (_cluster_config.enabled) {
        _gossip = std::make_unique<GossipService>(_cluster_config);

        // Set up callback to handle incoming route announcements
        _gossip->set_route_learn_callback([this](std::vector<TokenId> tokens, BackendId backend) {
            return learn_route_remote(std::move(tokens), backend);
        });

        // Set up callback to prune routes when a peer fails
        _gossip->set_route_prune_callback([this](BackendId backend) {
            return remove_routes_for_backend(backend);
        });

        log_router.info("Cluster mode enabled with {} peers", _cluster_config.peers.size());
    }

    _metrics.add_group("ranvier", {
        seastar::metrics::make_counter("router_cache_hits", stats_cache_hits,
            seastar::metrics::description("Total number of prefix cache hits")),
        seastar::metrics::make_counter("router_cache_misses", stats_cache_misses,
            seastar::metrics::description("Total number of prefix cache misses")),
        seastar::metrics::make_counter("router_routes_evicted", stats_routes_evicted,
            seastar::metrics::description("Total number of routes evicted due to capacity limits")),
        seastar::metrics::make_counter("router_routes_expired", stats_routes_expired,
            seastar::metrics::description("Total number of routes expired due to TTL")),
        seastar::metrics::make_counter("router_cluster_routes_pruned", stats_cluster_routes_pruned,
            seastar::metrics::description("Total number of routes pruned when cluster peer fails")),
        seastar::metrics::make_counter("router_prefix_affinity_routes", stats_prefix_affinity_routes,
            seastar::metrics::description("Total number of requests routed via prefix affinity"))
    });
}

seastar::future<> RouterService::initialize_shards() {
    // Initialize RadixTree on all other shards with the config from shard 0
    uint32_t block_alignment = _config.block_alignment;
    size_t max_routes = _config.max_routes;
    auto ttl_seconds = _config.ttl_seconds;
    auto drain_timeout = _config.backend_drain_timeout;
    bool prefix_affinity_enabled = _config.prefix_affinity_enabled;
    size_t prefix_token_length = _config.prefix_token_length;

    return seastar::parallel_for_each(boost::irange(1u, seastar::smp::count),
        [block_alignment, max_routes, ttl_seconds, drain_timeout, prefix_affinity_enabled, prefix_token_length](unsigned shard_id) {
            return seastar::smp::submit_to(shard_id, [block_alignment, max_routes, ttl_seconds, drain_timeout, prefix_affinity_enabled, prefix_token_length] {
                local_tree = std::make_unique<RadixTree>(block_alignment);
                local_max_routes = max_routes;
                local_ttl_seconds = ttl_seconds;
                local_backend_drain_timeout = drain_timeout;
                local_prefix_affinity_enabled = prefix_affinity_enabled;
                local_prefix_token_length = prefix_token_length;
                local_block_alignment = block_alignment;
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

std::optional<BackendId> RouterService::lookup(const std::vector<int32_t>& tokens,
                                                 const std::string& request_id) {
    if (!local_tree) return std::nullopt;
    auto result = local_tree->lookup(tokens);

    // Circuit breaker: if cache hit points to dead backend, treat as miss
    if (result.has_value()) {
        if (local_dead_backends.contains(result.value())) {
            if (!request_id.empty()) {
                log_router.debug("[{}] Cache hit for dead backend {}, treating as miss",
                                 request_id, result.value());
            }
            stats_cache_misses++;
            // Update MetricsService for ranvier_cache_hit_ratio gauge
            if (g_metrics) {
                metrics().record_cache_miss();
            }
            return std::nullopt;
        }
        if (!request_id.empty()) {
            log_router.debug("[{}] Cache hit: {} tokens -> backend {}",
                             request_id, tokens.size(), result.value());
        }
        stats_cache_hits++;
        // Update MetricsService for ranvier_cache_hit_ratio gauge
        if (g_metrics) {
            metrics().record_cache_hit();
        }
    } else {
        if (!request_id.empty()) {
            log_router.debug("[{}] Cache miss for {} tokens", request_id, tokens.size());
        }
        stats_cache_misses++;
        // Update MetricsService for ranvier_cache_hit_ratio gauge
        if (g_metrics) {
            metrics().record_cache_miss();
        }
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
        if (info.is_draining) {
            continue;  // Skip draining backends for new requests
        }
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

std::optional<BackendId> RouterService::get_backend_for_prefix(const std::vector<int32_t>& tokens,
                                                                 const std::string& request_id) {
    if (local_backend_ids.empty()) {
        return std::nullopt;
    }

    // Collect live backends (not dead, not draining)
    std::vector<BackendId> live_backends;
    for (BackendId id : local_backend_ids) {
        if (local_dead_backends.contains(id)) {
            continue;  // Skip dead backends
        }
        auto it = local_backends.find(id);
        if (it == local_backends.end()) {
            continue;
        }
        if (it->second.is_draining) {
            continue;  // Skip draining backends
        }
        live_backends.push_back(id);
    }

    if (live_backends.empty()) {
        return std::nullopt;
    }

    // Sort for deterministic ordering across shards
    std::sort(live_backends.begin(), live_backends.end());

    // Extract prefix (first N tokens)
    size_t prefix_len = std::min(tokens.size(), local_prefix_token_length);
    if (prefix_len == 0) {
        // No tokens to route on, fall back to first backend
        return live_backends[0];
    }

    // HYBRID ROUTING: ART lookup first, hash fallback
    //
    // 1. ART provides "Longest Prefix Match" - finds the longest known prefix
    //    that matches the beginning of this request. This enables partial
    //    prefix matching for similar requests with different suffixes.
    //
    // 2. If ART finds a match, we route to that backend (affinity to learned route)
    //
    // 3. If no match in ART, we use consistent hashing on the prefix for
    //    deterministic routing. The route will be learned after success.

    // Step 1: Try ART lookup for longest prefix match
    if (local_tree) {
        std::span<const TokenId> token_span(tokens.data(), tokens.size());
        auto art_result = local_tree->lookup(token_span);

        if (art_result.has_value()) {
            BackendId art_backend = art_result.value();

            // Verify the backend is still live
            if (std::find(live_backends.begin(), live_backends.end(), art_backend) != live_backends.end()) {
                // ART cache hit - route to learned backend
                if (!request_id.empty()) {
                    log_router.debug("[{}] Prefix affinity (ART hit): {} tokens -> backend {}",
                                     request_id, tokens.size(), art_backend);
                }
                stats_cache_hits++;
                stats_prefix_affinity_routes++;
                if (g_metrics) {
                    metrics().record_cache_hit();
                }
                return art_backend;
            }
            // Backend is dead/draining, fall through to hash-based selection
            log_router.debug("[{}] ART backend {} is unavailable, using hash fallback",
                             request_id, art_backend);
        }
    }

    // Step 2: No ART match (or backend unavailable) - use consistent hashing
    // This provides deterministic routing for new prefixes
    uint64_t prefix_hash = hash_prefix(tokens.data(), prefix_len, local_block_alignment);
    size_t index = prefix_hash % live_backends.size();
    BackendId selected = live_backends[index];

    if (!request_id.empty()) {
        log_router.debug("[{}] Prefix affinity (hash): {} tokens, hash={}, index={}/{} -> backend {}",
                         request_id, prefix_len, prefix_hash, index, live_backends.size(), selected);
    }

    // This is a cache miss - the route will be learned after successful response
    stats_cache_misses++;
    stats_prefix_affinity_routes++;
    if (g_metrics) {
        metrics().record_cache_miss();
    }

    return selected;
}

seastar::future<> RouterService::learn_route_global(std::vector<int32_t> tokens, BackendId backend,
                                                       const std::string& request_id) {
    // Truncate to prefix length - we only store the prefix in the ART, not the full sequence
    // This ensures that requests with the same prefix (e.g., same system prompt) but
    // different suffixes (e.g., different user queries) share the same routing entry.
    size_t prefix_len = std::min(tokens.size(), _config.prefix_token_length);
    if (prefix_len < tokens.size()) {
        tokens.resize(prefix_len);
    }

    // Log route learning with request_id on shard 0 before broadcasting
    if (!request_id.empty()) {
        log_router.info("[{}] Learning route: {} tokens (prefix) -> backend {}",
                        request_id, tokens.size(), backend);
    }

    // Broadcast to cluster peers if gossip is enabled
    seastar::future<> gossip_future = seastar::make_ready_future<>();
    if (_gossip && _gossip->is_enabled()) {
        // Copy tokens for gossip broadcast (tokens will be moved for shard broadcast)
        gossip_future = _gossip->broadcast_route(tokens, backend);
    }

    // Broadcast to all local shards with LOCAL origin
    auto shard_future = seastar::do_with(std::move(tokens), [backend](std::vector<int32_t>& shared_tokens) {
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

                // Insert with LOCAL origin (direct request on this node)
                local_tree->insert(tokens, backend, RouteOrigin::LOCAL);
                return seastar::make_ready_future<>();
            });
        });
    });

    // Wait for both gossip broadcast and shard updates
    return seastar::when_all_succeed(std::move(gossip_future), std::move(shard_future)).discard_result();
}

seastar::future<> RouterService::learn_route_remote(std::vector<int32_t> tokens, BackendId backend) {
    // Learn route from cluster peer - mark as REMOTE origin
    // REMOTE routes can be evicted more aggressively than LOCAL routes
    log_router.debug("Learning remote route: {} tokens -> backend {}", tokens.size(), backend);

    return seastar::do_with(std::move(tokens), [backend](std::vector<int32_t>& shared_tokens) {
        return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count), [backend, &shared_tokens] (unsigned shard_id) {
            return seastar::smp::submit_to(shard_id, [backend, tokens = shared_tokens] {
                if (!local_tree) return seastar::make_ready_future<>();

                // LRU eviction: prefer evicting REMOTE routes first when at capacity
                if (local_max_routes > 0) {
                    while (local_tree->route_count() >= local_max_routes) {
                        if (local_tree->evict_oldest_remote()) {
                            stats_routes_evicted++;
                        } else {
                            break;  // No more routes to evict
                        }
                    }
                }

                // Insert with REMOTE origin (learned from cluster gossip)
                local_tree->insert(tokens, backend, RouteOrigin::REMOTE);
                return seastar::make_ready_future<>();
            });
        });
    });
}

seastar::future<> RouterService::start_gossip() {
    if (_gossip) {
        return _gossip->start();
    }
    return seastar::make_ready_future<>();
}

seastar::future<> RouterService::stop_gossip() {
    if (_gossip) {
        return _gossip->stop();
    }
    return seastar::make_ready_future<>();
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

seastar::future<> RouterService::update_routing_config(const RoutingConfig& config) {
    // Update local config on shard 0
    _config = config;

    // Capture values to broadcast
    size_t max_routes = config.max_routes;
    auto ttl_seconds = config.ttl_seconds;
    auto drain_timeout = config.backend_drain_timeout;
    bool prefix_affinity_enabled = config.prefix_affinity_enabled;
    size_t prefix_token_length = config.prefix_token_length;
    uint32_t block_alignment = config.block_alignment;

    log_main.info("Hot-reload: Updating routing config on all shards (max_routes={}, ttl={}s, drain_timeout={}s, prefix_affinity={}, prefix_len={})",
                  max_routes, ttl_seconds.count(), drain_timeout.count(), prefix_affinity_enabled, prefix_token_length);

    // Broadcast to all shards using Seastar's async message passing
    return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count),
        [max_routes, ttl_seconds, drain_timeout, prefix_affinity_enabled, prefix_token_length, block_alignment](unsigned shard_id) {
            return seastar::smp::submit_to(shard_id, [max_routes, ttl_seconds, drain_timeout, prefix_affinity_enabled, prefix_token_length, block_alignment] {
                local_max_routes = max_routes;
                local_ttl_seconds = ttl_seconds;
                local_backend_drain_timeout = drain_timeout;
                local_prefix_affinity_enabled = prefix_affinity_enabled;
                local_prefix_token_length = prefix_token_length;
                local_block_alignment = block_alignment;
                return seastar::make_ready_future<>();
            });
        });
}

seastar::future<> RouterService::drain_backend_global(BackendId id) {
    // Check if backend exists and get its address for logging
    auto it = local_backends.find(id);
    if (it == local_backends.end()) {
        log_router.warn("Cannot drain backend {}: not found", id);
        return seastar::make_ready_future<>();
    }

    log_router.info("Starting drain for backend {} (timeout: {}s)", id, _config.backend_drain_timeout.count());

    auto now = std::chrono::steady_clock::now();

    // Broadcast draining state to all shards
    return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count), [id, now](unsigned shard_id) {
        return seastar::smp::submit_to(shard_id, [id, now] {
            auto it = local_backends.find(id);
            if (it != local_backends.end()) {
                it->second.is_draining = true;
                it->second.drain_start_time = now;
            }
            return seastar::make_ready_future<>();
        });
    });
}

void RouterService::set_pool_cleanup_callback(PoolCleanupCallback callback) {
    _pool_cleanup_callback = std::move(callback);
}

void RouterService::start_draining_reaper() {
    // Run the draining reaper every 5 seconds
    _draining_reaper_timer.set_callback([this] { run_draining_reaper(); });
    _draining_reaper_timer.arm_periodic(std::chrono::seconds(5));
    log_main.info("Backend draining reaper started (interval: 5s, timeout: {}s)",
                  _config.backend_drain_timeout.count());
}

void RouterService::stop_draining_reaper() {
    _draining_reaper_timer.cancel();
    log_main.info("Backend draining reaper stopped");
}

void RouterService::run_draining_reaper() {
    auto now = std::chrono::steady_clock::now();

    // Find backends on shard 0 that have been draining longer than the timeout
    std::vector<std::pair<BackendId, seastar::socket_address>> to_remove;

    for (const auto& [id, info] : local_backends) {
        if (info.is_draining) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - info.drain_start_time);
            if (elapsed >= local_backend_drain_timeout) {
                to_remove.emplace_back(id, info.addr);
            }
        }
    }

    // Remove each expired draining backend
    for (const auto& [id, addr] : to_remove) {
        log_router.info("Backend {} drain timeout expired, removing from all shards", id);

        // Call pool cleanup callback before removing (if set)
        if (_pool_cleanup_callback) {
            _pool_cleanup_callback(addr);
        }

        // Fire-and-forget the unregister (runs asynchronously)
        (void)unregister_backend_global(id);
    }
}

seastar::future<> RouterService::remove_routes_for_backend(BackendId b_id) {
    // Remove all REMOTE routes pointing to this backend
    // This is called when a cluster peer fails and we need to prune orphaned routes
    if (!local_tree) {
        return seastar::make_ready_future<>();
    }

    size_t removed = local_tree->remove_routes_by_backend(b_id, RouteOrigin::REMOTE);
    if (removed > 0) {
        stats_cluster_routes_pruned += removed;
        log_router.info("Shard {}: Pruned {} orphaned routes for failed peer backend {}",
                        seastar::this_shard_id(), removed, b_id);
    }

    return seastar::make_ready_future<>();
}

} // namespace ranvier
