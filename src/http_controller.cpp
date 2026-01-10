#include "http_controller.hpp"
#include "logging.hpp"
#include "request_rewriter.hpp"
#include "shard_load_metrics.hpp"
#include "tracing_service.hpp"

#include "stream_parser.hpp"

#include <algorithm>
#include <sstream>
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

// Check admin authentication - returns pair<authorized, error_message>
// If authorized, error_message contains the key identifier for audit logging
// If not authorized, error_message contains the reason for failure
// Uses string_view internally for zero-copy header inspection
std::pair<bool, std::string> HttpController::check_admin_auth_with_info(const seastar::http::request& req) const {
    // If no API key configured, allow all requests (auth disabled)
    if (!_config.auth.is_enabled()) {
        return {true, ""};
    }

    // Check Authorization header: "Bearer <token>"
    // Access header directly without copying the entire map
    auto auth_it = req._headers.find("Authorization");
    if (auth_it == req._headers.end()) {
        return {false, "missing Authorization header"};
    }

    // Use string_view for zero-copy parsing of the Authorization header
    std::string_view auth_header(auth_it->second);
    constexpr std::string_view bearer_prefix = "Bearer ";

    if (auth_header.size() <= bearer_prefix.size() ||
        auth_header.substr(0, bearer_prefix.size()) != bearer_prefix) {
        return {false, "invalid Authorization format (expected 'Bearer <token>')"};
    }

    // Extract token as string_view - no allocation until validation
    std::string_view token_view = auth_header.substr(bearer_prefix.size());

    // validate_token needs a string, so convert only the token portion
    std::string token(token_view);

    // Use constant-time comparison with expiry checking
    auto [valid, key_name] = _config.auth.validate_token(token);

    if (!valid) {
        if (key_name.find("expired") != std::string::npos) {
            // Key matched but is expired
            log_control.warn("Admin request rejected: key '{}' has expired", key_name);
            return {false, "API key has expired"};
        }
        return {false, "invalid API key"};
    }

    // Log successful authentication with key name (not the key value)
    log_control.debug("Admin request authenticated with key '{}'", key_name);
    return {true, key_name};
}

// Check admin authentication - returns true if authorized (simple wrapper)
bool HttpController::check_admin_auth(const seastar::http::request& req) const {
    auto [authorized, _] = check_admin_auth_with_info(req);
    return authorized;
}

// Get client IP from request, checking X-Forwarded-For for proxied requests
// Uses string_view internally to minimize copies during header inspection
std::string HttpController::get_client_ip(const seastar::http::request& req) {
    // Check X-Forwarded-For header first (for proxied requests)
    // Access header directly without map copy
    auto xff_it = req._headers.find("X-Forwarded-For");
    if (xff_it != req._headers.end() && !xff_it->second.empty()) {
        // X-Forwarded-For can contain multiple IPs; take the first (original client)
        std::string_view xff(xff_it->second);
        auto comma_pos = xff.find(',');
        if (comma_pos != std::string_view::npos) {
            // Only allocate string for the first IP (before comma)
            return std::string(xff.substr(0, comma_pos));
        }
        // Single IP - must copy for return
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

// Helper to create an auth-protected admin handler with detailed error messages
template <typename AuthCheckWithInfo, typename Func>
auto make_admin_handler(AuthCheckWithInfo&& auth_check_with_info, Func&& handler) {
    return new async_handler([
        auth_check_with_info = std::forward<AuthCheckWithInfo>(auth_check_with_info),
        handler = std::forward<Func>(handler)
    ](auto req, auto rep) mutable -> future<std::unique_ptr<seastar::httpd::reply>> {

        // Execute the captured private check with detailed info
        auto [authorized, info] = auth_check_with_info(*req);
        if (!authorized) {
            rep->set_status(seastar::http::reply::status_type::unauthorized);
            rep->add_header("WWW-Authenticate", "Bearer");
            // Provide specific error message
            std::string error_msg = "{\"error\": \"Unauthorized - " + info + "\"}";
            rep->write_body("json", error_msg);
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
    // Use check_admin_auth_with_info for detailed error messages
    auto auth_check = [this](const auto& req) { return this->check_admin_auth_with_info(req); };

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

    // 4. API KEY MANAGEMENT
    r.add(operation_type::POST, url("/admin/keys/reload"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_keys_reload(std::move(req), std::move(rep));
    }));

    // 5. STATE INSPECTION (for rvctl CLI)
    r.add(operation_type::GET, url("/admin/dump/tree"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_dump_tree(std::move(req), std::move(rep));
    }));

    r.add(operation_type::GET, url("/admin/dump/cluster"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_dump_cluster(std::move(req), std::move(rep));
    }));

    r.add(operation_type::GET, url("/admin/dump/backends"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_dump_backends(std::move(req), std::move(rep));
    }));

    // 6. MANAGEMENT OPERATIONS
    r.add(operation_type::POST, url("/admin/drain"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_drain_backend(std::move(req), std::move(rep));
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

    // Extract W3C Trace Context for distributed tracing
    std::string traceparent_header = extract_traceparent(req->_headers);
    std::optional<TraceContext> trace_ctx;
    if (!traceparent_header.empty()) {
        auto ctx = TraceContext::parse(traceparent_header);
        if (ctx.valid) {
            trace_ctx = ctx;
            log_proxy.debug("[{}] Trace context parsed: trace_id={}", request_id, ctx.trace_id);
        }
    }

    // Start root span for this request (if tracing is enabled)
    auto request_span = TracingService::start_span("ranvier.request", trace_ctx);
    request_span.set_attribute("http.method", "POST");
    request_span.set_attribute("http.url", "/v1/chat/completions");
    request_span.set_attribute("ranvier.request_id", request_id);

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

    // Backpressure: try to acquire semaphore unit (non-blocking)
    // If semaphore is exhausted, immediately return 503 instead of queueing
    auto semaphore_units = seastar::try_get_units(_request_semaphore, 1);
    if (!semaphore_units) {
        ++_requests_rejected_concurrency;
        log_proxy.warn("[{}] Request rejected - concurrency limit reached ({} concurrent)",
                       request_id, _config.backpressure.max_concurrent_requests);
        metrics().record_backpressure_rejection();
        rep->set_status(seastar::http::reply::status_type::service_unavailable);
        rep->add_header("X-Request-ID", request_id);
        rep->add_header("Retry-After", std::to_string(_config.backpressure.retry_after_seconds));
        rep->write_body("json", "{\"error\": \"Server overloaded - too many concurrent requests\"}");
        co_return std::move(rep);
    }

    // Backpressure: check persistence queue depth
    if (is_persistence_backpressured()) {
        ++_requests_rejected_persistence;
        log_proxy.warn("[{}] Request rejected - persistence queue backpressure", request_id);
        metrics().record_backpressure_rejection();
        rep->set_status(seastar::http::reply::status_type::service_unavailable);
        rep->add_header("X-Request-ID", request_id);
        rep->add_header("Retry-After", std::to_string(_config.backpressure.retry_after_seconds));
        rep->write_body("json", "{\"error\": \"Server overloaded - persistence queue full\"}");
        co_return std::move(rep);
    }

    // Track request start time for latency metrics (ingress timestamp)
    auto request_start = std::chrono::steady_clock::now();
    metrics().record_request();

    // Track shard load metrics for P2C load balancing
    // Use a flag to track if we need to decrement on exit (for streaming lambda handoff)
    bool shard_metrics_active = shard_load_metrics_initialized();
    if (shard_metrics_active) {
        shard_load_metrics().increment_active();
    }

    // Select target shard for request processing using P2C algorithm
    // This evaluates shard load and may select a different shard for processing
    uint32_t target_shard = select_target_shard();
    uint32_t local_shard = seastar::this_shard_id();

    // Log cross-shard dispatch decision for observability
    if (target_shard != local_shard && _lb_config.enabled) {
        log_proxy.debug("[{}] P2C selected shard {} (local: {})", request_id, target_shard, local_shard);
    }

    // RAII guard ensures active request counter is decremented even if exception thrown
    // Guard is released before entering the lambda, which takes over responsibility
    ActiveRequestGuard active_request_guard(metrics());

    // Zero-copy body access: use string_view for tokenization and parsing,
    // only create string copy when we need to forward/modify the body
    std::string_view body_view(req->content.data(), req->content.size());
    std::string client_ip = get_client_ip(*req);

    log_proxy.info("[{}] Request received from {} ({} bytes)",
                   request_id, client_ip, body_view.size());

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

    // Start tokenization span
    {
        auto tokenize_span = TracingService::start_child_span("ranvier.tokenize");

        // First, check if client provided pre-tokenized prompt_token_ids
        if (_config.accept_client_tokens) {
            // Use string_view for zero-copy token extraction
            auto token_result = RequestRewriter::extract_prompt_token_ids(body_view, _config.max_token_id);
            if (token_result.found) {
                if (token_result.valid) {
                    tokens = std::move(token_result.tokens);
                    used_client_tokens = true;
                    tokenize_span.set_attribute("ranvier.token_source", "client");
                    tokenize_span.set_attribute("ranvier.token_count", static_cast<int64_t>(tokens.size()));
                    log_proxy.debug("[{}] Using {} client-provided token IDs for routing",
                                   request_id, tokens.size());
                } else {
                    // Client provided invalid tokens - reject the request
                    log_proxy.warn("[{}] Invalid client tokens: {}", request_id, token_result.error);
                    tokenize_span.set_error(token_result.error);
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
            // Uses string_view for zero-copy extraction
            auto extracted_text = RequestRewriter::extract_text(body_view);
            if (extracted_text.has_value()) {
                // TokenizerService now accepts string_view
                tokens = _tokenizer.encode(extracted_text.value());
            } else {
                // Fallback: tokenize the entire body (legacy behavior)
                // Use string_view for zero-copy tokenization
                tokens = _tokenizer.encode(body_view);
            }
            tokenize_span.set_attribute("ranvier.token_source", "local");
            tokenize_span.set_attribute("ranvier.token_count", static_cast<int64_t>(tokens.size()));
        }
    } // tokenize_span ends here

    // Rewrite request body with token IDs if enabled and we tokenized locally
    // Skip rewriting if client already provided prompt_token_ids (it's already in the body)
    // Only allocate the forwarded_body string when we need to forward/rewrite
    std::string forwarded_body;
    if (_config.enable_token_forwarding && !tokens.empty() && !used_client_tokens) {
        // RequestRewriter::rewrite accepts string_view, returns modified body
        auto rewrite_result = RequestRewriter::rewrite(body_view, tokens);
        if (rewrite_result.success) {
            forwarded_body = std::move(rewrite_result.body);
            log_proxy.debug("[{}] Request rewritten with {} token IDs ({} -> {} bytes)",
                           request_id, tokens.size(), body_view.size(), forwarded_body.size());
        } else {
            // Rewrite failed, use original body (single copy here)
            forwarded_body = std::string(body_view);
            log_proxy.debug("[{}] Request rewrite skipped: {}",
                           request_id, rewrite_result.error);
        }
    } else {
        // No rewriting needed - single copy for forwarding
        forwarded_body = std::string(body_view);
    }

    BackendId target_id;
    std::string routing_mode_str;

    // Start route lookup span
    {
        auto route_span = TracingService::start_child_span("ranvier.route_lookup");

        if (_config.is_prefix_mode()) {
            // PREFIX mode: consistent hashing on prefix tokens for KV cache reuse
            routing_mode_str = "prefix";
            route_span.set_attribute("ranvier.routing_mode", "prefix");
            auto affinity_backend = _router.get_backend_for_prefix(tokens, request_id);
            if (affinity_backend.has_value()) {
                target_id = affinity_backend.value();
                route_span.set_attribute("ranvier.cache_hit", true);
                log_proxy.debug("[{}] Prefix affinity -> backend {}", request_id, target_id);
            } else {
                log_proxy.warn("[{}] No backends registered", request_id);
                route_span.set_error("No backends registered");
                metrics().record_failure();
                rep->add_header("X-Request-ID", request_id);
                rep->write_body("json", "{\"error\": \"No backends registered!\"}");
                co_return std::move(rep);
            }
        } else if (_config.is_radix_mode()) {
            // RADIX mode: radix tree lookup with random fallback (adaptive learning)
            routing_mode_str = "radix";
            route_span.set_attribute("ranvier.routing_mode", "radix");
            auto lookup_result = _router.lookup(tokens, request_id);
            if (lookup_result.has_value()) {
                target_id = lookup_result.value();
                route_span.set_attribute("ranvier.cache_hit", true);
                log_proxy.debug("[{}] Radix tree hit -> backend {}", request_id, target_id);
            } else {
                auto random_id = _router.get_random_backend();
                if (!random_id.has_value()) {
                    log_proxy.warn("[{}] No backends registered", request_id);
                    route_span.set_error("No backends registered");
                    metrics().record_failure();
                    rep->add_header("X-Request-ID", request_id);
                    rep->write_body("json", "{\"error\": \"No backends registered!\"}");
                    co_return std::move(rep);
                }
                target_id = random_id.value();
                route_span.set_attribute("ranvier.cache_hit", false);
                log_proxy.debug("[{}] Radix tree miss, random -> backend {}", request_id, target_id);
            }
        } else {
            // ROUND_ROBIN mode: always use random/weighted backend selection
            routing_mode_str = "round_robin";
            route_span.set_attribute("ranvier.routing_mode", "round_robin");
            auto random_id = _router.get_random_backend();
            if (!random_id.has_value()) {
                log_proxy.warn("[{}] No backends registered", request_id);
                route_span.set_error("No backends registered");
                metrics().record_failure();
                rep->add_header("X-Request-ID", request_id);
                rep->write_body("json", "{\"error\": \"No backends registered!\"}");
                co_return std::move(rep);
            }
            target_id = random_id.value();
            route_span.set_attribute("ranvier.cache_hit", false);
            log_proxy.debug("[{}] Round-robin -> backend {}", request_id, target_id);
        }

        route_span.set_attribute("ranvier.backend_id", static_cast<int64_t>(target_id));
    } // route_span ends here

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

    // Set final attributes on request span before entering streaming lambda
    request_span.set_attribute("ranvier.backend_id", static_cast<int64_t>(target_id));
    request_span.set_attribute("ranvier.routing_mode", routing_mode_str);

    // Generate traceparent header for propagation to backend
    // Format: "00-{trace_id}-{span_id}-{flags}"
    std::string backend_traceparent;
    if (request_span.is_recording()) {
        std::string span_trace_id = request_span.trace_id();
        std::string span_id = request_span.span_id();
        if (!span_trace_id.empty() && !span_id.empty()) {
            backend_traceparent = "00-" + span_trace_id + "-" + span_id + "-01";
        }
    }

    // Release the guard - lambda takes over responsibility for decrementing counter
    active_request_guard.release();

    // Move gate_holder and semaphore_units into the lambda to keep them alive during streaming.
    // This ensures the gate stays held and the semaphore slot is occupied until streaming completes.
    rep->write_body("text/event-stream", [this, target_addr, forwarded_body, tokens, target_id, connect_timeout, request_timeout, retry_config, fallback_enabled, request_start, request_id, backend_traceparent, shard_metrics_active, gate_holder = std::move(gate_holder), semaphore_units = std::move(*semaphore_units)](output_stream<char> client_out) mutable -> future<> {

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
            if (shard_metrics_active && shard_load_metrics_initialized()) {
                shard_load_metrics().decrement_active();
            }
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
            if (shard_metrics_active && shard_load_metrics_initialized()) {
                shard_load_metrics().decrement_active();
            }
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

        // Build HTTP request with tracing headers for backend
        // - X-Request-ID: Ranvier's internal request correlation ID
        // - traceparent: W3C Trace Context for distributed tracing
        // Use forwarded_body which may include prompt_token_ids if token forwarding is enabled
        sstring http_req =
            "POST /v1/chat/completions HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + to_sstring(forwarded_body.size()) + "\r\n"
            "X-Request-ID: " + request_id + "\r\n";

        // Add traceparent header if tracing is enabled
        if (!backend_traceparent.empty()) {
            http_req += "traceparent: " + backend_traceparent + "\r\n";
        }

        http_req += "Connection: keep-alive\r\n\r\n" + forwarded_body;

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
            if (shard_metrics_active && shard_load_metrics_initialized()) {
                shard_load_metrics().decrement_active();
            }
            co_await bundle.close();
            try { co_await client_out.close(); } catch (...) {}
            stream_closed = true;
            std::rethrow_exception(rethrow_exception);
        }

        if (write_failed) {
            // Clean up and send error to client
            metrics().record_failure();
            metrics().decrement_active_requests();
            if (shard_metrics_active && shard_load_metrics_initialized()) {
                shard_load_metrics().decrement_active();
            }
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
                if (shard_metrics_active && shard_load_metrics_initialized()) {
                    shard_load_metrics().decrement_active();
                }
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

                    // Queue route for async persistence (fire-and-forget, non-blocking)
                    if (_persistence) {
                        _persistence->queue_save_route(tokens, current_backend);
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
                    if (shard_metrics_active && shard_load_metrics_initialized()) {
                        shard_load_metrics().decrement_active();
                    }
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

        // Decrement active request counters
        metrics().decrement_active_requests();
        if (shard_metrics_active && shard_load_metrics_initialized()) {
            shard_load_metrics().decrement_active();
            shard_load_metrics().record_request_completed();
        }
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
        // Use string_view for zero-copy tokenization
        std::string_view content_view(req->content.data(), req->content.size());
        tokens = _tokenizer.encode(content_view);
    } catch (const std::exception& e) {
        log_control.warn("POST /admin/routes: failed to tokenize content for backend {}: {}", backend_id, e.what());
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Failed to tokenize request content\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    // Check if route will actually be stored (must meet block_alignment minimum)
    size_t token_count = tokens.size();
    size_t aligned_len = (token_count / _config.block_alignment) * _config.block_alignment;
    if (aligned_len == 0) {
        log_control.warn("POST /admin/routes: content too short ({} tokens, need at least {} for block_alignment={})",
                         token_count, _config.block_alignment, _config.block_alignment);
        rep->set_status(seastar::http::reply::status_type::bad_request);
        std::ostringstream oss;
        oss << "{\"error\": \"Content too short: " << token_count << " tokens, need at least "
            << _config.block_alignment << " (block_alignment)\"}";
        rep->write_body("json", oss.str());
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    // Queue route for async persistence (fire-and-forget, non-blocking)
    if (_persistence) {
        _persistence->queue_save_route(tokens, backend_id);
    }

    return _router.learn_route_global(tokens, backend_id).then([backend_id, token_count, aligned_len, rep = std::move(rep)]() mutable {
         std::ostringstream oss;
         oss << "{\"status\": \"ok\", \"backend_id\": " << backend_id
             << ", \"tokens\": " << token_count
             << ", \"stored_tokens\": " << aligned_len << "}";
         rep->write_body("json", oss.str());
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

    // Queue backend for async persistence (fire-and-forget, non-blocking)
    if (_persistence) {
        _persistence->queue_save_backend(id, std::string(ip_str), static_cast<uint16_t>(port), weight, priority);
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

    // Queue removal for async persistence (fire-and-forget, non-blocking)
    if (_persistence) {
        _persistence->queue_remove_routes_for_backend(id);
        _persistence->queue_remove_backend(id);
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

    // Queue routes removal for async persistence (fire-and-forget, non-blocking)
    if (_persistence) {
        _persistence->queue_remove_routes_for_backend(backend_id);
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

    // Queue clear-all for async persistence (fire-and-forget, non-blocking)
    if (_persistence) {
        _persistence->queue_clear_all();
    }

    log_control.warn("Queued clear of all persisted data (backends and routes). Restart required to clear in-memory state.");
    rep->write_body("json", "{\"status\": \"ok\", \"warning\": \"All persisted data queued for clearing. Restart to clear in-memory state.\"}");
    return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
}

// ---------------------------------------------------------
// API KEY MANAGEMENT
// ---------------------------------------------------------

future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_keys_reload(std::unique_ptr<seastar::httpd::request> req, std::unique_ptr<seastar::httpd::reply> rep) {
    // Usage: POST /admin/keys/reload
    // Triggers a hot-reload of the configuration file to pick up new API keys
    // Note: This triggers SIGHUP for async reload. The response returns current state,
    // but the new config will be applied shortly after the response is sent.

    // Get current key count (for informational purposes)
    size_t current_key_count = _config.auth.valid_key_count();

    if (!_config_reload_callback) {
        log_control.error("Config reload callback not configured");
        rep->set_status(seastar::http::reply::status_type::internal_server_error);
        rep->write_body("json", "{\"error\": \"Config reload not available\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    // Trigger the reload (sends SIGHUP to self for async processing)
    bool triggered = _config_reload_callback();

    if (!triggered) {
        log_control.error("Failed to trigger config reload");
        rep->set_status(seastar::http::reply::status_type::internal_server_error);
        rep->write_body("json", "{\"error\": \"Failed to trigger config reload\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    log_control.info("Config reload triggered via /admin/keys/reload endpoint");

    // Helper to escape JSON string values (prevent injection)
    auto escape_json_string = [](const std::string& s) -> std::string {
        std::string result;
        result.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\b': result += "\\b"; break;
                case '\f': result += "\\f"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        // Control character - encode as \u00XX
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                        result += buf;
                    } else {
                        result += c;
                    }
            }
        }
        return result;
    };

    // Build response with current key metadata (names only, not values)
    // Note: This shows the state BEFORE the reload completes
    std::string response = "{\"status\": \"reload_triggered\", \"message\": \"Configuration reload initiated\", "
                          "\"current_key_count\": " + std::to_string(current_key_count) + ", \"current_keys\": [";

    bool first = true;
    for (const auto& api_key : _config.auth.api_keys) {
        if (!first) response += ", ";
        first = false;
        response += "{\"name\": \"" + escape_json_string(api_key.name) + "\", \"expired\": " +
                   (api_key.is_expired() ? "true" : "false") + "}";
    }
    if (!_config.auth.admin_api_key.empty()) {
        if (!first) response += ", ";
        response += "{\"name\": \"legacy-key\", \"expired\": false}";
    }
    response += "]}";

    rep->write_body("json", response);
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

bool HttpController::is_persistence_backpressured() const {
    if (!_config.backpressure.enable_persistence_backpressure || !_persistence) {
        return false;
    }

    // Check if persistence queue is above threshold.
    // Note: queue_depth() acquires a mutex, but this is acceptable for backpressure checks:
    // 1. The mutex is uncontended on most requests (route learning is infrequent)
    // 2. Operators can disable this check via enable_persistence_backpressure=false
    // Future optimization: Use an atomic queue size counter if profiling shows contention.
    size_t max_depth = _persistence->max_queue_depth();
    if (max_depth == 0) {
        return false;  // Avoid division by zero; 0 means unlimited
    }
    size_t current_depth = _persistence->queue_depth();
    double fill_ratio = static_cast<double>(current_depth) / static_cast<double>(max_depth);

    return fill_ratio >= _config.backpressure.persistence_queue_threshold;
}

uint32_t HttpController::select_target_shard() {
    uint32_t local_shard = seastar::this_shard_id();

    // Fast path: load balancing disabled or single shard
    if (!_lb_config.enabled || !_load_balancer || seastar::smp::count <= 1) {
        ++_requests_local_dispatch;
        return local_shard;
    }

    // Update local shard's metrics snapshot in the load balancer cache
    if (shard_load_metrics_initialized()) {
        _load_balancer->local().update_local_snapshot();
    }

    // Use P2C algorithm to select target shard
    uint32_t target_shard = _load_balancer->local().select_shard();

    // Track dispatch metrics
    if (target_shard == local_shard) {
        ++_requests_local_dispatch;
    } else {
        ++_requests_cross_shard_dispatch;
    }

    return target_shard;
}

// ---------------------------------------------------------
// STATE INSPECTION HANDLERS (for rvctl CLI)
// ---------------------------------------------------------

// Helper to serialize a DumpNode to JSON
static std::string dump_node_to_json(const RadixTree::DumpNode& node, int indent_level = 0) {
    std::ostringstream ss;
    std::string indent(indent_level * 2, ' ');
    std::string inner_indent((indent_level + 1) * 2, ' ');

    ss << "{\n";
    ss << inner_indent << "\"type\": \"" << node.type << "\",\n";

    // Prefix array
    ss << inner_indent << "\"prefix\": [";
    for (size_t i = 0; i < node.prefix.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << node.prefix[i];
    }
    ss << "],\n";

    // Backend (optional)
    if (node.backend.has_value()) {
        ss << inner_indent << "\"backend\": " << node.backend.value() << ",\n";
    } else {
        ss << inner_indent << "\"backend\": null,\n";
    }

    ss << inner_indent << "\"origin\": \"" << node.origin << "\",\n";
    ss << inner_indent << "\"last_accessed_ms\": " << node.last_accessed_ms << ",\n";

    // Children array
    ss << inner_indent << "\"children\": [";
    if (!node.children.empty()) {
        ss << "\n";
        for (size_t i = 0; i < node.children.size(); ++i) {
            if (i > 0) ss << ",\n";
            ss << inner_indent << "  {\"edge\": " << node.children[i].first << ", \"node\": ";
            ss << dump_node_to_json(node.children[i].second, indent_level + 2);
            ss << "}";
        }
        ss << "\n" << inner_indent;
    }
    ss << "]\n";

    ss << indent << "}";
    return ss.str();
}

future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_dump_tree(
    std::unique_ptr<seastar::httpd::request> req,
    std::unique_ptr<seastar::httpd::reply> rep) {

    // Check for optional prefix filter (comma-separated token IDs)
    sstring prefix_str = req->get_query_param("prefix");

    std::vector<TokenId> prefix_filter;
    if (!prefix_str.empty()) {
        // Parse comma-separated token IDs
        std::string prefix_string{prefix_str};
        std::istringstream iss{prefix_string};
        std::string token_str;
        while (std::getline(iss, token_str, ',')) {
            try {
                prefix_filter.push_back(std::stoi(token_str));
            } catch (const std::exception& e) {
                log_control.warn("GET /admin/dump/tree: invalid prefix token '{}'", token_str);
                rep->set_status(seastar::http::reply::status_type::bad_request);
                rep->write_body("json", "{\"error\": \"Invalid prefix token ID\"}");
                return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
            }
        }
    }

    // Get tree dump
    std::string json_response;
    if (prefix_filter.empty()) {
        auto dump = _router.get_tree_dump();
        auto tree_stats = _router.get_all_backend_ids().size();  // For route count context

        std::ostringstream oss;
        oss << "{\n";
        oss << "  \"shard_id\": " << seastar::this_shard_id() << ",\n";
        oss << "  \"tree\": " << dump_node_to_json(dump, 1) << "\n";
        oss << "}";
        json_response = oss.str();
    } else {
        auto dump = _router.get_tree_dump_with_prefix(prefix_filter);
        if (dump.has_value()) {
            std::ostringstream oss;
            oss << "{\n";
            oss << "  \"shard_id\": " << seastar::this_shard_id() << ",\n";
            oss << "  \"prefix_filter\": [";
            for (size_t i = 0; i < prefix_filter.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << prefix_filter[i];
            }
            oss << "],\n";
            oss << "  \"tree\": " << dump_node_to_json(dump.value(), 1) << "\n";
            oss << "}";
            json_response = oss.str();
        } else {
            std::ostringstream oss;
            oss << "{\n";
            oss << "  \"shard_id\": " << seastar::this_shard_id() << ",\n";
            oss << "  \"prefix_filter\": [";
            for (size_t i = 0; i < prefix_filter.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << prefix_filter[i];
            }
            oss << "],\n";
            oss << "  \"tree\": null,\n";
            oss << "  \"error\": \"Prefix not found in tree\"\n";
            oss << "}";
            json_response = oss.str();
        }
    }

    rep->write_body("json", json_response);
    return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
}

future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_dump_cluster(
    std::unique_ptr<seastar::httpd::request> req,
    std::unique_ptr<seastar::httpd::reply> rep) {

    // GossipService is only on shard 0
    auto* gossip = _router.gossip_service();
    if (!gossip) {
        std::string response = "{\n"
            "  \"error\": \"Cluster mode not enabled\",\n"
            "  \"cluster_enabled\": false\n"
            "}";
        rep->write_body("json", response);
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    auto state = gossip->get_cluster_state();

    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"cluster_enabled\": true,\n";
    oss << "  \"quorum_state\": \"" << state.quorum_state << "\",\n";
    oss << "  \"quorum_required\": " << state.quorum_required << ",\n";
    oss << "  \"peers_alive\": " << state.peers_alive << ",\n";
    oss << "  \"total_peers\": " << state.total_peers << ",\n";
    oss << "  \"peers_recently_seen\": " << state.peers_recently_seen << ",\n";
    oss << "  \"is_draining\": " << (state.is_draining ? "true" : "false") << ",\n";
    oss << "  \"local_backend_id\": " << state.local_backend_id << ",\n";
    oss << "  \"peers\": [\n";

    for (size_t i = 0; i < state.peers.size(); ++i) {
        const auto& peer = state.peers[i];
        oss << "    {\n";
        oss << "      \"address\": \"" << peer.address << "\",\n";
        oss << "      \"port\": " << peer.port << ",\n";
        oss << "      \"is_alive\": " << (peer.is_alive ? "true" : "false") << ",\n";
        oss << "      \"last_seen_ms\": " << peer.last_seen_ms;
        if (peer.associated_backend.has_value()) {
            oss << ",\n      \"associated_backend\": " << peer.associated_backend.value() << "\n";
        } else {
            oss << ",\n      \"associated_backend\": null\n";
        }
        oss << "    }";
        if (i < state.peers.size() - 1) oss << ",";
        oss << "\n";
    }

    oss << "  ]\n";
    oss << "}";

    rep->write_body("json", oss.str());
    return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
}

future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_dump_backends(
    std::unique_ptr<seastar::httpd::request> req,
    std::unique_ptr<seastar::httpd::reply> rep) {

    auto backends = _router.get_all_backend_states();

    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"shard_id\": " << seastar::this_shard_id() << ",\n";
    oss << "  \"backend_count\": " << backends.size() << ",\n";
    oss << "  \"backends\": [\n";

    for (size_t i = 0; i < backends.size(); ++i) {
        const auto& b = backends[i];
        oss << "    {\n";
        oss << "      \"id\": " << b.id << ",\n";
        oss << "      \"address\": \"" << b.address << "\",\n";
        oss << "      \"port\": " << b.port << ",\n";
        oss << "      \"weight\": " << b.weight << ",\n";
        oss << "      \"priority\": " << b.priority << ",\n";
        oss << "      \"is_draining\": " << (b.is_draining ? "true" : "false") << ",\n";
        oss << "      \"is_dead\": " << (b.is_dead ? "true" : "false");
        if (b.drain_start_ms > 0) {
            oss << ",\n      \"drain_start_ms\": " << b.drain_start_ms << "\n";
        } else {
            oss << "\n";
        }
        oss << "    }";
        if (i < backends.size() - 1) oss << ",";
        oss << "\n";
    }

    oss << "  ]\n";
    oss << "}";

    rep->write_body("json", oss.str());
    return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
}

future<std::unique_ptr<seastar::httpd::reply>> HttpController::handle_drain_backend(
    std::unique_ptr<seastar::httpd::request> req,
    std::unique_ptr<seastar::httpd::reply> rep) {

    // Usage: POST /admin/drain?backend_id=1
    sstring id_str = req->get_query_param("backend_id");

    if (id_str.empty()) {
        log_control.warn("POST /admin/drain: missing backend_id parameter");
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Missing backend_id parameter\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    int backend_id;
    try {
        backend_id = std::stoi(std::string(id_str));
    } catch (const std::exception& e) {
        log_control.warn("POST /admin/drain: invalid backend_id '{}': {}", id_str, e.what());
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Invalid backend_id: must be a valid integer\"}");
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    }

    log_control.info("POST /admin/drain: initiating drain for backend {}", backend_id);

    // Initiate drain and optionally broadcast to cluster
    return _router.drain_backend_global(backend_id).then([this, backend_id, rep = std::move(rep)]() mutable {
        // If gossip is enabled, broadcast the draining state to cluster peers
        auto* gossip = _router.gossip_service();
        if (gossip && gossip->is_enabled()) {
            // Note: We can't easily broadcast for a specific backend from here
            // as broadcast_node_state uses the local backend ID.
            // For a full implementation, GossipService would need a method
            // to broadcast draining for arbitrary backend IDs.
            log_control.info("Backend {} marked as draining (cluster notification not sent - use node-level drain for cluster-wide notification)", backend_id);
        }

        std::ostringstream oss;
        oss << "{\n";
        oss << "  \"status\": \"ok\",\n";
        oss << "  \"backend_id\": " << backend_id << ",\n";
        oss << "  \"action\": \"drain_initiated\",\n";
        oss << "  \"message\": \"Backend will be removed after drain timeout\"\n";
        oss << "}";

        rep->write_body("json", oss.str());
        return make_ready_future<std::unique_ptr<seastar::httpd::reply>>(std::move(rep));
    });
}

} // namespace ranvier
