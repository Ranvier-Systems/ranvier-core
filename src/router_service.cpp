#include "router_service.hpp"
#include "gossip_service.hpp"
#include "logging.hpp"
#include "metrics_service.hpp"
#include "node_slab.hpp"
#include "parse_utils.hpp"

#include <seastar/core/coroutine.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/when_all.hh>
#include <boost/range/irange.hpp>
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <random>
#include <sstream>

// ============================================================================
// RouterService: Central Routing Orchestration
// ============================================================================
//
// This file is the #2 load-bearing file in the codebase (per Strategic Assessment).
// It orchestrates all routing decisions and manages cross-shard state propagation.
//
// HARD RULES APPLICABLE TO THIS FILE:
// ───────────────────────────────────────────────────────────────────────────────
//
// Rule #1 (No Blocking on Hot Path):
//   - route_request(), get_backend_for_prefix(), lookup() are HOT PATH functions
//   - They run on every incoming request and MUST be lock-free
//   - Use only thread_local g_shard_state (no cross-shard access on hot path)
//   - All data structures are lock-free (absl::flat_hash_*, RadixTree)
//
// Rule #5 (Timer Ownership):
//   - _ttl_timer, _batch_flush_timer, _draining_reaper_timer capture `this`
//   - stop() MUST cancel all timers before member destruction
//   - Timer callbacks check g_shard_state != nullptr as defense-in-depth
//
// Rule #6 (Metrics Deregistration):
//   - Metrics lambdas capture `this` and access g_shard_state
//   - stop() calls _metrics.clear() FIRST, before any other cleanup
//   - This prevents Prometheus scrapes during shutdown
//
// Rule #14 (Cross-Shard Dispatch):
//   - learn_route_global(), flush_route_batch() broadcast to all shards
//   - Uses foreign_ptr pattern: wrap in foreign_ptr → submit_to → local copy
//   - NEVER move heap-owning types directly across shards
//
// See .dev-context/claude-context.md for full Hard Rules documentation.
// ============================================================================

namespace ranvier {

// Backend info including weight, priority, draining state, and load tracking
struct BackendInfo {
    seastar::socket_address addr;
    uint32_t weight = 100;
    uint32_t priority = 0;
    bool is_draining = false;
    std::chrono::steady_clock::time_point drain_start_time;

    // Load tracking: in-flight requests to this backend (Rule #1: lock-free atomic)
    // Incremented by BackendRequestGuard on construction, decremented on destruction.
    // Uses relaxed ordering - we only need eventual visibility, not strict ordering.
    std::atomic<uint64_t> active_requests{0};

    // Default constructor
    BackendInfo() = default;

    // Constructor for common initialization pattern
    BackendInfo(seastar::socket_address addr_, uint32_t weight_, uint32_t priority_)
        : addr(std::move(addr_))
        , weight(weight_)
        , priority(priority_)
        , is_draining(false)
        , drain_start_time()
        , active_requests(0) {}

    // Copy constructor: atomics aren't copyable, so load the value explicitly
    BackendInfo(const BackendInfo& other)
        : addr(other.addr)
        , weight(other.weight)
        , priority(other.priority)
        , is_draining(other.is_draining)
        , drain_start_time(other.drain_start_time)
        , active_requests(other.active_requests.load(std::memory_order_relaxed)) {}

    // Move constructor: atomics aren't movable, so load the value explicitly
    BackendInfo(BackendInfo&& other) noexcept
        : addr(std::move(other.addr))
        , weight(other.weight)
        , priority(other.priority)
        , is_draining(other.is_draining)
        , drain_start_time(other.drain_start_time)
        , active_requests(other.active_requests.load(std::memory_order_relaxed)) {}

    // Copy assignment
    BackendInfo& operator=(const BackendInfo& other) {
        if (this != &other) {
            addr = other.addr;
            weight = other.weight;
            priority = other.priority;
            is_draining = other.is_draining;
            drain_start_time = other.drain_start_time;
            active_requests.store(other.active_requests.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
        }
        return *this;
    }

    // Move assignment
    BackendInfo& operator=(BackendInfo&& other) noexcept {
        if (this != &other) {
            addr = std::move(other.addr);
            weight = other.weight;
            priority = other.priority;
            is_draining = other.is_draining;
            drain_start_time = other.drain_start_time;
            active_requests.store(other.active_requests.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
        }
        return *this;
    }
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
        uint64_t multi_depth_routes_stored = 0;  // Routes stored via multi-depth learning
        // Compaction stats (cumulative since process start)
        uint64_t compaction_nodes_removed = 0;
        uint64_t compaction_nodes_shrunk = 0;
        uint64_t compaction_bytes_reclaimed = 0;
        uint64_t compaction_runs = 0;
        // Load-aware routing stats
        uint64_t load_aware_fallbacks = 0;       // Times we chose non-preferred backend due to load
        uint64_t cache_miss_due_to_load = 0;     // Routes diverted due to backend load (same as fallbacks)

        void reset() {
            cache_hits = 0;
            cache_misses = 0;
            routes_evicted = 0;
            routes_expired = 0;
            cluster_routes_pruned = 0;
            radix_tree_lookup_hits = 0;
            radix_tree_lookup_misses = 0;
            prefix_affinity_routes = 0;
            multi_depth_routes_stored = 0;
            compaction_nodes_removed = 0;
            compaction_nodes_shrunk = 0;
            compaction_bytes_reclaimed = 0;
            compaction_runs = 0;
            load_aware_fallbacks = 0;
            cache_miss_due_to_load = 0;
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
        // Load-aware routing configuration
        bool load_aware_routing = true;           // Enable load-aware backend selection
        uint64_t queue_depth_threshold = 4;       // Max in-flight before considering alternatives
        uint64_t queue_diff_threshold = 2;        // Min load difference to justify cache miss
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
        // Load-aware routing configuration
        config.load_aware_routing = cfg.load_aware_routing;
        config.queue_depth_threshold = cfg.queue_depth_threshold;
        config.queue_diff_threshold = cfg.queue_diff_threshold;

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
        // Load-aware routing configuration
        config.load_aware_routing = cfg.load_aware_routing;
        config.queue_depth_threshold = cfg.queue_depth_threshold;
        config.queue_diff_threshold = cfg.queue_diff_threshold;
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
// BackendRequestGuard Implementation
// ============================================================================
//
// RAII guard for tracking in-flight requests per backend.
// Rule #1: Uses atomic with relaxed ordering for lock-free operation.
//

BackendRequestGuard::BackendRequestGuard(BackendId id) : _backend_id(id), _active(false) {
    if (!g_shard_state) {
        return;  // Shard not initialized, guard remains inactive
    }

    auto it = g_shard_state->backends.find(id);
    if (it == g_shard_state->backends.end()) {
        return;  // Backend not found, guard remains inactive
    }

    // Increment active requests (Rule #1: relaxed ordering for lock-free counter)
    it->second.active_requests.fetch_add(1, std::memory_order_relaxed);
    _active = true;
}

BackendRequestGuard::~BackendRequestGuard() {
    if (!_active) {
        return;  // Nothing to decrement
    }

    if (!g_shard_state) {
        return;  // Shard state destroyed (shouldn't happen in normal operation)
    }

    auto it = g_shard_state->backends.find(_backend_id);
    if (it == g_shard_state->backends.end()) {
        return;  // Backend was removed while request was in-flight
    }

    // Decrement active requests (Rule #1: relaxed ordering)
    // Guard against underflow: if backend was removed and re-registered while request
    // was in-flight, the counter would be 0 (never incremented by this guard).
    // Decrementing 0 would wrap to UINT64_MAX, causing incorrect load readings.
    // In Seastar's cooperative model, no co_await between load and fetch_sub means
    // this check is safe without additional synchronization.
    uint64_t current = it->second.active_requests.load(std::memory_order_relaxed);
    if (current > 0) {
        it->second.active_requests.fetch_sub(1, std::memory_order_relaxed);
    }
}

BackendRequestGuard::BackendRequestGuard(BackendRequestGuard&& other) noexcept
    : _backend_id(other._backend_id), _active(other._active) {
    // Transfer ownership: source becomes inactive
    other._active = false;
}

BackendRequestGuard& BackendRequestGuard::operator=(BackendRequestGuard&& other) noexcept {
    if (this != &other) {
        // Decrement our current count if active (with underflow guard)
        if (_active && g_shard_state) {
            auto it = g_shard_state->backends.find(_backend_id);
            if (it != g_shard_state->backends.end()) {
                uint64_t current = it->second.active_requests.load(std::memory_order_relaxed);
                if (current > 0) {
                    it->second.active_requests.fetch_sub(1, std::memory_order_relaxed);
                }
            }
        }

        // Take ownership from other
        _backend_id = other._backend_id;
        _active = other._active;
        other._active = false;
    }
    return *this;
}

// ============================================================================
// Load Tracking Helper Functions
// ============================================================================

uint64_t get_backend_load(BackendId id) {
    if (!g_shard_state) {
        return 0;
    }

    auto it = g_shard_state->backends.find(id);
    if (it == g_shard_state->backends.end()) {
        return 0;
    }

    return it->second.active_requests.load(std::memory_order_relaxed);
}

std::pair<BackendId, uint64_t> get_least_loaded_backend(const std::vector<BackendId>& candidates) {
    if (!g_shard_state || candidates.empty()) {
        return {0, UINT64_MAX};
    }

    BackendId best_id = 0;
    uint64_t best_load = UINT64_MAX;

    for (BackendId id : candidates) {
        auto it = g_shard_state->backends.find(id);
        if (it == g_shard_state->backends.end()) {
            continue;  // Skip unknown backends
        }

        // Skip draining or dead backends
        if (it->second.is_draining) {
            continue;
        }
        if (g_shard_state->dead_backends.contains(id)) {
            continue;
        }

        uint64_t load = it->second.active_requests.load(std::memory_order_relaxed);
        if (load < best_load) {
            best_load = load;
            best_id = id;
            // Early exit: 0 is the minimum possible load, can't do better
            if (load == 0) {
                break;
            }
        }
    }

    return {best_id, best_load};
}

// Check if preferred backend is overloaded and find a less-loaded alternative.
// Returns the backend to use: either the preferred backend or a less-loaded alternative.
// Updates stats and metrics if diversion occurs.
//
// Parameters:
//   preferred_id: The backend selected by ART or hash
//   live_backends: List of available backends to consider
//   request_id: For logging (can be empty)
//   source: Description for logging ("ART" or "hash")
//
// Rule #1: Lock-free - all reads use atomic with relaxed ordering
//
// Performance notes:
// - Early exits minimize work in the common case (not overloaded)
// - Config values cached in shard-local state (no cross-shard access)
// - Atomic reads use relaxed ordering (no memory barriers)
static BackendId apply_load_aware_selection(
    BackendId preferred_id,
    const std::vector<BackendId>& live_backends,
    const std::string& request_id,
    const char* source)
{
    // Fast path: check if load-aware routing is enabled before any other work
    if (!g_shard_state || !g_shard_state->config.load_aware_routing) {
        return preferred_id;
    }

    // Cache config values locally to avoid repeated struct access
    const uint64_t queue_threshold = g_shard_state->config.queue_depth_threshold;
    const uint64_t diff_threshold = g_shard_state->config.queue_diff_threshold;

    // Check preferred backend's load (single atomic read)
    uint64_t preferred_load = get_backend_load(preferred_id);
    if (preferred_load <= queue_threshold) {
        return preferred_id;  // Not overloaded - fast path
    }

    // Preferred backend overloaded - find alternative
    auto [least_loaded_id, least_load] = get_least_loaded_backend(live_backends);

    // Validate we found a viable alternative with significant load difference
    if (least_loaded_id == 0 || preferred_load - least_load <= diff_threshold) {
        return preferred_id;
    }

    // Significant difference - divert to less-loaded backend
    g_shard_state->stats.cache_miss_due_to_load++;
    g_shard_state->stats.load_aware_fallbacks++;
    if (g_metrics) {
        metrics().record_load_aware_fallback();
    }

    log_router.debug("[{}] Load-aware routing: {} preferred backend {} has {} in-flight, "
                     "routing to {} with {} in-flight",
                     request_id, source, preferred_id, preferred_load,
                     least_loaded_id, least_load);

    return least_loaded_id;
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

        // Per-shard prune callback registration is deferred to start_gossip()
        // where smp::invoke_on_all can register on every shard safely.

        // Set up callback to handle node state changes (e.g., DRAINING notifications)
        // When a peer broadcasts DRAINING, set their backend weight to 0 to stop new traffic
        // NOTE: No `this` capture - handle_node_state_change is static (see above).
        _gossip->set_node_state_callback([](BackendId backend, NodeState state) {
            return RouterService::handle_node_state_change(backend, state);
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
        seastar::metrics::make_counter("router_multi_depth_routes_stored",
            [] { return g_shard_state ? g_shard_state->stats.multi_depth_routes_stored : 0UL; },
            seastar::metrics::description("Total number of routes stored via multi-depth learning")),
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
            seastar::metrics::description("Total number of compaction cycles executed")),

        // ====================================================================
        // Load-Aware Routing Metrics
        // ====================================================================

        // Counter: requests diverted to less-loaded backends
        seastar::metrics::make_counter("router_load_aware_fallbacks_total",
            [] { return g_shard_state ? g_shard_state->stats.load_aware_fallbacks : 0UL; },
            seastar::metrics::description("Total number of requests diverted to less-loaded backends due to queue depth")),

        // Counter: cache misses accepted for load balancing
        seastar::metrics::make_counter("router_cache_miss_due_to_load_total",
            [] { return g_shard_state ? g_shard_state->stats.cache_miss_due_to_load : 0UL; },
            seastar::metrics::description("Total number of cache misses accepted to avoid routing to overloaded backends"))
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

// ============================================================================
// TTL Timer Management
// ============================================================================
//
// HARD RULE #5 (Timer Ownership):
// Timer callbacks capture `this`, creating a use-after-free risk if the timer
// fires after RouterService destruction begins. Safety is ensured by:
//
//   1. stop() is called before destruction (Seastar service lifecycle)
//   2. stop() calls stop_ttl_timer() which cancels the timer
//   3. Timer cancellation prevents NEW callbacks from being scheduled
//
// IMPORTANT: If a callback is already queued when cancel() is called, it may
// still execute. The run_ttl_cleanup() callback is safe because:
//   - It checks g_shard_state before accessing shard-local data
//   - parallel_for_each + submit_to are async and don't block
//   - No member variables of RouterService are accessed in the callback body
//
// For stricter safety (e.g., if callbacks accessed `this` members), a gate guard
// pattern would be required: callback acquires _timer_gate.hold(), stop() closes
// gate first, then cancels timer.
//
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

    // ==========================================================================
    // Rule #5: Close timer gate before cancelling timers
    // ==========================================================================
    //
    // Timer callbacks capture 'this'. If a callback is already queued when we
    // cancel the timer, it may still execute with a dangling pointer. The gate
    // ensures we wait for any in-flight callbacks to complete before proceeding.
    //
    return _timer_gate.close().then([this] {
        // Gate closed - no in-flight TTL callbacks, safe to cancel timer
        stop_ttl_timer();
        stop_draining_reaper();

        // Stop gossip last (returns future, may have pending operations)
        return stop_gossip();
    }).then([] {
        log_main.info("RouterService stopped");
        return seastar::make_ready_future<>();
    });
}

void RouterService::run_ttl_cleanup() {
    // Rule #5: Acquire gate holder to prevent use-after-free during shutdown.
    // If gate is closed (service stopping), skip cleanup gracefully.
    seastar::gate::holder holder;
    try {
        holder = _timer_gate.hold();
    } catch (const seastar::gate_closed_exception&) {
        // Expected during shutdown - gate closed before timer cancelled
        log_main.debug("TTL cleanup skipped: service is stopping");
        return;
    }

    auto cutoff = std::chrono::steady_clock::now() - shard_state().config.ttl_seconds;

    // Keep gate holder alive for duration of async work via do_with
    (void)seastar::do_with(std::move(holder), [cutoff](seastar::gate::holder&) {
        return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count), [cutoff](unsigned shard_id) {
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
    });
}

// ============================================================================
// lookup() - Prefix Cache Lookup (HOT PATH)
// ============================================================================
//
// HARD RULE #1 (No Blocking on Hot Path):
// This function runs on every incoming request. It MUST remain lock-free:
//   - Uses only thread_local g_shard_state (no cross-shard access)
//   - RadixTree::lookup() is O(key_length), no allocations on hit
//   - dead_backends check uses absl::flat_hash_set (lock-free lookup)
//   - No co_await, no mutex, no blocking I/O
//
// If this function ever blocks, it stalls ALL requests on this shard.
//
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

// ============================================================================
// route_request() - Main Routing Entry Point (HOT PATH)
// ============================================================================
//
// HARD RULE #1 (No Blocking on Hot Path):
// This is the primary entry point for all routing decisions. It MUST be lock-free:
//   - Synchronous execution (no co_await)
//   - Uses only shard-local state via g_shard_state
//   - Delegates to get_backend_for_prefix() or get_random_backend() (both lock-free)
//   - All data structures (backends map, dead_backends set) are SIMD-accelerated
//     absl containers with O(1) average lookup
//
// The function extracts the routing mode from shard-local config (hot-reloadable
// via update_routing_config) and dispatches to the appropriate strategy:
//   - PREFIX: ART lookup + consistent hash fallback (learns routes)
//   - HASH: Consistent hash only (baseline comparison)
//   - RANDOM: Weighted random (no affinity)
//
RouteResult RouterService::route_request(const std::vector<int32_t>& tokens,
                                         const std::string& request_id,
                                         size_t prefix_boundary) {
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
        auto affinity_backend = get_backend_for_prefix(tokens, request_id, prefix_boundary);

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

// ============================================================================
// get_backend_for_prefix() - ART Lookup + Hash Fallback (HOT PATH)
// ============================================================================
//
// HARD RULE #1 (No Blocking on Hot Path):
// This function performs the core prefix-affinity routing. It MUST be lock-free:
//   - RadixTree::lookup() traverses the ART in O(key_length) without locks
//   - Backend filtering uses shard-local vectors and sets (no cross-shard access)
//   - Hash computation (FNV-1a) is pure CPU with no allocations
//   - std::sort on live_backends is O(n log n) but n is typically small (<100)
//
// Two-phase routing strategy:
//   1. ART lookup for longest prefix match (cache hit = route to learned backend)
//   2. Consistent hash fallback for cache miss (deterministic, will be learned)
//
// The prefix_boundary parameter enables system-message-aware routing: when set,
// routes are stored/looked up at the shared prefix boundary rather than the
// full token sequence, improving KV-cache reuse for multi-turn conversations.
//
std::optional<BackendId> RouterService::get_backend_for_prefix(const std::vector<int32_t>& tokens,
                                                                 const std::string& request_id,
                                                                 size_t prefix_boundary) {
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

    // Determine prefix length for hash fallback:
    // 1. If prefix_boundary is valid (> 0 and <= tokens.size()), use it
    //    This ensures consistent hashing across cluster nodes for requests
    //    with the same system message but different user queries.
    // 2. Otherwise, fall back to prefix_token_length (default: 128)
    size_t prefix_len;
    if (prefix_boundary > 0 && prefix_boundary <= tokens.size()) {
        prefix_len = prefix_boundary;
    } else {
        prefix_len = std::min(tokens.size(), state.config.prefix_token_length);
    }
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
                state.stats.cache_hits++;
                state.stats.prefix_affinity_routes++;
                if (g_metrics) {
                    metrics().record_cache_hit();
                }

                // Apply load-aware selection (may divert to less-loaded backend)
                BackendId final_backend = apply_load_aware_selection(
                    art_backend, live_backends, request_id, "ART");

                if (!request_id.empty() && final_backend == art_backend) {
                    log_router.debug("[{}] Prefix affinity (ART hit): {} tokens -> backend {}",
                                     request_id, tokens.size(), art_backend);
                }
                return final_backend;
            }
            // Backend is dead/draining, fall through to hash-based selection
            log_router.debug("[{}] ART backend {} is unavailable, using hash fallback",
                             request_id, art_backend);
        }
    }

    // Step 2: No ART match (or backend unavailable) - use consistent hashing
    // This provides deterministic routing for new prefixes
    // Uses prefix_boundary if provided (system message only) for multi-node consistency
    uint64_t prefix_hash = hash_prefix(tokens.data(), prefix_len, state.config.block_alignment);
    size_t index = prefix_hash % live_backends.size();
    BackendId selected = live_backends[index];

    // This is a cache miss - the route will be learned after successful response
    state.stats.cache_misses++;
    state.stats.prefix_affinity_routes++;
    if (g_metrics) {
        metrics().record_cache_miss();
    }

    // Apply load-aware selection (may divert to less-loaded backend)
    BackendId final_backend = apply_load_aware_selection(
        selected, live_backends, request_id, "hash");

    if (!request_id.empty() && final_backend == selected) {
        log_router.debug("[{}] Prefix affinity (hash): {} tokens, hash={}, index={}/{} -> backend {}",
                         request_id, prefix_len, prefix_hash, index, live_backends.size(), selected);
    }

    return final_backend;
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

// ============================================================================
// learn_route_global() - Cross-Shard Route Propagation
// ============================================================================
//
// HARD RULE #14 (Cross-Shard Dispatch):
// This function broadcasts a learned route to ALL shards using smp::submit_to.
// Cross-shard memory safety is critical:
//
//   - The `tokens` vector is allocated on the calling shard
//   - Moving it directly to another shard would cause wrong-shard deallocation
//   - We use foreign_ptr to wrap copies for safe cross-shard transfer:
//     1. Create std::unique_ptr<vector> with copy of tokens
//     2. Wrap in seastar::make_foreign() - tracks home shard
//     3. Pass foreign_ptr to target shard via submit_to
//     4. Target shard reads from foreign_ptr, creates LOCAL allocation
//     5. When foreign_ptr destructs, cleanup returns to home shard
//
// This pattern is used twice:
//   1. Gossip broadcast to shard 0 (GossipService only exists there)
//   2. Parallel broadcast to all local shards for ART insertion
//
// The function is async (returns future<>) because smp::submit_to is non-blocking
// but the caller may want to wait for propagation to complete.
//
seastar::future<> RouterService::learn_route_global(std::vector<int32_t> tokens, BackendId backend,
                                                       const std::string& request_id,
                                                       size_t prefix_boundary) {
    // Determine effective prefix length for route storage:
    // 1. If prefix_boundary > 0 and valid, use it (truncate to shared prefix like system messages)
    // 2. Otherwise, fall back to config.prefix_token_length (default: 128)
    //
    // prefix_boundary enables prefix-aware routing for multi-turn conversations.
    // When provided, routes are stored at the "shared prefix" boundary (e.g., system
    // message token count), so requests with the same system prompt but different
    // user queries route to the same backend for KV-cache efficiency.
    size_t effective_prefix_len;
    bool used_shared_prefix = false;
    if (prefix_boundary > 0 && prefix_boundary <= tokens.size()) {
        effective_prefix_len = prefix_boundary;
        used_shared_prefix = true;
    } else {
        effective_prefix_len = std::min(tokens.size(), _config.prefix_token_length);
    }

    if (effective_prefix_len < tokens.size()) {
        tokens.resize(effective_prefix_len);
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
        if (used_shared_prefix) {
            log_router.info("[{}] Learning route: {} tokens (shared_prefix) -> backend {}",
                            request_id, tokens.size(), backend);
        } else {
            log_router.info("[{}] Learning route: {} tokens (default_prefix) -> backend {}",
                            request_id, tokens.size(), backend);
        }
    }

    // Broadcast to cluster peers if gossip is enabled
    // NOTE: GossipService only exists on shard 0, so we must submit_to(0) to avoid
    // cross-shard access violations (the gate holder would be created on the wrong shard)
    //
    // MEMORY SAFETY: Use foreign_ptr to safely pass tokens to shard 0.
    // The tokens vector is allocated on the calling shard (could be any shard).
    // foreign_ptr ensures proper deallocation on the source shard.
    seastar::future<> gossip_future = seastar::make_ready_future<>();
    if (_gossip) {
        auto tokens_ptr = std::make_unique<std::vector<int32_t>>(tokens);
        auto foreign_tokens = seastar::make_foreign(std::move(tokens_ptr));

        gossip_future = seastar::smp::submit_to(0, [this, foreign_tokens = std::move(foreign_tokens), backend]() mutable {
            if (_gossip->is_enabled()) {
                // Create local copy on shard 0
                std::vector<int32_t> local_tokens(foreign_tokens->begin(), foreign_tokens->end());
                return _gossip->broadcast_route(local_tokens, backend);
            }
            return seastar::make_ready_future<>();
        });
    }

    // Broadcast to all local shards with LOCAL origin
    //
    // CRITICAL MEMORY SAFETY: Use foreign_ptr to safely pass tokens across shards.
    // Each shard gets a copy wrapped in foreign_ptr. The target shard reads from
    // foreign_ptr and creates LOCAL allocations. When foreign_ptr is destroyed,
    // it returns to shard 0 for proper deallocation.
    auto shard_future = seastar::do_with(std::move(tokens), [backend](std::vector<int32_t>& tokens) {
        return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count), [backend, &tokens] (unsigned shard_id) {
            // Create a copy wrapped in foreign_ptr for this shard
            auto tokens_ptr = std::make_unique<std::vector<int32_t>>(tokens);
            auto foreign_tokens = seastar::make_foreign(std::move(tokens_ptr));

            return seastar::smp::submit_to(shard_id, [backend, foreign_tokens = std::move(foreign_tokens)]() mutable {
                // Force local allocation on THIS shard
                std::vector<int32_t> local_tokens(foreign_tokens->begin(), foreign_tokens->end());

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
                tree->insert(local_tokens, backend, RouteOrigin::LOCAL);
                return seastar::make_ready_future<>();
            });
        });
    });

    // Wait for both gossip broadcast and shard updates
    return seastar::when_all_succeed(std::move(gossip_future), std::move(shard_future)).discard_result();
}

seastar::future<> RouterService::learn_route_global_multi(std::vector<int32_t> tokens, BackendId backend,
                                                           const std::string& request_id,
                                                           const std::vector<size_t>& prefix_boundaries) {
    // Multi-depth route learning: store routes at each provided boundary
    // This enables cache reuse at any conversation depth (Option C)

    if (prefix_boundaries.empty()) {
        // No boundaries provided - fall back to single-depth learning
        return learn_route_global(std::move(tokens), backend, request_id, 0);
    }

    // Filter boundaries: must be > 0 and <= tokens.size()
    std::vector<size_t> valid_boundaries;
    valid_boundaries.reserve(prefix_boundaries.size());
    for (size_t b : prefix_boundaries) {
        if (b > 0 && b <= tokens.size()) {
            valid_boundaries.push_back(b);
        }
    }

    if (valid_boundaries.empty()) {
        // No valid boundaries - fall back to default prefix length
        return learn_route_global(std::move(tokens), backend, request_id, 0);
    }

    // Log multi-depth learning
    if (!request_id.empty()) {
        std::string boundaries_str;
        for (size_t i = 0; i < valid_boundaries.size(); ++i) {
            if (i > 0) boundaries_str += ",";
            boundaries_str += std::to_string(valid_boundaries[i]);
        }
        log_router.info("[{}] Learning multi-depth routes: {} boundaries [{}] -> backend {}",
                        request_id, valid_boundaries.size(), boundaries_str, backend);
    }

    // Record metric for multi-depth routes
    if (g_shard_state) {
        shard_state().stats.multi_depth_routes_stored += valid_boundaries.size();
    }

    // Learn a route at each boundary depth
    std::vector<seastar::future<>> futures;
    futures.reserve(valid_boundaries.size());

    for (size_t boundary : valid_boundaries) {
        // Create a prefix at this boundary
        std::vector<int32_t> prefix(tokens.begin(), tokens.begin() + boundary);

        // Validate token count
        if (_config.max_route_tokens > 0 && prefix.size() > _config.max_route_tokens) {
            continue;  // Skip this boundary if it exceeds the limit
        }

        // Broadcast to all local shards
        futures.push_back(
            seastar::do_with(std::move(prefix), [backend](std::vector<int32_t>& prefix_tokens) {
                return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count), [backend, &prefix_tokens](unsigned shard_id) {
                    auto tokens_ptr = std::make_unique<std::vector<int32_t>>(prefix_tokens);
                    auto foreign_tokens = seastar::make_foreign(std::move(tokens_ptr));

                    return seastar::smp::submit_to(shard_id, [backend, foreign_tokens = std::move(foreign_tokens)]() mutable {
                        std::vector<int32_t> local_tokens(foreign_tokens->begin(), foreign_tokens->end());

                        if (!g_shard_state) return seastar::make_ready_future<>();
                        auto& state = shard_state();
                        RadixTree* tree = state.tree.get();
                        if (!tree) return seastar::make_ready_future<>();

                        // LRU eviction if at capacity
                        if (state.config.max_routes > 0) {
                            while (tree->route_count() >= state.config.max_routes) {
                                if (tree->evict_oldest()) {
                                    state.stats.routes_evicted++;
                                } else {
                                    break;
                                }
                            }
                        }

                        tree->insert(local_tokens, backend, RouteOrigin::LOCAL);
                        return seastar::make_ready_future<>();
                    });
                });
            })
        );
    }

    // Gossip: broadcast the deepest boundary only (full context)
    // Other depths are local optimizations that peers will compute themselves
    seastar::future<> gossip_future = seastar::make_ready_future<>();
    if (_gossip && !valid_boundaries.empty()) {
        size_t deepest = valid_boundaries.back();  // Already sorted
        std::vector<int32_t> deepest_prefix(tokens.begin(), tokens.begin() + deepest);
        auto tokens_ptr = std::make_unique<std::vector<int32_t>>(std::move(deepest_prefix));
        auto foreign_tokens = seastar::make_foreign(std::move(tokens_ptr));

        gossip_future = seastar::smp::submit_to(0, [this, foreign_tokens = std::move(foreign_tokens), backend]() mutable {
            if (_gossip->is_enabled()) {
                std::vector<int32_t> local_tokens(foreign_tokens->begin(), foreign_tokens->end());
                return _gossip->broadcast_route(local_tokens, backend);
            }
            return seastar::make_ready_future<>();
        });
    }

    futures.push_back(std::move(gossip_future));

    return seastar::when_all_succeed(futures.begin(), futures.end()).discard_result();
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

    // Register per-shard prune callbacks on ALL shards before starting gossip.
    // Each shard creates its own std::function locally, avoiding cross-shard
    // heap copies that risk wrong-shard deallocation (anti-pattern Bug #3).
    // The outer lambda captures nothing (function pointer), so it's safe to
    // send via smp::invoke_on_all. Each shard constructs the inner callback
    // locally on its own allocator.
    co_await seastar::smp::invoke_on_all([] {
        GossipConsensus::register_local_prune_callback([](BackendId backend) {
            return RouterService::remove_routes_for_backend(backend);
        });
    });

    start_batch_flush_timer();
    co_await _gossip->start();
}

seastar::future<> RouterService::stop_gossip() {
    if (!_gossip) {
        return seastar::make_ready_future<>();
    }

    // Shutdown sequence (order matters for correctness):
    // 1. Cancel timer - prevents new timer-triggered flushes
    // 2. Stop gossip - prevents new routes from arriving
    // 3. Flush remaining - ensures no buffered routes are lost
    // 4. Clear per-shard prune callbacks on all shards
    _batch_flush_timer.cancel();

    co_await _gossip->stop();
    log_router.info("Gossip stopped, flushing remaining route batch");
    co_await flush_route_batch();

    // Clear per-shard prune callbacks to prevent stale callback invocations
    co_await seastar::smp::invoke_on_all([] {
        GossipConsensus::clear_local_prune_callback();
    });
}

// ============================================================================
// Batch Flush Timer
// ============================================================================
//
// HARD RULE #5 (Timer Ownership):
// This timer callback captures `this` to access _pending_remote_routes and call
// flush_route_batch(). Safety is ensured by the shutdown sequence in stop_gossip():
//
//   1. _batch_flush_timer.cancel() - prevents new callbacks
//   2. _gossip->stop() - stops incoming routes
//   3. flush_route_batch() - drains any remaining buffered routes
//
// The callback uses fire-and-forget pattern ((void)future) because timer callbacks
// cannot return futures. Exceptions are caught and logged per Rule #9.
//
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

// ============================================================================
// flush_route_batch() - Batched Cross-Shard Route Distribution
// ============================================================================
//
// HARD RULE #14 (Cross-Shard Dispatch):
// This function broadcasts a batch of routes to all shards. It reduces SMP message
// overhead from O(routes × shards) to O(batches × shards) by batching.
//
// Memory safety follows the same foreign_ptr pattern as learn_route_global():
//   1. Batch is allocated on shard 0 (where GossipService runs)
//   2. Each shard receives a foreign_ptr-wrapped copy
//   3. Target shard creates LOCAL allocations from foreign_ptr data
//   4. foreign_ptr destructor returns to shard 0 for cleanup
//
// The double-copy (batch → foreign_ptr → local_batch) is intentional:
//   - First copy: Wrap in foreign_ptr for safe cross-shard transfer
//   - Second copy: Create shard-local allocation for ART insertion
//   - This ensures all memory operations happen on the correct shard's allocator
//
seastar::future<> RouterService::flush_route_batch() {
    // Rule #5: Acquire gate holder to prevent use-after-free during shutdown.
    seastar::gate::holder holder;
    try {
        holder = _timer_gate.hold();
    } catch (const seastar::gate_closed_exception&) {
        log_main.debug("Route batch flush skipped: service is stopping");
        return seastar::make_ready_future<>();
    }

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
    // CRITICAL MEMORY SAFETY: Seastar uses per-shard allocators. Memory allocated on
    // shard 0 must be freed on shard 0, not on other shards. We use foreign_ptr to:
    // 1. Wrap each batch copy so it can be safely passed to other shards
    // 2. Ensure proper deallocation on the source shard when foreign_ptr is destroyed
    // 3. Read from foreign_ptr on target shard and create LOCAL allocations
    //
    // Pattern: Create N copies on shard 0, wrap each in foreign_ptr, send to target shards.
    // Note: Gate holder is kept alive via do_with for duration of async work.
    return seastar::do_with(std::move(holder), std::move(batch),
        [](seastar::gate::holder&, std::vector<PendingRemoteRoute>& batch) {
        return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count),
            [&batch](unsigned shard_id) {
                // Create a copy wrapped in foreign_ptr for this shard
                // The copy is allocated on shard 0, foreign_ptr ensures it's freed on shard 0
                auto batch_ptr = std::make_unique<std::vector<PendingRemoteRoute>>(batch);
                auto foreign_batch = seastar::make_foreign(std::move(batch_ptr));

                return seastar::smp::submit_to(shard_id,
                    [foreign_batch = std::move(foreign_batch)]() mutable {
                        // Force local allocation on THIS shard by copying from foreign_ptr
                        // When foreign_batch destructor runs, it returns to shard 0 for cleanup
                        std::vector<PendingRemoteRoute> local_batch;
                        local_batch.reserve(foreign_batch->size());
                        for (const auto& route : *foreign_batch) {
                            local_batch.push_back(PendingRemoteRoute{
                                std::vector<int32_t>(route.tokens.begin(), route.tokens.end()),
                                route.backend
                            });
                        }
                        apply_route_batch_to_local_tree(local_batch);
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
                    auto port_opt = parse_port(std::string_view(addr_str).substr(colon_pos + 1));
                    bs.port = port_opt.value_or(0);
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

// ============================================================================
// Draining Reaper Timer
// ============================================================================
//
// HARD RULE #5 (Timer Ownership):
// This timer callback captures `this` to access backend state and call
// unregister_backend_global(). Safety is ensured by stop() calling
// stop_draining_reaper() before any member destruction.
//
// The callback accesses:
//   - g_shard_state (thread-local, safe)
//   - _config.backend_drain_timeout (member, safe because stop() cancels first)
//   - _pool_cleanup_callback (member, safe because stop() cancels first)
//
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
    // Rule #5: Acquire gate holder to prevent use-after-free during shutdown.
    seastar::gate::holder holder;
    try {
        holder = _timer_gate.hold();
    } catch (const seastar::gate_closed_exception&) {
        log_main.debug("Draining reaper skipped: service is stopping");
        return;
    }

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

void RouterService::register_backend_for_testing(BackendId id, seastar::socket_address addr,
                                                   uint32_t weight, uint32_t priority) {
    if (!g_shard_state) return;
    auto& state = *g_shard_state;
    state.backends[id] = BackendInfo{addr, weight, priority};

    bool exists = false;
    for (auto existing : state.backend_ids) {
        if (existing == id) { exists = true; break; }
    }
    if (!exists) {
        state.backend_ids.push_back(id);
    }
}

void RouterService::insert_route_for_testing(const std::vector<int32_t>& tokens, BackendId backend) {
    if (!g_shard_state) return;
    RadixTree* tree = g_shard_state->tree.get();
    if (!tree) return;
    tree->insert(tokens, backend, RouteOrigin::LOCAL);
}

void RouterService::set_backend_draining_for_testing(BackendId id) {
    if (!g_shard_state) return;
    auto it = g_shard_state->backends.find(id);
    if (it != g_shard_state->backends.end()) {
        it->second.is_draining = true;
        it->second.drain_start_time = std::chrono::steady_clock::now();
    }
}

void RouterService::mark_backend_dead_for_testing(BackendId id) {
    if (!g_shard_state) return;
    g_shard_state->dead_backends.insert(id);
}

void RouterService::unregister_backend_for_testing(BackendId id) {
    if (!g_shard_state) return;
    auto& state = *g_shard_state;
    state.backends.erase(id);
    auto it = std::find(state.backend_ids.begin(), state.backend_ids.end(), id);
    if (it != state.backend_ids.end()) {
        state.backend_ids.erase(it);
    }
    state.dead_backends.erase(id);
}

size_t RouterService::get_route_count_for_testing() {
    if (!g_shard_state) return 0;
    RadixTree* tree = g_shard_state->tree.get();
    if (!tree) return 0;
    return tree->route_count();
}

} // namespace ranvier
