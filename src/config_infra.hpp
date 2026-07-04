// Ranvier Core - Infrastructure Configuration
//
// Generic infrastructure config structs that are not specific to Ranvier's
// routing/tokenization domain. Includes server, pool, health, TLS, auth,
// database, timeout, metrics endpoint, rate limiting, retry, circuit breaker,
// shutdown, backpressure, cluster/gossip, K8s discovery, telemetry, and
// load balancing configs.
//
// For Ranvier-specific configs (RoutingConfig, AssetsConfig), see config_schema.hpp.
// For the top-level RanvierConfig aggregate, see config_schema.hpp.

#pragma once

#include <algorithm>
#include <array>
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

    // vLLM metrics scraping — scrape Prometheus /metrics from backends
    bool enable_vllm_metrics = true;                         // Scrape /metrics if available
    std::chrono::milliseconds vllm_metrics_timeout{200};     // Timeout for /metrics fetch
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
// Timeout Configuration
// =============================================================================

// Timeout configuration
struct TimeoutConfig {
    std::chrono::seconds connect_timeout{5};      // Timeout for establishing backend connection
    std::chrono::seconds request_timeout{300};    // Total timeout for entire request (5 min for LLM inference)
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

    // Instant at which the key becomes expired, parsed once by the config loader.
    // Semantics: the expiry date is valid *through* the end of that UTC day, so
    // this holds midnight UTC at the start of the following day. nullopt means
    // "never expires" — no expiry configured, or an unparseable date (which the
    // loader warns about and treats as non-expiring, matching legacy behavior).
    // Parsing lives in the loader (its layer), off the per-request auth path —
    // is_expired() is a lock-free, static-free comparison callable concurrently
    // on every shard.
    std::optional<std::chrono::system_clock::time_point> expires_at;

    // Check if this key has expired (returns false if no expiry set)
    bool is_expired() const {
        return expires_at.has_value() && std::chrono::system_clock::now() >= *expires_at;
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

    // Rule #4: Bounded container — the loader truncates over-cap api_keys lists.
    // Distinct from ShardLocalState::MAX_API_KEYS (router_service), which caps
    // per-backend upstream credentials; this caps operator auth keys.
    static constexpr size_t MAX_AUTH_API_KEYS = 1000;

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
// Metrics Endpoint Security Configuration
// =============================================================================

// Security configuration for the Prometheus metrics endpoint (port 9180)
// When auth_token and/or allowed_ips are set, requests must pass both checks (AND logic)
struct MetricsConfig {
    std::string auth_token;                    // Bearer token for /metrics auth (empty = no auth)
    std::vector<std::string> allowed_ips;      // IP allowlist (empty = allow all, supports exact IPs and CIDR notation)

    // Rule #4: Bounded container — reject configs with excessive IP entries
    static constexpr size_t MAX_ALLOWED_IPS = 1000;

    bool auth_enabled() const { return !auth_token.empty(); }
    bool ip_filter_enabled() const { return !allowed_ips.empty(); }
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

    // Priority queue scheduling
    bool enable_priority_queue = false;               // Gate: use priority-aware scheduling instead of direct semaphore

    // Default per-tier queue capacities [CRITICAL, HIGH, NORMAL, LOW].
    // Smaller tiers for higher priority keeps latency-sensitive requests from
    // being buried behind a deep queue; larger tiers absorb burst traffic.
    static constexpr uint32_t DEFAULT_TIER_CAPACITY_CRITICAL =  64;
    static constexpr uint32_t DEFAULT_TIER_CAPACITY_HIGH     = 128;
    static constexpr uint32_t DEFAULT_TIER_CAPACITY_NORMAL   = 256;
    static constexpr uint32_t DEFAULT_TIER_CAPACITY_LOW      = 512;
    std::array<uint32_t, 4> tier_capacity = {
        DEFAULT_TIER_CAPACITY_CRITICAL,
        DEFAULT_TIER_CAPACITY_HIGH,
        DEFAULT_TIER_CAPACITY_NORMAL,
        DEFAULT_TIER_CAPACITY_LOW,
    };

    // Per-agent queue depth limit.
    // Prevents a single paused agent from consuming an entire tier's capacity.
    // When an agent has >= max_per_agent_queued requests across all tiers,
    // new requests for that agent are dropped (Hard Rule #4: bounded).
    uint32_t max_per_agent_queued = 128;
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

    // Rule #4: Bounded container — caps configured peers (loader, YAML + env) and
    // the DNS-discovered merge in gossip_protocol::refresh_peers(). Must stay
    // well below GossipProtocol::MAX_DEDUP_PEERS (10000) so the per-peer dedup
    // maps keyed off this set never approach their own ceiling.
    static constexpr size_t MAX_PEERS = 256;
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

    // Cache-aware autoscaling telemetry (BACKLOG §21 P2). When off, the cluster
    // sole-holder path is dark: no HOT_PREFIX_DIGEST gossip is emitted, received
    // digests are ignored, and /v1/cache/topology + the sole-held gauges omit the
    // holders/sole_held/quorum surface (P1 hot-prefix reporting is unaffected).
    // Default off so P2 ships dark and is enabled per-cluster once stabilized.
    bool enable_cache_topology = false;
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

    // Emit OpenTelemetry GenAI semantic-convention attributes (gen_ai.*) on the
    // request span so Ranvier slots into the standard LLM-observability stack
    // (Datadog/GCP/Azure map the semconv natively).  Default on — costless when
    // tracing is disabled, and set_attribute() is a no-op on non-recording
    // spans.  Set false to suppress the gen_ai.* attributes specifically.
    bool genai_semconv = true;
};

// =============================================================================
// Telemetry Sink Configuration (aggregate metrics export)
// =============================================================================

// Pluggable telemetry sink for exporting aggregate routing/cache metrics to an
// operator-chosen backend. DISTINCT from `TelemetryConfig` above (which is
// OpenTelemetry distributed tracing) — this controls the periodic window-
// report emitter and the per-shard counters that feed it.
//
// Off by default. With `enabled=false`, the recording entry point compiles to
// a single predictable branch with no allocation, no map lookup, no histogram
// work — the routing path behaves exactly as today.
//
// See `src/telemetry_sink.hpp` for the sink contract and forward-compat
// discipline that bind concrete sink implementations.
struct TelemetrySinkConfig {
    // Master switch. Off by default — stock build behaves exactly as today.
    bool enabled = false;

    // Window length: how often the emitter on shard 0 builds a report and
    // hands it to the sink. Lower values give finer time resolution at the
    // cost of more cross-shard map_reduce work; the typical operator value
    // is 60s.
    std::chrono::seconds window{60};

    // Per-shard hard cap on distinct bucket keys (Hard Rule #4). The key's
    // backend-derived dimensions all come from operator config / the
    // registered backend, so real cardinality is small; this cap is defence
    // in depth. Keys beyond it fold into the `_overflow` sentinel (counted in
    // `WindowReport::buckets_overflowed`). The default carries headroom for the
    // product of the key's dimensions across a mixed fleet; memory is bounded
    // at any value, and the loader rejects values above its ceiling.
    size_t max_buckets = 512;
};

// =============================================================================
// Usage-Ledger Configuration (per-API-key usage event sink)
// =============================================================================

// Pluggable per-request usage-event sink for external metering / attribution /
// billing backends. DISTINCT from `TelemetrySinkConfig` above (aggregate
// routing/cache metrics) and from `TelemetryConfig` (OpenTelemetry tracing):
// this hands one UsageEvent per completed request to the installed sink on the
// shard that served it.
//
// Off by default. With `enabled=false`, no sink is created and the completion
// path is a single null check — behaviour is exactly as today. The seam itself
// has only the master switch; a concrete sink reads its own configuration
// out-of-band (endpoint, credentials, buffer bounds), exactly like the
// telemetry-sink factory. See `src/usage_ledger_sink.hpp` for the sink contract
// and `src/usage_ledger_schema.hpp` for the event schema + forward-compat
// discipline that bind concrete implementations.
struct UsageLedgerConfig {
    // Master switch. Off by default — stock build behaves exactly as today.
    bool enabled = false;
};

// =============================================================================
// GIE Endpoint-Picker (EPP) Configuration
// =============================================================================

// Exposes the routing core as a Gateway API Inference Extension (GIE)
// Endpoint-Picker: a gRPC `envoy.service.ext_proc.v3.ExternalProcessor` server
// that a GIE-conformant gateway delegates endpoint selection to (it returns the
// chosen backend via the x-gateway-destination-endpoint header). This is a
// COMPATIBILITY mode alongside the standalone inline data plane, not a
// replacement for it.
//
// Compiled only when the build sets WITH_GIE_EPP=ON (default OFF — gRPC is a
// heavy dependency and most deployments use the inline path; see
// CMakeLists.txt and src/gie_epp_server.hpp). When the binary lacks
// WITH_GIE_EPP, `enabled=true` logs a warning and the server stays inert.
// Off by default; toggling `enabled` requires a restart.
struct GieEppConfig {
    // Master switch. Off by default.
    bool enabled = false;
    // gRPC listen port for the ext_proc ExternalProcessor service.
    uint16_t port = 9002;
    // Listen address for the gRPC server.
    std::string listen_address = "0.0.0.0";
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

}  // namespace ranvier
