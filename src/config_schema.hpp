// Ranvier Core - Configuration Schema
//
// Ranvier-specific configuration structs: RoutingConfig, AssetsConfig, and
// the top-level RanvierConfig aggregate.
//
// For infrastructure configs (ServerConfig, PoolConfig, etc.), see config_infra.hpp.
// For loading and validation, see config_loader.hpp.
// For backward compatibility, include config.hpp (facade).

#pragma once

#include "config_infra.hpp"

namespace ranvier {

// =============================================================================
// Routing Configuration
// =============================================================================

// Routing and caching configuration
struct RoutingConfig {
    size_t min_token_length = 4;  // Minimum tokens before caching a route
    uint32_t backend_retry_limit = 5;  // Max attempts to find a live backend
    uint32_t block_alignment = 16;  // vLLM PagedAttention block size for route alignment
    size_t max_routes = 100000;  // Maximum number of routes in the prefix cache (0 = unlimited)
    size_t max_route_tokens = 8192;  // Maximum tokens per route (business-level limit, 0 = unlimited)
    std::chrono::seconds ttl_seconds{3600};  // TTL for cached routes (1 hour default)
    std::chrono::seconds backend_drain_timeout{60};  // Time to wait before fully removing a draining backend
    bool enable_token_forwarding = false;  // Forward pre-computed token IDs to backends (vLLM prompt_token_ids)
    bool accept_client_tokens = false;  // Accept pre-tokenized prompt_token_ids from clients for routing
    int32_t max_token_id = 200000;  // Maximum valid token ID (auto-configured from tokenizer vocab size at startup)

    // Routing mode: determines how requests are routed to backends
    // - "prefix": ART lookup + consistent hash fallback (best for KV cache, learns routes)
    // - "hash": Consistent hash only (no ART, no learning - measures hash baseline)
    // - "random": Weighted random distribution (baseline, no affinity)
    enum class RoutingMode { PREFIX, HASH, RANDOM };
    RoutingMode routing_mode = RoutingMode::PREFIX;  // Default: prefix-affinity with ART
    size_t prefix_token_length = 128;  // Number of tokens to use as routing key (default: 128)

    // =========================================================================
    // Hash Strategy (controls consistent hash fallback behavior)
    // =========================================================================
    // Determines how the hash fallback selects backends when ART has no match.
    // Also controls load-aware override behavior for both ART hits and hash misses.
    //
    // - JUMP:         Original jump consistent hash (Lamping & Veach 2014).
    //                 Uses separate load_aware_routing threshold for load balancing.
    // - BOUNDED_LOAD: Jump hash + capacity cap (Mirrokni et al. 2018).
    //                 Each backend capped at ceil(avg_load * (1 + epsilon)).
    //                 Subsumes load_aware_routing — no separate threshold needed.
    // - P2C:          Power-of-two-choices with primary affinity bias.
    //                 Hashes to 2 candidates, prefers primary unless secondary
    //                 is significantly less loaded. Proven O(log log n) max load.
    // - MODULAR:      Simple modular hash (key % num_backends). For benchmarking
    //                 only — reshuffles ALL keys on topology changes.
    enum class HashStrategy { JUMP, BOUNDED_LOAD, P2C, MODULAR };
    HashStrategy hash_strategy = HashStrategy::BOUNDED_LOAD;  // Default: bounded-load

    // Bounded-load epsilon: capacity headroom factor.
    // Each backend accepts at most ceil(avg_load * (1 + epsilon)) in-flight requests.
    // Lower epsilon = tighter balance but more affinity breaks.
    // Typical values: 0.25 (tight), 0.5 (moderate), 1.0 (loose).
    double bounded_load_epsilon = 0.25;

    // P2C load bias: minimum load difference to prefer secondary over primary.
    // Higher bias = stronger affinity to primary (hash-preferred) backend.
    // Only switch to secondary when: secondary_load + p2c_load_bias < primary_load.
    uint64_t p2c_load_bias = 2;

    // Prefix boundary detection for multi-turn conversations
    // When enabled, system messages are tokenized separately to identify the "shared prefix"
    // boundary. Routes are stored at this boundary instead of prefix_token_length, improving
    // cache hit rates for requests sharing the same system prompt.
    bool enable_prefix_boundary = true;  // Enable automatic prefix boundary detection
    size_t min_prefix_boundary_tokens = 4;  // Minimum system message tokens to use as boundary

    // Client-provided prefix boundary (prefix_token_count field in requests)
    // When enabled, clients can include a "prefix_token_count" field in their requests
    // to specify how many tokens constitute their shared prefix. This takes precedence
    // over automatic system message detection when present.
    bool accept_client_prefix_boundary = false;  // Accept client-provided prefix_token_count

    // Multi-depth route storage (Option C)
    // When enabled, routes are stored at multiple message boundaries for optimal cache
    // reuse in branching or continuing conversations. Each boundary represents a natural
    // breakpoint (end of system message, end of user turn, etc.)
    bool enable_multi_depth_routing = false;  // Store routes at multiple depths

    // =========================================================================
    // Load-Aware Routing
    // =========================================================================
    // When enabled, considers backend queue depth before routing to the
    // prefix-preferred backend. If the preferred backend is significantly more
    // loaded than its peers, routes to the least-loaded alternative (accepting
    // potential cache miss).
    //
    // Uses a relative threshold: preferred is "overloaded" when its queue depth
    // exceeds (median_load * load_imbalance_factor + load_imbalance_floor).
    // This auto-adapts to any workload, model size, or cluster size.
    bool load_aware_routing = true;           // Enable load-aware backend selection
    double load_imbalance_factor = 2.0;       // Divert when preferred > factor * median load
    uint64_t load_imbalance_floor = 2;        // Additive floor to prevent flapping at low load

    // =========================================================================
    // Cross-Shard Load Synchronization
    // =========================================================================
    // When enabled, each shard periodically broadcasts its active_requests
    // snapshot to all other shards. This prevents cross-shard burst-routing
    // hot-spotting where multiple shards independently pick the same "best"
    // backend because they only see their own shard's load counters.
    //
    // Without this, client-tokenize workloads (0.4ms overhead) cause near-
    // simultaneous routing decisions across shards, each seeing load=0 for
    // the same backends. With server-side tokenization (~10-12ms), natural
    // stagger mitigates this — but client-tokenize needs explicit sync.
    //
    // The broadcast interval controls the trade-off between SMP overhead and
    // load visibility freshness. Each broadcast generates O(shards²) SMP
    // messages (each shard sends to all others + foreign_ptr destructor
    // returns). At 100ms default, an 8-shard system generates ~1,120 SMP
    // messages/sec. At 5ms, that becomes ~24,000/sec — enough to congest
    // the reactor and inflate alien::run_on() completion latency (e.g.,
    // tokenization P50 from 12ms to 40ms).
    //
    // Disabled by default until validated in production benchmarks.
    // Enable via RANVIER_CROSS_SHARD_LOAD_SYNC=true with an appropriate
    // interval for your shard count and request rate.
    bool cross_shard_load_sync = false;                                    // Enable cross-shard load broadcasts
    std::chrono::milliseconds cross_shard_load_sync_interval{100};         // Broadcast interval (ms)

    // =========================================================================
    // Route Batch Flush Interval
    // =========================================================================
    // Controls the periodic flush interval for batched route learning (both
    // remote gossip routes on shard 0 and per-shard local routes).
    // Lower values improve cache affinity at the cost of higher SMP overhead.
    // Valid range: 1-1000ms. Default 20ms balances batch size for load-aware
    // routing decisions with signal freshness. Benchmarked across 5 intervals
    // (2ms-50ms) on 2 instances: 20ms gives best P99 (-77% to -86%), only
    // interval with P50 improvement, best throughput (+6% to +17%), and zero
    // transient hot-spotting failures (3/3 runs passed vs 1-2 failures at 10ms).
    std::chrono::milliseconds route_batch_flush_interval{20};              // Env: RANVIER_ROUTE_BATCH_FLUSH_INTERVAL_MS

    // Helper to check routing mode
    bool is_prefix_mode() const { return routing_mode == RoutingMode::PREFIX; }
    bool is_hash_mode() const { return routing_mode == RoutingMode::HASH; }
    bool is_random_mode() const { return routing_mode == RoutingMode::RANDOM; }
    bool uses_art() const { return routing_mode == RoutingMode::PREFIX; }
    bool should_learn_routes() const { return routing_mode == RoutingMode::PREFIX; }
};

// =============================================================================
// Assets Configuration
// =============================================================================

// Tokenizer/assets configuration
struct AssetsConfig {
    std::string tokenizer_path = "assets/gpt2.json";

    // Chat template format for message tokenization.
    // Controls how chat messages are formatted before tokenization so that
    // Ranvier's token sequences match what vLLM produces via apply_chat_template().
    // Values: "none" (legacy \n-joined), "llama3", "chatml", "mistral"
    // IMPORTANT: Must match the model family of the tokenizer JSON.
    std::string chat_template_format = "none";

    // Tokenization cache settings (optimization for repeated texts like system messages)
    // Expected hit rates: system messages 80-90%, role tags 95%+
    bool tokenization_cache_enabled = true;    // Enable LRU cache for tokenization results
    size_t tokenization_cache_size = 1000;     // Maximum cache entries (Rule #4: bounded)
    size_t tokenization_cache_max_text = 8192; // Don't cache texts longer than this (bytes)

    // Tokenizer thread pool settings
    // Offloads tokenization FFI to dedicated OS threads, fully freeing reactors.
    // Benchmarks show ~60% P99 TTFT reduction and ~20% throughput improvement.
    bool tokenizer_thread_pool_enabled = true;      // Disable via RANVIER_TOKENIZER_THREAD_POOL_ENABLED=false
    size_t tokenizer_thread_pool_queue_size = 256;  // Max pending jobs per shard (Rule #4)
    size_t tokenizer_thread_pool_min_text = 64;     // Min text length for thread pool dispatch (was 256; lowered to cover short prompts)
    size_t tokenizer_thread_pool_max_text = 65536;  // Max text length for thread pool dispatch

    // Local fallback semaphore: max concurrent reactor-blocking tokenizations per shard.
    // When both thread pool and cross-shard dispatch are unavailable, tokenize_locally()
    // blocks the reactor for 5-13ms. This semaphore limits concurrent local tokenizations
    // to prevent compounding stalls. If full, the request falls back to hash/random routing.
    // Default 1: at most one reactor-blocking tokenization per shard at a time.
    size_t tokenizer_local_fallback_max_concurrent = 1;
};

// =============================================================================
// Cost Estimation Configuration
// =============================================================================

// Cost estimation for priority tier assignment.
// Provides cost signals from request metadata (token estimates) that
// downstream priority tiers use for default priority assignment.
struct CostEstimationConfig {
    bool enabled = true;                        // Enable cost estimation on proxy requests
    double default_output_multiplier = 2.0;     // Multiplier for estimated output tokens when max_tokens absent
    uint64_t max_estimated_tokens = 1000000;    // Sanity cap on estimated tokens (Rule #4: bounded)
};

// =============================================================================
// Priority Tier Configuration
// =============================================================================

// Forward declaration — PriorityLevel is defined in http_controller.hpp.
// We use uint8_t here to avoid a circular include dependency; the config loader
// maps string values ("critical"/"high"/"normal"/"low") to PriorityLevel.
struct PriorityTierUserAgentEntry {
    std::string pattern;            // Substring match against User-Agent header
    uint8_t priority = 2;           // Maps to PriorityLevel (0=CRITICAL, 1=HIGH, 2=NORMAL, 3=LOW)
};

// Priority tier assignment configuration.
// Assigns priority levels to incoming requests based on headers, user-agent,
// and cost estimation. Used by the priority queue scheduler for
// queue-jumping and fair scheduling.
struct PriorityTierConfig {
    bool enabled = true;                            // Enable priority tier assignment
    std::string default_priority = "normal";        // Default priority ("critical"/"high"/"normal"/"low")
    double cost_threshold_high = 100.0;             // estimated_cost_units above which → HIGH
    double cost_threshold_low = 10.0;               // estimated_cost_units below which → LOW
    bool respect_header = true;                     // Honor X-Ranvier-Priority header

    // Known user-agent patterns with assigned priorities.
    // First substring match wins. MAX 64 entries (Hard Rule #4).
    static constexpr size_t MAX_KNOWN_USER_AGENTS = 64;
    std::vector<PriorityTierUserAgentEntry> known_user_agents = {
        {"Cursor",      0},   // CRITICAL
        {"claude-code", 0},   // CRITICAL
        {"cline",       1},   // HIGH
        {"aider",       1},   // HIGH
    };
};

// =============================================================================
// Intent Classification Configuration (VISION 1.4)
// =============================================================================

// Intent classification for wire-format inspection of incoming requests.
// Classifies requests as AUTOCOMPLETE (FIM), EDIT (code rewrite), or CHAT
// (default) to provide routing hints for downstream cost-based and agent-aware
// routing (VISION 2.3 / 3.2).
struct IntentClassificationConfig {
    bool enabled = true;                        // Enable intent classification on proxy requests

    // FIM field names to detect AUTOCOMPLETE intent (top-level JSON keys)
    // Hard Rule #4: bounded container with overflow counter
    static constexpr size_t MAX_FIM_FIELDS = 32;
    std::vector<std::string> fim_fields = {"suffix", "fim_prefix", "fim_middle", "fim_suffix"};

    // Keywords matched (case-insensitive substring) against the first system message
    // to detect EDIT intent
    // Hard Rule #4: bounded container with overflow counter
    static constexpr size_t MAX_EDIT_SYSTEM_KEYWORDS = 64;
    std::vector<std::string> edit_system_keywords = {"diff", "rewrite", "refactor", "edit", "patch", "apply"};

    // Tag patterns matched (literal substring) against the first system message
    // to detect EDIT intent
    // Hard Rule #4: bounded container with overflow counter
    static constexpr size_t MAX_EDIT_TAG_PATTERNS = 32;
    std::vector<std::string> edit_tag_patterns = {"<diff>", "<edit>", "<rewrite>", "<patch>"};
};

// =============================================================================
// Top-Level Configuration
// =============================================================================

// Top-level configuration
struct RanvierConfig {
    ServerConfig server;
    DatabaseConfig database;
    HealthConfig health;
    PoolConfig pool;
    RoutingConfig routing;
    TimeoutConfig timeouts;
    AssetsConfig assets;
    TlsConfig tls;
    AuthConfig auth;
    MetricsConfig metrics;
    RateLimitConfig rate_limit;
    RetryConfig retry;
    CircuitBreakerConfig circuit_breaker;
    ShutdownConfig shutdown;
    BackpressureConfig backpressure;
    ClusterConfig cluster;
    K8sDiscoveryConfig k8s_discovery;
    TelemetryConfig telemetry;
    LoadBalancingConfig load_balancing;
    CostEstimationConfig cost_estimation;
    PriorityTierConfig priority_tier;
    IntentClassificationConfig intent_classification;

    // Load configuration from YAML file (blocking - use only before reactor starts)
    static RanvierConfig load(const std::string& config_path);

    // Load with defaults (no file)
    static RanvierConfig defaults();

    // Validate configuration, returns error message if invalid, nullopt if valid
    static std::optional<std::string> validate(const RanvierConfig& config);

private:
    // Apply environment variable overrides
    void apply_env_overrides();

    // Helper to get env var as optional string
    static std::optional<std::string> get_env(const char* name);

    // Helper to parse numeric env vars
    template<typename T>
    static std::optional<T> get_env_as(const char* name);
};

}  // namespace ranvier
