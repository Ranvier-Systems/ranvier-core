#pragma once

#include "async_persistence.hpp"
#include "chat_template.hpp"
#include "circuit_breaker.hpp"
#include "config.hpp"
#include "connection_pool.hpp"
#include "cross_shard_request.hpp"
#include "intent_classifier.hpp"
#include "metrics_service.hpp"
#include "proxy_retry_policy.hpp"
#include "rate_limiter.hpp"
#include "request_scheduler.hpp"
#include "router_service.hpp"
#include "shard_load_balancer.hpp"
#include "shard_load_metrics.hpp"
#include "tokenizer_service.hpp"

#include <array>
#include <functional>
#include <limits>

#include <seastar/core/condition-variable.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sharded.hh>
#include <seastar/http/httpd.hh>
#include <seastar/http/request.hh>
#include <seastar/http/reply.hh>

namespace ranvier {

// Retry configuration
struct RetrySettings {
    uint32_t max_retries = 3;
    std::chrono::milliseconds initial_backoff{100};
    std::chrono::milliseconds max_backoff{5000};
    double backoff_multiplier = 2.0;
};

// Circuit breaker settings
struct CircuitBreakerSettings {
    bool enabled = true;
    uint32_t failure_threshold = 5;
    uint32_t success_threshold = 2;
    std::chrono::seconds recovery_timeout{30};
    bool fallback_enabled = true;
};

// Backpressure settings for concurrency control
struct BackpressureSettings {
    size_t max_concurrent_requests = 1000;            // Max concurrent requests per shard (0 = unlimited)
    bool enable_persistence_backpressure = true;      // Throttle writes when persistence queue is full
    double persistence_queue_threshold = 0.8;         // Start throttling at 80% of max_queue_depth
    uint32_t retry_after_seconds = 1;                 // Retry-After header value for 503 responses

    // Priority queue scheduling (defaults mirror BackpressureConfig)
    bool enable_priority_queue = false;
    std::array<uint32_t, 4> tier_capacity = {
        BackpressureConfig::DEFAULT_TIER_CAPACITY_CRITICAL,
        BackpressureConfig::DEFAULT_TIER_CAPACITY_HIGH,
        BackpressureConfig::DEFAULT_TIER_CAPACITY_NORMAL,
        BackpressureConfig::DEFAULT_TIER_CAPACITY_LOW,
    };
};

// Shard load balancing settings
// NOTE: Cross-shard dispatch is not yet implemented (requires foreign_ptr plumbing
// per Hard Rule #14). These settings control the ShardLoadBalancer snapshot cache
// and P2C algorithm, which will be used when dispatch is implemented.
struct LoadBalancingSettings {
    bool enabled = false;                             // Disabled until cross-shard dispatch is implemented
    double min_load_difference = 0.2;                 // Min load difference (ratio) to trigger dispatch
    uint64_t local_processing_threshold = 10;         // Process locally if active < this threshold
    uint64_t snapshot_refresh_interval_us = 1000;     // Snapshot cache refresh interval (microseconds)
};

// Context struct for proxy request state - reduces parameter passing and improves readability
// This bundles all state that flows through the proxy request lifecycle
struct ProxyContext {
    // Request identification
    std::string request_id;
    std::string backend_traceparent;
    std::string routing_mode;
    std::string endpoint;  // API endpoint path (e.g., "/v1/chat/completions" or "/v1/completions")

    // Request body and tokens
    std::string forwarded_body;
    std::vector<int32_t> tokens;
    size_t prefix_boundary = 0;  // Token count of shared prefix (e.g., system messages)
    std::vector<size_t> prefix_boundaries;  // Multi-depth boundaries for Option C routing

    // Target backend
    BackendId target_id;
    seastar::socket_address target_addr;
    BackendId current_backend;            // May differ from target_id after fallback
    seastar::socket_address current_addr;

    // Timing
    std::chrono::steady_clock::time_point request_start;
    std::chrono::steady_clock::time_point connect_start;
    seastar::lowres_clock::time_point request_deadline;

    // Configuration (captured from HttpControllerConfig)
    std::chrono::seconds connect_timeout;
    std::chrono::seconds request_timeout;
    RetrySettings retry_config;
    bool fallback_enabled;

    // State flags
    bool shard_metrics_active = false;
    bool timed_out = false;
    bool connection_failed = false;
    bool connection_error = false;
    bool client_disconnected = false;
    bool stream_closed = false;
    bool response_latency_recorded = false;

    // Retry tracking
    uint32_t retry_attempt = 0;
    std::chrono::milliseconds current_backoff;
    uint32_t fallback_attempts = 0;
    static constexpr uint32_t MAX_FALLBACK_ATTEMPTS = 3;

    // Stale connection retry tracking
    size_t bytes_written_to_client = 0;   // Total bytes written to client output stream
    uint32_t chunks_received = 0;         // Chunks received from backend
    uint32_t stale_retry_attempt = 0;     // Number of stale connection retries attempted

    // Cost estimation (populated before routing)
    uint64_t estimated_input_tokens = 0;   // Heuristic: content chars / 4
    uint64_t estimated_output_tokens = 0;  // max_tokens from request, or input * output_multiplier
    double estimated_cost_units = 0.0;     // input + (output * output_multiplier)

    // Priority tier (populated by extract_priority before routing)
    PriorityLevel priority = PriorityLevel::NORMAL;

    // Intent classification (populated by classify_intent before routing)
    // Routing preference derived from intent — advisory, not enforced yet.
    // AUTOCOMPLETE → prefer lowest-latency backend
    // EDIT → prefer highest-capability backend
    // CHAT → use normal prefix/cost routing
    // Actual intent-based route selection is not yet implemented.
    RequestIntent intent = RequestIntent::CHAT;

    // Agent identification for fair scheduling (User-Agent header value)
    std::string user_agent;

    // Queue timing (set by RequestScheduler::enqueue)
    std::chrono::steady_clock::time_point enqueue_time;

    // Promise fulfilled by process_priority_queue() when this request is dequeued
    // and a semaphore unit has been acquired. The handle_proxy coroutine awaits
    // the corresponding future before proceeding with the streaming lambda.
    seastar::promise<seastar::semaphore_units<>> dequeue_promise;
};

// Type alias: production scheduler uses ProxyContext
using RequestScheduler = BasicRequestScheduler<ProxyContext>;

// HTTP controller configuration
struct HttpControllerConfig {
    ConnectionPoolConfig pool;
    size_t min_token_length = 4;  // Minimum tokens before caching a route
    std::chrono::seconds connect_timeout{5};    // Timeout for backend connection
    std::chrono::seconds request_timeout{300};  // Total timeout for request
    AuthConfig auth;                            // Authentication configuration (multi-key support)
    RateLimiterConfig rate_limit;               // Rate limiting configuration
    RetrySettings retry;                        // Retry configuration
    CircuitBreakerSettings circuit_breaker;     // Circuit breaker configuration
    BackpressureSettings backpressure;          // Backpressure configuration
    LoadBalancingSettings load_balancing;       // Shard load balancing configuration
    std::chrono::seconds drain_timeout{30};     // Max time to wait for in-flight requests during shutdown
    bool enable_token_forwarding = false;       // Forward pre-computed token IDs to backends (vLLM prompt_token_ids)
    bool accept_client_tokens = false;          // Accept pre-tokenized prompt_token_ids from clients for routing
    int32_t max_token_id = 200000;              // Maximum valid token ID (auto-set from tokenizer vocab)
    RoutingConfig::RoutingMode routing_mode = RoutingConfig::RoutingMode::PREFIX;  // Routing mode
    uint32_t block_alignment = 16;              // vLLM PagedAttention block size for route alignment
    bool enable_prefix_boundary = true;         // Enable automatic prefix boundary detection
    size_t min_prefix_boundary_tokens = 4;      // Minimum system message tokens for prefix boundary
    bool accept_client_prefix_boundary = false; // Accept client-provided prefix_token_count
    bool enable_multi_depth_routing = false;    // Enable multi-depth route storage (Option C)
    ChatTemplate chat_template;                 // Pre-compiled chat template for vLLM-aligned tokenization
    uint32_t max_stale_retries = 1;              // Max retries for empty backend responses on stale pooled connections (0 = disabled)
    size_t max_request_body_bytes = 10 * 1024 * 1024;  // Max request body size for proxy endpoints (default: 10MB, 0 = unlimited)
    uint32_t dns_resolution_timeout_seconds = 5;        // Timeout for DNS resolution in backend registration (seconds)

    // Cost estimation settings
    bool cost_estimation_enabled = true;               // Enable cost estimation
    double cost_estimation_output_multiplier = 2.0;    // Default output token multiplier
    uint64_t cost_estimation_max_tokens = 1000000;     // Sanity cap on estimated tokens

    // Priority tier settings (copied from PriorityTierConfig at init)
    bool priority_tier_enabled = true;
    std::string priority_tier_default = "normal";
    double priority_tier_cost_threshold_high = 100.0;
    double priority_tier_cost_threshold_low = 10.0;
    bool priority_tier_respect_header = true;
    std::vector<PriorityTierUserAgentEntry> priority_tier_known_user_agents = {
        {"Cursor",      0},   // CRITICAL
        {"claude-code", 0},   // CRITICAL
        {"cline",       1},   // HIGH
        {"aider",       1},   // HIGH
    };

    // Intent classification settings (copied from IntentClassificationConfig at init)
    bool intent_classification_enabled = true;
    IntentClassifierConfig intent_classifier;

    // Local mode settings (copied from LocalModeConfig at init)
    LocalModeConfig local_mode;

    // Helper methods for routing mode checks
    bool is_prefix_mode() const { return routing_mode == RoutingConfig::RoutingMode::PREFIX; }
    bool is_hash_mode() const { return routing_mode == RoutingConfig::RoutingMode::HASH; }
    bool is_random_mode() const { return routing_mode == RoutingConfig::RoutingMode::RANDOM; }
    bool uses_art() const { return routing_mode == RoutingConfig::RoutingMode::PREFIX; }
    bool should_learn_routes() const { return routing_mode == RoutingConfig::RoutingMode::PREFIX; }
};

class HttpController {
public:
    // Takes sharded<TokenizerService> to access the local shard's tokenizer (thread-safe)
    HttpController(seastar::sharded<TokenizerService>& t, RouterService& r, HttpControllerConfig config = {})
        : _tokenizer(t), _router(r), _pool(config.pool), _config(config),
          _rate_limiter(config.rate_limit),
          _circuit_breaker(CircuitBreaker::Config{
              config.circuit_breaker.failure_threshold,
              config.circuit_breaker.success_threshold,
              config.circuit_breaker.recovery_timeout,
              config.circuit_breaker.enabled
          }),
          _persistence(nullptr),
          _load_balancer(nullptr),
          // Backpressure: 0 means unlimited (use max size_t as semaphore limit)
          _request_semaphore(effective_semaphore_limit(config.backpressure.max_concurrent_requests)),
          _scheduler(SchedulerSettings{config.backpressure.tier_capacity}) {
        // Initialize load balancer configuration
        _lb_config.enabled = config.load_balancing.enabled;
        _lb_config.min_load_difference = config.load_balancing.min_load_difference;
        _lb_config.local_processing_threshold = config.load_balancing.local_processing_threshold;
        _lb_config.snapshot_refresh_interval_us = config.load_balancing.snapshot_refresh_interval_us;
    }

private:
    // Helper to compute effective semaphore limit (0 = unlimited)
    static size_t effective_semaphore_limit(size_t configured_limit) {
        return configured_limit > 0 ? configured_limit : std::numeric_limits<size_t>::max();
    }

public:

    // Set optional async persistence manager (call before serving requests)
    // Uses fire-and-forget queueing to avoid blocking the reactor
    void set_persistence(AsyncPersistenceManager* manager) {
        _persistence = manager;
        _persistence_backpressure_active = (manager != nullptr && _config.backpressure.enable_persistence_backpressure);
    }

    // Set optional shard load balancer (call before serving requests)
    // Enables cross-shard request dispatch using Power of Two Choices algorithm
    void set_load_balancer(seastar::sharded<ShardLoadBalancer>* lb) { _load_balancer = lb; }

    // Set config reload callback (for /admin/keys/reload endpoint)
    // Callback returns true on success, false on failure
    // The callback is synchronous as it should just trigger the reload process
    using ConfigReloadCallback = std::function<bool()>;
    void set_config_reload_callback(ConfigReloadCallback callback) { _config_reload_callback = std::move(callback); }

    // Hot-reload: Update configuration at runtime
    void update_config(const HttpControllerConfig& config) {
        _config = config;
        _rate_limiter.update_config(config.rate_limit);
        _circuit_breaker.update_config(CircuitBreaker::Config{
            config.circuit_breaker.failure_threshold,
            config.circuit_breaker.success_threshold,
            config.circuit_breaker.recovery_timeout,
            config.circuit_breaker.enabled
        });
        // Update cached persistence backpressure flag
        _persistence_backpressure_active = (_persistence != nullptr && config.backpressure.enable_persistence_backpressure);
        // Update load balancer config
        _lb_config.enabled = config.load_balancing.enabled;
        _lb_config.min_load_difference = config.load_balancing.min_load_difference;
        _lb_config.local_processing_threshold = config.load_balancing.local_processing_threshold;
        _lb_config.snapshot_refresh_interval_us = config.load_balancing.snapshot_refresh_interval_us;
        if (_load_balancer) {
            _load_balancer->local().update_config(_lb_config);
        }
    }

    // Register all endpoints
    void register_routes(seastar::httpd::routes& r);

    // Rate limit helper - public for handler wrapper access
    bool check_rate_limit(const seastar::http::request& req);

    // Seastar sharded<> lifecycle: orderly per-shard cleanup (Hard Rule #5/#6).
    // Stops rate limiter timer and closes request gate. Safe to call even if
    // wait_for_drain() was already called (idempotent).
    seastar::future<> stop();

    // Graceful shutdown: Start draining (reject new requests)
    void start_draining();

    // Graceful shutdown: Wait for in-flight requests to complete or timeout
    seastar::future<> wait_for_drain();

    // Check if currently draining
    bool is_draining() const;

    // Remove circuit breaker entry for a deregistered backend (Rule #4: bounded container cleanup)
    // Called via RouterService callback when a backend is unregistered from all shards
    void remove_circuit(BackendId backend_id) {
        _circuit_breaker.remove_circuit(backend_id);
    }

    // Get circuit breaker metrics for Prometheus
    uint64_t get_circuits_removed() const { return _circuit_breaker.get_circuits_removed(); }

private:
    seastar::sharded<TokenizerService>& _tokenizer;  // Access via local() for thread safety
    RouterService& _router;
    ConnectionPool _pool;
    HttpControllerConfig _config;
    RateLimiter _rate_limiter;
    CircuitBreaker _circuit_breaker;
    AsyncPersistenceManager* _persistence;  // Async persistence (fire-and-forget, non-blocking)
    bool _persistence_backpressure_active{false};  // Cached: persistence != null && backpressure enabled
    ConfigReloadCallback _config_reload_callback;  // Callback for config hot-reload

    // Shard load balancing (P2C algorithm)
    seastar::sharded<ShardLoadBalancer>* _load_balancer;  // Cross-shard load balancer
    ShardLoadBalancerConfig _lb_config;  // Local copy of load balancer config

    // Graceful shutdown state
    // Plain bool: set/read only from this shard's reactor via invoke_on_all/local handler
    bool _draining{false};
    bool _stopped{false};                // Guards against double-stop (wait_for_drain + stop)
    seastar::gate _request_gate;         // Tracks in-flight requests

    // Backpressure: semaphore for concurrency limiting
    // Uses try_get_units() for immediate rejection (no queueing)
    seastar::semaphore _request_semaphore;

    // Priority-aware request scheduler
    RequestScheduler _scheduler;
    seastar::condition_variable _queue_cv;  // Signaled by enqueue(), waited by process_priority_queue()
    seastar::gate _queue_gate;              // Gate for background dequeue fiber (Rule #5)
    seastar::metrics::metric_groups _scheduler_metrics;  // Deregistered in stop() (Rule #6)

    // Backpressure metrics (shard-local — no cross-shard access)
    uint64_t _requests_rejected_concurrency{0};   // Rejected due to concurrency limit
    uint64_t _requests_rejected_persistence{0};   // Rejected due to persistence backpressure
    uint64_t _requests_rejected_body_size{0};     // Rejected due to request body size limit
    uint64_t _retries_skipped_circuit_open{0};    // Retries skipped because circuit breaker opened

    // Shard load balancing metrics (reserved for future cross-shard dispatch)
    uint64_t _requests_local_dispatch{0};         // Requests processed locally
    uint64_t _requests_cross_shard_dispatch{0};   // Requests dispatched cross-shard (always 0 until dispatch is implemented)

    // Check if persistence queue is under backpressure
    bool is_persistence_backpressured() const;

    // Extract request body size from Content-Length header or actual content.
    // Returns 0 if Content-Length is missing/unparseable and content is empty.
    static size_t get_request_body_size(const seastar::http::request& req);

    // Select target shard for request processing using P2C algorithm
    // Returns local shard if load balancing disabled or not beneficial
    uint32_t select_target_shard();

    // Helper handlers
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_proxy(
        std::unique_ptr<seastar::http::request> req,
        std::unique_ptr<seastar::http::reply> rep,
        std::string endpoint = "/v1/chat/completions");
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_broadcast_route(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_broadcast_backend(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);

    // Admin handlers
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_delete_backend(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_delete_routes(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_clear_all(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_keys_reload(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);

    // State inspection handlers (for rvctl CLI)
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_dump_tree(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_dump_cluster(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_dump_backends(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_dump_config(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_drain_backend(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);

    // Health check handler (public, no auth required)
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_health(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);

    // Scheduler stats handler (admin, auth required)
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_scheduler_stats(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);

    // Background dequeue loop for priority queue (gate-guarded, Rule #5)
    seastar::future<> process_priority_queue();

    // Dispatch a dequeued request through the normal proxy pipeline
    // Fire-and-forget with gate guard (Rule #18: no discarded futures)
    seastar::future<> dispatch_proxied_request(std::unique_ptr<ProxyContext> ctx, seastar::semaphore_units<> units);

    // Acquire a concurrency slot, either directly (try_get_units) or via the
    // priority queue scheduler.  Returns nullopt when the request should be
    // rejected with 503 (queue full / concurrency limit).
    // Rule #22: all params by value.
    struct AcquireResult {
        std::optional<seastar::semaphore_units<>> units;
        bool rejected = false;          // true → caller should 503
        std::string rejection_reason;   // populated when rejected
    };
    seastar::future<AcquireResult> acquire_concurrency_slot(
        PriorityLevel priority,
        std::string request_id,
        std::string user_agent,
        std::chrono::steady_clock::time_point request_start,
        std::optional<seastar::semaphore_units<>> early_units);

    // Auth helper - returns true if authorized, false otherwise
    bool check_admin_auth(const seastar::http::request& req) const;

    // Auth helper with detailed info - returns pair<authorized, error_or_key_name>
    std::pair<bool, std::string> check_admin_auth_with_info(const seastar::http::request& req) const;

    // Get client IP from request (checks X-Forwarded-For header)
    static std::string get_client_ip(const seastar::http::request& req);

    // Try to get an alternative backend when primary fails (fallback routing)
    std::optional<BackendId> get_fallback_backend(BackendId failed_id);

    // Proxy request helper methods - break down handle_proxy into manageable pieces
    // These are called from within the streaming lambda to handle different phases

    // Extract priority level from request headers, user-agent, and cost estimation.
    // Pure computation — no I/O, no futures.
    // Cascade: X-Ranvier-Priority header → User-Agent match → cost-based default.
    PriorityLevel extract_priority(const seastar::http::request& req,
                                   double estimated_cost_units,
                                   std::string_view body_view) const;

    // Estimate request cost from body content and max_tokens fields.
    // Pure computation — no I/O, no futures, no JSON ownership retained.
    struct CostEstimate {
        uint64_t input_tokens = 0;
        uint64_t output_tokens = 0;
        double cost_units = 0.0;
    };
    CostEstimate estimate_request_cost(size_t content_chars, uint64_t max_tokens_from_request) const;

    // Establish connection to backend with retry and fallback logic
    // Returns connected bundle or sets ctx->connection_failed on failure
    // Rule #21: pointers are values — safe across coroutine suspend points
    seastar::future<ConnectionBundle> establish_backend_connection(ProxyContext* ctx);

    // Send HTTP request to backend
    // Returns true on success, false on failure (sets appropriate ctx flags)
    seastar::future<bool> send_backend_request(ProxyContext* ctx, ConnectionBundle* bundle);

    // Stream response from backend to client
    // Handles the read loop, parsing, route learning, and client writes
    seastar::future<> stream_backend_response(
        ProxyContext* ctx,
        ConnectionBundle* bundle,
        seastar::output_stream<char>* client_out);

    // Write error message to client, handling disconnected clients gracefully
    seastar::future<> write_client_error(
        seastar::output_stream<char>* client_out,
        std::string error_msg);

    // Record final metrics and clean up after proxy request completes
    void record_proxy_completion_metrics(
        const ProxyContext& ctx,
        const std::chrono::steady_clock::time_point& backend_end);
};

} // namespace ranvier
