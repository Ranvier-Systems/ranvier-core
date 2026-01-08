#pragma once

#include "async_persistence.hpp"
#include "circuit_breaker.hpp"
#include "config.hpp"
#include "connection_pool.hpp"
#include "metrics_service.hpp"
#include "rate_limiter.hpp"
#include "router_service.hpp"
#include "tokenizer_service.hpp"

#include <atomic>
#include <functional>
#include <limits>

#include <seastar/core/gate.hh>
#include <seastar/core/semaphore.hh>
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
    std::chrono::seconds drain_timeout{30};     // Max time to wait for in-flight requests during shutdown
    bool enable_token_forwarding = false;       // Forward pre-computed token IDs to backends (vLLM prompt_token_ids)
    bool accept_client_tokens = false;          // Accept pre-tokenized prompt_token_ids from clients for routing
    int32_t max_token_id = 100000;              // Maximum valid token ID for validation (security)
    RoutingConfig::RoutingMode routing_mode = RoutingConfig::RoutingMode::PREFIX;  // Routing mode

    // Helper methods for routing mode checks
    bool is_prefix_mode() const { return routing_mode == RoutingConfig::RoutingMode::PREFIX; }
    bool is_radix_mode() const { return routing_mode == RoutingConfig::RoutingMode::RADIX; }
    bool is_round_robin_mode() const { return routing_mode == RoutingConfig::RoutingMode::ROUND_ROBIN; }
    bool should_learn_routes() const { return routing_mode != RoutingConfig::RoutingMode::ROUND_ROBIN; }
};

class HttpController {
public:
    HttpController(TokenizerService& t, RouterService& r, HttpControllerConfig config = {})
        : _tokenizer(t), _router(r), _pool(config.pool), _config(config),
          _rate_limiter(config.rate_limit),
          _circuit_breaker(CircuitBreaker::Config{
              config.circuit_breaker.failure_threshold,
              config.circuit_breaker.success_threshold,
              config.circuit_breaker.recovery_timeout,
              config.circuit_breaker.enabled
          }),
          _persistence(nullptr),
          // Backpressure: 0 means unlimited (use max size_t as semaphore limit)
          _request_semaphore(effective_semaphore_limit(config.backpressure.max_concurrent_requests)) {}

private:
    // Helper to compute effective semaphore limit (0 = unlimited)
    static size_t effective_semaphore_limit(size_t configured_limit) {
        return configured_limit > 0 ? configured_limit : std::numeric_limits<size_t>::max();
    }

public:

    // Set optional async persistence manager (call before serving requests)
    // Uses fire-and-forget queueing to avoid blocking the reactor
    void set_persistence(AsyncPersistenceManager* manager) { _persistence = manager; }

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

private:
    TokenizerService& _tokenizer;
    RouterService& _router;
    ConnectionPool _pool;
    HttpControllerConfig _config;
    RateLimiter _rate_limiter;
    CircuitBreaker _circuit_breaker;
    AsyncPersistenceManager* _persistence;  // Async persistence (fire-and-forget, non-blocking)
    ConfigReloadCallback _config_reload_callback;  // Callback for config hot-reload

    // Graceful shutdown state
    std::atomic<bool> _draining{false};  // Set to true to reject new requests
    seastar::gate _request_gate;         // Tracks in-flight requests

    // Backpressure: semaphore for concurrency limiting
    // Uses try_get_units() for immediate rejection (no queueing)
    seastar::semaphore _request_semaphore;

    // Backpressure metrics
    std::atomic<uint64_t> _requests_rejected_concurrency{0};   // Rejected due to concurrency limit
    std::atomic<uint64_t> _requests_rejected_persistence{0};   // Rejected due to persistence backpressure

    // Check if persistence queue is under backpressure
    bool is_persistence_backpressured() const;

    // Helper handlers
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_proxy(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_broadcast_route(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_broadcast_backend(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);

    // Admin handlers
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_delete_backend(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_delete_routes(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_clear_all(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_keys_reload(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);

    // Auth helper - returns true if authorized, false otherwise
    bool check_admin_auth(const seastar::http::request& req) const;

    // Auth helper with detailed info - returns pair<authorized, error_or_key_name>
    std::pair<bool, std::string> check_admin_auth_with_info(const seastar::http::request& req) const;

    // Get client IP from request (checks X-Forwarded-For header)
    static std::string get_client_ip(const seastar::http::request& req);

    // Try to get an alternative backend when primary fails (fallback routing)
    std::optional<BackendId> get_fallback_backend(BackendId failed_id);
};

} // namespace ranvier
