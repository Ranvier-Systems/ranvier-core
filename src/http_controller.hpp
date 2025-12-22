#pragma once

#include "connection_pool.hpp"
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

// HTTP controller configuration
struct HttpControllerConfig {
    ConnectionPoolConfig pool;
    size_t min_token_length = 4;  // Minimum tokens before caching a route
    std::chrono::seconds connect_timeout{5};    // Timeout for backend connection
    std::chrono::seconds request_timeout{300};  // Total timeout for request
    std::string admin_api_key = "";             // API key for admin endpoints (empty = no auth)
    RateLimiterConfig rate_limit;               // Rate limiting configuration
    RetrySettings retry;                        // Retry configuration
};

class HttpController {
public:
    HttpController(TokenizerService& t, RouterService& r, HttpControllerConfig config = {})
        : _tokenizer(t), _router(r), _pool(config.pool), _config(config),
          _rate_limiter(config.rate_limit), _persistence(nullptr) {}

    // Set optional persistence store (call before serving requests)
    void set_persistence(PersistenceStore* store) { _persistence = store; }

    // Register all endpoints
    void register_routes(seastar::httpd::routes& r);

private:
    TokenizerService& _tokenizer;
    RouterService& _router;
    ConnectionPool _pool;
    HttpControllerConfig _config;
    RateLimiter _rate_limiter;
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

    // Rate limit helper - returns true if allowed, false if rate limited
    bool check_rate_limit(const seastar::http::request& req);

    // Get client IP from request (checks X-Forwarded-For header)
    static std::string get_client_ip(const seastar::http::request& req);
};

} // namespace ranvier
