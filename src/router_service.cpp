#include "router_service.hpp"
#include "gossip_service.hpp"
#include "logging.hpp"
#include "metrics_service.hpp"
#include "node_slab.hpp"

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
#include <sstream>

namespace ranvier {

// Backend info including weight, priority, and draining state
struct BackendInfo {
    seastar::socket_address addr;
    uint32_t weight = 100;
    uint32_t priority = 0;
    bool is_draining = false;
    std::chrono::steady_clock::time_point drain_start_time;
};

// ============================================================================
// ShardLocalState: Unified Per-Shard State
// ============================================================================
//
// Encapsulates all thread-local state for a single shard. This provides:
//   1. Clear lifecycle management - init() in start(), reset() in stop()
//   2. Guaranteed destruction order - tree before slab
//   3. Easy testing - reset_for_testing() clears all state
//   4. Configuration hot-reload via update_config()
//
// Each shard has its own instance, accessed via g_shard_state.
// This follows Seastar's shared-nothing architecture (no locks needed).
//
struct ShardLocalState {
    // ========================================================================
    // Tree State (destruction order: tree before slab)
    // ========================================================================
    std::unique_ptr<NodeSlab> node_slab;  // Destroyed last
    std::unique_ptr<RadixTree> tree;       // Destroyed first

    // ========================================================================
    // Backend Tracking (SIMD-accelerated containers for lock-free lookups)
    // ========================================================================
    absl::flat_hash_map<BackendId, BackendInfo> backends;
    std::vector<BackendId> backend_ids;
    absl::flat_hash_set<BackendId> dead_backends;  // Circuit breaker blacklist

    // ========================================================================
    // Statistics Counters
    // ========================================================================
    struct Stats {
        uint64_t cache_hits = 0;
        uint64_t cache_misses = 0;
        uint64_t routes_evicted = 0;
        uint64_t routes_expired = 0;
        uint64_t cluster_routes_pruned = 0;
        uint64_t radix_tree_lookup_hits = 0;
        uint64_t radix_tree_lookup_misses = 0;
        uint64_t prefix_affinity_routes = 0;
        // Compaction stats (cumulative since process start)
        uint64_t compaction_nodes_removed = 0;
        uint64_t compaction_nodes_shrunk = 0;
        uint64_t compaction_bytes_reclaimed = 0;
        uint64_t compaction_runs = 0;

        void reset() {
            cache_hits = 0;
            cache_misses = 0;
            routes_evicted = 0;
            routes_expired = 0;
            cluster_routes_pruned = 0;
            radix_tree_lookup_hits = 0;
            radix_tree_lookup_misses = 0;
            prefix_affinity_routes = 0;
            compaction_nodes_removed = 0;
            compaction_nodes_shrunk = 0;
            compaction_bytes_reclaimed = 0;
            compaction_runs = 0;
        }
    } stats;

    // ========================================================================
    // Configuration (per-shard copy for lock-free access)
    // ========================================================================
    struct Config {
        size_t max_routes = 100000;
        size_t max_route_tokens = 8192;  // Business-level limit on tokens per route
        std::chrono::seconds ttl_seconds{3600};
        std::chrono::seconds backend_drain_timeout{60};
        RoutingConfig::RoutingMode routing_mode = RoutingConfig::RoutingMode::PREFIX;
        size_t prefix_token_length = 128;
        uint32_t block_alignment = 16;
    } config;

    // ========================================================================
    // Random Number Generator (seeded per-shard for deterministic testing)
    // ========================================================================
    std::mt19937 rng;

    // ========================================================================
    // Callbacks (shard-local, set by owning service)
    // ========================================================================
    std::function<void(BackendId)> circuit_cleanup_callback;

    // ========================================================================
    // Lifecycle Methods
    // ========================================================================

    // Initialize shard state with the given routing configuration.
    // Called once per shard during RouterService::start() / initialize_shards().
    void init(const RoutingConfig& cfg) {
        // Initialize tree state with guaranteed destruction order
        node_slab = std::make_unique<NodeSlab>();
        set_node_slab(node_slab.get());
        tree = std::make_unique<RadixTree>(cfg.block_alignment);

        // Copy configuration for lock-free access
        config.max_routes = cfg.max_routes;
        config.max_route_tokens = cfg.max_route_tokens;
        config.ttl_seconds = cfg.ttl_seconds;
        config.backend_drain_timeout = cfg.backend_drain_timeout;
        config.routing_mode = cfg.routing_mode;
        config.prefix_token_length = cfg.prefix_token_length;
        config.block_alignment = cfg.block_alignment;

        // Seed RNG with random device and time for better entropy
        std::random_device rd;
        auto time_seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        rng.seed(rd() ^ static_cast<std::mt19937::result_type>(time_seed));
    }

    // Update configuration on hot-reload (called from update_routing_config)
    void update_config(const RoutingConfig& cfg) {
        config.max_routes = cfg.max_routes;
        config.max_route_tokens = cfg.max_route_tokens;
        config.ttl_seconds = cfg.ttl_seconds;
        config.backend_drain_timeout = cfg.backend_drain_timeout;
        config.routing_mode = cfg.routing_mode;
        config.prefix_token_length = cfg.prefix_token_length;
        config.block_alignment = cfg.block_alignment;
    }

    // Reset all state (for testing or reconfiguration)
    void reset() {
        // Clear tree first (nodes allocated from slab)
        tree.reset();
        set_node_slab(nullptr);
        node_slab.reset();

        // Clear backend tracking
        backends.clear();
        backend_ids.clear();
        dead_backends.clear();

        // Reset statistics
        stats.reset();
    }

    // Reset for unit testing - clears all state and optionally reinitializes
    void reset_for_testing(const RoutingConfig* cfg = nullptr) {
        reset();
        if (cfg) {
            init(*cfg);
        }
    }

    // Destructor ensures proper cleanup order
    ~ShardLocalState() {
        // Explicit destruction order: tree before slab
        tree.reset();
        set_node_slab(nullptr);
        node_slab.reset();
    }
};

// Single thread_local instance per shard
thread_local std::unique_ptr<ShardLocalState> g_shard_state;

// ============================================================================
// Accessor Functions
// ============================================================================

// Get reference to shard state (asserts initialized)
static ShardLocalState& shard_state() {
    assert(g_shard_state && "Shard state not initialized - call init() first");
    return *g_shard_state;
}

// Convenience accessor for RadixTree (returns nullptr if not initialized)
static RadixTree* local_tree() {
    return g_shard_state ? g_shard_state->tree.get() : nullptr;
}

// ============================================================================
// Route Batching Helpers
// ============================================================================

// Apply a batch of remote routes to the local shard's RadixTree.
// This function runs on each shard and processes all routes in the batch.
// Called via smp::submit_to from flush_route_batch().
static void apply_route_batch_to_local_tree(const std::vector<PendingRemoteRoute>& batch) {
    if (!g_shard_state) return;
    auto& state = shard_state();
    RadixTree* tree = state.tree.get();
    if (!tree) return;

    for (const auto& route : batch) {
        // LRU eviction: prefer evicting REMOTE routes first when at capacity
        if (state.config.max_routes > 0) {
            while (tree->route_count() >= state.config.max_routes) {
                if (tree->evict_oldest_remote()) {
                    state.stats.routes_evicted++;
                } else {
                    break;  // No more routes to evict
                }
            }
        }

        // Insert with REMOTE origin (learned from cluster gossip)
        tree->insert(route.tokens, route.backend, RouteOrigin::REMOTE);
    }
}

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

RouterService::RouterService() : RouterService(RoutingConfig{}) {}

RouterService::RouterService(const RoutingConfig& config)
    : RouterService(config, ClusterConfig{}) {}

RouterService::RouterService(const RoutingConfig& routing_config, const ClusterConfig& cluster_config)
    : _config(routing_config), _cluster_config(cluster_config) {
    // Initialize shard 0's ShardLocalState
    g_shard_state = std::make_unique<ShardLocalState>();
    g_shard_state->init(routing_config);

    // Create GossipService if cluster mode is enabled
    if (_cluster_config.enabled) {
        _gossip = std::make_unique<GossipService>(_cluster_config);

        // Set the local backend ID for this node (used when broadcasting DRAINING on shutdown)
        _gossip->set_local_backend_id(_cluster_config.self_backend_id);
        if (_cluster_config.self_backend_id == 0) {
            log_router.warn("cluster.self_backend_id is 0 - graceful shutdown DRAINING "
                           "notifications will not identify this node correctly to peers. "
                           "Set self_backend_id to this node's backend ID for proper cluster drain.");
        }

        // Set up callback to handle incoming route announcements
        _gossip->set_route_learn_callback([this](std::vector<TokenId> tokens, BackendId backend) {
            return learn_route_remote(std::move(tokens), backend);
        });

        // Set up callback to prune routes when a peer fails
        _gossip->set_route_prune_callback([this](BackendId backend) {
            return remove_routes_for_backend(backend);
        });

        // Set up callback to handle node state changes (e.g., DRAINING notifications)
        // When a peer broadcasts DRAINING, set their backend weight to 0 to stop new traffic
        _gossip->set_node_state_callback([this](BackendId backend, NodeState state) {
            return handle_node_state_change(backend, state);
        });

        // Pre-allocate buffer for route batching to avoid reallocations during operation
        _pending_remote_routes.reserve(RouteBatchConfig::MAX_BATCH_SIZE);

        log_router.info("Cluster mode enabled with {} peers", _cluster_config.peers.size());
    }

    // ========================================================================
    // Metrics Registration
    // ========================================================================
    //
    // LIFECYCLE SAFETY:
    // These metrics lambdas access thread-local state (get_node_slab) and
    // member variables ([this] capture). To prevent use-after-free:
    //
    //   1. stop() MUST be called before RouterService destruction
    //   2. stop() calls _metrics.clear() FIRST, deregistering all lambdas
    //   3. Defense-in-depth: get_node_slab() lambdas have null-checks
    //
    // The [this] capture for _routes_dropped_overflow is safe because:
    //   - _metrics.clear() in stop() deregisters before 'this' is destroyed
    //   - Prometheus cannot scrape deregistered metrics
    //
    _metrics.add_group("ranvier", {
        // Note: Metrics access g_shard_state->stats. Null-checks are defense-in-depth;
        // primary safety is _metrics.clear() in stop() before g_shard_state is destroyed.
        seastar::metrics::make_counter("router_cache_hits",
            [] { return g_shard_state ? g_shard_state->stats.cache_hits : 0UL; },
            seastar::metrics::description("Total number of prefix cache hits")),
        seastar::metrics::make_counter("router_cache_misses",
            [] { return g_shard_state ? g_shard_state->stats.cache_misses : 0UL; },
            seastar::metrics::description("Total number of prefix cache misses")),
        seastar::metrics::make_counter("router_routes_evicted",
            [] { return g_shard_state ? g_shard_state->stats.routes_evicted : 0UL; },
            seastar::metrics::description("Total number of routes evicted due to capacity limits")),
        seastar::metrics::make_counter("router_routes_expired",
            [] { return g_shard_state ? g_shard_state->stats.routes_expired : 0UL; },
            seastar::metrics::description("Total number of routes expired due to TTL")),
        seastar::metrics::make_counter("router_cluster_routes_pruned",
            [] { return g_shard_state ? g_shard_state->stats.cluster_routes_pruned : 0UL; },
            seastar::metrics::description("Total number of routes pruned when cluster peer fails")),
        seastar::metrics::make_counter("router_prefix_affinity_routes",
            [] { return g_shard_state ? g_shard_state->stats.prefix_affinity_routes : 0UL; },
            seastar::metrics::description("Total number of requests routed via prefix affinity")),
        seastar::metrics::make_counter("router_routes_dropped_buffer_overflow_total",
            [this] { return _routes_dropped_overflow; },
            seastar::metrics::description("Total routes dropped due to remote route buffer overflow")),

        // ====================================================================
        // Radix Tree Performance Metrics
        // ====================================================================

        // Radix tree lookup hits: when find_node successfully finds a route
        seastar::metrics::make_counter("radix_tree_lookup_hits_total",
            [] { return g_shard_state ? g_shard_state->stats.radix_tree_lookup_hits : 0UL; },
            seastar::metrics::description("Total number of radix tree lookups that found a valid Backend")),

        // Radix tree lookup misses: when find_node fails to find a route
        seastar::metrics::make_counter("radix_tree_lookup_misses_total",
            [] { return g_shard_state ? g_shard_state->stats.radix_tree_lookup_misses : 0UL; },
            seastar::metrics::description("Total number of radix tree lookups that failed to find a route")),

        // Node count by type (Node4, Node16, Node48, Node256) - uses NodeSlab statistics
        // Note: Each lambda includes a null-check on get_node_slab() as defense-in-depth.
        // Primary safety is _metrics.clear() in stop(), but null-checks protect against
        // edge cases where NodeSlab is destroyed before metrics deregistration completes.
        seastar::metrics::make_gauge("radix_tree_node_count",
            seastar::metrics::description("Total number of radix tree nodes allocated (Node4 pool)"),
            {{"node_type", "Node4"}},
            [] {
                NodeSlab* slab = get_node_slab();
                if (!slab) return static_cast<double>(0);
                auto stats = slab->get_stats();
                return static_cast<double>(stats.allocated_nodes[0]);
            }),
        seastar::metrics::make_gauge("radix_tree_node_count",
            seastar::metrics::description("Total number of radix tree nodes allocated (Node16 pool)"),
            {{"node_type", "Node16"}},
            [] {
                NodeSlab* slab = get_node_slab();
                if (!slab) return static_cast<double>(0);
                auto stats = slab->get_stats();
                return static_cast<double>(stats.allocated_nodes[1]);
            }),
        seastar::metrics::make_gauge("radix_tree_node_count",
            seastar::metrics::description("Total number of radix tree nodes allocated (Node48 pool)"),
            {{"node_type", "Node48"}},
            [] {
                NodeSlab* slab = get_node_slab();
                if (!slab) return static_cast<double>(0);
                auto stats = slab->get_stats();
                return static_cast<double>(stats.allocated_nodes[2]);
            }),
        seastar::metrics::make_gauge("radix_tree_node_count",
            seastar::metrics::description("Total number of radix tree nodes allocated (Node256 pool)"),
            {{"node_type", "Node256"}},
            [] {
                NodeSlab* slab = get_node_slab();
                if (!slab) return static_cast<double>(0);
                auto stats = slab->get_stats();
                return static_cast<double>(stats.allocated_nodes[3]);
            }),

        // Slab utilization ratio: used / (used + free) for memory efficiency monitoring
        // A ratio close to 1.0 indicates high memory efficiency (little fragmentation)
        // A low ratio may indicate memory fragmentation or over-allocation
        seastar::metrics::make_gauge("radix_tree_slab_utilization_ratio",
            seastar::metrics::description("Ratio of used to total pre-allocated slab memory (0.0-1.0). Higher values indicate better memory efficiency."),
            [] {
                NodeSlab* slab = get_node_slab();
                if (!slab) return 0.0;
                auto stats = slab->get_stats();
                // Calculate total allocated and total capacity across all pools
                size_t total_allocated = 0;
                size_t total_capacity = 0;
                for (size_t i = 0; i < 4; i++) {
                    total_allocated += stats.allocated_nodes[i];
                    total_capacity += stats.allocated_nodes[i] + stats.free_list_size[i];
                }
                if (total_capacity == 0) return 0.0;
                return static_cast<double>(total_allocated) / static_cast<double>(total_capacity);
            }),
        // Compaction metrics - track memory reclamation from tombstoned nodes
        seastar::metrics::make_counter("radix_tree_compaction_nodes_removed_total",
            [] { return g_shard_state ? g_shard_state->stats.compaction_nodes_removed : 0UL; },
            seastar::metrics::description("Total empty nodes removed and returned to slab free list")),
        seastar::metrics::make_counter("radix_tree_compaction_nodes_shrunk_total",
            [] { return g_shard_state ? g_shard_state->stats.compaction_nodes_shrunk : 0UL; },
            seastar::metrics::description("Total nodes downsized (e.g., Node256 to Node48)")),
        seastar::metrics::make_counter("radix_tree_compaction_bytes_reclaimed_total",
            [] { return g_shard_state ? g_shard_state->stats.compaction_bytes_reclaimed : 0UL; },
            seastar::metrics::description("Total bytes reclaimed by compaction")),
        seastar::metrics::make_counter("radix_tree_compaction_runs_total",
            [] { return g_shard_state ? g_shard_state->stats.compaction_runs : 0UL; },
            seastar::metrics::description("Total number of compaction cycles executed"))
        // Note: radix_tree_average_prefix_skip_length gauge is registered in MetricsService
        // since it aggregates path compression data across all lookups via record_prefix_skip()
    });
}

seastar::future<> RouterService::initialize_shards() {
    // Initialize ShardLocalState on all other shards with the config from shard 0
    // Shard 0 is already initialized in the constructor
    RoutingConfig cfg = _config;  // Copy config for cross-shard transfer

    return seastar::parallel_for_each(boost::irange(1u, seastar::smp::count),
        [cfg](unsigned shard_id) {
            return seastar::smp::submit_to(shard_id, [cfg] {
                // Initialize ShardLocalState with unified init() method
                g_shard_state = std::make_unique<ShardLocalState>();
                g_shard_state->init(cfg);
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

seastar::future<> RouterService::stop() {
    // ==========================================================================
    // CRITICAL: Metrics Deregistration Must Happen First
    // ==========================================================================
    //
    // Metrics lambdas capture 'this' (for _routes_dropped_overflow) and access
    // thread-local state via get_node_slab(). If Prometheus scrapes during or
    // after shutdown:
    //   - [this] captures would access destroyed RouterService members
    //   - get_node_slab() could return dangling pointer to destroyed NodeSlab
    //
    // By clearing metrics first, we guarantee no metric collection can occur
    // during the rest of the shutdown sequence. The lambdas are deregistered
    // and will no longer be called by the metrics subsystem.
    //
    // Note: The get_node_slab() lambdas have null-checks as defense-in-depth,
    // but deregistration is the primary safety mechanism.
    //
    log_main.info("RouterService stopping: deregistering metrics");
    _metrics.clear();  // Deregister all metrics lambdas - prevents use-after-free

    // Now safe to stop timers (no metrics can observe partially-destroyed state)
    stop_ttl_timer();
    stop_draining_reaper();

    // Stop gossip last (returns future, may have pending operations)
    return stop_gossip().then([] {
        log_main.info("RouterService stopped");
        return seastar::make_ready_future<>();
    });
}

void RouterService::run_ttl_cleanup() {
    auto cutoff = std::chrono::steady_clock::now() - shard_state().config.ttl_seconds;

    // Run cleanup and compaction on all shards
    (void)seastar::parallel_for_each(boost::irange(0u, seastar::smp::count), [cutoff](unsigned shard_id) {
        return seastar::smp::submit_to(shard_id, [cutoff] {
            if (!g_shard_state) return seastar::make_ready_future<>();
            auto& state = shard_state();
            RadixTree* tree = state.tree.get();
            if (tree) {
                // Phase 1: Expire old routes (marks leaves as empty)
                size_t removed = tree->remove_expired(cutoff);
                if (removed > 0) {
                    state.stats.routes_expired += removed;
                    log_main.debug("Shard {}: Expired {} routes", seastar::this_shard_id(), removed);
                }

                // Phase 2: Compact tree (reclaims empty nodes, shrinks oversized nodes)
                auto compact_stats = tree->compact();
                state.stats.compaction_runs++;
                state.stats.compaction_nodes_removed += compact_stats.nodes_removed;
                state.stats.compaction_nodes_shrunk += compact_stats.nodes_shrunk;
                state.stats.compaction_bytes_reclaimed += compact_stats.bytes_reclaimed;
                if (compact_stats.nodes_removed > 0 || compact_stats.nodes_shrunk > 0) {
                    log_main.debug("Shard {}: Compaction removed {} nodes, shrunk {} nodes, reclaimed {} bytes",
                                   seastar::this_shard_id(),
                                   compact_stats.nodes_removed,
                                   compact_stats.nodes_shrunk,
                                   compact_stats.bytes_reclaimed);
                }
            }
            return seastar::make_ready_future<>();
        });
    });
}

std::optional<BackendId> RouterService::lookup(const std::vector<int32_t>& tokens,
                                                 const std::string& request_id) {
    if (!g_shard_state) return std::nullopt;
    auto& state = shard_state();
    RadixTree* tree = state.tree.get();
    if (!tree) return std::nullopt;

    // Use instrumented lookup to track prefix skip lengths for path compression metrics
    auto lookup_result = tree->lookup_instrumented(tokens);
    auto result = lookup_result.backend;

    // Record radix tree lookup hit/miss for performance metrics
    if (result.has_value()) {
        state.stats.radix_tree_lookup_hits++;
        if (g_metrics) {
            metrics().record_radix_tree_lookup_hit();
            // Record prefix skip length for path compression efficiency tracking
            // We record the total tokens skipped via path compression during this lookup.
            // The MetricsService tracks the running average across all lookups.
            if (lookup_result.prefix_tokens_skipped > 0) {
                metrics().record_prefix_skip(lookup_result.prefix_tokens_skipped);
            }
        }
    } else {
        state.stats.radix_tree_lookup_misses++;
        if (g_metrics) {
            metrics().record_radix_tree_lookup_miss();
        }
    }

    // Circuit breaker: if cache hit points to dead backend, treat as miss
    if (result.has_value()) {
        if (state.dead_backends.contains(result.value())) {
            if (!request_id.empty()) {
                log_router.debug("[{}] Cache hit for dead backend {}, treating as miss",
                                 request_id, result.value());
            }
            state.stats.cache_misses++;
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
        state.stats.cache_hits++;
        // Update MetricsService for ranvier_cache_hit_ratio gauge
        if (g_metrics) {
            metrics().record_cache_hit();
        }
    } else {
        if (!request_id.empty()) {
            log_router.debug("[{}] Cache miss for {} tokens", request_id, tokens.size());
        }
        state.stats.cache_misses++;
        // Update MetricsService for ranvier_cache_hit_ratio gauge
        if (g_metrics) {
            metrics().record_cache_miss();
        }
    }

    return result;
}

RouteResult RouterService::route_request(const std::vector<int32_t>& tokens,
                                         const std::string& request_id) {
    RouteResult result;

    // ==========================================================================
    // Fail-Open Mode Check
    // ==========================================================================
    // When fail-open mode is active (quorum lost + fail_open_on_quorum_loss=true),
    // bypass normal routing and use random selection to known-healthy backends.
    // This prioritizes availability over cache affinity during split-brain.
    //
    // Note: _gossip is only valid on shard 0. For other shards, we check if
    // gossip exists and defer to its is_fail_open_mode() which returns false
    // on non-shard-0 (as quorum state is only maintained on shard 0).
    if (_gossip && _gossip->is_fail_open_mode()) {
        result.routing_mode = "random";
        result.cache_hit = false;
        auto random_id = get_random_backend();

        if (random_id.has_value()) {
            result.backend_id = random_id.value();
            if (!request_id.empty()) {
                log_router.debug("[{}] Fail-open routing: {} tokens -> backend {} (split-brain active)",
                                 request_id, tokens.size(), random_id.value());
            }
        } else {
            result.error_message = "No backends registered (fail-open mode)";
        }
        return result;
    }

    // Use shard-local routing mode (hot-reloadable via update_routing_config)
    auto routing_mode = g_shard_state ? shard_state().config.routing_mode
                                      : RoutingConfig::RoutingMode::PREFIX;

    if (routing_mode == RoutingConfig::RoutingMode::PREFIX) {
        // PREFIX mode: ART lookup + consistent hash fallback (learns routes)
        result.routing_mode = "prefix";
        auto affinity_backend = get_backend_for_prefix(tokens, request_id);

        if (affinity_backend.has_value()) {
            result.backend_id = affinity_backend.value();
            // Note: get_backend_for_prefix internally tracks cache_hit via stats.cache_hits/misses
            // and returns a backend even on cache miss (via hash fallback).
            // For external visibility, we report cache_hit based on whether ART had a hit.
            // Since get_backend_for_prefix always returns a backend when backends exist,
            // cache_hit here means we found the route in ART (not hash fallback).
            // The internal stats.cache_hits counter was already incremented appropriately.
            result.cache_hit = true;  // Prefix mode always has affinity (ART or hash)
        } else {
            result.error_message = "No backends registered";
        }
    } else if (routing_mode == RoutingConfig::RoutingMode::HASH) {
        // HASH mode: consistent hash only (no ART, no learning)
        // Use this to measure baseline hash performance vs ART
        result.routing_mode = "hash";
        auto hash_backend = get_backend_by_hash(tokens, request_id);

        if (hash_backend.has_value()) {
            result.backend_id = hash_backend.value();
            result.cache_hit = true;  // Hash always provides affinity
        } else {
            result.error_message = "No backends registered";
        }
    } else {
        // RANDOM mode: weighted random selection (baseline, no affinity)
        result.routing_mode = "random";
        auto random_id = get_random_backend();

        if (random_id.has_value()) {
            result.backend_id = random_id.value();
            result.cache_hit = false;  // Random never uses cache
        } else {
            result.error_message = "No backends registered";
        }
    }

    return result;
}

std::optional<seastar::socket_address> RouterService::get_backend_address(BackendId id) {
    if (!g_shard_state) return std::nullopt;
    auto& state = shard_state();
    auto it = state.backends.find(id);
    if (it != state.backends.end()) {
        return it->second.addr;
    }
    return std::nullopt;
}

std::optional<BackendId> RouterService::get_random_backend() {
    if (!g_shard_state) return std::nullopt;
    auto& state = shard_state();

    if (state.backend_ids.empty()) {
        return std::nullopt;
    }

    // Collect live backends grouped by priority
    // Priority 0 = highest, backends with lower priority number are tried first
    std::map<uint32_t, std::vector<std::pair<BackendId, uint32_t>>> priority_groups;

    for (BackendId id : state.backend_ids) {
        if (state.dead_backends.contains(id)) {
            continue;  // Skip dead backends
        }
        auto it = state.backends.find(id);
        if (it == state.backends.end()) {
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
    uint64_t roll = dist(state.rng);

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
    if (!g_shard_state) return std::nullopt;
    auto& state = shard_state();

    if (state.backend_ids.empty()) {
        return std::nullopt;
    }

    // Collect live backends (not dead, not draining)
    std::vector<BackendId> live_backends;
    for (BackendId id : state.backend_ids) {
        if (state.dead_backends.contains(id)) {
            continue;  // Skip dead backends
        }
        auto it = state.backends.find(id);
        if (it == state.backends.end()) {
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
    size_t prefix_len = std::min(tokens.size(), state.config.prefix_token_length);
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
    RadixTree* tree = state.tree.get();
    if (tree) {
        std::span<const TokenId> token_span(tokens.data(), tokens.size());
        auto art_result = tree->lookup(token_span);

        if (art_result.has_value()) {
            BackendId art_backend = art_result.value();

            // Verify the backend is still live
            if (std::find(live_backends.begin(), live_backends.end(), art_backend) != live_backends.end()) {
                // ART cache hit - route to learned backend
                if (!request_id.empty()) {
                    log_router.debug("[{}] Prefix affinity (ART hit): {} tokens -> backend {}",
                                     request_id, tokens.size(), art_backend);
                }
                state.stats.cache_hits++;
                state.stats.prefix_affinity_routes++;
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
    uint64_t prefix_hash = hash_prefix(tokens.data(), prefix_len, state.config.block_alignment);
    size_t index = prefix_hash % live_backends.size();
    BackendId selected = live_backends[index];

    if (!request_id.empty()) {
        log_router.debug("[{}] Prefix affinity (hash): {} tokens, hash={}, index={}/{} -> backend {}",
                         request_id, prefix_len, prefix_hash, index, live_backends.size(), selected);
    }

    // This is a cache miss - the route will be learned after successful response
    state.stats.cache_misses++;
    state.stats.prefix_affinity_routes++;
    if (g_metrics) {
        metrics().record_cache_miss();
    }

    return selected;
}

std::optional<BackendId> RouterService::get_backend_by_hash(const std::vector<int32_t>& tokens,
                                                             const std::string& request_id) {
    // HASH-ONLY mode: consistent hashing without ART lookup or learning
    // This provides a baseline to measure ART's added value:
    // - Same prefix → same backend (deterministic)
    // - Different suffix on same prefix → likely different backend (no LPM)
    if (!g_shard_state) return std::nullopt;
    auto& state = shard_state();

    if (state.backend_ids.empty()) {
        return std::nullopt;
    }

    // Collect live backends (not dead, not draining)
    std::vector<BackendId> live_backends;
    for (BackendId id : state.backend_ids) {
        if (state.dead_backends.contains(id)) {
            continue;
        }
        auto it = state.backends.find(id);
        if (it == state.backends.end()) {
            continue;
        }
        if (it->second.is_draining) {
            continue;
        }
        live_backends.push_back(id);
    }

    if (live_backends.empty()) {
        return std::nullopt;
    }

    // Sort for deterministic ordering across shards
    std::sort(live_backends.begin(), live_backends.end());

    // Extract prefix (first N tokens)
    size_t prefix_len = std::min(tokens.size(), state.config.prefix_token_length);
    if (prefix_len == 0) {
        return live_backends[0];
    }

    // Consistent hash on prefix tokens
    uint64_t prefix_hash = hash_prefix(tokens.data(), prefix_len, state.config.block_alignment);
    size_t index = prefix_hash % live_backends.size();
    BackendId selected = live_backends[index];

    if (!request_id.empty()) {
        log_router.debug("[{}] Hash routing: {} tokens, hash={}, index={}/{} -> backend {}",
                         request_id, prefix_len, prefix_hash, index, live_backends.size(), selected);
    }

    // No ART involvement - no cache_hit/miss tracking for stats
    // (hash mode is for measuring baseline, not production metrics)
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

    // ========================================================================
    // Business-Layer Token Count Validation
    // ========================================================================
    // Validate token count AFTER truncation to enforce business-level limits.
    // This check belongs in RouterService (business layer), not persistence.
    // The persistence layer only handles serialization and storage.
    if (_config.max_route_tokens > 0 && tokens.size() > _config.max_route_tokens) {
        log_router.warn("Route rejected: {} tokens exceeds limit {} (backend={}, request={})",
                        tokens.size(), _config.max_route_tokens, backend,
                        request_id.empty() ? "N/A" : request_id);
        return seastar::make_ready_future<>();
    }

    // Log route learning with request_id on shard 0 before broadcasting
    if (!request_id.empty()) {
        log_router.info("[{}] Learning route: {} tokens (prefix) -> backend {}",
                        request_id, tokens.size(), backend);
    }

    // Broadcast to cluster peers if gossip is enabled
    // NOTE: GossipService only exists on shard 0, so we must submit_to(0) to avoid
    // cross-shard access violations (the gate holder would be created on the wrong shard)
    seastar::future<> gossip_future = seastar::make_ready_future<>();
    if (_gossip) {
        // Copy tokens for gossip broadcast (tokens will be moved for shard broadcast below)
        auto tokens_copy = tokens;
        gossip_future = seastar::smp::submit_to(0, [this, tokens_copy = std::move(tokens_copy), backend] {
            if (_gossip->is_enabled()) {
                return _gossip->broadcast_route(tokens_copy, backend);
            }
            return seastar::make_ready_future<>();
        });
    }

    // Broadcast to all local shards with LOCAL origin
    auto shard_future = seastar::do_with(std::move(tokens), [backend](std::vector<int32_t>& shared_tokens) {
        return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count), [backend, &shared_tokens] (unsigned shard_id) {
            return seastar::smp::submit_to(shard_id, [backend, &shared_tokens] {
                // IMPORTANT: Force a fresh allocation on this shard.
                // shared_tokens is held by do_with on shard 0. If we captured it by value,
                // the copy's heap would be allocated on shard 0, violating Seastar's
                // shared-nothing model and causing crashes on free.
                std::vector<int32_t> tokens(shared_tokens.begin(), shared_tokens.end());

                if (!g_shard_state) return seastar::make_ready_future<>();
                auto& state = shard_state();
                RadixTree* tree = state.tree.get();
                if (!tree) return seastar::make_ready_future<>();

                // LRU eviction: if at capacity, evict oldest routes first
                if (state.config.max_routes > 0) {
                    while (tree->route_count() >= state.config.max_routes) {
                        if (tree->evict_oldest()) {
                            state.stats.routes_evicted++;
                        } else {
                            break;  // No more routes to evict
                        }
                    }
                }

                // Insert with LOCAL origin (direct request on this node)
                tree->insert(tokens, backend, RouteOrigin::LOCAL);
                return seastar::make_ready_future<>();
            });
        });
    });

    // Wait for both gossip broadcast and shard updates
    return seastar::when_all_succeed(std::move(gossip_future), std::move(shard_future)).discard_result();
}

// ============================================================================
// Route Batching Implementation
// ============================================================================
//
// Remote routes arrive via GossipService on shard 0. Instead of immediately
// broadcasting each route to all shards (which causes an "SMP storm" with high
// ingestion rates), we batch them and flush periodically or when full.
//
// This reduces cross-core message traffic from O(routes × shards) to O(batches × shards).

seastar::future<> RouterService::learn_route_remote(std::vector<int32_t> tokens, BackendId backend) {
    // ========================================================================
    // Business-Layer Token Count Validation (Remote Routes)
    // ========================================================================
    // Validate remote routes before buffering. Remote peers should also enforce
    // this limit, but we validate defensively to reject malformed gossip messages.
    if (_config.max_route_tokens > 0 && tokens.size() > _config.max_route_tokens) {
        log_router.warn("Remote route rejected: {} tokens exceeds limit {} (backend={})",
                        tokens.size(), _config.max_route_tokens, backend);
        return seastar::make_ready_future<>();
    }

    log_router.debug("Buffering remote route: {} tokens -> backend {}", tokens.size(), backend);

    // Enforce hard buffer limit to prevent OOM from gossip flooding
    // Strategy: batch-drop oldest routes to amortize O(n) erase cost
    // We drop OVERFLOW_DROP_COUNT routes at once rather than one per insert
    if (_pending_remote_routes.size() >= RouteBatchConfig::MAX_BUFFER_SIZE) {
        // Calculate how many to drop (at least 1, up to OVERFLOW_DROP_COUNT)
        size_t drop_count = std::min(RouteBatchConfig::OVERFLOW_DROP_COUNT,
                                     _pending_remote_routes.size());
        _pending_remote_routes.erase(_pending_remote_routes.begin(),
                                     _pending_remote_routes.begin() + static_cast<ptrdiff_t>(drop_count));
        _routes_dropped_overflow += drop_count;

        // Log warning on each batch drop (already rate-limited by drop batching)
        log_router.warn("Route buffer overflow, dropped {} oldest routes (total dropped: {})",
                       drop_count, _routes_dropped_overflow);
    }

    // IMPORTANT: Force a fresh allocation on the local shard.
    // The tokens parameter may have been moved from another shard (via gossip broadcast).
    // If we just move them, the heap memory stays on the source shard, and when we
    // try to free them later (in flush_route_batch), Seastar's per-shard allocator
    // will reject the free() call with "invalid pointer".
    // By copying to a new vector, we allocate on THIS shard, making cleanup safe.
    _pending_remote_routes.push_back(PendingRemoteRoute{
        std::vector<TokenId>(tokens.begin(), tokens.end()),  // Force local allocation
        backend
    });

    // Flush immediately if buffer is full (don't wait for timer)
    if (_pending_remote_routes.size() >= RouteBatchConfig::MAX_BATCH_SIZE) {
        log_router.debug("Batch buffer full ({} routes), triggering immediate flush",
                         _pending_remote_routes.size());
        return flush_route_batch();
    }

    return seastar::make_ready_future<>();
}

seastar::future<> RouterService::start_gossip() {
    if (!_gossip) {
        return seastar::make_ready_future<>();
    }

    start_batch_flush_timer();
    return _gossip->start();
}

seastar::future<> RouterService::stop_gossip() {
    if (!_gossip) {
        return seastar::make_ready_future<>();
    }

    // Shutdown sequence (order matters for correctness):
    // 1. Cancel timer - prevents new timer-triggered flushes
    // 2. Stop gossip - prevents new routes from arriving
    // 3. Flush remaining - ensures no buffered routes are lost
    _batch_flush_timer.cancel();

    return _gossip->stop().then([this] {
        log_router.info("Gossip stopped, flushing remaining route batch");
        return flush_route_batch();
    });
}

void RouterService::start_batch_flush_timer() {
    _batch_flush_timer.set_callback([this] {
        // Timer callbacks can't return futures, so handle errors inline
        (void)flush_route_batch().handle_exception([](std::exception_ptr ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                log_router.error("Route batch flush failed: {}", e.what());
            }
        });
    });

    _batch_flush_timer.arm_periodic(RouteBatchConfig::FLUSH_INTERVAL);
    log_router.info("Route batch flush timer started (interval: {}ms, max_batch: {}, max_buffer: {})",
                    RouteBatchConfig::FLUSH_INTERVAL.count(),
                    RouteBatchConfig::MAX_BATCH_SIZE,
                    RouteBatchConfig::MAX_BUFFER_SIZE);
}

seastar::future<> RouterService::flush_route_batch() {
    if (_pending_remote_routes.empty()) {
        return seastar::make_ready_future<>();
    }

    // Atomically take ownership of pending routes, leaving buffer empty for new arrivals
    // Note: C++ guarantees moved-from std::vector is empty
    auto batch = std::move(_pending_remote_routes);

    log_router.debug("Broadcasting batch of {} routes to {} shards",
                     batch.size(), seastar::smp::count);

    // Send ONE message per shard containing the entire batch.
    //
    // PERFORMANCE NOTE: Each shard receives a COPY of the batch. The copy MUST happen
    // inside the lambda (on the target shard) to satisfy Seastar's shared-nothing model.
    // If we captured by value, the heap would be allocated on shard 0, and freeing it
    // on other shards would crash with "invalid pointer".
    return seastar::do_with(std::move(batch), [](std::vector<PendingRemoteRoute>& shared_batch) {
        return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count),
            [&shared_batch](unsigned shard_id) {
                return seastar::smp::submit_to(shard_id, [&shared_batch] {
                    // Force fresh allocation on this shard
                    std::vector<PendingRemoteRoute> batch(shared_batch.begin(), shared_batch.end());
                    apply_route_batch_to_local_tree(batch);
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
                if (!g_shard_state) return seastar::make_ready_future<>();
                auto& state = shard_state();
                state.backends[id] = BackendInfo{addr, weight, priority};

                // Update vector (check for duplicates)
                bool exists = false;
                for (auto existing : state.backend_ids) {
                    if (existing == id) { exists = true; break; }
                }
                if (!exists) {
                    state.backend_ids.push_back(id);
                }

                return seastar::make_ready_future<>();
            });
        });
    });
}

seastar::future<> RouterService::unregister_backend_global(BackendId id) {
    return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count), [id] (unsigned shard_id) {
        return seastar::smp::submit_to(shard_id, [id] {
            if (!g_shard_state) return seastar::make_ready_future<>();
            auto& state = shard_state();

            // Remove from backends map
            state.backends.erase(id);

            // Remove from backend IDs vector
            auto it = std::find(state.backend_ids.begin(), state.backend_ids.end(), id);
            if (it != state.backend_ids.end()) {
                state.backend_ids.erase(it);
            }

            // Also remove from dead backends set if present
            state.dead_backends.erase(id);

            // Clean up circuit breaker entry (Rule #4: bounded container cleanup)
            if (state.circuit_cleanup_callback) {
                state.circuit_cleanup_callback(id);
                log_router.debug("Shard {}: Cleaned up circuit for deregistered backend {}",
                                 seastar::this_shard_id(), id);
            }

            return seastar::make_ready_future<>();
        });
    });
}

std::vector<BackendId> RouterService::get_all_backend_ids() const {
    if (!g_shard_state) return {};
    return shard_state().backend_ids;
}

std::vector<RouterService::BackendState> RouterService::get_all_backend_states() const {
    if (!g_shard_state) return {};
    auto& shard = shard_state();
    std::vector<BackendState> result;
    result.reserve(shard.backends.size());

    for (const auto& [id, info] : shard.backends) {
        BackendState bs;
        bs.id = id;

        // Extract address and port from socket_address
        auto addr = info.addr;
        std::ostringstream oss;
        oss << addr;
        std::string addr_str = oss.str();

        // Parse address:port format
        // Handle both IPv4 (192.168.1.1:8080) and IPv6 ([::1]:8080) formats
        // Seastar formats IPv6 as [addr]:port
        if (!addr_str.empty() && addr_str.back() >= '0' && addr_str.back() <= '9') {
            auto colon_pos = addr_str.find_last_of(':');
            // For IPv6, the colon before port comes after the closing bracket
            // For IPv4, it's just the last colon
            if (colon_pos != std::string::npos && colon_pos > 0) {
                // Verify this is the port separator, not part of IPv6 address
                bool is_port_separator = (addr_str[colon_pos - 1] == ']') ||  // IPv6: [::1]:8080
                                         (addr_str.find('[') == std::string::npos);  // IPv4: no brackets
                if (is_port_separator) {
                    bs.address = addr_str.substr(0, colon_pos);
                    try {
                        bs.port = static_cast<uint16_t>(std::stoi(addr_str.substr(colon_pos + 1)));
                    } catch (const std::exception&) {
                        bs.port = 0;  // Failed to parse port
                    }
                } else {
                    bs.address = addr_str;
                    bs.port = 0;
                }
            } else {
                bs.address = addr_str;
                bs.port = 0;
            }
        } else {
            bs.address = addr_str;
            bs.port = 0;
        }

        bs.weight = info.weight;
        bs.priority = info.priority;
        bs.is_draining = info.is_draining;
        bs.is_dead = shard.dead_backends.contains(id);

        if (info.is_draining) {
            // Convert steady_clock to wall-clock time:
            // Calculate elapsed time since drain started, then subtract from current wall time
            auto elapsed = std::chrono::steady_clock::now() - info.drain_start_time;
            auto wall_start = std::chrono::system_clock::now() - elapsed;
            bs.drain_start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                wall_start.time_since_epoch()).count();
        } else {
            bs.drain_start_ms = 0;
        }

        result.push_back(std::move(bs));
    }

    return result;
}

RadixTree::DumpNode RouterService::get_tree_dump() const {
    RadixTree* tree = local_tree();
    if (!tree) {
        return RadixTree::DumpNode{"empty", {}, std::nullopt, "LOCAL", 0, {}};
    }
    return tree->dump();
}

std::optional<RadixTree::DumpNode> RouterService::get_tree_dump_with_prefix(const std::vector<TokenId>& prefix) const {
    RadixTree* tree = local_tree();
    if (!tree) {
        return std::nullopt;
    }
    return tree->dump_with_prefix(prefix);
}

seastar::future<> RouterService::set_backend_status_global(BackendId id, bool is_alive) {
    // Check local state (Core 0) to deduplicate logs
    if (!g_shard_state) return seastar::make_ready_future<>();
    bool is_currently_marked_dead = shard_state().dead_backends.contains(id);

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
            if (!g_shard_state) return seastar::make_ready_future<>();
            auto& state = shard_state();
            if (is_alive) {
                state.dead_backends.erase(id);
            } else {
                state.dead_backends.insert(id);
            }
            return seastar::make_ready_future<>();
        });
    });
}

seastar::future<> RouterService::update_routing_config(const RoutingConfig& config) {
    // Update local config on shard 0
    _config = config;

    const char* mode_str = config.routing_mode == RoutingConfig::RoutingMode::PREFIX ? "prefix" :
                           config.routing_mode == RoutingConfig::RoutingMode::HASH ? "hash" : "random";
    log_main.info("Hot-reload: Updating routing config on all shards (max_routes={}, ttl={}s, drain_timeout={}s, mode={}, prefix_len={})",
                  config.max_routes, config.ttl_seconds.count(), config.backend_drain_timeout.count(),
                  mode_str, config.prefix_token_length);

    // Broadcast to all shards using Seastar's async message passing
    // Copy config for cross-shard transfer
    RoutingConfig cfg = config;
    return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count),
        [cfg](unsigned shard_id) {
            return seastar::smp::submit_to(shard_id, [cfg] {
                if (!g_shard_state) return seastar::make_ready_future<>();
                shard_state().update_config(cfg);
                return seastar::make_ready_future<>();
            });
        });
}

seastar::future<> RouterService::drain_backend_global(BackendId id) {
    // Check if backend exists and get its address for logging
    if (!g_shard_state) return seastar::make_ready_future<>();
    auto& shard = shard_state();
    auto it = shard.backends.find(id);
    if (it == shard.backends.end()) {
        log_router.warn("Cannot drain backend {}: not found", id);
        return seastar::make_ready_future<>();
    }

    log_router.info("Starting drain for backend {} (timeout: {}s)", id, _config.backend_drain_timeout.count());

    auto now = std::chrono::steady_clock::now();

    // Broadcast draining state to all shards
    return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count), [id, now](unsigned shard_id) {
        return seastar::smp::submit_to(shard_id, [id, now] {
            if (!g_shard_state) return seastar::make_ready_future<>();
            auto& state = shard_state();
            auto it = state.backends.find(id);
            if (it != state.backends.end()) {
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
    if (!g_shard_state) return;
    auto& shard = shard_state();
    auto now = std::chrono::steady_clock::now();

    // Find backends on shard 0 that have been draining longer than the timeout
    std::vector<std::pair<BackendId, seastar::socket_address>> to_remove;

    for (const auto& [id, info] : shard.backends) {
        if (info.is_draining) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - info.drain_start_time);
            if (elapsed >= shard.config.backend_drain_timeout) {
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
    if (!g_shard_state) return seastar::make_ready_future<>();
    auto& state = shard_state();
    RadixTree* tree = state.tree.get();
    if (!tree) {
        return seastar::make_ready_future<>();
    }

    size_t removed = tree->remove_routes_by_backend(b_id, RouteOrigin::REMOTE);
    if (removed > 0) {
        state.stats.cluster_routes_pruned += removed;
        log_router.info("Shard {}: Pruned {} orphaned routes for failed peer backend {}",
                        seastar::this_shard_id(), removed, b_id);
    }

    return seastar::make_ready_future<>();
}

seastar::future<> RouterService::handle_node_state_change(BackendId backend, NodeState node_state) {
    if (node_state == NodeState::DRAINING) {
        log_router.info("Received DRAINING notification for backend {} - setting weight to 0", backend);

        // Broadcast weight=0 to all shards to stop new traffic to this backend
        // This is more graceful than removing the backend entirely, as it allows
        // in-flight requests to complete while preventing new routing decisions
        return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count),
            [backend](unsigned shard_id) {
                return seastar::smp::submit_to(shard_id, [backend] {
                    if (!g_shard_state) return seastar::make_ready_future<>();
                    auto& state = shard_state();
                    auto it = state.backends.find(backend);
                    if (it != state.backends.end()) {
                        // Set weight to 0 - backend won't be selected for new requests
                        // but existing connections continue working
                        it->second.weight = 0;
                        it->second.is_draining = true;
                        it->second.drain_start_time = std::chrono::steady_clock::now();
                        log_router.debug("Shard {}: Backend {} weight set to 0 (draining)",
                                        seastar::this_shard_id(), backend);
                    }
                    return seastar::make_ready_future<>();
                });
            });
    } else if (node_state == NodeState::ACTIVE) {
        // Node is back online - restore default weight (could be enhanced to store original weight)
        log_router.info("Received ACTIVE notification for backend {} - restoring weight", backend);
        return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count),
            [backend](unsigned shard_id) {
                return seastar::smp::submit_to(shard_id, [backend] {
                    if (!g_shard_state) return seastar::make_ready_future<>();
                    auto& state = shard_state();
                    auto it = state.backends.find(backend);
                    if (it != state.backends.end() && it->second.is_draining) {
                        // Restore default weight
                        it->second.weight = 100;
                        it->second.is_draining = false;
                        log_router.debug("Shard {}: Backend {} restored to active (weight=100)",
                                        seastar::this_shard_id(), backend);
                    }
                    return seastar::make_ready_future<>();
                });
            });
    }

    return seastar::make_ready_future<>();
}

// ============================================================================
// Testing Support
// ============================================================================

void RouterService::reset_shard_state_for_testing(const RoutingConfig* cfg) {
    if (g_shard_state) {
        g_shard_state->reset_for_testing(cfg);
    } else if (cfg) {
        // If no state exists but config provided, create new state
        g_shard_state = std::make_unique<ShardLocalState>();
        g_shard_state->init(*cfg);
    }
}

void RouterService::set_circuit_cleanup_callback(CircuitCleanupCallback callback) {
    if (g_shard_state) {
        g_shard_state->circuit_cleanup_callback = std::move(callback);
    }
}

} // namespace ranvier
