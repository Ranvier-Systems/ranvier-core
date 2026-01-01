#include "http_controller.hpp"
#include "logging.hpp"
#include "request_rewriter.hpp"

#include "stream_parser.hpp"

#include <algorithm>
#include <system_error>

#include <seastar/core/coroutine.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/http/handlers.hh>
#include <seastar/http/function_handlers.hh>
#include <seastar/net/api.hh>

using namespace seastar;

namespace ranvier {

// Helper to check if an exception is a connection error (broken pipe or connection reset)
// These errors occur when the backend closes the connection unexpectedly
enum class ConnectionErrorType {
    NONE,
    BROKEN_PIPE,      // EPIPE - write to closed socket
    CONNECTION_RESET  // ECONNRESET - connection reset by peer
};

inline ConnectionErrorType classify_connection_error(std::exception_ptr ep) {
    if (!ep) return ConnectionErrorType::NONE;

    try {
        std::rethrow_exception(ep);
    } catch (const std::system_error& e) {
        // Check for EPIPE (broken pipe) - occurs when writing to a closed socket
        if (e.code() == std::errc::broken_pipe ||
            e.code().value() == EPIPE) {
            return ConnectionErrorType::BROKEN_PIPE;
        }
        // Check for ECONNRESET (connection reset by peer)
        if (e.code() == std::errc::connection_reset ||
            e.code().value() == ECONNRESET) {
            return ConnectionErrorType::CONNECTION_RESET;
        }
    } catch (...) {
        // Not a system_error, not a connection error we handle specially
    }
    return ConnectionErrorType::NONE;
}

inline const char* connection_error_to_string(ConnectionErrorType type) {
    switch (type) {
        case ConnectionErrorType::BROKEN_PIPE: return "broken pipe (EPIPE)";
        case ConnectionErrorType::CONNECTION_RESET: return "connection reset (ECONNRESET)";
        default: return "unknown";
    }
}

// RAII guard for active request counter
// Ensures decrement happens even if an exception is thrown during request setup
class ActiveRequestGuard {
    MetricsService& _metrics;
    bool _released = false;
public:
    explicit ActiveRequestGuard(MetricsService& metrics) : _metrics(metrics) {
        _metrics.increment_active_requests();
    }
    ~ActiveRequestGuard() {
        if (!_released) {
            _metrics.decrement_active_requests();
        }
    }
    // Non-copyable, non-movable (prevent accidental double-decrement)
    ActiveRequestGuard(const ActiveRequestGuard&) = delete;
    ActiveRequestGuard& operator=(const ActiveRequestGuard&) = delete;
    ActiveRequestGuard(ActiveRequestGuard&&) = delete;
    ActiveRequestGuard& operator=(ActiveRequestGuard&&) = delete;
    // Release ownership - caller takes responsibility for decrementing
    void release() { _released = true; }
};

// Helper: explicit seastar::httpd:: (Server) types
template <typename Func>
struct async_handler : public seastar::httpd::handler_base {
    Func _func;
    async_handler(Func&& f) : _func(std::move(f)) {}
    future<std::unique_ptr<seastar::httpd::reply>> handle(const sstring& path, std::unique_ptr<seastar::httpd::request> req, std::unique_ptr<seastar::httpd::reply> rep) override {
        return _func(std::move(req), std::move(rep));
    }
};

// Check admin authentication - returns true if authorized
bool HttpController::check_admin_auth(const seastar::http::request& req) const {
    // If no API key configured, allow all requests (auth disabled)
    if (_config.admin_api_key.empty()) {
        return true;
    }

    // Check Authorization header: "Bearer <token>"
    auto auth_it = req._headers.find("Authorization");
    if (auth_it == req._headers.end()) {
        return false;
    }

    const auto& auth_header = auth_it->second;
    const std::string bearer_prefix = "Bearer ";
    if (auth_header.size() <= bearer_prefix.size() ||
        auth_header.substr(0, bearer_prefix.size()) != bearer_prefix) {
        return false;
    }

    std::string token = auth_header.substr(bearer_prefix.size());
    return token == _config.admin_api_key;
}

// Get client IP from request, checking X-Forwarded-For for proxied requests
std::string HttpController::get_client_ip(const seastar::http::request& req) {
    // Check X-Forwarded-For header first (for proxied requests)
    auto xff_it = req._headers.find("X-Forwarded-For");
    if (xff_it != req._headers.end() && !xff_it->second.empty()) {
        // X-Forwarded-For can contain multiple IPs; take the first (original client)
        auto& xff = xff_it->second;
        auto comma_pos = xff.find(',');
        if (comma_pos != std::string::npos) {
            return std::string(xff.substr(0, comma_pos));
        }
        return std::string(xff);
    }

    // Fall back to direct client address
    // Note: In Seastar, we'd need the connection info which isn't directly available here
    // For now, return a default that will work for non-proxied setups
    return "direct";
}

// Check rate limit for a request
bool HttpController::check_rate_limit(const seastar::http::request& req) {
    std::string client_ip = get_client_ip(req);
    return _rate_limiter.allow(client_ip);
}

// Get a fallback backend when primary fails
std::optional<BackendId> HttpController::get_fallback_backend(BackendId failed_id) {
    if (!_config.circuit_breaker.fallback_enabled) {
        return std::nullopt;
    }

    // Try to get a different backend
    auto all_backends = _router.get_all_backend_ids();
    for (auto id : all_backends) {
        if (id != failed_id && _circuit_breaker.allow_request(id)) {
            return id;
        }
    }
    return std::nullopt;
}

// Helper to create an auth-protected admin handler
template <typename AuthCheck, typename Func>
auto make_admin_handler(AuthCheck&& auth_check, Func&& handler) {
    return new async_handler([
        auth_check = std::forward<AuthCheck>(auth_check),
        handler = std::forward<Func>(handler)
    ](auto req, auto rep) mutable -> future<std::unique_ptr<seastar::httpd::reply>> {

        // Execute the captured private check
        if (!auth_check(*req)) {
            rep->set_status(seastar::http::reply::status_type::unauthorized);
            rep->add_header("WWW-Authenticate", "Bearer");
            rep->write_body("json", "{\"error\": \"Unauthorized - valid API key required\"}");
            return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
        }
        return handler(std::move(req), std::move(rep));
    });
}

// Helper to create a rate-limited handler
template <typename RateLimitCheck, typename Func>
auto make_rate_limited_handler(RateLimitCheck&& rate_limit_check, Func&& handler) {
    return new async_handler([
        rate_limit_check = std::forward<RateLimitCheck>(rate_limit_check),
        handler = std::forward<Func>(handler)
    ](auto req, auto rep) mutable
        -> future<std::unique_ptr<seastar::httpd::reply>> {
        if (!rate_limit_check(*req)) {
            metrics().record_rate_limited();
            rep->set_status(seastar::http::reply::status_type::service_unavailable);
            rep->add_header("Retry-After", "1");
            rep->write_body("json", "{\"error\": \"Rate limit exceeded - try again later\"}");
            return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
        }
        return handler(std::move(req), std::move(rep));
    });
}

void HttpController::register_routes(seastar::httpd::routes& r) {
    using namespace seastar::httpd;

    // Define the check once as a local lambda to keep the calls clean.
    auto rate_limit_check = [this](const auto& req) { return this->check_rate_limit(req); };

    // 1. DATA PLANE (rate limited)
    r.add(operation_type::POST, url("/v1/chat/completions"), make_rate_limited_handler(rate_limit_check, [this](auto req, auto rep) {
        return this->handle_proxy(std::move(req), std::move(rep));
    }));

    // Define the check once as a local lambda to keep the calls clean.
    auto auth_check = [this](const auto& req) { return this->check_admin_auth(req); };

    // 2. CONTROL PLANE - Create/Update (auth protected)
    r.add(operation_type::POST, url("/admin/routes"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_broadcast_route(std::move(req), std::move(rep));
    }));

    r.add(operation_type::POST, url("/admin/backends"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_broadcast_backend(std::move(req), std::move(rep));
    }));

    // 3. CONTROL PLANE - Delete (auth protected)
    r.add(operation_type::DELETE, url("/admin/backends"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_delete_backend(std::move(req), std::move(rep));
    }));

    r.add(operation_type::DELETE, url("/admin/routes"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_delete_routes(std::move(req), std::move(rep));
    }));

    r.add(operation_type::POST, url("/admin/clear"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_clear_all(std::move(req), std::move(rep));
    }));
}

// ---------------------------------------------------------
// PROXY HANDLER
// ---------------------------------------------------------
future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_proxy(std::unique_ptr<seastar::httpd::request> req, std::unique_ptr<seastar::httpd::reply> rep) {
    // Extract or generate request ID for distributed tracing
    std::string request_id = extract_request_id(req->_headers);
    if (request_id.empty()) {
        request_id = generate_request_id();
    }

    // Check if we're draining - reject new requests with 503
    if (_draining.load(std::memory_order_relaxed)) {
        log_proxy.info("[{}] Request rejected - server is draining", request_id);
        rep->set_status(seastar::http::reply::status_type::service_unavailable);
        rep->add_header("X-Request-ID", request_id);
        rep->add_header("Retry-After", "5");
        rep->write_body("json", "{\"error\": \"Server is shutting down\"}");
        co_return std::move(rep);
    }

    // Enter the request gate to track this in-flight request
    seastar::gate::holder gate_holder;
    try {
        gate_holder = _request_gate.hold();
    } catch (const seastar::gate_closed_exception&) {
        // Gate already closed during shutdown
        log_proxy.info("[{}] Request rejected - gate closed", request_id);
        rep->set_status(seastar::http::reply::status_type::service_unavailable);
        rep->add_header("X-Request-ID", request_id);
        rep->add_header("Retry-After", "5");
        rep->write_body("json", "{\"error\": \"Server is shutting down\"}");
        co_return std::move(rep);
    }

    // Track request start time for latency metrics (ingress timestamp)
    auto request_start = std::chrono::steady_clock::now();
    metrics().record_request();

    // RAII guard ensures active request counter is decremented even if exception thrown
    // Guard is released before entering the lambda, which takes over responsibility
    ActiveRequestGuard active_request_guard(metrics());

    std::string body = req->content;
    std::string client_ip = get_client_ip(*req);

    log_proxy.info("[{}] Request received from {} ({} bytes)",
                   request_id, client_ip, body.size());

    // 1. Validation
    if (!_tokenizer.is_loaded()) {
        log_proxy.warn("[{}] Tokenizer not loaded", request_id);
        metrics().record_failure();
        // active_request_guard destructor will decrement counter
        rep->add_header("X-Request-ID", request_id);
        rep->write_body("json", "{\"error\": \"Tokenizer not loaded\"}");
        co_return std::move(rep);
    }

    // Timing: capture routing decision start
    auto routing_start = std::chrono::steady_clock::now();

    // Get tokens for routing - either from client or by tokenizing locally
    std::vector<int32_t> tokens;
    bool used_client_tokens = false;

    // First, check if client provided pre-tokenized prompt_token_ids
    if (_config.accept_client_tokens) {
        auto token_result = RequestRewriter::extract_prompt_token_ids(body, _config.max_token_id);
        if (token_result.found) {
            if (token_result.valid) {
                tokens = std::move(token_result.tokens);
                used_client_tokens = true;
                log_proxy.debug("[{}] Using {} client-provided token IDs for routing",
                               request_id, tokens.size());
            } else {
                // Client provided invalid tokens - reject the request
                log_proxy.warn("[{}] Invalid client tokens: {}", request_id, token_result.error);
                metrics().record_failure();
                // active_request_guard destructor will decrement counter
                rep->add_header("X-Request-ID", request_id);
                rep->set_status(seastar::http::reply::status_type::bad_request);
                rep->write_body("json", "{\"error\": \"Invalid prompt_token_ids: " + token_result.error + "\"}");
                co_return std::move(rep);
            }
        }
    }

    // If no client tokens, tokenize locally
    if (!used_client_tokens) {
        // Extract text from request body for tokenization (prompt or messages content)
        // This ensures we tokenize exactly what we use for routing, not metadata
        auto extracted_text = RequestRewriter::extract_text(body);
        if (extracted_text.has_value()) {
            tokens = _tokenizer.encode(extracted_text.value());
        } else {
            // Fallback: tokenize the entire body (legacy behavior)
            tokens = _tokenizer.encode(body);
        }
    }

    // Rewrite request body with token IDs if enabled and we tokenized locally
    // Skip rewriting if client already provided prompt_token_ids (it's already in the body)
    std::string forwarded_body = body;
    if (_config.enable_token_forwarding && !tokens.empty() && !used_client_tokens) {
        auto rewrite_result = RequestRewriter::rewrite(body, tokens);
        if (rewrite_result.success) {
            forwarded_body = std::move(rewrite_result.body);
            log_proxy.debug("[{}] Request rewritten with {} token IDs ({} -> {} bytes)",
                           request_id, tokens.size(), body.size(), forwarded_body.size());
        } else {
            log_proxy.debug("[{}] Request rewrite skipped: {}",
                           request_id, rewrite_result.error);
        }
    }

    BackendId target_id;

    if (_config.is_prefix_mode()) {
        // PREFIX mode: consistent hashing on prefix tokens for KV cache reuse
        auto affinity_backend = _router.get_backend_for_prefix(tokens, request_id);
        if (affinity_backend.has_value()) {
            target_id = affinity_backend.value();
            log_proxy.debug("[{}] Prefix affinity -> backend {}", request_id, target_id);
        } else {
            log_proxy.warn("[{}] No backends registered", request_id);
            metrics().record_failure();
            rep->add_header("X-Request-ID", request_id);
            rep->write_body("json", "{\"error\": \"No backends registered!\"}");
            co_return std::move(rep);
        }
    } else if (_config.is_radix_mode()) {
        // RADIX mode: radix tree lookup with random fallback (adaptive learning)
        auto lookup_result = _router.lookup(tokens, request_id);
        if (lookup_result.has_value()) {
            target_id = lookup_result.value();
            log_proxy.debug("[{}] Radix tree hit -> backend {}", request_id, target_id);
        } else {
            auto random_id = _router.get_random_backend();
            if (!random_id.has_value()) {
                log_proxy.warn("[{}] No backends registered", request_id);
                metrics().record_failure();
                rep->add_header("X-Request-ID", request_id);
                rep->write_body("json", "{\"error\": \"No backends registered!\"}");
                co_return std::move(rep);
            }
            target_id = random_id.value();
            log_proxy.debug("[{}] Radix tree miss, random -> backend {}", request_id, target_id);
        }
    } else {
        // ROUND_ROBIN mode: always use random/weighted backend selection
        auto random_id = _router.get_random_backend();
        if (!random_id.has_value()) {
            log_proxy.warn("[{}] No backends registered", request_id);
            metrics().record_failure();
            rep->add_header("X-Request-ID", request_id);
            rep->write_body("json", "{\"error\": \"No backends registered!\"}");
            co_return std::move(rep);
        }
        target_id = random_id.value();
        log_proxy.debug("[{}] Round-robin -> backend {}", request_id, target_id);
    }

    // Timing: record routing decision latency (post-routing timestamp)
    auto routing_end = std::chrono::steady_clock::now();
    metrics().record_routing_latency(
        MetricsService::to_seconds(routing_end - routing_start));

    // Circuit breaker check - try fallback if circuit is open
    if (!_circuit_breaker.allow_request(target_id)) {
        log_proxy.info("[{}] Circuit open for backend {}, trying fallback", request_id, target_id);
        metrics().record_circuit_open();
        auto fallback = get_fallback_backend(target_id);
        if (fallback.has_value()) {
            target_id = fallback.value();
            metrics().record_fallback();
            log_proxy.info("[{}] Using fallback backend {}", request_id, target_id);
        } else {
            log_proxy.warn("[{}] All backends unavailable (circuit breaker open)", request_id);
            metrics().record_failure();
            // active_request_guard destructor will decrement counter
            rep->add_header("X-Request-ID", request_id);
            rep->write_body("json", "{\"error\": \"All backends unavailable (circuit breaker open)\"}");
            co_return std::move(rep);
        }
    }

    auto target_addr_opt = _router.get_backend_address(target_id);
    if (!target_addr_opt.has_value()) {
        log_proxy.warn("[{}] Backend {} IP not found", request_id, target_id);
        metrics().record_failure();
        // active_request_guard destructor will decrement counter
        rep->add_header("X-Request-ID", request_id);
        rep->write_body("json", "{\"error\": \"Backend IP not found\"}");
        co_return std::move(rep);
    }
    socket_address target_addr = target_addr_opt.value();
    log_proxy.info("[{}] Routing to backend {} at {}", request_id, target_id, target_addr);

    // 2. Setup Streaming with Timeout, Retry, and Circuit Breaker
    // Capture config for the lambda
    auto connect_timeout = _config.connect_timeout;
    auto request_timeout = _config.request_timeout;
    auto retry_config = _config.retry;
    auto fallback_enabled = _config.circuit_breaker.fallback_enabled;

    // Add X-Request-ID and X-Backend-ID to response headers before streaming
    rep->add_header("X-Request-ID", request_id);
    rep->add_header("X-Backend-ID", std::to_string(target_id));

    // Release the guard - lambda takes over responsibility for decrementing counter
    active_request_guard.release();

    rep->write_body("text/event-stream", [this, target_addr, forwarded_body, tokens, target_id, connect_timeout, request_timeout, retry_config, fallback_enabled, request_start, request_id](output_stream<char> client_out) -> future<> {

        // Calculate request deadline
        auto request_deadline = lowres_clock::now() + request_timeout;

        // Track backend operation timing
        auto connect_start = std::chrono::steady_clock::now();
        bool response_latency_recorded = false;

        ConnectionBundle bundle;
        bool timed_out = false;
        bool connection_failed = false;
        bool client_disconnected = false;  // Track if client disconnected mid-stream
        bool stream_closed = false;        // Track if client_out was already closed
        BackendId current_backend = target_id;
        socket_address current_addr = target_addr;

        // 1. GET CONNECTION FROM POOL (with connect timeout, retry, and fallback)
        uint32_t retry_attempt = 0;
        auto current_backoff = retry_config.initial_backoff;
        uint32_t fallback_attempts = 0;
        const uint32_t max_fallback_attempts = 3;

        while (retry_attempt <= retry_config.max_retries) {
            // Check if we've exceeded overall request deadline
            if (lowres_clock::now() >= request_deadline) {
                log_proxy.warn("[{}] Request deadline exceeded during connection retry", request_id);
                connection_failed = true;
                _circuit_breaker.record_failure(current_backend);
                break;
            }

            auto connect_deadline = lowres_clock::now() + connect_timeout;
            auto conn_future = with_timeout(connect_deadline, _pool.get(current_addr, request_id));

            bool this_attempt_failed = false;
            bundle = co_await std::move(conn_future).handle_exception([&](auto ep) {
                this_attempt_failed = true;
                return ConnectionBundle{}; // Return empty bundle
            });

            if (!this_attempt_failed) {
                // Connection successful - record connect latency
                auto connect_end = std::chrono::steady_clock::now();
                metrics().record_connect_latency(
                    MetricsService::to_seconds(connect_end - connect_start));
                connection_failed = false;
                log_proxy.info("[{}] Connection established to backend {} at {}", request_id, current_backend, current_addr);
                break;
            }

            // Record failure for circuit breaker
            _circuit_breaker.record_failure(current_backend);

            // Try fallback to a different backend before retrying
            if (fallback_enabled && fallback_attempts < max_fallback_attempts) {
                auto fallback = get_fallback_backend(current_backend);
                if (fallback.has_value()) {
                    auto fallback_addr = _router.get_backend_address(fallback.value());
                    if (fallback_addr.has_value()) {
                        log_proxy.info("[{}] Falling back from backend {} to {}", request_id, current_backend, fallback.value());
                        current_backend = fallback.value();
                        current_addr = fallback_addr.value();
                        fallback_attempts++;
                        // Don't increment retry_attempt for fallback - this is a different backend
                        continue;
                    }
                }
            }

            // No fallback available - check if we should retry same backend
            if (retry_attempt < retry_config.max_retries) {
                log_proxy.warn("[{}] Connection attempt {} failed, retrying in {}ms",
                    request_id, retry_attempt + 1, current_backoff.count());

                // Wait with exponential backoff before retry
                co_await seastar::sleep(current_backoff);

                // Increase backoff for next attempt (with cap)
                auto next_backoff = std::chrono::milliseconds(
                    static_cast<int64_t>(current_backoff.count() * retry_config.backoff_multiplier));
                current_backoff = std::min(next_backoff, retry_config.max_backoff);
            } else {
                log_proxy.warn("[{}] Connection failed after {} retries and {} fallbacks",
                    request_id, retry_config.max_retries + 1, fallback_attempts);
                connection_failed = true;
            }

            retry_attempt++;
        }

        if (connection_failed) {
            log_proxy.error("[{}] Backend connection failed after retries", request_id);
            metrics().record_failure();
            metrics().decrement_active_requests();
            sstring error_msg = "data: {\"error\": \"Backend connection failed after retries\"}\n\n";
            try {
                co_await client_out.write(error_msg);
                co_await client_out.flush();
            } catch (...) {
                // Ignore write errors - client may have disconnected
            }
            co_await client_out.close();
            co_return;
        }

        // 2. SEND REQUEST (with timeout check)
        if (lowres_clock::now() >= request_deadline) {
            log_proxy.warn("[{}] Request timeout before sending", request_id);
            metrics().record_timeout();
            metrics().decrement_active_requests();
            bundle.is_valid = false;
            co_await bundle.close();
            sstring error_msg = "data: {\"error\": \"Request timed out\"}\n\n";
            try {
                co_await client_out.write(error_msg);
                co_await client_out.flush();
            } catch (...) {
                // Ignore write errors - client may have disconnected
            }
            co_await client_out.close();
            co_return;
        }

        // Build HTTP request with X-Request-ID header for backend tracing
        // Use forwarded_body which may include prompt_token_ids if token forwarding is enabled
        sstring http_req =
            "POST /v1/chat/completions HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + to_sstring(forwarded_body.size()) + "\r\n"
            "X-Request-ID: " + request_id + "\r\n"
            "Connection: keep-alive\r\n\r\n" +
            forwarded_body;

        log_proxy.info("[{}] Sending request to backend ({} bytes)", request_id, forwarded_body.size());

        // Send request with broken pipe/connection reset handling
        bool write_failed = false;
        std::exception_ptr rethrow_exception;
        try {
            co_await bundle.out.write(http_req);
            co_await bundle.out.flush();
        } catch (...) {
            auto err_type = classify_connection_error(std::current_exception());
            if (err_type != ConnectionErrorType::NONE) {
                log_proxy.warn("[{}] Backend write failed: {} - closing connection",
                               request_id, connection_error_to_string(err_type));
                write_failed = true;
                bundle.is_valid = false;
                _circuit_breaker.record_failure(current_backend);
                metrics().record_connection_error();
            } else {
                // Capture exception for re-throw after cleanup
                // (co_await not permitted in catch handlers)
                rethrow_exception = std::current_exception();
            }
        }

        // Handle non-connection errors: cleanup and rethrow outside catch block
        if (rethrow_exception) {
            metrics().record_failure();
            metrics().decrement_active_requests();
            co_await bundle.close();
            try { co_await client_out.close(); } catch (...) {}
            stream_closed = true;
            std::rethrow_exception(rethrow_exception);
        }

        if (write_failed) {
            // Clean up and send error to client
            metrics().record_failure();
            metrics().decrement_active_requests();
            co_await bundle.close();
            sstring error_msg = "data: {\"error\": \"Backend connection lost during request\"}\n\n";
            try {
                co_await client_out.write(error_msg);
                co_await client_out.flush();
            } catch (...) {
                // Ignore write errors - client may have disconnected
            }
            co_await client_out.close();
            co_return;
        }

        // 3. The Read Loop (with timeout and connection error handling)
        StreamParser parser;
        bool connection_error = false;
        size_t chunks_received = 0;
        while (true) {
            // Check request timeout before each read
            if (lowres_clock::now() >= request_deadline) {
                log_proxy.warn("[{}] Request timeout after {}s", request_id, request_timeout.count());
                timed_out = true;
                bundle.is_valid = false;
                break;
            }

            // Read with per-chunk timeout (use remaining time, capped at 30s per read)
            auto remaining = std::chrono::duration_cast<std::chrono::seconds>(request_deadline - lowres_clock::now());
            auto read_timeout = std::min(remaining, std::chrono::seconds(30));
            auto read_deadline = lowres_clock::now() + read_timeout;

            bool read_failed = false;
            temporary_buffer<char> chunk;
            try {
                auto read_future = with_timeout(read_deadline, bundle.in.read());
                chunk = co_await std::move(read_future).handle_exception([&](auto ep) {
                    // Check for connection errors first
                    auto err_type = classify_connection_error(ep);
                    if (err_type != ConnectionErrorType::NONE) {
                        log_proxy.warn("[{}] Backend read failed: {}",
                                       request_id, connection_error_to_string(err_type));
                        connection_error = true;
                    } else {
                        log_proxy.warn("[{}] Read timeout waiting for backend response", request_id);
                    }
                    read_failed = true;
                    return temporary_buffer<char>{}; // Return empty buffer
                });
            } catch (...) {
                // Catch any exceptions that weren't handled by handle_exception
                auto err_type = classify_connection_error(std::current_exception());
                if (err_type != ConnectionErrorType::NONE) {
                    log_proxy.warn("[{}] Backend read failed: {}",
                                   request_id, connection_error_to_string(err_type));
                    connection_error = true;
                    read_failed = true;
                    bundle.is_valid = false;
                } else {
                    // Capture exception for re-throw after cleanup
                    // (co_await not permitted in catch handlers)
                    rethrow_exception = std::current_exception();
                }
            }

            // Handle non-connection errors: cleanup and rethrow outside catch block
            if (rethrow_exception) {
                metrics().record_failure();
                metrics().decrement_active_requests();
                bundle.is_valid = false;
                co_await bundle.close();
                try { co_await client_out.close(); } catch (...) {}
                stream_closed = true;
                std::rethrow_exception(rethrow_exception);
            }

            if (read_failed) {
                if (connection_error) {
                    // bundle.is_valid already set to false in catch block above
                    _circuit_breaker.record_failure(current_backend);
                } else {
                    timed_out = true;
                    bundle.is_valid = false;
                }
                break;
            }

            // EOF logic
            if (chunk.empty()) {
                log_proxy.info("[{}] Backend response complete (EOF after {} chunks)", request_id, chunks_received);
                bundle.is_valid = false;
                break;
            }
            chunks_received++;
            log_proxy.debug("[{}] Received chunk #{} ({} bytes)", request_id, chunks_received, chunk.size());

            auto res = parser.push(std::move(chunk));

            // Snooping Logic - record success and learn route
            if (res.header_snoop_success) {
                // Backend responded successfully - record for circuit breaker
                _circuit_breaker.record_success(current_backend);

                // Record response latency (time to first byte)
                if (!response_latency_recorded) {
                    auto response_time = std::chrono::steady_clock::now();
                    auto first_byte_latency = MetricsService::to_seconds(response_time - connect_start);
                    // Record in legacy histogram
                    metrics().record_response_latency(first_byte_latency);
                    // Record in per-backend histogram for GPU model comparison
                    metrics().record_first_byte_latency_by_id(current_backend, first_byte_latency);
                    response_latency_recorded = true;
                    log_proxy.info("[{}] First byte received from backend {} (latency: {:.3f}s)",
                                    request_id, current_backend, first_byte_latency);
                }

                // Learn route in the ART for future prefix matching
                // Learn routes in PREFIX and RADIX modes; skip for ROUND_ROBIN mode
                // The ART insert is idempotent - existing routes just get their timestamp updated
                if (_config.should_learn_routes() && tokens.size() >= _config.min_token_length) {
                    // Route learning is best-effort; don't fail the request if it fails
                    (void)_router.learn_route_global(tokens, current_backend, request_id)
                        .handle_exception([request_id](auto) {
                            log_proxy.debug("[{}] Route learning failed (non-fatal)", request_id);
                        });

                    if (_persistence) {
                        try {
                            _persistence->save_route(tokens, current_backend);
                        } catch (...) {
                            // Best-effort persistence, ignore failures
                            log_proxy.debug("[{}] Route persistence failed (non-fatal)", request_id);
                        }
                    }
                }
            }

            // Write to client with broken pipe handling
            bool client_write_error = false;
            if (!res.data.empty()) {
                try {
                    co_await client_out.write(res.data);
                    co_await client_out.flush();
                } catch (...) {
                    auto err_type = classify_connection_error(std::current_exception());
                    if (err_type != ConnectionErrorType::NONE) {
                        log_proxy.warn("[{}] Client write failed: {} - client disconnected",
                                       request_id, connection_error_to_string(err_type));
                        // Client disconnected, clean up backend connection and exit
                        bundle.is_valid = false;
                        client_disconnected = true;
                        break;
                    }
                    // Capture exception for re-throw after cleanup
                    // (co_await not permitted in catch handlers)
                    rethrow_exception = std::current_exception();
                }

                // Handle non-connection errors: cleanup and rethrow outside catch block
                if (rethrow_exception) {
                    metrics().record_failure();
                    metrics().decrement_active_requests();
                    bundle.is_valid = false;
                    stream_closed = true;
                    co_await bundle.close();
                    try { co_await client_out.close(); } catch (...) {}
                    std::rethrow_exception(rethrow_exception);
                }
            }

            if (res.done) {
                log_proxy.debug("[{}] Stream complete", request_id);
                break;
            }
        }

        // Send error to client if needed (skip if client already disconnected)
        if (client_disconnected) {
            // Client disconnected mid-stream - don't count as success or failure
            // The request was partially served; this is a client-side abort
            log_proxy.info("[{}] Client disconnected mid-stream", request_id);
        } else if (timed_out) {
            // Record failure for circuit breaker on timeout
            log_proxy.warn("[{}] Request timed out", request_id);
            _circuit_breaker.record_failure(current_backend);
            metrics().record_timeout();
            sstring error_msg = "data: {\"error\": \"Request timed out\"}\n\n";
            try {
                co_await client_out.write(error_msg);
                co_await client_out.flush();
            } catch (...) {
                // Client may have disconnected, ignore write errors
                auto err_type = classify_connection_error(std::current_exception());
                if (err_type == ConnectionErrorType::NONE) {
                    log_proxy.debug("[{}] Failed to send timeout error to client", request_id);
                }
            }
        } else if (connection_error) {
            // Backend connection was lost unexpectedly
            log_proxy.warn("[{}] Backend connection error occurred", request_id);
            metrics().record_connection_error();
            sstring error_msg = "data: {\"error\": \"Backend connection lost\"}\n\n";
            try {
                co_await client_out.write(error_msg);
                co_await client_out.flush();
            } catch (...) {
                // Client may have disconnected, ignore write errors
                auto err_type = classify_connection_error(std::current_exception());
                if (err_type == ConnectionErrorType::NONE) {
                    log_proxy.debug("[{}] Failed to send error to client", request_id);
                }
            }
        } else if (!connection_failed) {
            // Request completed successfully
            log_proxy.info("[{}] Request completed successfully", request_id);
            metrics().record_success();
        }

        // Record backend total latency (from connection start to completion)
        if (!connection_failed && !connection_error) {
            auto backend_end = std::chrono::steady_clock::now();
            auto backend_latency = MetricsService::to_seconds(backend_end - connect_start);
            // Record in legacy histogram
            metrics().record_backend_total_latency(backend_latency);
            // Record in per-backend histogram for GPU model comparison (e.g., H100 vs A100)
            metrics().record_backend_latency_by_id(current_backend, backend_latency);
        }

        // 4. CLEANUP - Always ensure proper resource recovery
        // For failed connections, explicitly close the bundle instead of pooling
        if (connection_failed || connection_error || timed_out || !bundle.is_valid) {
            // Don't return broken connections to the pool
            bundle.is_valid = false;
            try {
                co_await bundle.close();
            } catch (...) {
                log_proxy.trace("[{}] Error closing backend connection", request_id);
            }
        } else {
            // Return healthy connection to pool
            try {
                _pool.put(std::move(bundle), request_id);
            } catch (...) {
                log_proxy.trace("[{}] Error returning connection to pool", request_id);
            }
        }

        // Close client output stream (skip if already closed in exception path)
        if (!stream_closed) {
            try {
                co_await client_out.close();
            } catch (...) {
                // Ignore errors closing client connection
                log_proxy.trace("[{}] Error closing client output stream", request_id);
            }
        }

        // Record total request duration (end-to-end from ingress to completion)
        auto request_end = std::chrono::steady_clock::now();
        auto total_latency = MetricsService::to_seconds(request_end - request_start);
        // Record in legacy histogram
        metrics().record_request_latency(total_latency);
        // Record in new advanced histogram with optimized buckets
        metrics().record_router_total_latency(total_latency);

        // Decrement active request counter
        metrics().decrement_active_requests();
    });

    co_return std::move(rep);
}

// ---------------------------------------------------------
// CONTROL PLANE HANDLERS
// ---------------------------------------------------------

future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_broadcast_route(std::unique_ptr<seastar::httpd::request> req, std::unique_ptr<seastar::httpd::reply> rep) {
    sstring id_str = req->get_query_param("backend_id");
    if (id_str.empty()) {
        log_control.warn("POST /admin/routes: missing backend_id parameter");
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Missing backend_id\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    int backend_id;
    try {
        backend_id = std::stoi(std::string(id_str));
    } catch (const std::exception& e) {
        log_control.warn("POST /admin/routes: invalid backend_id '{}': {}", id_str, e.what());
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Invalid backend_id: must be a valid integer\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    std::vector<int32_t> tokens;
    try {
        tokens = _tokenizer.encode(req->content);
    } catch (const std::exception& e) {
        log_control.warn("POST /admin/routes: failed to tokenize content for backend {}: {}", backend_id, e.what());
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Failed to tokenize request content\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    // Persist the route
    if (_persistence) {
        try {
            _persistence->save_route(tokens, backend_id);
        } catch (...) {
            log_control.warn("Failed to persist route for backend {}", backend_id);
        }
    }

    return _router.learn_route_global(tokens, backend_id).then([backend_id, rep = std::move(rep)]() mutable {
         rep->write_body("json", "{\"status\": \"ok\", \"route_added\": " + std::to_string(backend_id) + "}");
         return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    });
}

future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_broadcast_backend(std::unique_ptr<seastar::httpd::request> req, std::unique_ptr<seastar::httpd::reply> rep) {
    // Usage: POST /admin/backends?id=1&ip=192.168.4.51&port=11434&weight=100&priority=0
    sstring id_str = req->get_query_param("id");
    sstring ip_str = req->get_query_param("ip");
    sstring port_str = req->get_query_param("port");
    sstring weight_str = req->get_query_param("weight");
    sstring priority_str = req->get_query_param("priority");

    // Check for required parameters
    if (id_str.empty() || port_str.empty() || ip_str.empty()) {
        log_control.warn("POST /admin/backends: missing required parameter (id={}, ip={}, port={})",
            id_str.empty() ? "<missing>" : id_str,
            ip_str.empty() ? "<missing>" : ip_str,
            port_str.empty() ? "<missing>" : port_str);
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Missing id, ip, or port\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    int id, port;
    uint32_t weight = 100;
    uint32_t priority = 0;

    try {
        id = std::stoi(std::string(id_str));
        port = std::stoi(std::string(port_str));
        if (port < 1 || port > 65535) {
            throw std::out_of_range("port out of range");
        }
        if (!weight_str.empty()) {
            weight = static_cast<uint32_t>(std::stoi(std::string(weight_str)));
        }
        if (!priority_str.empty()) {
            priority = static_cast<uint32_t>(std::stoi(std::string(priority_str)));
        }
    } catch (const std::exception& e) {
        log_control.warn("POST /admin/backends: invalid parameter (id={}, port={}, weight={}, priority={}): {}",
            id_str, port_str, weight_str, priority_str, e.what());
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Invalid parameter: id, port, weight, and priority must be valid integers\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    // Parse and validate IP address
    socket_address addr;
    try {
        addr = socket_address(ipv4_addr(std::string(ip_str), port));
    } catch (const std::exception& e) {
        log_control.warn("POST /admin/backends: invalid IP address '{}': {}", ip_str, e.what());
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Invalid IP address format\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    // Persist the backend registration
    if (_persistence) {
        try {
            _persistence->save_backend(id, std::string(ip_str), static_cast<uint16_t>(port), weight, priority);
        } catch (...) {
            log_control.warn("Failed to persist backend {} registration", id);
        }
    }

    return _router.register_backend_global(id, addr, weight, priority).then(
        [id, ip_str, port, weight, priority, rep = std::move(rep)]() mutable {
        log_control.info("Registered Backend {} -> {}:{} (weight={}, priority={})",
            id, ip_str, port, weight, priority);
        rep->write_body("json", "{\"status\": \"ok\", \"weight\": " + std::to_string(weight) +
            ", \"priority\": " + std::to_string(priority) + "}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    });
}

// ---------------------------------------------------------
// ADMIN DELETE HANDLERS
// ---------------------------------------------------------

future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_delete_backend(std::unique_ptr<seastar::httpd::request> req, std::unique_ptr<seastar::httpd::reply> rep) {
    // Usage: DELETE /admin/backends?id=1
    sstring id_str = req->get_query_param("id");

    if (id_str.empty()) {
        log_control.warn("DELETE /admin/backends: missing id parameter");
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Missing id parameter\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    int id;
    try {
        id = std::stoi(std::string(id_str));
    } catch (const std::exception& e) {
        log_control.warn("DELETE /admin/backends: invalid id '{}': {}", id_str, e.what());
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Invalid id: must be a valid integer\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    // Remove from persistence first
    if (_persistence) {
        try {
            _persistence->remove_routes_for_backend(id);
            _persistence->remove_backend(id);
        } catch (...) {
            log_control.warn("Failed to remove backend {} from persistence", id);
        }
    }

    // Remove from in-memory state across all shards
    return _router.unregister_backend_global(id).then([id, rep = std::move(rep)]() mutable {
        log_control.info("Deleted Backend {} and its persisted routes", id);
        rep->write_body("json", "{\"status\": \"ok\", \"backend_deleted\": " + std::to_string(id) + "}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    });
}

future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_delete_routes(std::unique_ptr<seastar::httpd::request> req, std::unique_ptr<seastar::httpd::reply> rep) {
    // Usage: DELETE /admin/routes?backend_id=1
    sstring id_str = req->get_query_param("backend_id");

    if (id_str.empty()) {
        log_control.warn("DELETE /admin/routes: missing backend_id parameter");
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Missing backend_id parameter\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    int backend_id;
    try {
        backend_id = std::stoi(std::string(id_str));
    } catch (const std::exception& e) {
        log_control.warn("DELETE /admin/routes: invalid backend_id '{}': {}", id_str, e.what());
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Invalid backend_id: must be a valid integer\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    // Remove routes from persistence
    if (_persistence) {
        try {
            _persistence->remove_routes_for_backend(backend_id);
        } catch (...) {
            log_control.warn("Failed to remove routes for backend {} from persistence", backend_id);
        }
    }

    // Note: In-memory routes in the RadixTree are not removed immediately.
    // They will be gone after restart. For full removal, we'd need to
    // traverse the entire tree which is expensive.
    log_control.info("Deleted persisted routes for Backend {} (in-memory routes cleared on restart)", backend_id);
    rep->write_body("json", "{\"status\": \"ok\", \"routes_deleted_for_backend\": " + std::to_string(backend_id) + ", \"note\": \"In-memory routes will be cleared on restart\"}");
    return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
}

future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_clear_all(std::unique_ptr<seastar::httpd::request> req, std::unique_ptr<seastar::httpd::reply> rep) {
    // Usage: POST /admin/clear
    // WARNING: This is destructive!

    if (_persistence) {
        try {
            _persistence->clear_all();
        } catch (...) {
            log_control.error("Failed to clear persistence data");
            rep->set_status(seastar::http::reply::status_type::internal_server_error);
            rep->write_body("json", "{\"error\": \"Failed to clear persistence data\"}");
            return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
        }
    }

    log_control.warn("Cleared all persisted data (backends and routes). Restart required to clear in-memory state.");
    rep->write_body("json", "{\"status\": \"ok\", \"warning\": \"All persisted data cleared. Restart to clear in-memory state.\"}");
    return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
}

// ---------------------------------------------------------
// GRACEFUL SHUTDOWN
// ---------------------------------------------------------

void HttpController::start_draining() {
    _draining.store(true, std::memory_order_relaxed);
    log_proxy.info("Draining started - rejecting new requests");
}

future<> HttpController::wait_for_drain() {
    auto drain_timeout = _config.drain_timeout;
    log_proxy.info("Waiting for in-flight requests to complete (timeout: {}s)", drain_timeout.count());

    // Close the gate and wait for all holders to be released
    auto gate_close_future = _request_gate.close();

    // Race between gate closing and timeout
    auto deadline = lowres_clock::now() + drain_timeout;

    return with_timeout(deadline, std::move(gate_close_future))
        .then([] {
            log_proxy.info("All in-flight requests completed");
        })
        .handle_exception_type([](const seastar::timed_out_error&) {
            log_proxy.warn("Drain timeout reached - some requests may be interrupted");
        });
}

bool HttpController::is_draining() const {
    return _draining.load(std::memory_order_relaxed);
}

} // namespace ranvier
