#pragma once

#include "backend_registry.hpp"
#include "radix_tree.hpp"
#include "config.hpp"
#include "gossip_service.hpp"  // For NodeState

#include <absl/container/flat_hash_map.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/core/timer.hh>
#include <seastar/net/socket_defs.hh>

namespace ranvier {

// ============================================================================
// Route Batching Types
// ============================================================================
// These types support batching of route updates to prevent "SMP storms."
//
// Two batching systems exist:
//   1. Remote route batching: Routes from gossip peers buffer on shard 0
//      (_pending_remote_routes in RouterService) and flush periodically.
//   2. Local route batching: Routes learned from proxied requests buffer
//      on each shard independently (pending_local_routes in ShardLocalState)
//      and flush periodically. This avoids O(shards) SMP messages per request.

// A pending remote route waiting to be batched and broadcast to all shards
struct PendingRemoteRoute {
    std::vector<int32_t> tokens;
    BackendId backend;
};

// A pending locally-learned route waiting to be batched and broadcast
// Stored in shard-local buffers, flushed by per-shard timers
struct PendingLocalRoute {
    std::vector<int32_t> tokens;
    BackendId backend;
};

// Configuration constants for route batching behavior
struct RouteBatchConfig {
    // Maximum routes to buffer before forcing an immediate flush
    static constexpr size_t MAX_BATCH_SIZE = 100;

    // Hard upper limit on buffer size to prevent OOM from gossip flooding
    // When exceeded, oldest routes are dropped to make room for new ones
    static constexpr size_t MAX_BUFFER_SIZE = 10000;

    // Number of routes to drop at once when buffer overflows
    // Batching drops amortizes the O(n) vector erase cost across multiple inserts
    // Drop count chosen to clear ~10% of buffer, reducing drop frequency while preserving recency
    static constexpr size_t OVERFLOW_DROP_COUNT = 1000;

    // Default timer interval for periodic flushes (ensures bounded latency)
    // Configurable via RANVIER_ROUTE_BATCH_FLUSH_INTERVAL_MS env var (1-1000ms)
    static constexpr std::chrono::milliseconds DEFAULT_FLUSH_INTERVAL{20};
};

// Forward declaration
class GossipService;

// ============================================================================
// Unified Route Result
// ============================================================================
// Encapsulates all routing decisions in a single return type.
// This enables HttpController to make one call and handle one error path,
// moving routing mode logic into RouterService where it belongs.

struct RouteResult {
    std::optional<BackendId> backend_id;  // Selected backend (nullopt if routing failed)
    std::string routing_mode;             // "prefix", "hash", or "random"
    bool cache_hit = false;               // True if route was found via ART lookup (not hash fallback)
    std::string error_message;            // Non-empty if backend_id is nullopt

    // GPU load observability (populated when vLLM metrics influence routing)
    double backend_load_at_decision = 0.0;  // GPU load score when a load divert fired
    bool was_load_redirect = false;          // True if the load term moved dispatch off placement

    // Cost-based routing observability (populated when cost routing is enabled)
    bool was_cost_redirect = false;          // True if the cost-budget term moved dispatch
    bool was_fast_lane = false;              // True if a small request was cost-diverted
    double backend_cost_at_decision = 0.0;   // Cost budget when route was chosen

    // The PLACEMENT winner of the unified route score: the stable choice
    // (anchor, possibly moved by the hardware-price term) before the
    // transient load/cost terms pick the dispatch target. Equal to
    // backend_id when no transient divert occurred. Lets the route-learning
    // policy pin prefixes to stable placement instead of load-driven
    // targets. Zero when no backend was selected.
    BackendId original_selected = 0;

    // Number of tokens of the input prefix that were matched (cache_hit
    // path) or 0 (cache_miss). Populated by route_request() so the
    // telemetry sink can plot a prefix-reuse-depth histogram per bucket.
    // This is the effective lookup length (prefix_boundary if > 0, else
    // min(tokens.size(), config.prefix_token_length)) — not the ART's
    // internal path-compressed depth.
    uint32_t matched_prefix_depth = 0;
};

// Result from get_backend_for_prefix(), distinguishing ART hits from hash fallback.
struct PrefixRouteResult {
    std::optional<BackendId> backend_id;  // Selected backend (nullopt if no backends)
    bool art_hit = false;                 // True only when ART lookup found a live backend

    // GPU load observability (populated when the score's load term diverts)
    double backend_load_at_decision = 0.0;  // GPU load score of the placement backend
    bool was_load_redirect = false;          // True if the load term moved dispatch off placement

    // Cost-based routing observability (populated when cost routing is enabled)
    bool was_cost_redirect = false;          // True if the cost-budget term moved dispatch
    bool was_fast_lane = false;              // True if a small request was cost-diverted
    double backend_cost_at_decision = 0.0;   // Cost budget when route was chosen

    // The PLACEMENT winner of the unified route score (see route_scorer.hpp):
    // the ART/hash anchor, possibly moved by the stable hardware-price term.
    // For BOUNDED_LOAD and P2C the anchor is the post-probe / post-secondary
    // result — internal probing is part of the strategy itself, not a
    // transient divert.
    BackendId original_selected = 0;

    // See RouteResult::matched_prefix_depth. Mirrored here so route_request()
    // can propagate the value out to its caller.
    uint32_t matched_prefix_depth = 0;
};

// ============================================================================
// BackendRequestGuard: RAII guard for tracking in-flight requests per backend
// ============================================================================
//
// Create when a request is routed to a backend. The destructor decrements the
// counter on ANY exit path (success, error, timeout, exception).
//
// Usage (future PR - not yet integrated):
//   auto guard = BackendRequestGuard(backend_id);
//   co_return co_await seastar::do_with(std::move(guard), [...](auto& g) {
//       // proxy request to backend
//   });
//
// Design:
//   - Move-only for safe ownership transfer through do_with chains
//   - Shard-local operation only (accesses thread_local g_shard_state)
//   - Shard-local: uses plain integer increment/decrement (no atomic needed)
//
class BackendRequestGuard {
public:
    // Construct guard and increment active_requests for the backend.
    // If backend_id is invalid (not in shard state), guard is inactive (no-op).
    explicit BackendRequestGuard(BackendId id);

    // Decrement active_requests if guard is active
    ~BackendRequestGuard();

    // Move constructor: transfer ownership (source becomes inactive)
    BackendRequestGuard(BackendRequestGuard&& other) noexcept;

    // Move assignment: transfer ownership (source becomes inactive)
    BackendRequestGuard& operator=(BackendRequestGuard&& other) noexcept;

    // Non-copyable (prevent double-decrement)
    BackendRequestGuard(const BackendRequestGuard&) = delete;
    BackendRequestGuard& operator=(const BackendRequestGuard&) = delete;

    // Accessor for the backend this guard is tracking
    BackendId backend_id() const { return _backend_id; }

    // Check if guard is active (owns the increment)
    bool is_active() const { return _active; }

private:
    BackendId _backend_id{0};
    bool _active{false};  // True if we own the increment
};

// ============================================================================
// CostBudgetGuard: RAII guard for cost budget tracking per backend
// ============================================================================
//
// Reserves cost budget on construction, releases on destruction.
// Ensures symmetric reserve/release on ALL exit paths (success, error, exception).
// Move-only, same pattern as BackendRequestGuard.
//
class CostBudgetGuard {
public:
    // Construct guard and reserve cost budget on the backend.
    // If cost <= 0 or cost routing is disabled, guard is inactive (no-op).
    CostBudgetGuard(BackendId id, double cost);

    // Release cost budget if guard is active
    ~CostBudgetGuard();

    // Move constructor: transfer ownership
    CostBudgetGuard(CostBudgetGuard&& other) noexcept;

    // Move assignment: transfer ownership
    CostBudgetGuard& operator=(CostBudgetGuard&& other) noexcept;

    // Non-copyable
    CostBudgetGuard(const CostBudgetGuard&) = delete;
    CostBudgetGuard& operator=(const CostBudgetGuard&) = delete;

    BackendId backend_id() const { return _backend_id; }
    double cost() const { return _cost; }
    bool is_active() const { return _active; }

private:
    BackendId _backend_id{0};
    double _cost{0.0};
    bool _active{false};
};

// ============================================================================
// Load Tracking Helper Functions (shard-local, lock-free)
// ============================================================================

// Get current in-flight request count for a backend (shard-local read)
// Returns 0 if backend not found in shard state
uint64_t get_backend_load(BackendId id);

// ============================================================================
// Cost Budget Tracking Functions (shard-local, lock-free)
// ============================================================================

// Reserve cost budget when routing a request to a backend.
// Returns false if budget would exceed max_cost_per_backend.
bool reserve_cost_budget(BackendId id, double cost);

// Release cost budget when a request completes or fails.
void release_cost_budget(BackendId id, double cost);

// Get current cost budget for a backend (for routing decisions).
double get_backend_cost(BackendId id);

// Get composite load for a backend, blending shard-local active_requests
// with GPU load score from vLLM metrics (when available via broadcast cache).
// Returns uint64_t for backward compatibility with P2C/bounded-load comparisons.
uint64_t get_composite_backend_load(BackendId id);

// Find the least loaded backend from a list of candidates
// Returns pair of (backend_id, load). Returns (0, UINT64_MAX) if no candidates found.
// O(n) scan where n is typically <10 backends
std::pair<BackendId, uint64_t> get_least_loaded_backend(const std::vector<BackendId>& candidates);

class RouterService : public BackendRegistry {
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

    // ==========================================================================
    // Lifecycle Management
    // ==========================================================================
    //
    // RouterService registers metrics lambdas that may capture 'this' or access
    // thread-local state (NodeSlab). To prevent use-after-free when Prometheus
    // scrapes during/after shutdown:
    //
    // 1. stop() MUST be called before RouterService destruction
    // 2. Metrics are deregistered FIRST, before any other cleanup
    // 3. Only then are timers cancelled and gossip stopped
    //
    // Destruction order: metrics -> timers -> gossip -> destructor
    //
    seastar::future<> stop();

    // 1. DATA PLANE (Fast Lookups)

    // Unified routing entry point - encapsulates all routing mode logic
    // Returns RouteResult with backend_id, routing_mode, cache_hit, and error_message
    // This is the primary method HttpController should call for routing decisions.
    // request_id: Optional request ID for tracing (empty string if not tracing)
    // prefix_boundary: Optional token count for "shared prefix" (e.g., system message length).
    //                  If provided and > 0, the hash fallback uses this boundary instead of
    //                  prefix_token_length. This ensures consistent routing across cluster nodes
    //                  for requests sharing the same system prompt but different user queries.
    RouteResult route_request(const std::vector<int32_t>& tokens,
                              const std::string& request_id = "",
                              size_t prefix_boundary = 0,
                              double estimated_cost = 0.0);

    // Find which Backend ID owns this prefix (radix tree lookup)
    // request_id: Optional request ID for tracing (empty string if not tracing)
    std::optional<BackendId> lookup(const std::vector<int32_t>& tokens,
                                     const std::string& request_id = "");

    // Resolve ID -> IP:Port
    std::optional<seastar::socket_address> get_backend_address(BackendId id) const override;

    // 2. CONTROL PLANE (Async Broadcasts)
    // Teach the tree a new prefix (Prefix -> ID) with LRU eviction
    // request_id: Optional request ID for tracing (empty string if not tracing)
    // prefix_boundary: Optional token count for "shared prefix" (e.g., system message length).
    //                  If provided and > 0, route is stored at this boundary instead of
    //                  full prefix_token_length. This enables prefix-aware routing for
    //                  multi-turn conversations where requests share a common system prompt.
    // Also broadcasts to cluster peers if gossip is enabled
    seastar::future<bool> learn_route_global(std::vector<int32_t> tokens, BackendId backend,
                                              const std::string& request_id = "",
                                              size_t prefix_boundary = 0);

    // Multi-depth route learning (Option C)
    // Stores routes at multiple prefix boundaries for optimal cache reuse in
    // branching or continuing conversations. Each boundary represents a natural
    // breakpoint (e.g., end of system message, end of user turn, etc.)
    //
    // prefix_boundaries: Vector of cumulative token counts where routes should be stored.
    //                   Routes are stored at each boundary, allowing lookups to match
    //                   at any conversation depth.
    //
    // Example: boundaries = [256, 306, 406] stores 3 routes:
    //   - tokens[0..256] → backend (system message)
    //   - tokens[0..306] → backend (system + user1)
    //   - tokens[0..406] → backend (system + user1 + assistant1)
    seastar::future<> learn_route_global_multi(std::vector<int32_t> tokens, BackendId backend,
                                                const std::string& request_id,
                                                const std::vector<size_t>& prefix_boundaries);

    // Learn a route from a remote cluster peer (marks as REMOTE origin)
    // REMOTE routes can be evicted more aggressively than LOCAL routes
    seastar::future<> learn_route_remote(std::vector<int32_t> tokens, BackendId backend);

    // Start the gossip service (call after Seastar is initialized)
    // Also starts the route batch flush timer for remote route updates
    seastar::future<> start_gossip();

    // Stop the gossip service (call before shutdown)
    // Ensures all pending route batches are flushed before stopping
    seastar::future<> stop_gossip();

    // Teach the system a new server (ID -> IP:Port) with optional weight and priority
    // Weight: relative load balancing weight (default 100, higher = more traffic)
    // Priority: priority group (default 0 = highest, lower priority backends used for fallback)
    seastar::future<> register_backend_global(BackendId id, seastar::socket_address addr,
                                               uint32_t weight = 100, uint32_t priority = 0,
                                               bool supports_token_ids = true,
                                               double compression_ratio = 1.0,
                                               BackendType type = BackendType::VLLM,
                                               PoolRole pool_role = PoolRole::UNIFIED) override;

    // Remove a backend from all shards
    seastar::future<> unregister_backend_global(BackendId id) override;

    // Per-backend API key store. Side-map separate from BackendInfo so the
    // credential boundary stays in one named place and
    // the abstract BackendRegistry interface doesn't gain a credential
    // parameter that K8s / admin / persistence paths must ignore.
    //
    // `set_backend_api_key_global` broadcasts the key to every shard
    // (mirroring register_backend_global's parallel_for_each shape) and
    // is intended to be called once at startup right after the backend
    // is registered. `get_backend_api_key` is synchronous and shard-
    // local — http_controller calls it on the request path to decide
    // whether to inject `Authorization: Bearer <key>`.
    //
    // Keys live in memory only — never written to SQLite, never logged.
    seastar::future<> set_backend_api_key_global(BackendId id, std::string api_key);
    std::string get_backend_api_key(BackendId id) const;

    // Per-backend telemetry-sink labels (operator-set, broadcast to every
    // shard). Kept as a side-map rather than on BackendInfo's abstract
    // interface so callers that don't care about telemetry don't have to
    // thread them through. Neither label is parsed from client input —
    // both are operator-controlled so cardinality stays bounded and the
    // dimensions stay content-free. See src/telemetry_schema.hpp.
    seastar::future<> set_backend_labels_global(BackendId id,
                                                HardwareLabel hardware_label,
                                                std::string model_family);

    // Per-backend hardware-tier / cost fields (operator-set, broadcast to
    // every shard; same side-broadcast shape as the telemetry labels above so
    // BackendRegistry's abstract interface stays unchanged). `gpu_tier` is a
    // coarse informational label (e.g. "h100", "a10g"); `cost_per_hour` is
    // the operator's hourly price and, when > 0, opts the backend into the
    // cache-miss hardware-cost preference: misses prefer the cheapest priced
    // live backend, while ART hits stay on the cache-warm backend regardless
    // of cost. Both default to unset (""/0.0) = pre-feature behavior.
    // Call after register_backend_global resolves — registration overwrites
    // BackendInfo wholesale, resetting both fields to their defaults.
    seastar::future<> set_backend_hardware_cost_global(BackendId id,
                                                       std::string gpu_tier,
                                                       double cost_per_hour);
    // Telemetry bucket dimensions for a backend, resolved in one shard-local
    // lookup. `backend_type` is the engine class already on the backend (not an
    // operator-set label); returning it here lets the request site build the
    // bucket key from a single lookup, with no extra hot-path work.
    struct BackendTelemetryLabels {
        BackendType   backend_type   = BackendType::VLLM;
        HardwareLabel hardware_label = HardwareLabel::UNSPECIFIED;
        std::string   model_family;   // "" → caller treats as "unspecified"
    };
    BackendTelemetryLabels telemetry_labels(BackendId id) const;

    // Calling shard's cumulative ART route-eviction count. Returns 0 when
    // shard state is not initialised.
    static uint64_t get_local_routes_evicted();

    // Start draining a backend (stops new requests, allows existing cache hits)
    // After backend_drain_timeout, the backend will be fully removed
    seastar::future<> drain_backend_global(BackendId id);

    // Get a backend using weighted random selection within the highest available priority group
    std::optional<BackendId> get_random_backend();

    // Get a backend using prefix-affinity routing (ART + consistent hash fallback)
    // Routes requests with the same prefix to the same backend for KV cache reuse
    // prefix_boundary: If > 0, used for hash fallback instead of prefix_token_length.
    //                  Ensures consistent routing across cluster nodes for same system prompt.
    // Returns PrefixRouteResult with art_hit=true only on actual ART cache hit.
    PrefixRouteResult get_backend_for_prefix(const std::vector<int32_t>& tokens,
                                             const std::string& request_id = "",
                                             size_t prefix_boundary = 0,
                                             double estimated_cost = 0.0);

    // Get a backend using consistent hash only (no ART, no learning)
    // Used to measure baseline hash performance vs ART
    std::optional<BackendId> get_backend_by_hash(const std::vector<int32_t>& tokens,
                                                  const std::string& request_id = "");

    // Get list of all IDs (For the Health Checker to iterate)
    std::vector<BackendId> get_all_backend_ids() const override;

    // ==========================================================================
    // Admin API - State Inspection
    // ==========================================================================

    // Backend info for admin API
    struct BackendState {
        BackendId id;
        std::string address;
        uint16_t port;
        uint32_t weight;
        uint32_t priority;
        bool is_draining;
        bool is_dead;
        bool supports_token_ids;  // Whether backend supports vLLM prompt_token_ids
        double compression_ratio;  // KV-cache compression ratio (>= 1.0, 1.0 = no compression)
        BackendType type;  // Engine class (VLLM, SGLANG, CEREBRAS, ...)
        PoolRole pool_role;  // Disaggregated pool role (unified/prefill/decode)
        int64_t drain_start_ms;  // 0 if not draining
    };

    // Get all backend states for admin inspection
    std::vector<BackendState> get_all_backend_states() const;

    // Check if a backend supports vLLM's prompt_token_ids field.
    // Returns false if backend not found (safe default: don't inject unknown fields).
    bool backend_supports_token_ids(BackendId id) const;

    // Engine class for the given backend. Returns BackendType::VLLM if the
    // backend is not registered — matches the historical assumption baked
    // into the learning/scrape paths.
    BackendType backend_type(BackendId id) const override;

    // Whether ART route learning is worthwhile for this backend. Returns
    // false when the backend's type is in the no-cache set (e.g. Cerebras
    // keeps the whole model in on-chip SRAM, so prefix affinity earns
    // nothing). Gates the learning sites only; the lookup path is
    // unaffected — if a route already points at a non-cacheable backend
    // we still honor it. Safe default for not-found is true: backend
    // registration propagates across shards asynchronously, so a gossip
    // route may legitimately arrive before this shard sees the backend;
    // defaulting to false would silently drop those.
    bool should_cache_routes_for(BackendId id) const;

    // Get tree dump for admin inspection (local shard only)
    RadixTree::DumpNode get_tree_dump() const;

    // Get tree dump with prefix filter (local shard only)
    std::optional<RadixTree::DumpNode> get_tree_dump_with_prefix(const std::vector<TokenId>& prefix) const;

    // Circuit Breaker API
    seastar::future<> set_backend_status_global(BackendId id, bool is_alive) override;

    // Hot-reload: Update routing configuration on all shards
    seastar::future<> update_routing_config(const RoutingConfig& config);

    // Callback type for pool cleanup when a backend is removed
    using PoolCleanupCallback = std::function<void(seastar::socket_address)>;

    // Set callback to be invoked when a backend is fully removed (for pool cleanup)
    void set_pool_cleanup_callback(PoolCleanupCallback callback);

    // Set vLLM load score callback (delegates to HealthService::get_backend_load)
    // Uses std::function to avoid linking HealthService into test binaries.
    using LoadScoreCallback = std::function<double(BackendId)>;
    void set_load_score_callback(LoadScoreCallback cb) { _load_score_callback = std::move(cb); }

    // Override BackendRegistry::get_backend_load_score to invoke the callback
    double get_backend_load_score(BackendId id) const override {
        if (_load_score_callback) return _load_score_callback(id);
        return 0.0;
    }

    // Callback type for circuit breaker cleanup when a backend is unregistered
    // Called on each shard during unregister_backend_global() to clean up circuit entries
    using CircuitCleanupCallback = std::function<void(BackendId)>;

    // Set shard-local callback for circuit cleanup (must be called on each shard)
    // The callback is stored in thread-local state and invoked during backend unregistration
    static void set_circuit_cleanup_callback(CircuitCleanupCallback callback);

    // Start the draining reaper timer (call after Seastar is initialized)
    void start_draining_reaper();

    // Stop the draining reaper timer (call before shutdown)
    void stop_draining_reaper();

    // ---- Local Route Batching (shard-local, no cross-shard access on enqueue) ----

    // Start per-shard local batch flush timers (call via smp::invoke_on_all)
    // flush_interval: configured interval from RANVIER_ROUTE_BATCH_FLUSH_INTERVAL_MS
    static void start_local_batch_timer(std::chrono::milliseconds flush_interval);

    // Stop per-shard local batch flush timers and drain pending routes
    // (call via smp::invoke_on_all during shutdown)
    static seastar::future<> stop_local_batch_timer();

    // ---- Cross-Shard Load Synchronization (per-shard) ----

    // Start per-shard load sync timers (call via smp::invoke_on_all)
    // Each shard periodically broadcasts its active_requests snapshot to all
    // other shards, giving routing decisions a global view of backend load.
    static void start_load_sync_timer();

    // Stop per-shard load sync timers (call via smp::invoke_on_all during shutdown)
    static seastar::future<> stop_load_sync_timer();

    // ---- GPU Load Broadcast (HealthService → all shards) ----

    // Broadcast GPU load scores from shard 0 to all shards.
    // Called by HealthService::run_loop() after scraping vLLM metrics.
    // Takes scores by value (Rule #22: coroutine params by value).
    static seastar::future<> broadcast_gpu_load(
        absl::flat_hash_map<BackendId, double> scores);

    // Broadcast effective cache pressure from shard 0 to all shards.
    // Called by HealthService::run_loop() alongside GPU load broadcast.
    // pressure_map: BackendId → effective_cache_pressure (0.0–1.0).
    // Takes map by value (Rule #22: coroutine params by value).
    static seastar::future<> broadcast_cache_headroom(
        absl::flat_hash_map<BackendId, double> pressure_map);

    // ---- Cache-Residency State (vLLM scrape → local shards + cluster peers) ----

    // One backend's cache-residency sample. cache_usage and residency_weight are
    // both normalized to [0.0, 1.0]; residency_weight is the estimated prefix
    // retention (see VLLMMetrics::estimated_prefix_retention()).
    struct CacheStateSample {
        double cache_usage = 0.0;
        double residency_weight = 0.0;
    };

    // Distribute locally-scraped cache state: upsert residency into every shard's
    // residency cache (so routing on every shard sees it) AND gossip each
    // backend's CACHE_STATE to cluster peers. Called by HealthService::run_loop()
    // on shard 0 after scraping vLLM /metrics.
    // Takes map by value (Rule #22: coroutine params by value).
    static seastar::future<> broadcast_cache_state_global(
        absl::flat_hash_map<BackendId, CacheStateSample> samples);

    // Apply a peer-reported cache-residency sample received via gossip: upsert
    // the residency weight into every shard's residency cache. Called from the
    // gossip CACHE_STATE callback (shard 0). Does NOT re-gossip (no echo).
    // Takes params by value (Rule #22: coroutine params by value).
    static seastar::future<> apply_peer_cache_state(
        BackendId backend_id, double cache_usage, double residency_weight);

    // Flush locally-buffered routes to all shards (runs on calling shard)
    // Deduplicates within the batch, broadcasts via parallel_for_each,
    // and submits gossip batch to shard 0
    static seastar::future<> flush_local_route_batch();

    // Buffer a locally-learned route for batched broadcast
    // Pushes into the shard-local buffer; triggers immediate flush if full.
    // Returns a ready future unless buffer is full, in which case it
    // returns the future from flush_local_route_batch().
    static seastar::future<> buffer_local_route(std::vector<int32_t> tokens, BackendId backend);

    // Static: Only uses thread_local g_shard_state, safe to call from any shard
    static seastar::future<> remove_routes_for_backend(BackendId b_id);

    // Handle node state change notifications from cluster peers
    // When a peer broadcasts DRAINING, this sets their backend weight to 0
    // Static: Only uses thread_local g_shard_state, safe to call from any shard
    static seastar::future<> handle_node_state_change(BackendId backend, NodeState state);

    // Get the gossip service (for broadcasting node state on shutdown)
    GossipService* gossip_service() { return _gossip.get(); }

    // ==========================================================================
    // Cache Event Support (Push-Based Cache Eviction Notifications)
    // ==========================================================================

    // Evict routes matching a prefix hash from a specific backend.
    // Called from cache event handler. Dispatches to all shards via smp::invoke_on_all.
    // Returns total number of routes evicted across all shards.
    // Rule #22: all params by value (coroutine).
    static seastar::future<uint32_t> evict_by_prefix_hash_global(
        uint64_t prefix_hash, BackendId backend_id, uint64_t event_timestamp_ms);

    // Shard-local eviction: removes routes matching (prefix_hash, backend_id) on this shard.
    // Returns number of routes evicted on this shard.
    static uint32_t evict_by_prefix_hash_local(
        uint64_t prefix_hash, BackendId backend_id, uint64_t event_timestamp_ms);

    // Apply a "loaded" cache event from a backend.
    //
    // Loaded events do NOT carry token IDs in the wire format. The
    // RadixTree is keyed on tokens, so we cannot insert a new route from
    // (prefix_hash, backend_id) alone. The implementation is therefore the
    // strict subset: a loaded event is only "applied" when this shard already
    // has the (prefix_hash, backend_id) pair in its prefix_hash_index — in
    // which case it acts as a timestamp refresh / liveness ping for conflict
    // resolution against later evict events. Loaded events for unknown
    // (hash, backend) pairs are counted as ignored.
    //
    // Returns count of shards on which the load was applied (0 = ignored
    // everywhere). Materializing brand-new routes from a loaded event would
    // require the wire format to carry tokens (or a hash → tokens side
    // index) and is intentionally out of scope.
    // Rule #22: all params by value (coroutine).
    static seastar::future<uint32_t> load_route_global(
        uint64_t prefix_hash, BackendId backend_id, uint64_t event_timestamp_ms);

    // Shard-local "loaded" application; returns 1 if applied on this shard.
    static uint32_t load_route_local(
        uint64_t prefix_hash, BackendId backend_id, uint64_t event_timestamp_ms);

    // Update prefix hash index entry for a specific backend (for testing).
    static void update_prefix_hash_index_for_testing(uint64_t prefix_hash, BackendId backend_id);

    // Get cache event stats for metrics (shard-local read).
    struct CacheEventStatsSnapshot {
        uint64_t events_received = 0;
        uint64_t evictions_applied = 0;
        uint64_t evictions_stale = 0;
        uint64_t evictions_unknown = 0;
        uint64_t auth_failures = 0;
        uint64_t parse_errors = 0;
        uint64_t loads_applied = 0;
        uint64_t loads_ignored_stale = 0;
        uint64_t loads_ignored_unknown = 0;
        uint64_t loads_ignored_different_backend = 0;
    };
    static CacheEventStatsSnapshot get_cache_event_stats();

    // Record cache event statistics (shard-local, called from http_controller)
    static void record_cache_event_received();
    static void record_cache_event_auth_failure();
    static void record_cache_event_parse_error();

    // ==========================================================================
    // Testing Support
    // ==========================================================================
    // Reset shard-local state for unit testing. This clears all per-shard state
    // including backends, routes, and statistics. If a config is provided, the
    // state will be reinitialized with that configuration.
    //
    // IMPORTANT: Only call these in test code, not in production.
    static void reset_shard_state_for_testing(const RoutingConfig* cfg = nullptr);

    // Register a backend in shard-local state (bypasses async cross-shard broadcast).
    // Allows unit tests to set up backend state without a running Seastar reactor.
    static void register_backend_for_testing(BackendId id, seastar::socket_address addr,
                                              uint32_t weight = 100, uint32_t priority = 0,
                                              bool supports_token_ids = true,
                                              double compression_ratio = 1.0,
                                              BackendType type = BackendType::VLLM,
                                              PoolRole pool_role = PoolRole::UNIFIED);

    // Write a key directly into the shard-local api_key side-map.
    // Bypasses set_backend_api_key_global's parallel_for_each broadcast so
    // get_backend_api_key can be unit-tested without a running reactor.
    static void set_backend_api_key_for_testing(BackendId id, std::string api_key);

    // Insert a route directly into the shard-local RadixTree (bypasses async broadcast).
    static void insert_route_for_testing(const std::vector<int32_t>& tokens, BackendId backend);

    // Mark a backend as draining in shard-local state.
    static void set_backend_draining_for_testing(BackendId id);

    // Clear draining flag on a backend (simulates ACTIVE state transition).
    static void clear_backend_draining_for_testing(BackendId id);

    // Mark a backend as dead (circuit-breaker quarantine) in shard-local state.
    static void mark_backend_dead_for_testing(BackendId id);

    // Set cache headroom (effective cache pressure) for a backend in shard-local state.
    // pressure: 0.0 (empty cache, full headroom) to 1.0 (full cache, no headroom).
    static void set_cache_headroom_for_testing(BackendId id, double pressure);

    // Set cache residency weight for a backend in shard-local state.
    // residency: 0.0 (prefix almost certainly evicted) to 1.0 (almost certainly resident).
    static void set_residency_for_testing(BackendId id, double residency);

    // Write hardware-tier / cost fields directly into shard-local BackendInfo.
    // Bypasses set_backend_hardware_cost_global's broadcast so the cache-miss
    // cost preference can be unit-tested without a running reactor.
    static void set_backend_hardware_cost_for_testing(BackendId id, std::string gpu_tier,
                                                      double cost_per_hour);

    // Remove a backend from shard-local state (bypasses async cross-shard broadcast).
    static void unregister_backend_for_testing(BackendId id);

    // Get the number of routes in the shard-local RadixTree.
    static size_t get_route_count_for_testing();

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

    // Gate for ALL timer callbacks (Rule #5: Timer-Captures-This)
    // Protects: _ttl_timer, _batch_flush_timer, _draining_reaper_timer
    // Each callback acquires holder at entry; stop() closes gate before cancelling timers
    seastar::gate _timer_gate;

    // Draining reaper timer (runs on shard 0, checks for expired draining backends)
    seastar::timer<> _draining_reaper_timer;

    // Batch flush timer for remote routes (runs on shard 0)
    seastar::timer<> _batch_flush_timer;

    // Buffer for pending remote routes (shard 0 only)
    // Routes are accumulated here and broadcast in batches to reduce SMP message traffic
    std::vector<PendingRemoteRoute> _pending_remote_routes;

    // Counter for routes dropped due to buffer overflow (for metrics and rate-limited logging)
    uint64_t _routes_dropped_overflow = 0;

    // Callback for pool cleanup when a backend is fully removed
    PoolCleanupCallback _pool_cleanup_callback;

    // vLLM load score callback (set by Application after HealthService init)
    LoadScoreCallback _load_score_callback;

    // Perform TTL cleanup on all shards
    void run_ttl_cleanup();

    // Rule #17: Yielding per-shard TTL cleanup coroutine.
    // Named (not lambda) to avoid Rule #16 when called from smp::submit_to.
    // Compression-aware: each backend may have a different cutoff based on its
    // compression_ratio. default_cutoff applies to backends not in the map.
    // Rule #21: coroutine takes map by value (frame outlives caller stack).
    static seastar::future<> ttl_cleanup_on_shard(
        std::chrono::steady_clock::time_point default_cutoff,
        absl::flat_hash_map<BackendId, std::chrono::steady_clock::time_point> backend_cutoffs);

    // Check for backends that have been draining long enough and fully remove them
    void run_draining_reaper();

    // ---- Route Batching (private implementation) ----

    // Start the periodic timer that flushes pending route batches
    void start_batch_flush_timer();

    // Flush all pending remote routes to all shards
    // Returns a future that completes when all shards have processed the batch
    seastar::future<> flush_route_batch();
};

} // namespace ranvier
