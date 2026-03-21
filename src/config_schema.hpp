// Ranvier Core - Configuration Schema
//
// Pure data structures for configuration. No YAML parsing logic.
// This header defines all *Config structs, enums, and type aliases.
//
// For loading and validation, see config_loader.hpp.
// For backward compatibility, include config.hpp (facade).

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace ranvier {

// =============================================================================
// Server Configuration
// =============================================================================

// Server configuration (HTTP API and Prometheus)
struct ServerConfig {
    std::string bind_address = "0.0.0.0";
    uint16_t api_port = 8080;
    uint16_t metrics_port = 9180;
    size_t max_request_body_bytes = 10 * 1024 * 1024;  // Max request body size for proxy endpoints (default: 10MB, 0 = unlimited)
    uint32_t dns_resolution_timeout_seconds = 5;        // Timeout for DNS resolution in backend registration (seconds)
};

// =============================================================================
// Database Configuration
// =============================================================================

// Database/persistence configuration
struct DatabaseConfig {
    std::string path = "ranvier.db";
    std::string journal_mode = "WAL";
    std::string synchronous = "NORMAL";
};

// =============================================================================
// Health Check Configuration
// =============================================================================

// Health check configuration
struct HealthConfig {
    std::chrono::seconds check_interval{5};
    std::chrono::seconds check_timeout{3};
    uint32_t failure_threshold = 3;
    uint32_t recovery_threshold = 2;
};

// =============================================================================
// Connection Pool Configuration
// =============================================================================

// Connection pool configuration
struct PoolConfig {
    size_t max_connections_per_host = 10;
    std::chrono::seconds idle_timeout{60};
    size_t max_total_connections = 100;
    bool tcp_nodelay = true;
};

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
// Timeout Configuration
// =============================================================================

// Timeout configuration
struct TimeoutConfig {
    std::chrono::seconds connect_timeout{5};      // Timeout for establishing backend connection
    std::chrono::seconds request_timeout{300};    // Total timeout for entire request (5 min for LLM inference)
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
// TLS Configuration
// =============================================================================

// TLS configuration
struct TlsConfig {
    bool enabled = false;                     // Enable TLS for API server
    std::string cert_path = "";               // Path to certificate file (PEM)
    std::string key_path = "";                // Path to private key file (PEM)
};

// =============================================================================
// Authentication Configuration
// =============================================================================

// API key with metadata for rotation and audit
struct ApiKey {
    std::string key;                                  // The actual API key value (e.g., rnv_prod_abc123...)
    std::string name;                                 // Human-readable name for audit logs (e.g., "production-deploy")
    std::string created;                              // ISO 8601 creation date (e.g., "2025-01-01")
    std::optional<std::string> expires;               // Optional ISO 8601 expiry date (e.g., "2025-12-31")
    std::vector<std::string> roles;                   // Future RBAC prep (e.g., ["admin"], ["viewer"])

    // Check if this key has expired (returns false if no expiry set)
    bool is_expired() const {
        if (!expires.has_value() || expires->empty()) {
            return false;
        }
        // Parse expiry date (YYYY-MM-DD format) and compare with current date
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm now_tm = *std::gmtime(&now_time_t);

        // Parse expiry date
        std::tm expiry_tm = {};
        std::istringstream iss(*expires);
        iss >> std::get_time(&expiry_tm, "%Y-%m-%d");
        if (iss.fail()) {
            return false;  // Invalid date format, treat as non-expired
        }

        // Compare dates (expiry is valid until end of that day)
        if (now_tm.tm_year < expiry_tm.tm_year) return false;
        if (now_tm.tm_year > expiry_tm.tm_year) return true;
        if (now_tm.tm_mon < expiry_tm.tm_mon) return false;
        if (now_tm.tm_mon > expiry_tm.tm_mon) return true;
        return now_tm.tm_mday > expiry_tm.tm_mday;
    }

    // Get a safe prefix of the key for logging (first 8 chars or key name)
    std::string get_log_identifier() const {
        if (!name.empty()) {
            return name;
        }
        if (key.length() >= 12) {
            return key.substr(0, 12) + "...";
        }
        return "***";
    }
};

// Authentication configuration
struct AuthConfig {
    std::string admin_api_key = "";           // Legacy: single API key for admin endpoints (empty = no auth)
    std::vector<ApiKey> api_keys;             // New: multiple API keys with metadata

    // Constant-time string comparison to prevent timing attacks
    static bool secure_compare(const std::string& a, const std::string& b) {
        if (a.length() != b.length()) {
            // Still do a comparison to maintain constant time for equal-length strings
            volatile size_t dummy = 0;
            for (size_t i = 0; i < std::max(a.length(), b.length()); ++i) {
                dummy ^= (i < a.length() ? a[i] : 0) ^ (i < b.length() ? b[i] : 0);
            }
            (void)dummy;
            return false;
        }
        volatile unsigned char result = 0;
        for (size_t i = 0; i < a.length(); ++i) {
            result |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
        }
        return result == 0;
    }

    // Check if authentication is enabled
    bool is_enabled() const {
        return !admin_api_key.empty() || !api_keys.empty();
    }

    // Validate a token against configured keys
    // Returns: pair<is_valid, key_name_for_audit> (key_name is empty if invalid)
    std::pair<bool, std::string> validate_token(const std::string& token) const {
        // First check new multi-key format (takes precedence)
        for (const auto& api_key : api_keys) {
            if (secure_compare(api_key.key, token)) {
                if (api_key.is_expired()) {
                    return {false, api_key.get_log_identifier() + " (expired)"};
                }
                return {true, api_key.get_log_identifier()};
            }
        }

        // Fall back to legacy single key
        if (!admin_api_key.empty() && secure_compare(admin_api_key, token)) {
            return {true, "legacy-key"};
        }

        return {false, ""};
    }

    // Get count of valid (non-expired) keys
    size_t valid_key_count() const {
        size_t count = 0;
        for (const auto& api_key : api_keys) {
            if (!api_key.is_expired()) {
                ++count;
            }
        }
        if (!admin_api_key.empty()) {
            ++count;
        }
        return count;
    }
};

// =============================================================================
// Rate Limiting Configuration
// =============================================================================

// Rate limiting configuration
struct RateLimitConfig {
    bool enabled = false;                     // Enable rate limiting
    uint32_t requests_per_second = 100;       // Max requests per second per client
    uint32_t burst_size = 50;                 // Allow burst above rate limit
};

// =============================================================================
// Retry Configuration
// =============================================================================

// Retry configuration for transient failures
struct RetryConfig {
    uint32_t max_retries = 3;                         // Maximum retry attempts (0 = no retries)
    std::chrono::milliseconds initial_backoff{100};   // Initial backoff delay
    std::chrono::milliseconds max_backoff{5000};      // Maximum backoff delay
    double backoff_multiplier = 2.0;                  // Exponential backoff multiplier
    uint32_t max_stale_retries = 1;                   // Max retries for stale/empty backend responses (0 = disabled)
};

// =============================================================================
// Circuit Breaker Configuration
// =============================================================================

// Circuit breaker configuration for graceful degradation
struct CircuitBreakerConfig {
    bool enabled = true;                              // Enable circuit breaker
    uint32_t failure_threshold = 5;                   // Failures before opening circuit
    uint32_t success_threshold = 2;                   // Successes in half-open to close
    std::chrono::seconds recovery_timeout{30};        // Time before trying half-open
    bool fallback_enabled = true;                     // Try alternative backends on failure
};

// =============================================================================
// Shutdown Configuration
// =============================================================================

// Shutdown configuration for graceful drain
struct ShutdownConfig {
    std::chrono::seconds drain_timeout{30};           // Max time to wait for in-flight requests
    std::chrono::seconds shutdown_timeout{60};        // Max time for entire shutdown sequence
    std::chrono::milliseconds gossip_broadcast_timeout{5000}; // Max time to broadcast DRAINING state
};

// =============================================================================
// Backpressure Configuration
// =============================================================================

// Backpressure configuration for system stability
struct BackpressureConfig {
    // Concurrency limits per shard
    size_t max_concurrent_requests = 1000;            // Max concurrent requests per shard (0 = unlimited)

    // Persistence queue integration
    bool enable_persistence_backpressure = true;      // Throttle writes when persistence queue is full
    double persistence_queue_threshold = 0.8;         // Start throttling at 80% of max_queue_depth

    // Response configuration
    uint32_t retry_after_seconds = 1;                 // Retry-After header value for 503 responses
};

// =============================================================================
// Cluster / Gossip Configuration
// =============================================================================

// Discovery type for DNS-based peer discovery
enum class DiscoveryType {
    STATIC,  // Use static peer list only (default)
    A,       // Use DNS A records (IPs only, use default gossip_port)
    SRV      // Use DNS SRV records (IPs and ports)
};

// Gossip TLS configuration for mTLS/DTLS encryption
struct GossipTlsConfig {
    bool enabled = false;                              // Enable DTLS encryption for gossip
    std::string cert_path = "";                        // Path to node certificate (PEM)
    std::string key_path = "";                         // Path to private key (PEM)
    std::string ca_path = "";                          // Path to CA certificate for peer verification (PEM)
    bool verify_peer = true;                           // Require mutual TLS (peer certificate verification)
    std::chrono::seconds cert_reload_interval{300};    // Interval to check for certificate changes (0 = disabled)
    bool allow_plaintext_fallback = false;             // Allow plaintext if TLS handshake fails (NOT recommended)
};

// Cluster configuration for distributed mode (gossip-based state sync)
struct ClusterConfig {
    bool enabled = false;                                  // Enable distributed mode
    uint16_t gossip_port = 9190;                          // UDP port for gossip protocol
    std::vector<std::string> peers;                       // Static list of peer addresses (IP:Port)
    std::chrono::milliseconds gossip_interval{1000};      // Interval between gossip rounds
    std::chrono::seconds gossip_heartbeat_interval{5};    // Interval between heartbeat broadcasts
    std::chrono::seconds gossip_peer_timeout{15};         // Time before marking a peer as dead

    // DNS-based peer discovery
    std::string discovery_dns_name;                        // DNS name to query for peer discovery (empty = disabled)
    DiscoveryType discovery_type = DiscoveryType::STATIC;  // Type of DNS records to query
    std::chrono::seconds discovery_refresh_interval{30};   // Interval between DNS refresh queries

    // Reliable delivery settings (ACKs, retries, deduplication)
    bool gossip_reliable_delivery = true;                  // Enable reliable delivery with ACKs
    std::chrono::milliseconds gossip_ack_timeout{100};     // Timeout before retrying unACK'd packets
    uint32_t gossip_max_retries = 3;                       // Maximum retry attempts before giving up
    size_t gossip_dedup_window = 1000;                     // Size of per-peer sequence window for dedup

    // TLS/DTLS encryption for gossip (mTLS)
    GossipTlsConfig tls;                                   // DTLS configuration for encrypted gossip

    // Split-brain detection / Quorum settings
    bool quorum_enabled = true;                            // Enable quorum-based split-brain detection
    double quorum_threshold = 0.5;                         // Fraction of peers required (N*threshold+1 for majority)
    bool reject_routes_on_quorum_loss = true;              // Reject new route writes when quorum is lost
    uint32_t quorum_warning_threshold = 1;                 // Warn when alive peers <= required + this threshold
    std::chrono::seconds quorum_check_window{30};          // Window to count recently seen peers (check_quorum)

    // Fail-open mode for split-brain handling (inference workloads)
    // When enabled during quorum loss, requests are routed randomly to healthy backends
    // instead of being rejected. This prioritizes availability over consistency.
    bool fail_open_on_quorum_loss = false;                 // true = random routing during split-brain, false = reject (default)
    bool accept_gossip_on_quorum_loss = false;             // true = accept incoming gossip during split-brain (stale > none)

    // DTLS Security Lockdown
    bool mtls_enabled = false;                             // Enforce mTLS: drop non-DTLS packets when TLS enabled

    // Node self-identification for graceful shutdown notifications
    // When this node shuts down, it broadcasts DRAINING with this ID so peers
    // can set the backend's weight to 0 and stop sending new traffic
    int32_t self_backend_id = 0;                           // This node's backend ID (0 = unset, must configure for cluster)
};

// =============================================================================
// Kubernetes Discovery Configuration
// =============================================================================

// Kubernetes service discovery configuration
struct K8sDiscoveryConfig {
    bool enabled = false;                                  // Enable K8s discovery
    std::string api_server = "https://kubernetes.default.svc";  // K8s API server URL
    std::string namespace_name = "default";                // Namespace to watch
    std::string service_name;                              // Service name to watch (required if enabled)
    uint16_t target_port = 8080;                          // Target port for GPU backends

    // Authentication (uses in-cluster service account by default)
    std::string token_path = "/var/run/secrets/kubernetes.io/serviceaccount/token";
    std::string ca_cert_path = "/var/run/secrets/kubernetes.io/serviceaccount/ca.crt";

    // Polling configuration
    std::chrono::seconds poll_interval{30};                // Interval between full syncs
    std::chrono::seconds watch_timeout{300};               // Watch connection timeout

    // Watch reconnection settings
    std::chrono::seconds watch_reconnect_delay{5};         // Delay before reconnecting watch after failure
    std::chrono::seconds watch_reconnect_max_delay{60};    // Maximum reconnect delay (for exponential backoff)

    // TLS settings
    bool verify_tls = true;                                // Verify server certificate

    // Label selector (optional, filters EndpointSlices)
    std::string label_selector;

    // DNS resolution settings
    std::chrono::seconds dns_timeout{5};                   // Timeout for DNS resolution
    uint32_t dns_max_retries = 3;                          // Max retry attempts for transient DNS failures
    std::chrono::milliseconds dns_initial_backoff{100};    // Initial backoff delay for DNS retries
};

// =============================================================================
// Telemetry Configuration
// =============================================================================

// OpenTelemetry distributed tracing configuration
struct TelemetryConfig {
    bool enabled = false;                                  // Enable OpenTelemetry tracing
    std::string otlp_endpoint = "http://localhost:4318";   // OTLP HTTP endpoint (Jaeger, Tempo, etc.)
    std::string service_name = "ranvier";                  // Service name for traces
    double sample_rate = 1.0;                              // Trace sampling rate (0.0-1.0)
    std::chrono::milliseconds export_interval{5000};       // Batch export interval
    size_t max_queue_size = 2048;                          // Max pending spans in queue
    size_t max_export_batch_size = 512;                    // Max spans per export batch
};

// =============================================================================
// Load Balancing Configuration
// =============================================================================

// Shard load balancing configuration (P2C algorithm)
// Distributes requests across CPU cores to prevent hot shards
struct LoadBalancingConfig {
    bool enabled = true;                                   // Enable cross-shard load balancing
    double min_load_difference = 0.2;                      // Min load difference ratio to trigger dispatch
    uint64_t local_processing_threshold = 10;              // Process locally if active requests < threshold
    uint64_t snapshot_refresh_interval_us = 1000;          // Shard metrics snapshot refresh interval (microseconds)
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
    RateLimitConfig rate_limit;
    RetryConfig retry;
    CircuitBreakerConfig circuit_breaker;
    ShutdownConfig shutdown;
    BackpressureConfig backpressure;
    ClusterConfig cluster;
    K8sDiscoveryConfig k8s_discovery;
    TelemetryConfig telemetry;
    LoadBalancingConfig load_balancing;

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
