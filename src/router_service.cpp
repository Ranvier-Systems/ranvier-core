#include "router_service.hpp"
#include "gossip_service.hpp"
#include "logging.hpp"
#include "metrics_service.hpp"
#include "node_slab.hpp"
#include "parse_utils.hpp"

#include <seastar/core/coroutine.hh>
#include <seastar/core/smp.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/when_all.hh>
#include <boost/range/irange.hpp>
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>

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
//   - flush_local_route_batch(), flush_route_batch() broadcast to all shards
//   - learn_route_global() buffers locally; flush broadcasts in batches
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

    // Whether this backend supports vLLM's prompt_token_ids field.
    // When false, token IDs are stripped from forwarded requests to avoid
    // 400 rejections from backends that don't recognize the field (e.g., Ollama).
    // Default true for backward compatibility with existing vLLM deployments.
    bool supports_token_ids = true;

    // Load tracking: in-flight requests to this backend.
    // Shard-local only — BackendInfo lives in thread_local ShardLocalState,
    // accessed exclusively from one reactor thread.  No atomic needed.
    uint64_t active_requests = 0;

    // Cost tracking: sum of estimated_cost_units for in-flight requests.
    // Shard-local, same as active_requests — no atomics needed.
    double current_cost_budget = 0.0;

    // KV-cache compression ratio (>= 1.0). A backend with 6x compression
    // has 6x effective cache capacity. Default 1.0 (no compression).
    // Set via per-backend config or admin API. Technology-agnostic.
    double compression_ratio = 1.0;

    BackendInfo() = default;

    BackendInfo(seastar::socket_address addr_, uint32_t weight_, uint32_t priority_,
                bool supports_token_ids_ = true, double compression_ratio_ = 1.0)
        : addr(std::move(addr_))
        , weight(weight_)
        , priority(priority_)
        , supports_token_ids(supports_token_ids_)
        , compression_ratio(compression_ratio_) {}
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
        uint64_t gpu_load_redirects = 0;         // Times GPU-load-driven redirect occurred
        // Local route batching stats (per-shard)
        uint64_t local_routes_batched = 0;        // Routes added to local buffer
        uint64_t local_batch_flushes = 0;          // Number of local flush operations
        uint64_t local_routes_deduplicated = 0;    // Routes skipped by dedup within batch
        uint64_t routes_deduplicated_pre_buffer = 0; // Routes skipped by ART lookup before buffering
        uint64_t local_routes_dropped_overflow = 0; // Routes dropped due to buffer overflow
        // Cross-shard load sync stats
        uint64_t load_sync_broadcasts = 0;         // Number of load snapshot broadcasts sent
        uint64_t load_sync_snapshots_received = 0; // Number of snapshots received from other shards
        // Cost-based routing stats
        uint64_t cost_redirects = 0;               // Times cost budget changed backend
        uint64_t fast_lane_routes = 0;              // Small requests fast-laned
        uint64_t budget_exhausted = 0;              // No budget available anywhere
        uint64_t cost_reserved_total = 0;           // Total cost reservations made
        uint64_t cost_released_total = 0;           // Total cost releases made
        // Capacity-aware hash fallback stats
        uint64_t headroom_redirects = 0;            // Times cache headroom changed backend selection

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
            gpu_load_redirects = 0;
            local_routes_batched = 0;
            local_batch_flushes = 0;
            local_routes_deduplicated = 0;
            routes_deduplicated_pre_buffer = 0;
            local_routes_dropped_overflow = 0;
            load_sync_broadcasts = 0;
            load_sync_snapshots_received = 0;
            cost_redirects = 0;
            fast_lane_routes = 0;
            budget_exhausted = 0;
            cost_reserved_total = 0;
            cost_released_total = 0;
            headroom_redirects = 0;
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
        // Load-aware routing configuration (relative threshold)
        bool load_aware_routing = true;           // Enable load-aware backend selection
        double load_imbalance_factor = 2.0;       // Divert when preferred > factor * median load
        uint64_t load_imbalance_floor = 2;        // Additive floor to prevent flapping at low load
        // Hash strategy configuration
        RoutingConfig::HashStrategy hash_strategy = RoutingConfig::HashStrategy::BOUNDED_LOAD;
        double bounded_load_epsilon = 0.25;
        uint64_t p2c_load_bias = 2;
        // Cross-shard load sync configuration
        bool cross_shard_load_sync = false;
        std::chrono::milliseconds cross_shard_load_sync_interval{100};
        // GPU load integration (vLLM-aware routing)
        double gpu_load_weight = 10.0;
        std::chrono::seconds gpu_load_cache_ttl{30};
        // Cost-based routing configuration
        bool cost_routing_enabled = false;
        double cost_routing_max_cost = 10000.0;
        double cost_routing_small_threshold = 500.0;
        bool cost_routing_fast_lane = true;
        double cost_routing_imbalance_factor = 2.0;
        // Capacity-aware hash fallback
        double capacity_headroom_weight = 5.0;
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
    // Local Route Batching (shard-local, no cross-shard access on enqueue)
    // ========================================================================
    // Buffer for locally-learned routes pending batch broadcast.
    // Each shard accumulates routes independently; only the flush broadcasts
    // cross-shard, amortizing O(shards) SMP messages across the batch.
    std::vector<PendingLocalRoute> pending_local_routes;

    // Per-shard timer that flushes the local route buffer periodically
    seastar::timer<> local_flush_timer;

    // Per-shard gate for local flush timer callbacks (Rule #5)
    // stop_local_batch_timer() closes the gate before cancelling the timer
    seastar::gate local_flush_gate;

    // Gossip pointer (set on shard 0 only; nullptr on other shards)
    // Used by flush_local_route_batch() to broadcast routes to cluster peers
    GossipService* gossip_ptr = nullptr;

    // Whether gossip is enabled (set on all shards during init for fast checks)
    bool gossip_enabled = false;

    // ========================================================================
    // GPU Load Cache (per-shard, broadcast from shard 0 by HealthService)
    // ========================================================================
    // Caches vLLM GPU load scores (0.0–1.0) from HealthService for use in
    // routing decisions on all shards. HealthService runs on shard 0 only;
    // broadcast_gpu_load() distributes scores to all shards after each scrape.
    // Hot-path reads are shard-local only (Rule #1).
    struct GpuLoadCache {
        absl::flat_hash_map<BackendId, double> scores;  // BackendId → load_score (0.0–1.0)
        std::chrono::steady_clock::time_point updated_at;
        static constexpr size_t MAX_ENTRIES = 256;       // Hard Rule #4
    } gpu_load_cache;

    // ========================================================================
    // Cache Headroom Cache (per-shard, broadcast from shard 0 by HealthService)
    // ========================================================================
    // Caches per-backend effective cache pressure (0.0–1.0) for capacity-aware
    // hash fallback. Higher values mean more cache fullness (less headroom).
    // broadcast_cache_headroom() distributes values to all shards after scrape.
    // Hot-path reads are shard-local only (Rule #1).
    struct CacheHeadroomCache {
        absl::flat_hash_map<BackendId, double> pressure;  // BackendId → effective_cache_pressure (0.0–1.0)
        std::chrono::steady_clock::time_point updated_at;
        static constexpr size_t MAX_ENTRIES = 256;         // Hard Rule #4
    } cache_headroom_cache;

    // ========================================================================
    // Cross-Shard Load Synchronization
    // ========================================================================
    // Each shard periodically broadcasts its active_requests snapshot to all
    // other shards. This gives routing decisions a global load view, preventing
    // multiple shards from routing to the same "least loaded" backend when they
    // each only see their own shard's counters.
    //
    // Storage: per-source-shard snapshots, indexed by shard_id.
    // On each broadcast, the receiving shard subtracts the old snapshot for the
    // source shard from cross_shard_load, stores the new snapshot, and adds the
    // new values — an incremental O(backends) update instead of a full O(shards
    // * backends) recomputation.
    //
    // cross_shard_load[backend_id] = sum of active_requests from ALL OTHER shards
    // This is added to the local active_requests in get_backend_load() to give
    // a global estimate.

    // Per-source-shard load snapshots: shard_load_snapshots[source_shard][backend_id] = active_requests
    // Outer vector sized to smp::count during init; inner maps updated on each broadcast.
    std::vector<absl::flat_hash_map<BackendId, uint64_t>> shard_load_snapshots;

    // Aggregated cross-shard load: sum of all other shards' active_requests per backend.
    // Updated incrementally on each snapshot receive (subtract old, add new).
    absl::flat_hash_map<BackendId, uint64_t> cross_shard_load;

    // Per-shard timer that broadcasts load snapshots periodically
    seastar::timer<> load_sync_timer;

    // Per-shard gate for load sync timer callbacks (Rule #5)
    seastar::gate load_sync_gate;

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
        config.load_imbalance_factor = cfg.load_imbalance_factor;
        config.load_imbalance_floor = cfg.load_imbalance_floor;
        // Hash strategy configuration
        config.hash_strategy = cfg.hash_strategy;
        config.bounded_load_epsilon = cfg.bounded_load_epsilon;
        config.p2c_load_bias = cfg.p2c_load_bias;
        // Cross-shard load sync configuration
        config.cross_shard_load_sync = cfg.cross_shard_load_sync;
        config.cross_shard_load_sync_interval = cfg.cross_shard_load_sync_interval;
        // GPU load integration
        config.gpu_load_weight = cfg.gpu_load_weight;
        config.gpu_load_cache_ttl = cfg.gpu_load_cache_ttl;
        // Cost-based routing configuration
        config.cost_routing_enabled = cfg.cost_routing.enabled;
        config.cost_routing_max_cost = cfg.cost_routing.max_cost_per_backend;
        config.cost_routing_small_threshold = cfg.cost_routing.small_request_threshold;
        config.cost_routing_fast_lane = cfg.cost_routing.enable_fast_lane;
        config.cost_routing_imbalance_factor = cfg.cost_routing.cost_imbalance_factor;
        // Capacity-aware hash fallback
        config.capacity_headroom_weight = cfg.capacity_headroom_weight;

        // Pre-allocate cross-shard load snapshot storage (one entry per shard)
        // Resized to smp::count so we can index by shard_id without bounds checks.
        // Each entry starts empty; populated on first broadcast receive.
        shard_load_snapshots.resize(seastar::smp::count);

        // Pre-allocate local route batch buffer to avoid reallocations during operation
        pending_local_routes.reserve(RouteBatchConfig::MAX_BATCH_SIZE);

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
        config.load_imbalance_factor = cfg.load_imbalance_factor;
        config.load_imbalance_floor = cfg.load_imbalance_floor;
        // Hash strategy configuration
        config.hash_strategy = cfg.hash_strategy;
        config.bounded_load_epsilon = cfg.bounded_load_epsilon;
        config.p2c_load_bias = cfg.p2c_load_bias;
        // Cross-shard load sync configuration
        config.cross_shard_load_sync = cfg.cross_shard_load_sync;
        config.cross_shard_load_sync_interval = cfg.cross_shard_load_sync_interval;
        // GPU load integration
        config.gpu_load_weight = cfg.gpu_load_weight;
        config.gpu_load_cache_ttl = cfg.gpu_load_cache_ttl;
        // Cost-based routing configuration
        config.cost_routing_enabled = cfg.cost_routing.enabled;
        config.cost_routing_max_cost = cfg.cost_routing.max_cost_per_backend;
        config.cost_routing_small_threshold = cfg.cost_routing.small_request_threshold;
        config.cost_routing_fast_lane = cfg.cost_routing.enable_fast_lane;
        config.cost_routing_imbalance_factor = cfg.cost_routing.cost_imbalance_factor;
        // Capacity-aware hash fallback
        config.capacity_headroom_weight = cfg.capacity_headroom_weight;
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

        // Clear local route batch buffer
        pending_local_routes.clear();
        gossip_ptr = nullptr;
        gossip_enabled = false;

        // Clear GPU load cache
        gpu_load_cache.scores.clear();

        // Clear cache headroom cache
        cache_headroom_cache.pressure.clear();

        // Clear cross-shard load sync state
        for (auto& snapshot : shard_load_snapshots) {
            snapshot.clear();
        }
        cross_shard_load.clear();

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

    // ========================================================================
    // Live Backend Helpers (shared filter: not dead, not missing, not draining)
    // ========================================================================

    // Return sorted vector of live backend IDs.
    // Used by get_backend_for_prefix() and get_backend_by_hash().
    std::vector<BackendId> get_live_backends() const {
        std::vector<BackendId> result;
        for (BackendId id : backend_ids) {
            if (dead_backends.contains(id)) continue;
            auto it = backends.find(id);
            if (it == backends.end()) continue;
            if (it->second.is_draining) continue;
            result.push_back(id);
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    // Return live backends with BackendInfo pointers for weight/priority access.
    // Used by get_random_backend().
    std::vector<std::pair<BackendId, const BackendInfo*>> get_live_backend_infos() const {
        std::vector<std::pair<BackendId, const BackendInfo*>> result;
        for (BackendId id : backend_ids) {
            if (dead_backends.contains(id)) continue;
            auto it = backends.find(id);
            if (it == backends.end()) continue;
            if (it->second.is_draining) continue;
            result.emplace_back(id, &it->second);
        }
        return result;
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
// BackendInfo lives in thread_local ShardLocalState — all access is from a
// single reactor thread, so plain uint64_t is sufficient (no atomic needed).
//

BackendRequestGuard::BackendRequestGuard(BackendId id) : _backend_id(id), _active(false) {
    if (!g_shard_state) {
        return;  // Shard not initialized, guard remains inactive
    }

    auto it = g_shard_state->backends.find(id);
    if (it == g_shard_state->backends.end()) {
        return;  // Backend not found, guard remains inactive
    }

    ++it->second.active_requests;
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

    // Guard against underflow: if backend was removed and re-registered while
    // request was in-flight, the counter would be 0.
    if (it->second.active_requests > 0) {
        --it->second.active_requests;
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
            if (it != g_shard_state->backends.end() && it->second.active_requests > 0) {
                --it->second.active_requests;
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
// CostBudgetGuard Implementation
// ============================================================================

CostBudgetGuard::CostBudgetGuard(BackendId id, double cost)
    : _backend_id(id), _cost(cost) {
    if (cost > 0.0 && g_shard_state && g_shard_state->config.cost_routing_enabled) {
        auto it = g_shard_state->backends.find(id);
        if (it != g_shard_state->backends.end()) {
            // Always reserve (symmetric with release). Budget is advisory.
            it->second.current_cost_budget += cost;
            g_shard_state->stats.cost_reserved_total++;
            _active = true;
        }
    }
}

CostBudgetGuard::~CostBudgetGuard() {
    if (_active && g_shard_state) {
        auto it = g_shard_state->backends.find(_backend_id);
        if (it != g_shard_state->backends.end()) {
            it->second.current_cost_budget -= _cost;
            if (it->second.current_cost_budget < 0.0) {
                it->second.current_cost_budget = 0.0;
            }
            g_shard_state->stats.cost_released_total++;
        }
    }
}

CostBudgetGuard::CostBudgetGuard(CostBudgetGuard&& other) noexcept
    : _backend_id(other._backend_id), _cost(other._cost), _active(other._active) {
    other._active = false;
}

CostBudgetGuard& CostBudgetGuard::operator=(CostBudgetGuard&& other) noexcept {
    if (this != &other) {
        // Release current reservation if active
        if (_active && g_shard_state) {
            auto it = g_shard_state->backends.find(_backend_id);
            if (it != g_shard_state->backends.end()) {
                it->second.current_cost_budget -= _cost;
                if (it->second.current_cost_budget < 0.0) {
                    it->second.current_cost_budget = 0.0;
                }
                g_shard_state->stats.cost_released_total++;
            }
        }
        _backend_id = other._backend_id;
        _cost = other._cost;
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

    uint64_t local_load = it->second.active_requests;

    // Add cross-shard load hints if sync is enabled.
    // cross_shard_load contains the sum of active_requests from all OTHER shards,
    // giving routing decisions a global view of backend utilization.
    if (g_shard_state->config.cross_shard_load_sync) {
        auto cs_it = g_shard_state->cross_shard_load.find(id);
        if (cs_it != g_shard_state->cross_shard_load.end()) {
            local_load += cs_it->second;
        }
    }

    return local_load;
}

// ============================================================================
// GPU Load Cache Accessors (shard-local, lock-free)
// ============================================================================

// Get cached GPU load score for a backend from the per-shard GPU load cache.
// Returns -1.0 if no valid cached score is available (no metrics, stale cache,
// or backend not found). Staleness threshold is configurable via gpu_load_cache_ttl
// (default 30s, should be ≥2x the health check scrape interval).
//
// Rule #1: Lock-free — shard-local read only, no cross-shard access.
static double get_cached_gpu_load(BackendId id) {
    if (!g_shard_state) return -1.0;
    auto& cache = g_shard_state->gpu_load_cache;

    // Stale check: if cache is older than configured TTL, ignore
    auto age = std::chrono::steady_clock::now() - cache.updated_at;
    if (age > g_shard_state->config.gpu_load_cache_ttl) return -1.0;

    auto it = cache.scores.find(id);
    if (it == cache.scores.end()) return -1.0;
    return it->second;
}

// Compute composite load for a backend, blending shard-local active_requests
// with GPU load score from vLLM metrics (when available).
//
// When vLLM metrics are available:
//   composite = local_active_requests + (gpu_load_score * gpu_load_weight)
// When vLLM metrics are unavailable:
//   composite = local_active_requests (existing behavior, backward compatible)
//
// Returns uint64_t for backward compatibility with existing P2C/bounded-load
// comparisons that use integer load values.
//
// Rule #1: Lock-free — all reads are shard-local plain integers + cache lookup.
uint64_t get_composite_backend_load(BackendId id) {
    uint64_t local_load = get_backend_load(id);

    auto gpu_score = get_cached_gpu_load(id);
    if (gpu_score < 0.0) {
        return local_load;  // No GPU metrics available — fall back to existing behavior
    }

    // Scale GPU score to comparable range with active requests.
    // gpu_load_weight controls the influence of GPU metrics on routing.
    uint64_t gpu_contribution = static_cast<uint64_t>(
        gpu_score * g_shard_state->config.gpu_load_weight);

    return local_load + gpu_contribution;
}

// ============================================================================
// Cache Headroom Accessors (shard-local, lock-free)
// ============================================================================

// Get cached effective cache pressure for a backend from the per-shard cache.
// Returns -1.0 if no valid cached value is available (no metrics, stale cache,
// or backend not found). Uses same staleness threshold as GPU load cache.
//
// Rule #1: Lock-free — shard-local read only, no cross-shard access.
static double get_cached_cache_pressure(BackendId id) {
    if (!g_shard_state) return -1.0;
    auto& cache = g_shard_state->cache_headroom_cache;

    // Stale check: reuse GPU load cache TTL (both are refreshed from same scrape cycle)
    auto age = std::chrono::steady_clock::now() - cache.updated_at;
    if (age > g_shard_state->config.gpu_load_cache_ttl) return -1.0;

    auto it = cache.pressure.find(id);
    if (it == cache.pressure.end()) return -1.0;
    return it->second;
}

// Compute capacity-adjusted load for a backend, blending composite load with
// a penalty for cache fullness. The penalty scales with estimated request cost:
// large-context requests care more about available cache headroom.
//
//   adjusted = composite_load + capacity_headroom_weight * cache_pressure * (1.0 + cost_scale)
//
// where cost_scale = min(estimated_cost / cost_routing_max_cost, 1.0)
//
// When cache headroom data is unavailable, falls back to composite load.
// When capacity_headroom_weight is 0.0, this equals get_composite_backend_load().
//
// Rule #1: Lock-free — all reads are shard-local.
static uint64_t get_capacity_adjusted_load(BackendId id, double estimated_cost) {
    uint64_t base = get_composite_backend_load(id);

    if (!g_shard_state || g_shard_state->config.capacity_headroom_weight <= 0.0) {
        return base;
    }

    double cache_pressure = get_cached_cache_pressure(id);
    if (cache_pressure < 0.0) return base;  // No headroom data available

    // Scale penalty by request size: larger requests should penalize cache-full
    // backends more aggressively, since they'll generate more KV-cache entries.
    double cost_scale = 0.0;
    if (estimated_cost > 0.0 && g_shard_state->config.cost_routing_max_cost > 0.0) {
        cost_scale = std::min(estimated_cost / g_shard_state->config.cost_routing_max_cost, 1.0);
    }

    double penalty = g_shard_state->config.capacity_headroom_weight
                   * cache_pressure * (1.0 + cost_scale);
    return base + static_cast<uint64_t>(penalty);
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

        // Use get_composite_backend_load() which includes cross-shard load hints
        // and GPU load scores (when available) for a global view of utilization.
        uint64_t load = get_composite_backend_load(id);
        if (load < best_load) {
            best_load = load;
            best_id = id;
        }
    }

    return {best_id, best_load};
}

// ============================================================================
// Cost Budget Tracking (shard-local, lock-free)
// ============================================================================
//
// Per-backend cost budget tracks the sum of estimated_cost_units for in-flight
// requests. This prevents head-of-line blocking where a backend processing one
// huge request appears equally loaded as one processing many tiny requests.
//
// All operations are shard-local (thread_local g_shard_state) — no locks needed.

bool reserve_cost_budget(BackendId id, double cost) {
    if (!g_shard_state || cost <= 0.0) return true;
    auto it = g_shard_state->backends.find(id);
    if (it == g_shard_state->backends.end()) return false;

    double max_budget = g_shard_state->config.cost_routing_max_cost;
    // Scale budget ceiling by compression ratio: compressed backends have more
    // effective KV-cache capacity, so they can accept proportionally more cost.
    double effective_max = max_budget * it->second.compression_ratio;
    bool within_budget = (it->second.current_cost_budget + cost <= effective_max);
    // Always add cost to ensure symmetric reserve/release. The budget is advisory —
    // routing decisions check budget *before* reserving, and this function's return
    // value is informational only. Asymmetric reserve/release causes budget drift.
    it->second.current_cost_budget += cost;
    g_shard_state->stats.cost_reserved_total++;
    return within_budget;
}

void release_cost_budget(BackendId id, double cost) {
    if (!g_shard_state || cost <= 0.0) return;
    auto it = g_shard_state->backends.find(id);
    if (it == g_shard_state->backends.end()) return;

    it->second.current_cost_budget -= cost;
    // Clamp to zero to avoid negative drift from floating-point accumulation
    if (it->second.current_cost_budget < 0.0) {
        it->second.current_cost_budget = 0.0;
    }
    g_shard_state->stats.cost_released_total++;
}

double get_backend_cost(BackendId id) {
    if (!g_shard_state) return 0.0;
    auto it = g_shard_state->backends.find(id);
    if (it == g_shard_state->backends.end()) return 0.0;
    return it->second.current_cost_budget;
}

// Find backend with lowest cost budget among candidates.
// O(n) scan, same pattern as get_least_loaded_backend.
// Callers pass pre-filtered live_backends (dead/draining already excluded).
static std::pair<BackendId, double> get_least_cost_backend(
    const std::vector<BackendId>& candidates) {
    if (!g_shard_state || candidates.empty()) {
        return {0, std::numeric_limits<double>::max()};
    }

    BackendId best_id = 0;
    double best_cost = std::numeric_limits<double>::max();

    for (BackendId id : candidates) {
        auto it = g_shard_state->backends.find(id);
        if (it == g_shard_state->backends.end()) continue;

        double cost = it->second.current_cost_budget;
        if (cost < best_cost) {
            best_cost = cost;
            best_id = id;
        }
    }
    return {best_id, best_cost};
}

// Find a backend with available budget, excluding one backend.
// Uses random-two-choices selection to avoid thundering herd.
// Returns nullopt if no backend has sufficient budget headroom.
// Stack-allocated: no heap allocation for up to 256 backends.
static std::optional<BackendId> find_backend_with_budget(
    const std::vector<BackendId>& candidates,
    double request_cost,
    double max_budget,
    BackendId exclude_id) {
    if (!g_shard_state || candidates.size() < 2) return std::nullopt;

    // Build list of eligible candidates on the stack (no heap allocation).
    // 256 is the hard limit on backends (ShardLocalState::GpuLoadCache::MAX_ENTRIES).
    static constexpr size_t MAX_ELIGIBLE = 256;
    std::array<BackendId, MAX_ELIGIBLE> eligible{};
    size_t eligible_count = 0;

    for (BackendId id : candidates) {
        if (id == exclude_id) continue;
        auto it = g_shard_state->backends.find(id);
        if (it == g_shard_state->backends.end()) continue;
        // Scale budget ceiling by backend's compression ratio
        double effective_max = max_budget * it->second.compression_ratio;
        if (it->second.current_cost_budget + request_cost <= effective_max) {
            if (eligible_count < MAX_ELIGIBLE) {
                eligible[eligible_count++] = id;
            }
        }
    }

    if (eligible_count == 0) return std::nullopt;
    if (eligible_count == 1) return eligible[0];

    // Random-two-choices: pick 2 random candidates, return whichever has more headroom
    auto& rng = g_shard_state->rng;
    std::uniform_int_distribution<size_t> dist(0, eligible_count - 1);
    size_t idx_a = dist(rng);
    size_t idx_b = dist(rng);
    if (idx_a == idx_b) {
        idx_b = (idx_b + 1) % eligible_count;
    }

    double cost_a = get_backend_cost(eligible[idx_a]);
    double cost_b = get_backend_cost(eligible[idx_b]);
    return (cost_a <= cost_b) ? eligible[idx_a] : eligible[idx_b];
}

// Check if preferred backend is significantly more loaded than its peers and
// find a less-loaded alternative if so.
//
// Uses a RELATIVE threshold: the preferred backend is "overloaded" when its
// queue depth exceeds (median_load * factor + floor). This auto-adapts to any
// workload, model size, or cluster size — no per-model tuning needed.
//
// Called once per request from get_backend_for_prefix(), after the final backend
// has been selected by either the ART or hash path. This single call site avoids
// redundant O(N) scans and keeps load-balancing logic in one place.
//
// Parameters:
//   preferred_id: The backend selected by ART or hash
//   live_backends: List of available backends to consider
//   request_id: For logging (can be empty)
//   source: Description for logging ("ART" or "hash")
//
// Rule #1: Lock-free - shard-local state, no synchronization needed
//
// Performance notes:
// - Early exits minimize work in the common case (balanced load)
// - Config values cached in shard-local state (no cross-shard access)
// - All reads are shard-local plain integers (no atomics needed)
// - Median computed via nth_element (O(n), n is typically <16 backends)
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

    // Need at least 2 backends for load comparison to make sense
    if (live_backends.size() < 2) {
        return preferred_id;
    }

    // Cache config values locally to avoid repeated struct access
    const double factor = g_shard_state->config.load_imbalance_factor;
    const uint64_t floor = g_shard_state->config.load_imbalance_floor;

    // Check preferred backend's composite load (local + GPU)
    uint64_t preferred_load = get_composite_backend_load(preferred_id);

    // Fast path: zero load is never overloaded
    if (preferred_load == 0) {
        return preferred_id;
    }

    // Collect loads from all live backends to compute median.
    // live_backends.size() is typically <16, so stack allocation is fine.
    // Rule #4: bounded by live_backends.size() (already bounded upstream).
    std::vector<uint64_t> loads;
    loads.reserve(live_backends.size());
    for (BackendId id : live_backends) {
        loads.push_back(get_composite_backend_load(id));
    }

    // Compute median via nth_element (O(n), partial sort)
    size_t mid = loads.size() / 2;
    std::nth_element(loads.begin(), loads.begin() + static_cast<ptrdiff_t>(mid), loads.end());
    uint64_t median = loads[mid];

    // Relative threshold: divert only if preferred is significantly above median
    // The floor prevents flapping when all backends are at low/zero load
    uint64_t threshold = static_cast<uint64_t>(static_cast<double>(median) * factor) + floor;
    if (preferred_load <= threshold) {
        return preferred_id;
    }

    // Preferred backend is a genuine outlier — find the least-loaded alternative
    auto [least_loaded_id, least_load] = get_least_loaded_backend(live_backends);

    // No viable alternative found
    if (least_loaded_id == 0) {
        return preferred_id;
    }

    // Divert to less-loaded backend
    g_shard_state->stats.load_aware_fallbacks++;
    if (g_metrics) {
        metrics().record_load_aware_fallback();
    }

    log_router.debug("[{}] Load-aware routing: {} preferred backend {} has {} in-flight "
                     "(median={}, threshold={}), routing to {} with {} in-flight",
                     request_id, source, preferred_id, preferred_load,
                     median, threshold, least_loaded_id, least_load);

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

// Jump consistent hash (Lamping & Veach, 2014).
// Maps a 64-bit key to a bucket in [0, num_buckets) such that adding or
// removing a bucket remaps only ~1/n keys.  This preserves KV-cache affinity
// during backend topology changes, unlike modular hashing which reshuffles all.
inline int32_t jump_consistent_hash(uint64_t key, int32_t num_buckets) {
    int64_t b = -1, j = 0;
    while (j < num_buckets) {
        b = j;
        key = key * 2862933555777941757ULL + 1;
        j = static_cast<int64_t>(
            static_cast<double>(b + 1) *
            (static_cast<double>(1LL << 31) /
             static_cast<double>((key >> 33) + 1)));
    }
    return static_cast<int32_t>(b);
}

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

// ============================================================================
// Hash Strategy: Bounded-Load Consistent Hashing
// ============================================================================
// Mirrokni, Thorup, Zadimoghaddam (Google, 2018).
// Caps each backend at ceil(avg_load * (1 + epsilon)). When the primary
// jump-hash bucket exceeds capacity, probe subsequent buckets until one
// with headroom is found. Falls back to least-loaded on full saturation.
//
// This replaces apply_load_aware_selection() for BOUNDED_LOAD strategy —
// load awareness is built into the hash selection itself.
//
// Rule #1: Lock-free — all reads are shard-local plain integers.
// Rule #4: probe loop bounded by live_backends.size().
static BackendId bounded_load_select(
    uint64_t prefix_hash,
    const std::vector<BackendId>& live_backends,
    double epsilon,
    const std::string& request_id,
    double estimated_cost = 0.0)
{
    const auto n = static_cast<int32_t>(live_backends.size());
    if (n <= 0) return 0;
    if (n == 1) return live_backends[0];

    // Compute total capacity-adjusted load across all live backends for average.
    // When cache headroom data is available and capacity_headroom_weight > 0,
    // backends with fuller caches appear more loaded — especially for large requests.
    uint64_t total_load = 0;
    for (BackendId id : live_backends) {
        total_load += get_capacity_adjusted_load(id, estimated_cost);
    }
    double avg = static_cast<double>(total_load) / static_cast<double>(n);
    // Cap: at least 1 to avoid starving all backends when idle
    uint64_t cap = std::max(static_cast<uint64_t>(1),
                            static_cast<uint64_t>(std::ceil(avg * (1.0 + epsilon))));

    // Probe from primary hash bucket, then sequential offsets
    for (int32_t probe = 0; probe < n; ++probe) {
        int32_t idx = jump_consistent_hash(prefix_hash + static_cast<uint64_t>(probe), n);
        BackendId candidate = live_backends[idx];
        uint64_t load = get_capacity_adjusted_load(candidate, estimated_cost);
        if (load < cap) {
            if (probe > 0) {
                // Diverted from primary — record as load-aware fallback
                g_shard_state->stats.load_aware_fallbacks++;
                if (g_metrics) {
                    metrics().record_load_aware_fallback();
                }
                log_router.debug("[{}] Bounded-load: primary over cap ({}>{}), "
                                 "probe {} -> backend {} (load={})",
                                 request_id, get_capacity_adjusted_load(live_backends[jump_consistent_hash(prefix_hash, n)], estimated_cost),
                                 cap, probe, candidate, load);
            }
            return candidate;
        }
    }

    // All backends at or above cap — fall back to least-loaded
    auto [least_id, least_load] = get_least_loaded_backend(live_backends);
    if (least_id != 0) {
        g_shard_state->stats.load_aware_fallbacks++;
        if (g_metrics) {
            metrics().record_load_aware_fallback();
        }
        log_router.debug("[{}] Bounded-load: all over cap ({}), least-loaded -> backend {} (load={})",
                         request_id, cap, least_id, least_load);
        return least_id;
    }

    // Absolute fallback: primary bucket
    return live_backends[jump_consistent_hash(prefix_hash, n)];
}

// ============================================================================
// Hash Strategy: Power of Two Choices (P2C) with Primary Affinity Bias
// ============================================================================
// Hashes to two candidate backends. Always prefers the primary (hash-selected)
// backend unless the secondary is significantly less loaded (by p2c_load_bias).
// This preserves cache affinity when load is balanced but breaks affinity
// gracefully under hot-spotting.
//
// Proven to reduce max load from O(log n) to O(log log n).
//
// The secondary candidate uses the request_id hash as a salt to break
// deterministic collisions (all same-prefix requests getting the same
// secondary). For ART hits, request_salt provides the diversification.
//
// Rule #1: Lock-free — shard-local reads only.
static BackendId p2c_select(
    uint64_t prefix_hash,
    uint64_t request_salt,
    const std::vector<BackendId>& live_backends,
    uint64_t load_bias,
    const std::string& request_id,
    double estimated_cost = 0.0)
{
    const auto n = static_cast<int32_t>(live_backends.size());
    if (n <= 0) return 0;
    if (n == 1) return live_backends[0];

    int32_t primary_idx = jump_consistent_hash(prefix_hash, n);
    // XOR with salt to get a different bucket for the secondary choice
    int32_t secondary_idx = jump_consistent_hash(prefix_hash ^ request_salt, n);

    // If same bucket, step to next (wrap around)
    if (secondary_idx == primary_idx) {
        secondary_idx = (primary_idx + 1) % n;
    }

    BackendId primary = live_backends[primary_idx];
    BackendId secondary = live_backends[secondary_idx];

    // Use capacity-adjusted load: backends with fuller caches appear more
    // loaded, especially for large requests. Falls back to composite load
    // when headroom data is unavailable or weight is 0.
    uint64_t p_load = get_capacity_adjusted_load(primary, estimated_cost);
    uint64_t s_load = get_capacity_adjusted_load(secondary, estimated_cost);

    // Prefer primary for cache affinity — only switch if secondary is
    // clearly less loaded (by at least load_bias)
    if (s_load + load_bias < p_load) {
        g_shard_state->stats.load_aware_fallbacks++;
        if (g_metrics) {
            metrics().record_load_aware_fallback();
        }
        log_router.debug("[{}] P2C: primary backend {} has {} in-flight, "
                         "secondary backend {} has {} (bias={}), switching",
                         request_id, primary, p_load, secondary, s_load, load_bias);
        return secondary;
    }

    return primary;
}

RouterService::RouterService() : RouterService(RoutingConfig{}) {}

RouterService::RouterService(const RoutingConfig& config)
    : RouterService(config, ClusterConfig{}) {}

RouterService::RouterService(const RoutingConfig& routing_config, const ClusterConfig& cluster_config)
    : _config(routing_config), _cluster_config(cluster_config) {
    // Initialize shard 0's ShardLocalState
    g_shard_state = std::make_unique<ShardLocalState>();
    g_shard_state->init(routing_config);

    // Log configured route batch flush interval (parsed from env/YAML by config_loader)
    if (routing_config.route_batch_flush_interval != RouteBatchConfig::DEFAULT_FLUSH_INTERVAL) {
        log_router.info("Route batch flush interval configured: {}ms",
                        routing_config.route_batch_flush_interval.count());
    }

    // Log active hash strategy for operational visibility
    const char* strategy_name = "jump";
    switch (routing_config.hash_strategy) {
    case RoutingConfig::HashStrategy::BOUNDED_LOAD: strategy_name = "bounded_load"; break;
    case RoutingConfig::HashStrategy::P2C:          strategy_name = "p2c"; break;
    case RoutingConfig::HashStrategy::MODULAR:      strategy_name = "modular"; break;
    case RoutingConfig::HashStrategy::JUMP:
    default:                                        strategy_name = "jump"; break;
    }
    log_router.info("Hash strategy: {} (epsilon={}, p2c_bias={})",
                    strategy_name, routing_config.bounded_load_epsilon,
                    routing_config.p2c_load_bias);

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
        // Route Table Size & Memory Metrics (BACKLOG 3.1)
        // ====================================================================

        // Current number of active routes in the radix tree (shard-local)
        seastar::metrics::make_gauge("routes_total",
            seastar::metrics::description("Current number of active routes in the shard-local radix tree. Use for capacity planning."),
            [] {
                if (!g_shard_state) return static_cast<double>(0);
                auto* tree = g_shard_state->tree.get();
                if (!tree) return static_cast<double>(0);
                return static_cast<double>(tree->route_count());
            }),

        // Estimated memory consumed by radix tree nodes (bytes, shard-local)
        seastar::metrics::make_gauge("radix_tree_bytes",
            seastar::metrics::description("Estimated memory in bytes consumed by allocated radix tree nodes. Use for capacity planning."),
            [] {
                if (!g_shard_state) return static_cast<double>(0);
                auto* tree = g_shard_state->tree.get();
                if (!tree) return static_cast<double>(0);
                return static_cast<double>(tree->estimate_memory_bytes());
            }),

        // ====================================================================
        // Load-Aware Routing Metrics
        // ====================================================================

        // Counter: requests diverted to less-loaded backends
        seastar::metrics::make_counter("router_load_aware_fallbacks_total",
            [] { return g_shard_state ? g_shard_state->stats.load_aware_fallbacks : 0UL; },
            seastar::metrics::description("Total number of requests diverted to less-loaded backends due to queue depth")),

        // ====================================================================
        // Local Route Batching Metrics
        // ====================================================================

        // Counter: routes buffered into local batch (before dedup/broadcast)
        seastar::metrics::make_counter("router_local_routes_batched_total",
            [] { return g_shard_state ? g_shard_state->stats.local_routes_batched : 0UL; },
            seastar::metrics::description("Total number of locally-learned routes buffered for batch broadcast")),

        // Counter: flush operations performed
        seastar::metrics::make_counter("router_local_batch_flushes_total",
            [] { return g_shard_state ? g_shard_state->stats.local_batch_flushes : 0UL; },
            seastar::metrics::description("Total number of local route batch flush operations")),

        // Counter: routes eliminated by dedup within batches
        seastar::metrics::make_counter("router_local_routes_deduplicated_total",
            [] { return g_shard_state ? g_shard_state->stats.local_routes_deduplicated : 0UL; },
            seastar::metrics::description("Total number of duplicate routes eliminated within local batches")),

        // Counter: routes skipped by ART lookup before buffering (§21.7)
        seastar::metrics::make_counter("router_routes_deduplicated_total",
            [] { return g_shard_state ? g_shard_state->stats.routes_deduplicated_pre_buffer : 0UL; },
            seastar::metrics::description("Total routes skipped because ART already contained the same prefix-backend mapping")),

        // Counter: routes dropped due to local buffer overflow
        seastar::metrics::make_counter("router_local_routes_dropped_overflow_total",
            [] { return g_shard_state ? g_shard_state->stats.local_routes_dropped_overflow : 0UL; },
            seastar::metrics::description("Total number of locally-learned routes dropped due to buffer overflow")),

        // Cross-shard load sync metrics
        // Counter: load snapshot broadcasts sent from this shard
        seastar::metrics::make_counter("router_load_sync_broadcasts_total",
            [] { return g_shard_state ? g_shard_state->stats.load_sync_broadcasts : 0UL; },
            seastar::metrics::description("Total number of cross-shard load snapshot broadcasts sent")),

        // Counter: load snapshots received from other shards
        seastar::metrics::make_counter("router_load_sync_snapshots_received_total",
            [] { return g_shard_state ? g_shard_state->stats.load_sync_snapshots_received : 0UL; },
            seastar::metrics::description("Total number of cross-shard load snapshots received from other shards")),

        // ====================================================================
        // GPU Load Integration Metrics
        // ====================================================================

        // Counter: GPU-load-driven redirects (subset of load_aware_fallbacks)
        seastar::metrics::make_counter("router_gpu_load_redirects_total",
            [] { return g_shard_state ? g_shard_state->stats.gpu_load_redirects : 0UL; },
            seastar::metrics::description("Total GPU-load-driven routing redirects (vLLM metrics influenced decision)")),

        // Gauge: GPU load cache staleness in seconds
        seastar::metrics::make_gauge("router_gpu_load_cache_age_seconds",
            seastar::metrics::description("Age of GPU load cache in seconds (staleness indicator)"),
            [] {
                if (!g_shard_state) return 0.0;
                auto& cache = g_shard_state->gpu_load_cache;
                if (cache.scores.empty()) return 0.0;
                auto age = std::chrono::steady_clock::now() - cache.updated_at;
                return std::chrono::duration<double>(age).count();
            }),

        // Gauge: number of entries in GPU load cache
        seastar::metrics::make_gauge("router_gpu_load_cache_size",
            seastar::metrics::description("Number of entries in per-shard GPU load cache"),
            [] {
                if (!g_shard_state) return static_cast<double>(0);
                return static_cast<double>(g_shard_state->gpu_load_cache.scores.size());
            }),

        // ====================================================================
        // Cost-Based Routing Metrics
        // ====================================================================

        // Counter: cost-driven routing redirects (backend changed due to cost budget)
        seastar::metrics::make_counter("router_cost_redirects_total",
            [] { return g_shard_state ? g_shard_state->stats.cost_redirects : 0UL; },
            seastar::metrics::description("Total cost-driven routing redirects")),

        // Counter: small requests routed via fast lane
        seastar::metrics::make_counter("router_fast_lane_total",
            [] { return g_shard_state ? g_shard_state->stats.fast_lane_routes : 0UL; },
            seastar::metrics::description("Total small requests routed via cost fast lane")),

        // Counter: no budget available anywhere (best-effort fallthrough)
        seastar::metrics::make_counter("router_budget_exhausted_total",
            [] { return g_shard_state ? g_shard_state->stats.budget_exhausted : 0UL; },
            seastar::metrics::description("Total times all backends exceeded cost budget")),

        // Counter: total cost reserved across all backends
        seastar::metrics::make_counter("router_cost_reserved_total",
            [] { return g_shard_state ? g_shard_state->stats.cost_reserved_total : 0UL; },
            seastar::metrics::description("Total cost budget reservations made")),

        // Counter: total cost released across all backends
        seastar::metrics::make_counter("router_cost_released_total",
            [] { return g_shard_state ? g_shard_state->stats.cost_released_total : 0UL; },
            seastar::metrics::description("Total cost budget releases made"))

        // Note: radix_tree_average_prefix_skip_length gauge is registered in MetricsService
        // since it aggregates path compression data across all lookups via record_prefix_skip()
    });
}

seastar::future<> RouterService::initialize_shards() {
    // Initialize ShardLocalState on all other shards with the config from shard 0
    // Shard 0 is already initialized in the constructor
    RoutingConfig cfg = _config;  // Copy config for cross-shard transfer
    bool has_gossip = (_gossip != nullptr);

    return seastar::parallel_for_each(boost::irange(1u, seastar::smp::count),
        [cfg, has_gossip](unsigned shard_id) {
            return seastar::smp::submit_to(shard_id, [cfg, has_gossip] {
                // Initialize ShardLocalState with unified init() method
                // (includes cost routing config via RoutingConfig::cost_routing)
                g_shard_state = std::make_unique<ShardLocalState>();
                g_shard_state->init(cfg);
                // Set gossip_enabled flag on all shards (gossip_ptr only on shard 0)
                g_shard_state->gossip_enabled = has_gossip;
                return seastar::make_ready_future<>();
            });
        }).then([this] {
            // Set up shard 0's gossip state (shard 0 was initialized in constructor)
            if (g_shard_state) {
                g_shard_state->gossip_enabled = (_gossip != nullptr);
                g_shard_state->gossip_ptr = _gossip.get();
            }

            // Start per-shard local batch flush timers on ALL shards
            // Pass the configured flush interval (from RoutingConfig, consistent across shards)
            auto interval = _config.route_batch_flush_interval;
            return seastar::smp::invoke_on_all([interval] {
                start_local_batch_timer(interval);
            });
        }).then([] {
            // Start per-shard cross-shard load sync timers on ALL shards
            return seastar::smp::invoke_on_all([] {
                start_load_sync_timer();
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
    // Stop per-shard local batch flush timers first
    // ==========================================================================
    //
    // Local batch timers are shard-local (in ShardLocalState), not owned by
    // RouterService. Stop them on all shards before proceeding with the rest
    // of the shutdown sequence. This ensures no new cross-shard broadcasts
    // are initiated from local batch flushes during shutdown.
    //
    return seastar::smp::invoke_on_all([] {
        return stop_local_batch_timer();
    }).then([] {
        // ==========================================================================
        // Stop per-shard cross-shard load sync timers
        // ==========================================================================
        //
        // Load sync timers are shard-local (in ShardLocalState). Stop them on all
        // shards before proceeding to prevent stale cross-shard SMP messages during
        // shutdown.
        //
        return seastar::smp::invoke_on_all([] {
            return stop_load_sync_timer();
        });
    }).then([this] {
        // ==========================================================================
        // Rule #5: Close timer gate before cancelling timers
        // ==========================================================================
        //
        // Timer callbacks capture 'this'. If a callback is already queued when we
        // cancel the timer, it may still execute with a dangling pointer. The gate
        // ensures we wait for any in-flight callbacks to complete before proceeding.
        //
        return _timer_gate.close();
    }).then([this] {
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

// Rule #17: Yielding TTL cleanup coroutine for radix_tree operations.
// Named coroutine (not lambda) to avoid Rule #16 Lambda Coroutine Fiasco
// when called from smp::submit_to. Yields between tree phases so each
// phase's synchronous traversal runs in its own reactor timeslice.
seastar::future<> RouterService::ttl_cleanup_on_shard(
    std::chrono::steady_clock::time_point cutoff) {
    if (!g_shard_state) co_return;
    auto& state = shard_state();
    RadixTree* tree = state.tree.get();
    if (!tree) co_return;

    // Phase 1: Expire old routes (marks leaves as empty)
    size_t removed = tree->remove_expired(cutoff);
    if (removed > 0) {
        state.stats.routes_expired += removed;
        log_main.debug("Shard {}: Expired {} routes", seastar::this_shard_id(), removed);
    }

    // Yield between phases to bound per-continuation CPU time
    co_await seastar::coroutine::maybe_yield();

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
            // Rule #17: ttl_cleanup_on_shard is a named coroutine that yields
            // between tree phases to avoid reactor stalls on large trees.
            // Using a named function (not a coroutine lambda) avoids Rule #16.
            return seastar::smp::submit_to(shard_id, [cutoff] {
                return RouterService::ttl_cleanup_on_shard(cutoff);
            });
        });
    }).handle_exception([](std::exception_ptr ep) {
        // Rule #9: every catch must log at warn level
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            log_main.warn("TTL cleanup failed: {}", e.what());
        }
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
            // Record prefix hit by compression tier
            auto bi = state.backends.find(result.value());
            double cr = (bi != state.backends.end()) ? bi->second.compression_ratio : 1.0;
            metrics().record_prefix_hit_by_compression_tier(cr);
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
                                         size_t prefix_boundary,
                                         double estimated_cost) {
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
        auto prefix_result = get_backend_for_prefix(tokens, request_id, prefix_boundary, estimated_cost);

        if (prefix_result.backend_id.has_value()) {
            result.backend_id = prefix_result.backend_id.value();
            result.cache_hit = prefix_result.art_hit;
            result.was_load_redirect = prefix_result.was_load_redirect;
            result.backend_load_at_decision = prefix_result.backend_load_at_decision;
            result.was_cost_redirect = prefix_result.was_cost_redirect;
            result.was_fast_lane = prefix_result.was_fast_lane;
            result.backend_cost_at_decision = prefix_result.backend_cost_at_decision;
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

std::optional<seastar::socket_address> RouterService::get_backend_address(BackendId id) const {
    if (!g_shard_state) return std::nullopt;
    auto& state = shard_state();
    auto it = state.backends.find(id);
    if (it != state.backends.end()) {
        return it->second.addr;
    }
    return std::nullopt;
}

bool RouterService::backend_supports_token_ids(BackendId id) const {
    if (!g_shard_state) return false;
    auto& state = shard_state();
    auto it = state.backends.find(id);
    if (it != state.backends.end()) {
        return it->second.supports_token_ids;
    }
    // Backend not found — safe default: don't inject unknown fields
    return false;
}

std::optional<BackendId> RouterService::get_random_backend() {
    if (!g_shard_state) return std::nullopt;
    auto& state = shard_state();

    if (state.backend_ids.empty()) {
        return std::nullopt;
    }

    auto live_infos = state.get_live_backend_infos();

    // Pass 1: find the highest priority group (lowest priority number)
    // and accumulate its total weight — no heap allocation needed.
    uint32_t min_priority = std::numeric_limits<uint32_t>::max();
    for (const auto& [id, info] : live_infos) {
        if (info->weight > 0 && info->priority < min_priority) {
            min_priority = info->priority;
        }
    }

    if (min_priority == std::numeric_limits<uint32_t>::max()) {
        return std::nullopt;  // No live backends with weight > 0
    }

    uint64_t total_weight = 0;
    for (const auto& [id, info] : live_infos) {
        if (info->priority == min_priority && info->weight > 0) {
            total_weight += info->weight;
        }
    }

    // Pass 2: weighted random selection over the highest-priority candidates
    std::uniform_int_distribution<uint64_t> dist(0, total_weight - 1);
    uint64_t roll = dist(state.rng);

    uint64_t cumulative = 0;
    BackendId last_candidate = 0;
    for (const auto& [id, info] : live_infos) {
        if (info->priority != min_priority || info->weight == 0) continue;
        cumulative += info->weight;
        last_candidate = id;
        if (roll < cumulative) {
            return id;
        }
    }

    // Fallback (shouldn't reach here)
    return last_candidate;
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
PrefixRouteResult RouterService::get_backend_for_prefix(const std::vector<int32_t>& tokens,
                                                         const std::string& request_id,
                                                         size_t prefix_boundary,
                                                         double estimated_cost) {
    if (!g_shard_state) return {};
    auto& state = shard_state();

    if (state.backend_ids.empty()) {
        return {};
    }

    auto live_backends = state.get_live_backends();
    if (live_backends.empty()) {
        return {};
    }

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
        // No tokens to route on, fall back to first backend (not an ART hit)
        return {live_backends[0], false};
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

    // Track which branch selected the backend for the single load-aware call below
    BackendId selected = 0;
    bool art_hit = false;
    const char* source = "hash";
    // Hash values only computed in the hash fallback path; declared here for debug logging
    uint64_t prefix_hash = 0;
    int32_t hash_index = 0;

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
                    // Record prefix hit by compression tier
                    auto bi = state.backends.find(art_backend);
                    double cr = (bi != state.backends.end()) ? bi->second.compression_ratio : 1.0;
                    metrics().record_prefix_hit_by_compression_tier(cr);
                }

                selected = art_backend;
                art_hit = true;
                source = "ART";
            } else {
                // Backend is dead/draining, fall through to hash-based selection
                log_router.debug("[{}] ART backend {} is unavailable, using hash fallback",
                                 request_id, art_backend);
            }
        }
    }

    // Step 2: No ART match (or backend unavailable) — hash fallback.
    // Dispatch to configured hash strategy.
    if (!art_hit) {
        prefix_hash = hash_prefix(tokens.data(), prefix_len, state.config.block_alignment);

        switch (state.config.hash_strategy) {
        case RoutingConfig::HashStrategy::BOUNDED_LOAD:
            // Bounded-load has built-in load awareness — no separate step 3 needed.
            // estimated_cost enables capacity-aware fallback when headroom data is available.
            selected = bounded_load_select(prefix_hash, live_backends,
                                           state.config.bounded_load_epsilon, request_id,
                                           estimated_cost);
            break;

        case RoutingConfig::HashStrategy::P2C: {
            // Use FNV-1a of request_id as salt (or prefix_hash rotated if no request_id)
            uint64_t salt = prefix_hash;
            if (!request_id.empty()) {
                salt = FNV_OFFSET_BASIS;
                for (char c : request_id) {
                    salt ^= static_cast<uint8_t>(c);
                    salt *= FNV_PRIME;
                }
            }
            selected = p2c_select(prefix_hash, salt, live_backends,
                                  state.config.p2c_load_bias, request_id,
                                  estimated_cost);
            break;
        }

        case RoutingConfig::HashStrategy::MODULAR:
            // Simple modular hash — for benchmarking baseline only
            selected = live_backends[static_cast<int32_t>(prefix_hash %
                           static_cast<uint64_t>(live_backends.size()))];
            break;

        case RoutingConfig::HashStrategy::JUMP:
        default:
            // Original jump consistent hash
            hash_index = jump_consistent_hash(prefix_hash, static_cast<int32_t>(live_backends.size()));
            selected = live_backends[hash_index];
            break;
        }

        // Track whether cache headroom influenced the selection.
        // Compare capacity-adjusted load with base composite load for selected backend:
        // if they differ, headroom data contributed to the decision.
        if (estimated_cost > 0.0 && state.config.capacity_headroom_weight > 0.0) {
            uint64_t base_load = get_composite_backend_load(selected);
            uint64_t adj_load = get_capacity_adjusted_load(selected, estimated_cost);
            if (adj_load != base_load) {
                state.stats.headroom_redirects++;
                if (g_metrics) {
                    metrics().record_headroom_redirect();
                }
            }
        }

        // This is a cache miss — the route will be learned after successful response
        state.stats.cache_misses++;
        state.stats.prefix_affinity_routes++;
        if (g_metrics) {
            metrics().record_cache_miss();
        }
    }

    // Step 3: Apply load-aware override.
    // BOUNDED_LOAD and P2C have load awareness built in for both ART and hash paths.
    // JUMP and MODULAR use the legacy median-based threshold.
    BackendId final_backend = selected;
    switch (state.config.hash_strategy) {
    case RoutingConfig::HashStrategy::BOUNDED_LOAD: {
        // For ART hits under bounded-load, check if the ART-selected backend
        // exceeds the capacity cap. If so, fall back to bounded-load selection.
        if (art_hit) {
            uint64_t total_load = 0;
            for (BackendId id : live_backends) {
                total_load += get_capacity_adjusted_load(id, estimated_cost);
            }
            double avg = static_cast<double>(total_load) / static_cast<double>(live_backends.size());
            uint64_t cap = std::max(static_cast<uint64_t>(1),
                                    static_cast<uint64_t>(std::ceil(avg * (1.0 + state.config.bounded_load_epsilon))));
            uint64_t sel_load = get_capacity_adjusted_load(selected, estimated_cost);
            if (sel_load >= cap) {
                prefix_hash = hash_prefix(tokens.data(), prefix_len, state.config.block_alignment);
                final_backend = bounded_load_select(prefix_hash, live_backends,
                                                    state.config.bounded_load_epsilon, request_id,
                                                    estimated_cost);
            }
        }
        // Hash path: already handled in step 2
        break;
    }
    case RoutingConfig::HashStrategy::P2C:
        // For ART hits under P2C, check if ART-selected backend is overloaded
        // vs a random alternative. Use same bias threshold.
        if (art_hit && live_backends.size() >= 2) {
            auto [least_id, least_load] = get_least_loaded_backend(live_backends);
            uint64_t sel_load = get_composite_backend_load(selected);
            if (least_id != 0 && least_load + state.config.p2c_load_bias < sel_load) {
                final_backend = least_id;
                g_shard_state->stats.load_aware_fallbacks++;
                if (g_metrics) {
                    metrics().record_load_aware_fallback();
                }
                log_router.debug("[{}] P2C (ART override): backend {} has {} in-flight, "
                                 "least-loaded backend {} has {} (bias={})",
                                 request_id, selected, sel_load, least_id, least_load,
                                 state.config.p2c_load_bias);
            }
        }
        break;
    case RoutingConfig::HashStrategy::JUMP:
    case RoutingConfig::HashStrategy::MODULAR:
    default:
        // Legacy median-based load-aware override for both ART and hash paths
        final_backend = apply_load_aware_selection(selected, live_backends, request_id, source);
        break;
    }

    // Track GPU-load-driven redirect for observability.
    // Note: This is a heuristic — we check if GPU metrics existed for the original
    // backend when a redirect happened, but the redirect may have been caused by
    // local load alone. The metric is useful for "are GPU metrics influencing
    // routing?" not precise per-redirect attribution.
    bool was_load_redirect = false;
    double load_at_decision = 0.0;
    if (final_backend != selected) {
        auto gpu_score = get_cached_gpu_load(selected);
        if (gpu_score >= 0.0) {
            // GPU metrics were present when this redirect occurred
            was_load_redirect = true;
            load_at_decision = gpu_score;
            g_shard_state->stats.gpu_load_redirects++;
            log_router.debug("[{}] GPU load redirect: backend {} overloaded (gpu_load={:.2f}), "
                             "redirected to backend {}",
                             request_id, selected, gpu_score, final_backend);
        }
    }

    // ====================================================================
    // Step 4: Cost-aware override
    // ====================================================================
    // Only runs when cost_routing.enabled is true and estimated_cost > 0.
    // Overlays on top of Steps 1-3 — does NOT modify ART or hash strategies.
    //
    // 4a. Small request fast lane: route cheap requests to least-cost backend
    // 4b. Large request budget check: avoid overloading already expensive backends
    // Note: cost budget reservation is handled by CostBudgetGuard in http_controller
    bool was_cost_redirect = false;
    bool was_fast_lane = false;
    double cost_at_decision = 0.0;

    if (g_shard_state && state.config.cost_routing_enabled && estimated_cost > 0.0) {
        double selected_cost = 0.0;
        double selected_compression = 1.0;
        auto sel_it = state.backends.find(final_backend);
        if (sel_it != state.backends.end()) {
            selected_cost = sel_it->second.current_cost_budget;
            selected_compression = sel_it->second.compression_ratio;
        }
        double max_budget = state.config.cost_routing_max_cost;
        cost_at_decision = selected_cost;

        // 4a. Small request fast lane
        if (estimated_cost < state.config.cost_routing_small_threshold &&
            state.config.cost_routing_fast_lane) {

            auto [least_cost_id, least_cost] = get_least_cost_backend(live_backends);
            if (least_cost_id != 0 && least_cost_id != final_backend &&
                selected_cost > least_cost * state.config.cost_routing_imbalance_factor) {
                BackendId prev = final_backend;
                final_backend = least_cost_id;
                was_cost_redirect = true;
                was_fast_lane = true;
                cost_at_decision = get_backend_cost(final_backend);
                state.stats.cost_redirects++;
                state.stats.fast_lane_routes++;
                log_router.debug("[{}] Cost fast lane: backend {} cost={:.1f} > {:.1f}*{:.1f}, "
                                 "redirected small request (cost={:.1f}) to backend {} (cost={:.1f})",
                                 request_id, prev, selected_cost,
                                 least_cost, state.config.cost_routing_imbalance_factor,
                                 estimated_cost, least_cost_id, least_cost);
            }
        }
        // 4b. Large request budget check
        // Scale budget ceiling by selected backend's compression ratio:
        // compressed backends have more effective KV-cache capacity.
        else {
            double effective_max = max_budget * selected_compression;
            if (selected_cost + estimated_cost > effective_max) {
                auto alt = find_backend_with_budget(live_backends, estimated_cost,
                                                    max_budget, final_backend);
                if (alt.has_value()) {
                    BackendId prev = final_backend;
                    final_backend = *alt;
                    was_cost_redirect = true;
                    cost_at_decision = get_backend_cost(final_backend);
                    state.stats.cost_redirects++;
                    log_router.debug("[{}] Cost budget redirect: backend {} budget={:.1f} + cost={:.1f} > max={:.1f}, "
                                     "redirected to backend {} (budget={:.1f})",
                                     request_id, prev, selected_cost, estimated_cost, effective_max,
                                     *alt, get_backend_cost(*alt));
                } else {
                    // No backend has budget — route anyway (best effort, advisory budget)
                    state.stats.budget_exhausted++;
                    log_router.debug("[{}] Cost budget exhausted: all backends over budget, "
                                     "routing to backend {} anyway (best effort)",
                                     request_id, final_backend);
                }
            }
        } // else (large request path)
    }

    if (!request_id.empty() && final_backend == selected && !was_cost_redirect) {
        if (art_hit) {
            log_router.debug("[{}] Prefix affinity (ART hit): {} tokens -> backend {}",
                             request_id, tokens.size(), selected);
        } else {
            log_router.debug("[{}] Prefix affinity (hash): {} tokens, hash={}, index={}/{} -> backend {}",
                             request_id, prefix_len, prefix_hash, hash_index, live_backends.size(), selected);
        }
    }

    return {final_backend, art_hit, load_at_decision, was_load_redirect,
            was_cost_redirect, was_fast_lane, cost_at_decision};
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

    auto live_backends = state.get_live_backends();
    if (live_backends.empty()) {
        return std::nullopt;
    }

    // Extract prefix (first N tokens)
    size_t prefix_len = std::min(tokens.size(), state.config.prefix_token_length);
    if (prefix_len == 0) {
        return live_backends[0];
    }

    // Consistent hash on prefix tokens — dispatch to configured strategy
    uint64_t prefix_hash = hash_prefix(tokens.data(), prefix_len, state.config.block_alignment);
    BackendId selected;

    switch (state.config.hash_strategy) {
    case RoutingConfig::HashStrategy::BOUNDED_LOAD:
        selected = bounded_load_select(prefix_hash, live_backends,
                                       state.config.bounded_load_epsilon, request_id);
        break;

    case RoutingConfig::HashStrategy::P2C: {
        uint64_t salt = prefix_hash;
        if (!request_id.empty()) {
            salt = FNV_OFFSET_BASIS;
            for (char c : request_id) {
                salt ^= static_cast<uint8_t>(c);
                salt *= FNV_PRIME;
            }
        }
        selected = p2c_select(prefix_hash, salt, live_backends,
                              state.config.p2c_load_bias, request_id);
        break;
    }

    case RoutingConfig::HashStrategy::MODULAR:
        selected = live_backends[static_cast<int32_t>(prefix_hash %
                       static_cast<uint64_t>(live_backends.size()))];
        break;

    case RoutingConfig::HashStrategy::JUMP:
    default: {
        int32_t index = jump_consistent_hash(prefix_hash, static_cast<int32_t>(live_backends.size()));
        selected = live_backends[index];
        break;
    }
    }

    if (!request_id.empty()) {
        log_router.debug("[{}] Hash routing: {} tokens, hash={} -> backend {}",
                         request_id, prefix_len, prefix_hash, selected);
    }

    // No ART involvement - no cache_hit/miss tracking for stats
    // (hash mode is for measuring baseline, not production metrics)
    return selected;
}

// Forward declaration: defined in Local Route Batching Implementation section below.
static bool push_local_route(ShardLocalState& state, std::vector<int32_t> tokens, BackendId backend);

// ============================================================================
// learn_route_global() - Batched Local Route Learning
// ============================================================================
//
// Instead of immediately broadcasting to all shards (O(shards) SMP messages
// per request), this function buffers the route in the shard-local
// pending_local_routes buffer. The per-shard flush timer (or buffer-full
// trigger) drains the buffer and broadcasts in a single batch.
//
// This reduces SMP message traffic from O(shards) per request to
// O(shards) per batch, dramatically reducing SMP contention under load.
//
// Validation (prefix truncation, max_route_tokens) is still performed
// inline before buffering. Gossip and cross-shard broadcast are deferred
// to flush_local_route_batch().
//
seastar::future<bool> RouterService::learn_route_global(std::vector<int32_t> tokens, BackendId backend,
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
    // Use shard-local config for lock-free, hot-reloadable access.
    // _config is a member on shard 0 — reading it from shard N would be a cross-shard access.
    if (!g_shard_state) return seastar::make_ready_future<bool>(false);
    const auto& cfg = g_shard_state->config;
    auto& state = shard_state();

    size_t effective_prefix_len;
    bool used_shared_prefix = false;
    if (prefix_boundary > 0 && prefix_boundary <= tokens.size()) {
        effective_prefix_len = prefix_boundary;
        used_shared_prefix = true;
    } else {
        effective_prefix_len = std::min(tokens.size(), cfg.prefix_token_length);
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
    if (cfg.max_route_tokens > 0 && tokens.size() > cfg.max_route_tokens) {
        log_router.warn("Route rejected: {} tokens exceeds limit {} (backend={}, request={})",
                        tokens.size(), cfg.max_route_tokens, backend,
                        request_id.empty() ? "N/A" : request_id);
        return seastar::make_ready_future<bool>(false);
    }

    // ========================================================================
    // Deduplication: skip if ART already maps this prefix to the same backend
    // ========================================================================
    // O(k) shard-local lookup — no SMP message, no future. Benign TOCTOU
    // race: another shard could evict between check and insert, but redundant
    // inserts are safe. The goal is eliminating the common case (§21.7).
    RadixTree* tree = state.tree.get();
    if (tree) {
        std::span<const TokenId> token_span(tokens.data(), tokens.size());
        auto existing = tree->lookup(token_span);
        if (existing.has_value() && existing.value() == backend) {
            state.stats.routes_deduplicated_pre_buffer++;
            log_router.debug("[{}] Route dedup: prefix ({} tokens) -> backend {} already in ART, skipping",
                             request_id.empty() ? "N/A" : request_id, tokens.size(), backend);
            return seastar::make_ready_future<bool>(false);
        }
    }

    // Log route learning with request_id before buffering
    if (!request_id.empty()) {
        if (used_shared_prefix) {
            log_router.info("[{}] Buffering route: {} tokens (shared_prefix) -> backend {}",
                            request_id, tokens.size(), backend);
        } else {
            log_router.info("[{}] Buffering route: {} tokens (default_prefix) -> backend {}",
                            request_id, tokens.size(), backend);
        }
    }

    // Buffer the route for batched broadcast (no SMP messages on hot path)
    return buffer_local_route(std::move(tokens), backend).then([] { return true; });
}

seastar::future<> RouterService::learn_route_global_multi(std::vector<int32_t> tokens, BackendId backend,
                                                           const std::string& request_id,
                                                           const std::vector<size_t>& prefix_boundaries) {
    // Multi-depth route learning: store routes at each provided boundary
    // This enables cache reuse at any conversation depth (Option C)
    //
    // Batched version: Instead of broadcasting each boundary to all shards
    // immediately, buffer all boundaries into the shard-local pending_local_routes
    // buffer. The per-shard flush timer handles cross-shard broadcast and gossip.

    if (!g_shard_state) return seastar::make_ready_future<>();

    // Use shard-local config for lock-free, hot-reloadable access (same as learn_route_global)
    const size_t max_route_tokens = g_shard_state->config.max_route_tokens;

    if (prefix_boundaries.empty()) {
        // No boundaries provided - fall back to single-depth learning
        return learn_route_global(std::move(tokens), backend, request_id, 0).discard_result();
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
        return learn_route_global(std::move(tokens), backend, request_id, 0).discard_result();
    }

    // Log multi-depth learning
    if (!request_id.empty()) {
        std::string boundaries_str;
        for (size_t i = 0; i < valid_boundaries.size(); ++i) {
            if (i > 0) boundaries_str += ",";
            boundaries_str += std::to_string(valid_boundaries[i]);
        }
        log_router.info("[{}] Buffering multi-depth routes: {} boundaries [{}] -> backend {}",
                        request_id, valid_boundaries.size(), boundaries_str, backend);
    }

    // Record metric for multi-depth routes
    auto& state = shard_state();
    state.stats.multi_depth_routes_stored += valid_boundaries.size();

    // Push all boundaries into the shard-local buffer directly.
    // This is O(1) per push (shard-local vector append, no futures).
    // The flush timer or buffer-full check handles cross-shard broadcast.
    bool needs_flush = false;
    for (size_t boundary : valid_boundaries) {
        // Validate token count (using shard-local config)
        if (max_route_tokens > 0 && boundary > max_route_tokens) {
            continue;  // Skip this boundary if it exceeds the limit
        }

        std::vector<int32_t> prefix(tokens.begin(), tokens.begin() + boundary);
        needs_flush = push_local_route(state, std::move(prefix), backend);
    }

    // Trigger flush only once after all pushes (not per-boundary)
    if (needs_flush) {
        return flush_local_route_batch();
    }

    return seastar::make_ready_future<>();
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
    // Rule 22: coroutine converts any pre-future throw into a failed future
    // ========================================================================
    // Business-Layer Token Count Validation (Remote Routes)
    // ========================================================================
    // Validate remote routes before buffering. Remote peers should also enforce
    // this limit, but we validate defensively to reject malformed gossip messages.
    // Use shard-local config for consistency with hot-reload (same pattern as learn_route_global).
    const size_t max_route_tokens = g_shard_state ? g_shard_state->config.max_route_tokens : 0;
    if (max_route_tokens > 0 && tokens.size() > max_route_tokens) {
        log_router.warn("Remote route rejected: {} tokens exceeds limit {} (backend={})",
                        tokens.size(), max_route_tokens, backend);
        co_return;
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
        co_await flush_route_batch();
    }
}

seastar::future<> RouterService::start_gossip() {
    if (!_gossip) {
        co_return;
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
        co_return;
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

    _batch_flush_timer.arm_periodic(_config.route_batch_flush_interval);
    log_router.info("Route batch flush timer started (interval: {}ms, max_batch: {}, max_buffer: {})",
                    _config.route_batch_flush_interval.count(),
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

// ============================================================================
// Local Route Batching Implementation
// ============================================================================
//
// Locally-learned routes (from proxied requests) are buffered per-shard and
// flushed periodically, eliminating the O(shards) SMP messages that
// learn_route_global() previously sent on every request.
//
// Each shard independently:
//   1. Accumulates routes in pending_local_routes (shard-local, no locks)
//   2. Flushes on timer tick or when buffer is full
//   3. Deduplicates within the batch (same tokens+backend → skip)
//   4. Applies batch to local tree directly (no SMP for self)
//   5. Broadcasts batch to all other shards via parallel_for_each
//   6. Submits batch to shard 0 for gossip (if enabled)

// Apply a batch of locally-learned routes to the local shard's RadixTree.
// Similar to apply_route_batch_to_local_tree but uses LOCAL origin.
static void apply_local_batch_to_tree(const std::vector<PendingLocalRoute>& batch) {
    if (!g_shard_state) return;
    auto& state = shard_state();
    RadixTree* tree = state.tree.get();
    if (!tree) return;

    for (const auto& route : batch) {
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
        tree->insert(route.tokens, route.backend, RouteOrigin::LOCAL);
    }
}

// FNV-1a hash over raw token bytes (no block alignment, used only for dedup)
static uint64_t hash_tokens_for_dedup(const std::vector<int32_t>& tokens) {
    uint64_t hash = FNV_OFFSET_BASIS;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(tokens.data());
    size_t byte_len = tokens.size() * sizeof(int32_t);
    for (size_t i = 0; i < byte_len; ++i) {
        hash ^= data[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

// Dedup key: combines token hash with backend ID
struct LocalRouteDedup {
    uint64_t token_hash;
    BackendId backend;

    bool operator==(const LocalRouteDedup& o) const {
        return token_hash == o.token_hash && backend == o.backend;
    }

    template <typename H>
    friend H AbslHashValue(H h, const LocalRouteDedup& k) {
        return H::combine(std::move(h), k.token_hash, k.backend);
    }
};

// Remove duplicate (same prefix + same backend) entries from a local batch.
// Keeps the first occurrence of each unique (tokens, backend) pair.
// Uses FNV-1a hash of tokens as proxy for equality (collision probability
// is negligible for the bounded batch sizes we operate on).
static size_t deduplicate_local_batch(std::vector<PendingLocalRoute>& batch) {
    if (batch.size() <= 1) return 0;

    absl::flat_hash_set<LocalRouteDedup> seen;
    seen.reserve(batch.size());
    size_t write = 0;
    for (size_t read = 0; read < batch.size(); ++read) {
        LocalRouteDedup key{hash_tokens_for_dedup(batch[read].tokens), batch[read].backend};
        if (seen.insert(key).second) {
            if (write != read) {
                batch[write] = std::move(batch[read]);
            }
            ++write;
        }
    }
    size_t removed = batch.size() - write;
    batch.resize(write);
    return removed;
}

// Push a route into the shard-local buffer, handling overflow if needed.
// Returns true if the buffer is now full and the caller should trigger a flush.
// This is the single point of truth for overflow+push logic, used by both
// buffer_local_route() and learn_route_global_multi().
static bool push_local_route(ShardLocalState& state, std::vector<int32_t> tokens, BackendId backend) {
    // Enforce hard buffer limit to prevent OOM (Rule #4: bounded containers)
    // Strategy: batch-drop oldest routes (same pattern as remote route batching)
    if (state.pending_local_routes.size() >= RouteBatchConfig::MAX_BUFFER_SIZE) {
        size_t drop_count = std::min(RouteBatchConfig::OVERFLOW_DROP_COUNT,
                                     state.pending_local_routes.size());
        state.pending_local_routes.erase(
            state.pending_local_routes.begin(),
            state.pending_local_routes.begin() + static_cast<ptrdiff_t>(drop_count));
        state.stats.local_routes_dropped_overflow += drop_count;

        log_router.warn("Shard {}: Local route buffer overflow, dropped {} oldest routes "
                       "(total dropped: {})",
                       seastar::this_shard_id(), drop_count,
                       state.stats.local_routes_dropped_overflow);
    }

    state.pending_local_routes.push_back(PendingLocalRoute{std::move(tokens), backend});
    state.stats.local_routes_batched++;

    return state.pending_local_routes.size() >= RouteBatchConfig::MAX_BATCH_SIZE;
}

seastar::future<> RouterService::buffer_local_route(std::vector<int32_t> tokens, BackendId backend) {
    // Rule 22: coroutine converts any pre-future throw into a failed future
    if (!g_shard_state) co_return;
    auto& state = shard_state();

    if (push_local_route(state, std::move(tokens), backend)) {
        log_router.debug("Shard {}: Local batch buffer full ({} routes), triggering immediate flush",
                         seastar::this_shard_id(), state.pending_local_routes.size());
        co_await flush_local_route_batch();
    }
}

seastar::future<> RouterService::flush_local_route_batch() {
    if (!g_shard_state) return seastar::make_ready_future<>();
    auto& state = shard_state();

    // Rule #5: Acquire shard-local gate holder to prevent use-after-free during shutdown
    seastar::gate::holder holder;
    try {
        holder = state.local_flush_gate.hold();
    } catch (const seastar::gate_closed_exception&) {
        return seastar::make_ready_future<>();
    }

    if (state.pending_local_routes.empty()) {
        return seastar::make_ready_future<>();
    }

    // Atomically take ownership of pending routes.
    // Re-reserve immediately so the buffer doesn't regrow through multiple
    // reallocations (0→1→2→4→...→128) on the next fill cycle.
    auto batch = std::move(state.pending_local_routes);
    state.pending_local_routes.reserve(RouteBatchConfig::MAX_BATCH_SIZE);
    state.stats.local_batch_flushes++;

    // Deduplicate: same (token_hash, backend) → keep first, skip rest
    size_t deduped = deduplicate_local_batch(batch);
    state.stats.local_routes_deduplicated += deduped;

    log_router.debug("Shard {}: Flushing local batch of {} routes ({} deduplicated) to {} shards",
                     seastar::this_shard_id(), batch.size(), deduped, seastar::smp::count);

    // Apply to local tree directly (no SMP needed for THIS shard)
    apply_local_batch_to_tree(batch);

    bool should_gossip = state.gossip_enabled;
    unsigned this_shard = seastar::this_shard_id();

    // Broadcast batch to all other shards + gossip via shard 0
    //
    // CRITICAL MEMORY SAFETY (Rule #14): Use foreign_ptr to safely pass data
    // across shards. The batch is allocated on the calling shard. Each target
    // shard receives a foreign_ptr-wrapped copy, reads from it, and creates
    // LOCAL allocations. foreign_ptr destructor returns cleanup to source shard.
    return seastar::do_with(std::move(holder), std::move(batch),
        [should_gossip, this_shard](seastar::gate::holder&, std::vector<PendingLocalRoute>& batch) {
            // Broadcast to all OTHER shards
            auto shard_future = seastar::parallel_for_each(
                boost::irange(0u, seastar::smp::count),
                [&batch, this_shard](unsigned shard_id) {
                    if (shard_id == this_shard) {
                        // Already applied to local tree above
                        return seastar::make_ready_future<>();
                    }

                    // Wrap batch copy in foreign_ptr for safe cross-shard transfer
                    auto batch_ptr = std::make_unique<std::vector<PendingLocalRoute>>(batch);
                    auto foreign_batch = seastar::make_foreign(std::move(batch_ptr));

                    return seastar::smp::submit_to(shard_id,
                        [foreign_batch = std::move(foreign_batch)]() mutable {
                            // Force local allocation on THIS shard by copying from foreign_ptr
                            std::vector<PendingLocalRoute> local_batch;
                            local_batch.reserve(foreign_batch->size());
                            for (const auto& route : *foreign_batch) {
                                local_batch.push_back(PendingLocalRoute{
                                    std::vector<int32_t>(route.tokens.begin(), route.tokens.end()),
                                    route.backend
                                });
                            }
                            apply_local_batch_to_tree(local_batch);
                            return seastar::make_ready_future<>();
                        });
                });

            // Gossip: submit batch to shard 0 for cluster broadcast
            seastar::future<> gossip_future = seastar::make_ready_future<>();
            if (should_gossip && !batch.empty()) {
                auto gossip_batch = std::make_unique<std::vector<PendingLocalRoute>>(batch);
                auto foreign_gossip = seastar::make_foreign(std::move(gossip_batch));

                gossip_future = seastar::smp::submit_to(0,
                    [foreign_gossip = std::move(foreign_gossip)]() mutable {
                        if (!g_shard_state || !g_shard_state->gossip_ptr) {
                            return seastar::make_ready_future<>();
                        }
                        auto* gossip = g_shard_state->gossip_ptr;
                        if (!gossip->is_enabled()) {
                            return seastar::make_ready_future<>();
                        }

                        // Create local copies of each route and broadcast via gossip
                        // Use parallel_for_each to avoid sequential-await (Rule #2)
                        auto local_routes = std::make_unique<std::vector<PendingLocalRoute>>();
                        local_routes->reserve(foreign_gossip->size());
                        for (const auto& route : *foreign_gossip) {
                            local_routes->push_back(PendingLocalRoute{
                                std::vector<int32_t>(route.tokens.begin(), route.tokens.end()),
                                route.backend
                            });
                        }

                        return seastar::do_with(std::move(local_routes),
                            [gossip](std::unique_ptr<std::vector<PendingLocalRoute>>& routes) {
                                return seastar::parallel_for_each(
                                    routes->begin(), routes->end(),
                                    [gossip](const PendingLocalRoute& route) {
                                        return gossip->broadcast_route(route.tokens, route.backend);
                                    });
                            });
                    });
            }

            return seastar::when_all_succeed(
                std::move(shard_future), std::move(gossip_future)).discard_result();
        });
}

// ============================================================================
// Local Batch Flush Timer (per-shard)
// ============================================================================
//
// HARD RULE #5 (Timer Ownership):
// The timer callback accesses shard-local state (g_shard_state) to call
// flush_local_route_batch(). Safety is ensured by stop_local_batch_timer():
//   1. Closes local_flush_gate - waits for in-flight flushes to complete
//   2. Cancels timer - prevents new callbacks
//   3. Drains remaining buffer with one final flush
//

void RouterService::start_local_batch_timer(std::chrono::milliseconds flush_interval) {
    if (!g_shard_state) return;
    auto& state = shard_state();

    state.local_flush_timer.set_callback([] {
        // Timer callbacks can't return futures, so handle errors inline
        (void)flush_local_route_batch().handle_exception([](std::exception_ptr ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                log_router.error("Shard {}: Local route batch flush failed: {}",
                                 seastar::this_shard_id(), e.what());
            }
        });
    });

    state.local_flush_timer.arm_periodic(flush_interval);
    log_router.info("Shard {}: Local route batch flush timer started (interval: {}ms)",
                    seastar::this_shard_id(), flush_interval.count());
}

seastar::future<> RouterService::stop_local_batch_timer() {
    if (!g_shard_state) return seastar::make_ready_future<>();
    auto& state = shard_state();

    // Shutdown sequence:
    // 1. Close gate - waits for any in-flight flush callbacks to complete
    // 2. Cancel timer - prevents new callbacks
    // 3. Final flush - drains any remaining buffered routes
    return state.local_flush_gate.close().then([] {
        if (!g_shard_state) return seastar::make_ready_future<>();
        g_shard_state->local_flush_timer.cancel();

        // Final flush of any remaining routes (gate is closed, so flush_local_route_batch
        // will skip the gate check - we do a manual drain here instead)
        if (g_shard_state->pending_local_routes.empty()) {
            return seastar::make_ready_future<>();
        }

        log_router.info("Shard {}: Draining {} remaining local routes on shutdown",
                        seastar::this_shard_id(), g_shard_state->pending_local_routes.size());

        // Apply remaining routes to local tree only (skip cross-shard broadcast during shutdown)
        apply_local_batch_to_tree(g_shard_state->pending_local_routes);
        g_shard_state->pending_local_routes.clear();
        return seastar::make_ready_future<>();
    });
}

// ============================================================================
// Cross-Shard Load Synchronization (per-shard)
// ============================================================================
//
// Prevents cross-shard burst-routing hot-spotting by giving each shard a
// global view of backend load across all shards.
//
// Pattern: Each shard periodically snapshots its own active_requests map and
// broadcasts it to all other shards via fire-and-forget smp::submit_to.
// On receipt, each shard stores the snapshot indexed by source shard_id and
// recomputes the aggregated cross_shard_load map.
//
// The cross_shard_load map is read by get_backend_load() on the hot path,
// adding O(1) hash lookup overhead per routing decision.
//
// HARD RULE #5 (Timer Ownership):
// Timer callback accesses g_shard_state. Safety: stop_load_sync_timer() closes
// load_sync_gate before cancelling the timer, waiting for in-flight broadcasts.
//
// HARD RULE #14 (Cross-Shard Dispatch):
// Load snapshots are small (BackendId -> uint64_t, typically <16 entries).
// We use foreign_ptr to transfer them safely across shards.
//

// Receive a load snapshot from another shard and update cross_shard_load
// incrementally. Called on the receiving shard via smp::submit_to.
//
// Incremental update: subtract the OLD snapshot for source_shard from the
// aggregate, then add the NEW snapshot. This is O(old_entries + new_entries)
// instead of O(shards * backends) for a full recomputation on every receive.
static void apply_load_snapshot(unsigned source_shard,
                                const std::vector<std::pair<BackendId, uint64_t>>& snapshot_pairs) {
    if (!g_shard_state) return;
    auto& state = *g_shard_state;

    if (source_shard >= state.shard_load_snapshots.size()) return;
    state.stats.load_sync_snapshots_received++;

    // Step 1: Subtract old snapshot values from cross_shard_load
    auto& old_snapshot = state.shard_load_snapshots[source_shard];
    for (const auto& [backend_id, old_load] : old_snapshot) {
        auto it = state.cross_shard_load.find(backend_id);
        if (it != state.cross_shard_load.end()) {
            if (it->second <= old_load) {
                state.cross_shard_load.erase(it);
            } else {
                it->second -= old_load;
            }
        }
    }

    // Step 2: Store new snapshot (convert pairs to map)
    old_snapshot.clear();
    for (const auto& [backend_id, load] : snapshot_pairs) {
        old_snapshot[backend_id] = load;
    }

    // Step 3: Add new snapshot values to cross_shard_load
    for (const auto& [backend_id, load] : old_snapshot) {
        state.cross_shard_load[backend_id] += load;
    }
}

// Broadcast this shard's current active_requests to all other shards.
// Called from the per-shard load sync timer callback.
//
// Optimization: Serialize snapshot as vector<pair<BackendId, uint64_t>> (POD
// pairs, cheaper to copy than flat_hash_map). A single foreign_ptr wraps the
// snapshot and is shared read-only across the parallel_for_each on the sending
// shard. Each target shard receives a foreign_ptr to its own copy and reads
// directly from it — one allocation per target instead of two, and one
// foreign_ptr destructor return instead of N-1.
static seastar::future<> broadcast_load_snapshot() {
    if (!g_shard_state) return seastar::make_ready_future<>();
    auto& state = *g_shard_state;

    // Rule #5: Acquire gate holder for the async lifetime of this broadcast
    seastar::gate::holder holder;
    try {
        holder = state.load_sync_gate.hold();
    } catch (const seastar::gate_closed_exception&) {
        return seastar::make_ready_future<>();
    }

    // Build snapshot as flat vector of pairs (cheaper to copy than hash map).
    // Only include backends with non-zero load to minimize transfer size.
    using SnapshotVec = std::vector<std::pair<BackendId, uint64_t>>;
    auto snapshot = std::make_unique<SnapshotVec>();
    for (const auto& [id, info] : state.backends) {
        if (info.active_requests > 0) {
            snapshot->emplace_back(id, info.active_requests);
        }
    }

    state.stats.load_sync_broadcasts++;
    unsigned this_shard = seastar::this_shard_id();

    // Wrap in foreign_ptr for safe cross-shard read access (Rule #14).
    // The parallel_for_each runs on THIS shard; it reads from the foreign_ptr
    // to create per-target copies. Only one foreign_ptr destructor return.
    auto foreign_snapshot = seastar::make_foreign(std::move(snapshot));

    return seastar::do_with(std::move(holder), std::move(foreign_snapshot),
        [this_shard](seastar::gate::holder&,
                     seastar::foreign_ptr<std::unique_ptr<SnapshotVec>>& snapshot) {
            return seastar::parallel_for_each(
                boost::irange(0u, seastar::smp::count),
                [this_shard, &snapshot](unsigned target_shard) {
                    if (target_shard == this_shard) {
                        return seastar::make_ready_future<>();
                    }

                    // Rule #14: Create a copy for the target shard wrapped in
                    // foreign_ptr. The target reads directly from the foreign_ptr
                    // data (safe for read-only access) and passes it to
                    // apply_load_snapshot which builds a local map.
                    auto copy = std::make_unique<SnapshotVec>(*snapshot);
                    auto foreign_copy = seastar::make_foreign(std::move(copy));

                    return seastar::smp::submit_to(target_shard,
                        [source = this_shard, foreign_copy = std::move(foreign_copy)]() mutable {
                            // Read from foreign_ptr (safe) and apply snapshot.
                            // apply_load_snapshot builds a local map internally.
                            apply_load_snapshot(source, *foreign_copy);
                            return seastar::make_ready_future<>();
                        });
                });
        });
}

void RouterService::start_load_sync_timer() {
    if (!g_shard_state) return;
    auto& state = shard_state();

    if (!state.config.cross_shard_load_sync) {
        log_router.debug("Shard {}: Cross-shard load sync disabled", seastar::this_shard_id());
        return;
    }

    // Only meaningful with multiple shards
    if (seastar::smp::count < 2) {
        log_router.debug("Shard {}: Cross-shard load sync skipped (single shard)", seastar::this_shard_id());
        return;
    }

    state.load_sync_timer.set_callback([] {
        // Timer callbacks can't return futures, so handle errors inline
        (void)broadcast_load_snapshot().handle_exception([](std::exception_ptr ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                log_router.error("Shard {}: Load sync broadcast failed: {}",
                                 seastar::this_shard_id(), e.what());
            }
        });
    });

    state.load_sync_timer.arm_periodic(state.config.cross_shard_load_sync_interval);
    log_router.info("Shard {}: Cross-shard load sync timer started (interval: {}ms)",
                    seastar::this_shard_id(), state.config.cross_shard_load_sync_interval.count());
}

seastar::future<> RouterService::stop_load_sync_timer() {
    if (!g_shard_state) return seastar::make_ready_future<>();
    auto& state = shard_state();

    // Shutdown sequence:
    // 1. Close gate - waits for any in-flight broadcast callbacks to complete
    // 2. Cancel timer - prevents new callbacks
    return state.load_sync_gate.close().then([] {
        if (!g_shard_state) return seastar::make_ready_future<>();
        g_shard_state->load_sync_timer.cancel();
        log_router.info("Shard {}: Cross-shard load sync timer stopped", seastar::this_shard_id());
        return seastar::make_ready_future<>();
    });
}

// ============================================================================
// GPU Load Broadcast (shard 0 → all shards)
// ============================================================================
//
// Called from HealthService::run_loop() on shard 0 after scraping vLLM /metrics.
// Distributes GPU load scores to all shards so routing decisions on every shard
// can incorporate GPU-level signals without cross-shard calls on the hot path.
//
// Uses foreign_ptr to safely transfer the shared map across shards (Rule #0).
// Takes scores by value (Rule #22: coroutine params by value).
//
seastar::future<> RouterService::broadcast_gpu_load(
        absl::flat_hash_map<BackendId, double> scores) {
    auto shared = seastar::make_foreign(
        std::make_unique<absl::flat_hash_map<BackendId, double>>(std::move(scores)));

    // Use do_with to anchor the foreign_ptr lifetime explicitly, matching
    // the pattern in broadcast_load_snapshot(). While co_await keeps the
    // coroutine frame alive, do_with makes the lifetime guarantee visible
    // and resilient to future refactoring.
    co_await seastar::do_with(std::move(shared),
        [](auto& shared) {
            return seastar::smp::invoke_on_all(
                [&shared] {
                    if (!g_shard_state) return;
                    auto& cache = g_shard_state->gpu_load_cache;
                    cache.scores.clear();
                    for (const auto& [id, score] : *shared) {
                        if (cache.scores.size() >= ShardLocalState::GpuLoadCache::MAX_ENTRIES) break;
                        cache.scores[id] = score;
                    }
                    cache.updated_at = std::chrono::steady_clock::now();
                });
        });
}

seastar::future<> RouterService::broadcast_cache_headroom(
        absl::flat_hash_map<BackendId, double> pressure_map) {
    auto shared = seastar::make_foreign(
        std::make_unique<absl::flat_hash_map<BackendId, double>>(std::move(pressure_map)));

    // Same pattern as broadcast_gpu_load(): foreign_ptr anchored via do_with,
    // then invoke_on_all distributes to every shard's local cache.
    co_await seastar::do_with(std::move(shared),
        [](auto& shared) {
            return seastar::smp::invoke_on_all(
                [&shared] {
                    if (!g_shard_state) return;
                    auto& cache = g_shard_state->cache_headroom_cache;
                    cache.pressure.clear();
                    for (const auto& [id, p] : *shared) {
                        if (cache.pressure.size() >= ShardLocalState::CacheHeadroomCache::MAX_ENTRIES) break;
                        cache.pressure[id] = p;
                    }
                    cache.updated_at = std::chrono::steady_clock::now();
                });
        });
}

seastar::future<> RouterService::register_backend_global(BackendId id, seastar::socket_address addr,
                                                          uint32_t weight, uint32_t priority,
                                                          bool supports_token_ids,
                                                          double compression_ratio) {
    return seastar::do_with(addr, weight, priority, supports_token_ids, compression_ratio,
        [id](seastar::socket_address& shared_addr, uint32_t& shared_weight,
             uint32_t& shared_priority, bool& shared_supports_token_ids,
             double& shared_compression_ratio) {
        return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count),
            [id, &shared_addr, &shared_weight, &shared_priority, &shared_supports_token_ids,
             &shared_compression_ratio] (unsigned shard_id) {
            return seastar::smp::submit_to(shard_id, [id, addr = shared_addr,
                                                       weight = shared_weight,
                                                       priority = shared_priority,
                                                       supports_token_ids = shared_supports_token_ids,
                                                       compression_ratio = shared_compression_ratio] {
                if (!g_shard_state) return seastar::make_ready_future<>();
                auto& state = shard_state();
                state.backends[id] = BackendInfo{addr, weight, priority, supports_token_ids, compression_ratio};

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

        // Extract address and port from socket_address.
        // Uses fmt::format (stack-friendly) instead of std::ostringstream (heap alloc).
        // Seastar formats: IPv4 "192.168.1.1:8080", IPv6 "[::1]:8080"
        auto addr_str = fmt::format("{}", info.addr);
        bs.port = 0;

        // Find the port separator (last ':' that's not inside an IPv6 address)
        auto colon_pos = addr_str.find_last_of(':');
        if (colon_pos != std::string::npos && colon_pos > 0) {
            bool is_port_separator = (addr_str[colon_pos - 1] == ']') ||  // IPv6: [::1]:8080
                                     (addr_str.find('[') == std::string::npos);  // IPv4: no brackets
            if (is_port_separator) {
                bs.address = addr_str.substr(0, colon_pos);
                bs.port = parse_port(std::string_view(addr_str).substr(colon_pos + 1)).value_or(0);
            } else {
                bs.address = std::move(addr_str);
            }
        } else {
            bs.address = std::move(addr_str);
        }

        bs.weight = info.weight;
        bs.priority = info.priority;
        bs.is_draining = info.is_draining;
        bs.is_dead = shard.dead_backends.contains(id);
        bs.supports_token_ids = info.supports_token_ids;
        bs.compression_ratio = info.compression_ratio;

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

    if (to_remove.empty()) return;

    // Call pool cleanup callbacks synchronously before launching async removal
    for (const auto& [id, addr] : to_remove) {
        log_router.info("Backend {} drain timeout expired, removing from all shards", id);
        if (_pool_cleanup_callback) {
            _pool_cleanup_callback(addr);
        }
    }

    // Rule #5: Keep gate holder alive for the full async lifetime of all
    // unregister_backend_global() futures via do_with. Without this, the holder
    // would drop when run_draining_reaper() returns, leaving the futures
    // unprotected — stop() could close the gate and destroy members while the
    // futures are still in flight.
    (void)seastar::do_with(std::move(holder), std::move(to_remove),
        [this](seastar::gate::holder&, std::vector<std::pair<BackendId, seastar::socket_address>>& to_remove) {
            return seastar::parallel_for_each(to_remove,
                [this](const std::pair<BackendId, seastar::socket_address>& entry) {
                    return unregister_backend_global(entry.first);
                });
        }).handle_exception([](std::exception_ptr ep) {
            // Rule #9: every catch must log at warn level
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                log_router.warn("Failed to unregister drained backends: {}", e.what());
            }
        });
}

seastar::future<> RouterService::remove_routes_for_backend(BackendId b_id) {
    // Remove all REMOTE routes pointing to this backend
    // This is called when a cluster peer fails and we need to prune orphaned routes
    if (!g_shard_state) co_return;
    auto& state = shard_state();
    RadixTree* tree = state.tree.get();
    if (!tree) {
        co_return;
    }

    size_t removed = tree->remove_routes_by_backend(b_id, RouteOrigin::REMOTE);
    if (removed > 0) {
        state.stats.cluster_routes_pruned += removed;
        log_router.info("Shard {}: Pruned {} orphaned routes for failed peer backend {}",
                        seastar::this_shard_id(), removed, b_id);
    }

    // Rule #17: Yield after tree traversal to avoid reactor stall.
    // remove_routes_by_backend traverses potentially large tree (up to max_routes).
    co_await seastar::coroutine::maybe_yield();
}

seastar::future<> RouterService::handle_node_state_change(BackendId backend, NodeState node_state) {
    if (node_state == NodeState::DRAINING) {
        log_router.info("Received DRAINING notification for backend {} - marking as draining", backend);

        // Broadcast draining state to all shards to stop new traffic to this backend.
        // The is_draining flag is checked by get_live_backends()/get_live_backend_infos(),
        // which all routing paths use.  We intentionally preserve the original weight so
        // it is restored correctly when the backend returns to ACTIVE.
        return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count),
            [backend](unsigned shard_id) {
                return seastar::smp::submit_to(shard_id, [backend] {
                    if (!g_shard_state) return seastar::make_ready_future<>();
                    auto& state = shard_state();
                    auto it = state.backends.find(backend);
                    if (it != state.backends.end()) {
                        it->second.is_draining = true;
                        it->second.drain_start_time = std::chrono::steady_clock::now();
                        log_router.debug("Shard {}: Backend {} marked draining (weight {} preserved)",
                                        seastar::this_shard_id(), backend, it->second.weight);
                    }
                    return seastar::make_ready_future<>();
                });
            });
    } else if (node_state == NodeState::ACTIVE) {
        log_router.info("Received ACTIVE notification for backend {} - restoring to active", backend);
        return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count),
            [backend](unsigned shard_id) {
                return seastar::smp::submit_to(shard_id, [backend] {
                    if (!g_shard_state) return seastar::make_ready_future<>();
                    auto& state = shard_state();
                    auto it = state.backends.find(backend);
                    if (it != state.backends.end() && it->second.is_draining) {
                        it->second.is_draining = false;
                        log_router.debug("Shard {}: Backend {} restored to active (weight {})",
                                        seastar::this_shard_id(), backend, it->second.weight);
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
                                                   uint32_t weight, uint32_t priority,
                                                   bool supports_token_ids,
                                                   double compression_ratio) {
    if (!g_shard_state) return;
    auto& state = *g_shard_state;
    state.backends[id] = BackendInfo{addr, weight, priority, supports_token_ids, compression_ratio};

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

void RouterService::clear_backend_draining_for_testing(BackendId id) {
    if (!g_shard_state) return;
    auto it = g_shard_state->backends.find(id);
    if (it != g_shard_state->backends.end()) {
        it->second.is_draining = false;
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

void RouterService::set_cache_headroom_for_testing(BackendId id, double pressure) {
    if (!g_shard_state) return;
    auto& cache = g_shard_state->cache_headroom_cache;
    if (cache.pressure.size() >= ShardLocalState::CacheHeadroomCache::MAX_ENTRIES) return;
    cache.pressure[id] = pressure;
    cache.updated_at = std::chrono::steady_clock::now();
}

} // namespace ranvier
