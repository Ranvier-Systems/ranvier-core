#pragma once

#include "async_persistence.hpp"
#include "circuit_breaker.hpp"
#include "config.hpp"
#include "connection_pool.hpp"
#include "cross_shard_request.hpp"
#include "metrics_service.hpp"
#include "rate_limiter.hpp"
#include "router_service.hpp"
#include "shard_load_balancer.hpp"
#include "shard_load_metrics.hpp"
#include "tokenizer_service.hpp"

#include <atomic>
#include <functional>
#include <limits>

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
};

// Shard load balancing settings
struct LoadBalancingSettings {
    bool enabled = true;                              // Enable cross-shard load balancing
    double min_load_difference = 0.2;                 // Min load difference (ratio) to trigger dispatch
    uint64_t local_processing_threshold = 10;         // Process locally if active < this threshold
    uint64_t snapshot_refresh_interval_us = 1000;     // Snapshot cache refresh interval (microseconds)
};

// Forward declaration for connection bundle
struct ConnectionBundle;

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
};

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
    int32_t max_token_id = 200000;              // Maximum valid token ID for validation (security)
    RoutingConfig::RoutingMode routing_mode = RoutingConfig::RoutingMode::PREFIX;  // Routing mode
    uint32_t block_alignment = 16;              // vLLM PagedAttention block size for route alignment
    bool enable_prefix_boundary = true;         // Enable automatic prefix boundary detection
    size_t min_prefix_boundary_tokens = 4;      // Minimum system message tokens for prefix boundary
    bool accept_client_prefix_boundary = false; // Accept client-provided prefix_token_count
    bool enable_multi_depth_routing = false;    // Enable multi-depth route storage (Option C)

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
          _request_semaphore(effective_semaphore_limit(config.backpressure.max_concurrent_requests)) {
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
    void set_persistence(AsyncPersistenceManager* manager) { _persistence = manager; }

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
    ConfigReloadCallback _config_reload_callback;  // Callback for config hot-reload

    // Shard load balancing (P2C algorithm)
    seastar::sharded<ShardLoadBalancer>* _load_balancer;  // Cross-shard load balancer
    ShardLoadBalancerConfig _lb_config;  // Local copy of load balancer config

    // Graceful shutdown state
    std::atomic<bool> _draining{false};  // Set to true to reject new requests
    seastar::gate _request_gate;         // Tracks in-flight requests

    // Backpressure: semaphore for concurrency limiting
    // Uses try_get_units() for immediate rejection (no queueing)
    seastar::semaphore _request_semaphore;

    // Backpressure metrics
    std::atomic<uint64_t> _requests_rejected_concurrency{0};   // Rejected due to concurrency limit
    std::atomic<uint64_t> _requests_rejected_persistence{0};   // Rejected due to persistence backpressure

    // Shard load balancing metrics
    std::atomic<uint64_t> _requests_local_dispatch{0};         // Requests processed locally
    std::atomic<uint64_t> _requests_cross_shard_dispatch{0};   // Requests dispatched cross-shard

    // Check if persistence queue is under backpressure
    bool is_persistence_backpressured() const;

    // Select target shard for request processing using P2C algorithm
    // Returns local shard if load balancing disabled or not beneficial
    uint32_t select_target_shard();

    // Helper handlers
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_proxy(
        std::unique_ptr<seastar::http::request> req,
        std::unique_ptr<seastar::http::reply> rep,
        std::string_view endpoint = "/v1/chat/completions");
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
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_drain_backend(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);

    // Health check handler (public, no auth required)
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_health(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);

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

    // Establish connection to backend with retry and fallback logic
    // Returns connected bundle or sets ctx.connection_failed on failure
    seastar::future<ConnectionBundle> establish_backend_connection(ProxyContext& ctx);

    // Send HTTP request to backend
    // Returns true on success, false on failure (sets appropriate ctx flags)
    seastar::future<bool> send_backend_request(ProxyContext& ctx, ConnectionBundle& bundle);

    // Stream response from backend to client
    // Handles the read loop, parsing, route learning, and client writes
    seastar::future<> stream_backend_response(
        ProxyContext& ctx,
        ConnectionBundle& bundle,
        seastar::output_stream<char>& client_out);

    // Write error message to client, handling disconnected clients gracefully
    seastar::future<> write_client_error(
        seastar::output_stream<char>& client_out,
        std::string_view error_msg);

    // Record final metrics and clean up after proxy request completes
    void record_proxy_completion_metrics(
        const ProxyContext& ctx,
        const std::chrono::steady_clock::time_point& backend_end);
};

} // namespace ranvier
