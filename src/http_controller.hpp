#pragma once

#include "circuit_breaker.hpp"
#include "connection_pool.hpp"
#include "metrics_service.hpp"
#include "persistence.hpp"
#include "rate_limiter.hpp"
#include "router_service.hpp"
#include "tokenizer_service.hpp"

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

// HTTP controller configuration
struct HttpControllerConfig {
    ConnectionPoolConfig pool;
    size_t min_token_length = 4;  // Minimum tokens before caching a route
    std::chrono::seconds connect_timeout{5};    // Timeout for backend connection
    std::chrono::seconds request_timeout{300};  // Total timeout for request
    std::string admin_api_key = "";             // API key for admin endpoints (empty = no auth)
    RateLimiterConfig rate_limit;               // Rate limiting configuration
    RetrySettings retry;                        // Retry configuration
    CircuitBreakerSettings circuit_breaker;     // Circuit breaker configuration
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
          _persistence(nullptr) {}

    // Set optional persistence store (call before serving requests)
    void set_persistence(PersistenceStore* store) { _persistence = store; }

    // Register all endpoints
    void register_routes(seastar::httpd::routes& r);

    // Rate limit helper - public for handler wrapper access
    bool check_rate_limit(const seastar::http::request& req);

private:
    TokenizerService& _tokenizer;
    RouterService& _router;
    ConnectionPool _pool;
    HttpControllerConfig _config;
    RateLimiter _rate_limiter;
    CircuitBreaker _circuit_breaker;
    PersistenceStore* _persistence;

    // Helper handlers
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_proxy(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_broadcast_route(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_broadcast_backend(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);

    // Admin handlers
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_delete_backend(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_delete_routes(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);
    seastar::future<std::unique_ptr<seastar::http::reply>> handle_clear_all(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep);

    // Auth helper - returns true if authorized, false otherwise
    bool check_admin_auth(const seastar::http::request& req) const;

    // Get client IP from request (checks X-Forwarded-For header)
    static std::string get_client_ip(const seastar::http::request& req);

    // Try to get an alternative backend when primary fails (fallback routing)
    std::optional<BackendId> get_fallback_backend(BackendId failed_id);
};

} // namespace ranvier
