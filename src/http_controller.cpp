#include "http_controller.hpp"
#include "agent_registry.hpp"
#include "boundary_detector.hpp"
#include "cache_event_parser.hpp"
#include "logging.hpp"
#include "parse_utils.hpp"
#include "request_rewriter.hpp"
#include "shard_load_metrics.hpp"
#include "text_validator.hpp"
#include "tracing_service.hpp"

#include "stream_parser.hpp"

#include <algorithm>
#include <cctype>
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
#include <seastar/net/dns.hh>
#include <seastar/net/inet_address.hh>

using namespace seastar;

namespace ranvier {

// ConnectionErrorType, classify_connection_error, and connection_error_to_string
// are defined in proxy_retry_policy.hpp (extracted for testability).

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
    future<std::unique_ptr<seastar::http::reply>> handle(const sstring& path, std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep) override {
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

// Add CORS headers to a reply (for dashboard cross-port access)
inline void add_cors_headers(seastar::http::reply& rep) {
    rep.add_header("Access-Control-Allow-Origin", "*");
    rep.add_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    rep.add_header("Access-Control-Allow-Headers", "Authorization, Content-Type");
}

// Helper to create an auth-protected admin handler with detailed error messages.
// When enable_cors is true, adds CORS headers to every response (for dashboard on :9180).
template <typename AuthCheckWithInfo, typename Func>
auto make_admin_handler(AuthCheckWithInfo&& auth_check_with_info, Func&& handler, bool enable_cors = false) {
    return new async_handler([
        auth_check_with_info = std::forward<AuthCheckWithInfo>(auth_check_with_info),
        handler = std::forward<Func>(handler),
        enable_cors
    ](auto req, auto rep) mutable -> future<std::unique_ptr<seastar::http::reply>> {

        // Execute the captured private check with detailed info
        auto [authorized, info] = auth_check_with_info(*req);
        if (!authorized) {
            rep->set_status(seastar::http::reply::status_type::unauthorized);
            rep->add_header("WWW-Authenticate", "Bearer");
            if (enable_cors) add_cors_headers(*rep);
            // Provide specific error message (escape to prevent malformed JSON)
            std::string error_msg = "{\"error\": \"Unauthorized - " + escape_json_string(info) + "\"}";
            rep->write_body("json", error_msg);
            return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
        }
        return handler(std::move(req), std::move(rep)).then(
            [enable_cors](std::unique_ptr<seastar::http::reply> rep) {
                if (enable_cors) add_cors_headers(*rep);
                return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
            });
    });
}

// Helper to create a rate-limited handler
template <typename RateLimitCheck, typename Func>
auto make_rate_limited_handler(RateLimitCheck&& rate_limit_check, Func&& handler) {
    return new async_handler([
        rate_limit_check = std::forward<RateLimitCheck>(rate_limit_check),
        handler = std::forward<Func>(handler)
    ](auto req, auto rep) mutable
        -> future<std::unique_ptr<seastar::http::reply>> {
        if (!rate_limit_check(*req)) {
            metrics().record_rate_limited();
            rep->set_status(seastar::http::reply::status_type::service_unavailable);
            rep->add_header("Retry-After", "1");
            rep->write_body("json", "{\"error\": \"Rate limit exceeded - try again later\"}");
            return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
        }
        return handler(std::move(req), std::move(rep));
    });
}

void HttpController::register_routes(seastar::httpd::routes& r) {
    using namespace seastar::httpd;

    // CORS flag — gated behind dashboard config (enabled in local mode, disabled in cloud)
    const bool cors = _config.dashboard.enable_cors;

    // 0. HEALTH CHECK (public, no auth required - for load balancer probes)
    r.add(operation_type::GET, url("/health"), new async_handler<std::function<future<std::unique_ptr<seastar::http::reply>>(std::unique_ptr<seastar::http::request>, std::unique_ptr<seastar::http::reply>)>>(
        [this](auto req, auto rep) {
            return this->handle_health(std::move(req), std::move(rep));
        }));

    // Define the check once as a local lambda to keep the calls clean.
    auto rate_limit_check = [this](const auto& req) { return this->check_rate_limit(req); };

    // 1. DATA PLANE (rate limited)
    r.add(operation_type::POST, url("/v1/chat/completions"), make_rate_limited_handler(rate_limit_check, [this](auto req, auto rep) {
        return this->handle_proxy(std::move(req), std::move(rep), "/v1/chat/completions");
    }));
    r.add(operation_type::POST, url("/v1/completions"), make_rate_limited_handler(rate_limit_check, [this](auto req, auto rep) {
        return this->handle_proxy(std::move(req), std::move(rep), "/v1/completions");
    }));

    // Define the check once as a local lambda to keep the calls clean.
    // Use check_admin_auth_with_info for detailed error messages
    auto auth_check = [this](const auto& req) { return this->check_admin_auth_with_info(req); };

    // CORS preflight handlers for admin endpoints used by the dashboard on :9180.
    // Only registered when CORS is enabled (local mode). The dashboard calls
    // GET /admin/dump/backends, GET /admin/scheduler/stats, GET /admin/agents,
    // and POST /admin/agents/{pause,resume}. Browsers send OPTIONS preflight
    // for cross-origin POST requests.
    if (cors) {
        auto make_preflight = []() {
            return new async_handler<std::function<future<std::unique_ptr<seastar::http::reply>>(
                std::unique_ptr<seastar::http::request>, std::unique_ptr<seastar::http::reply>)>>(
                [](auto /*req*/, auto rep) {
                    add_cors_headers(*rep);
                    rep->set_status(seastar::http::reply::status_type{204});
                    rep->done();
                    return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
                });
        };
        r.add(operation_type::OPTIONS, url("/admin/dump/backends"), make_preflight());
        r.add(operation_type::OPTIONS, url("/admin/scheduler/stats"), make_preflight());
        r.add(operation_type::OPTIONS, url("/admin/agents"), make_preflight());
        r.add(operation_type::OPTIONS, url("/admin/agents/pause"), make_preflight());
        r.add(operation_type::OPTIONS, url("/admin/agents/resume"), make_preflight());
    }

    // 2. CONTROL PLANE - Create/Update (auth protected)
    r.add(operation_type::POST, url("/admin/routes"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_broadcast_route(std::move(req), std::move(rep));
    }, cors));

    r.add(operation_type::POST, url("/admin/backends"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_broadcast_backend(std::move(req), std::move(rep));
    }, cors));

    // 3. CONTROL PLANE - Delete (auth protected)
    r.add(operation_type::DELETE, url("/admin/backends"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_delete_backend(std::move(req), std::move(rep));
    }, cors));

    r.add(operation_type::DELETE, url("/admin/routes"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_delete_routes(std::move(req), std::move(rep));
    }, cors));

    r.add(operation_type::POST, url("/admin/clear"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_clear_all(std::move(req), std::move(rep));
    }, cors));

    // 4. API KEY MANAGEMENT
    r.add(operation_type::POST, url("/admin/keys/reload"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_keys_reload(std::move(req), std::move(rep));
    }, cors));

    // 5. STATE INSPECTION (for rvctl CLI)
    r.add(operation_type::GET, url("/admin/dump/tree"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_dump_tree(std::move(req), std::move(rep));
    }, cors));

    r.add(operation_type::GET, url("/admin/dump/cluster"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_dump_cluster(std::move(req), std::move(rep));
    }, cors));

    r.add(operation_type::GET, url("/admin/dump/backends"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_dump_backends(std::move(req), std::move(rep));
    }, cors));

    r.add(operation_type::GET, url("/admin/config"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_dump_config(std::move(req), std::move(rep));
    }, cors));

    // 6. MANAGEMENT OPERATIONS
    r.add(operation_type::POST, url("/admin/drain"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_drain_backend(std::move(req), std::move(rep));
    }, cors));

    // Backend vLLM metrics endpoint
    r.add(operation_type::GET, url("/admin/backends/metrics"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_backend_metrics(std::move(req), std::move(rep));
    }, cors));

    // 7. SCHEDULER STATS (admin, auth required)
    r.add(operation_type::GET, url("/admin/scheduler/stats"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_scheduler_stats(std::move(req), std::move(rep));
    }, cors));

    // 8. AGENT REGISTRY (admin, auth required)
    // Note: these endpoints are shard-local. Each shard tracks its own agent
    // counters independently. Cross-shard aggregation is deferred to a future session.
    r.add(operation_type::GET, url("/admin/agents"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_list_agents(std::move(req), std::move(rep));
    }, cors));
    r.add(operation_type::GET, url("/admin/agents/stats"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_agent_stats(std::move(req), std::move(rep));
    }, cors));
    r.add(operation_type::POST, url("/admin/agents/pause"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_pause_agent(std::move(req), std::move(rep));
    }, cors));
    r.add(operation_type::POST, url("/admin/agents/resume"), make_admin_handler(auth_check, [this](auto req, auto rep) {
        return this->handle_resume_agent(std::move(req), std::move(rep));
    }, cors));

    // 9. CACHE EVENTS ENDPOINT (push-based cache eviction notifications)
    // Uses its own auth check (Bearer token from cache_events.auth_token config)
    // rather than admin auth. Only registered when cache_events.enabled is true.
    if (_config.cache_events.enabled) {
        r.add(operation_type::POST, url("/v1/cache/events"),
            new async_handler<std::function<future<std::unique_ptr<seastar::http::reply>>(
                std::unique_ptr<seastar::http::request>, std::unique_ptr<seastar::http::reply>)>>(
                [this](auto req, auto rep) {
                    return this->handle_cache_events(std::move(req), std::move(rep));
                }));
    }

    // Start rate limiter cleanup timer (Hard Rule #5: timer with gate guard)
    _rate_limiter.start();

    // Register scheduler metrics (Rule #6: deregistered in stop() before member destruction)
    namespace sm = seastar::metrics;
    const char* priority_labels[] = {"critical", "high", "normal", "low"};
    for (size_t i = 0; i < 4; ++i) {
        _scheduler_metrics.add_group("ranvier", {
            sm::make_gauge("scheduler_queue_depth",
                sm::description("Current queue depth per priority tier"),
                {{"priority", priority_labels[i]}},
                [this, i] { return static_cast<double>(_scheduler.queue_depth(i)); }),

            sm::make_gauge("scheduler_dropped_total",
                sm::description("Requests dropped due to full priority queue"),
                {{"priority", priority_labels[i]}},
                [this, i] { return static_cast<double>(_scheduler.overflow_drops()[i]); }),  // ref to array, no copy
        });
    }
    _scheduler_metrics.add_group("ranvier", {
        sm::make_gauge("scheduler_enqueued_total",
            sm::description("Total requests enqueued into priority scheduler"),
            [this] { return static_cast<double>(_scheduler.total_enqueued_count()); }),

        sm::make_gauge("scheduler_dequeued_total",
            sm::description("Total requests dequeued from priority scheduler"),
            [this] { return static_cast<double>(_scheduler.total_dequeued_count()); }),

        sm::make_gauge("scheduler_enabled",
            sm::description("Whether priority queue scheduling is enabled (0 or 1)"),
            [this] { return _config.backpressure.enable_priority_queue ? 1.0 : 0.0; }),

        sm::make_gauge("scheduler_agents_tracked",
            sm::description("Number of unique agents tracked for fair scheduling"),
            [this] { return static_cast<double>(_scheduler.agents_tracked()); }),

        // Pause-aware scheduling metrics
        sm::make_gauge("scheduler_paused_skips_total",
            sm::description("Total times dequeue() skipped a paused agent's request"),
            [this] { return static_cast<double>(_scheduler.paused_skips()); }),

        sm::make_gauge("scheduler_per_agent_drops_total",
            sm::description("Requests dropped due to per-agent queue depth limit"),
            [this] { return static_cast<double>(_scheduler.per_agent_drops()); }),
    });

    // Register agent registry metrics (Rule #6: deregistered in stop())
    if (_agent_registry) {
        _agent_metrics.add_group("ranvier", {
            sm::make_gauge("agents_tracked",
                sm::description("Number of agents tracked by the agent registry"),
                [this] { return static_cast<double>(_agent_registry->agent_count()); }),

            sm::make_gauge("agents_overflow_drops",
                sm::description("Agents dropped due to MAX_KNOWN_AGENTS bound"),
                [this] { return static_cast<double>(_agent_registry->overflow_drops()); }),
        });
    }

    // Start priority queue background dequeue loop if enabled (Rule #5: gate-guarded)
    if (_config.backpressure.enable_priority_queue) {
        (void)process_priority_queue().handle_exception([](auto ep) {
            // Rule #9: log at warn level
            try { std::rethrow_exception(ep); }
            catch (const std::exception& e) {
                log_proxy.warn("Priority queue loop terminated with exception: {}", e.what());
            } catch (...) {
                log_proxy.warn("Priority queue loop terminated with unknown exception");
            }
        });
    }
}

// ---------------------------------------------------------
// PROXY HELPER METHODS
// ---------------------------------------------------------

// Write error message to client, handling disconnected clients gracefully.
// Streaming (SSE/chunked) responses use SSE format: data: {"error": "..."}\n\n
// Non-streaming (Content-Length) responses use plain JSON: {"error": "..."}
future<> HttpController::write_client_error(
    output_stream<char>* client_out,
    std::string error_msg,
    bool is_streaming) {
    try {
        sstring msg;
        if (is_streaming) {
            msg = sstring("data: {\"error\": \"") + sstring(error_msg) + "\"}\n\n";
        } else {
            msg = sstring("{\"error\": \"") + sstring(error_msg) + "\"}";
        }
        co_await client_out->write(msg);
        co_await client_out->flush();
    } catch (...) {
        // Client may have disconnected - classify to determine severity
        auto err_type = classify_connection_error(std::current_exception());
        if (err_type != ConnectionErrorType::NONE) {
            // Connection error (EPIPE, ECONNRESET) - expected when client disconnects
            log_proxy.debug("Client disconnected while sending error: {} ({})",
                           error_msg, connection_error_to_string(err_type));
        } else {
            // Rule #9: Unknown error type - log at warn level
            log_proxy.warn("Failed to send error to client (unexpected exception): {}", error_msg);
        }
    }
}

// Estimate request cost from pre-extracted content length and max_tokens.
// Pure computation — no I/O, no futures, no JSON parsing.
HttpController::CostEstimate HttpController::estimate_request_cost(
        size_t content_chars, uint64_t max_tokens_from_request) const {
    CostEstimate est;

    // Input: chars / 4 (rough chars-per-token approximation)
    est.input_tokens = std::min(
        static_cast<uint64_t>(content_chars / 4),
        _config.cost_estimation_max_tokens);

    // Output: use request's max_tokens if present, otherwise input * multiplier
    if (max_tokens_from_request > 0) {
        est.output_tokens = std::min(max_tokens_from_request, _config.cost_estimation_max_tokens);
    } else {
        est.output_tokens = std::min(
            static_cast<uint64_t>(est.input_tokens * _config.cost_estimation_output_multiplier),
            _config.cost_estimation_max_tokens);
    }

    est.cost_units = static_cast<double>(est.input_tokens)
        + (static_cast<double>(est.output_tokens) * _config.cost_estimation_output_multiplier);

    return est;
}

// Helper to parse a string to PriorityLevel (case-insensitive)
static std::optional<PriorityLevel> parse_priority_string(std::string_view s) {
    // Manual case-insensitive comparison for a small fixed set
    if (s.size() < 3 || s.size() > 8) return std::nullopt;

    // Convert to lowercase in a small stack buffer
    char buf[9];
    for (size_t i = 0; i < s.size(); ++i) {
        buf[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    }
    std::string_view lower(buf, s.size());

    if (lower == "critical") return PriorityLevel::CRITICAL;
    if (lower == "high")     return PriorityLevel::HIGH;
    if (lower == "normal")   return PriorityLevel::NORMAL;
    if (lower == "low")      return PriorityLevel::LOW;
    return std::nullopt;
}

// Helper to parse the default_priority config string to PriorityLevel
static PriorityLevel default_priority_from_string(const std::string& s) {
    auto p = parse_priority_string(s);
    return p.value_or(PriorityLevel::NORMAL);
}

// Extract priority level from request using cascade:
//   1. X-Ranvier-Priority header (if respect_header enabled)
//   2. User-Agent match against known patterns
//   3. Cost-based default from estimated_cost_units
PriorityLevel HttpController::extract_priority(
        const seastar::http::request& req,
        double estimated_cost_units,
        std::string_view body_view) const {

    // Step 1: Explicit header
    if (_config.priority_tier_respect_header) {
        auto it = req._headers.find("X-Ranvier-Priority");
        if (it != req._headers.end()) {
            auto parsed = parse_priority_string(it->second);
            if (parsed.has_value()) {
                return *parsed;
            }
            // Rule #9: Log invalid header at warn level
            log_proxy.warn("Invalid X-Ranvier-Priority header value '{}', falling through",
                          it->second);
        }
    }

    // Step 2: User-Agent match
    // NOTE: ua is seastar::sstring whose size_type is uint32_t.  Its find()
    // returns a 32-bit npos (0xFFFFFFFF) which != std::string::npos on 64-bit
    // platforms (0xFFFFFFFFFFFFFFFF), causing every find-miss to look like a
    // match.  Use the sstring's own npos via decltype.
    auto ua_it = req._headers.find("User-Agent");
    if (ua_it != req._headers.end()) {
        const auto& ua = ua_it->second;
        using ua_type = std::decay_t<decltype(ua)>;
        for (const auto& entry : _config.priority_tier_known_user_agents) {
            if (ua.find(entry.pattern) != ua_type::npos) {
                auto matched_priority = static_cast<PriorityLevel>(entry.priority);
                // Promote to CRITICAL if request has "stream": true
                if (matched_priority != PriorityLevel::CRITICAL) {
                    // Quick substring check for "stream":true in body
                    // (avoids full JSON parse; sufficient for heuristic promotion)
                    if (body_view.find("\"stream\":true") != std::string_view::npos ||
                        body_view.find("\"stream\": true") != std::string_view::npos) {
                        return PriorityLevel::CRITICAL;
                    }
                }
                return matched_priority;
            }
        }
    }

    // Step 3: Cost-based default
    if (estimated_cost_units > _config.priority_tier_cost_threshold_high) {
        return PriorityLevel::HIGH;
    }
    if (estimated_cost_units < _config.priority_tier_cost_threshold_low) {
        return PriorityLevel::LOW;
    }

    return default_priority_from_string(_config.priority_tier_default);
}

// Record final metrics and clean up after proxy request completes
void HttpController::record_proxy_completion_metrics(
    const ProxyContext& ctx,
    const std::chrono::steady_clock::time_point& backend_end) {

    // Record backend total latency (from connection start to completion)
    if (!ctx.connection_failed && !ctx.connection_error) {
        auto backend_latency = MetricsService::to_seconds(backend_end - ctx.connect_start);
        // Record in legacy histogram
        metrics().record_backend_total_latency(backend_latency);
        // Record in per-backend histogram for GPU model comparison (e.g., H100 vs A100)
        metrics().record_backend_latency_by_id(ctx.current_backend, backend_latency);
    }

    // Record total request duration (end-to-end from ingress to completion)
    auto total_latency = MetricsService::to_seconds(backend_end - ctx.request_start);
    // Record in legacy histogram
    metrics().record_request_latency(total_latency);
    // Record in new advanced histogram with optimized buckets
    metrics().record_router_total_latency(total_latency);
}

// Establish connection to backend with retry and fallback logic
future<ConnectionBundle> HttpController::establish_backend_connection(ProxyContext* ctx) {
    ctx->connect_start = std::chrono::steady_clock::now();
    ctx->current_backoff = ctx->retry_config.initial_backoff;

    ConnectionBundle bundle;

    while (ctx->retry_attempt <= ctx->retry_config.max_retries) {
        // Check if we've exceeded overall request deadline
        if (lowres_clock::now() >= ctx->request_deadline) {
            log_proxy.warn("[{}] Request deadline exceeded during connection retry", ctx->request_id);
            ctx->connection_failed = true;
            _circuit_breaker.record_failure(ctx->current_backend);
            break;
        }

        auto connect_deadline = lowres_clock::now() + ctx->connect_timeout;
        auto conn_future = with_timeout(connect_deadline, _pool.get(ctx->current_addr, ctx->request_id));

        bool this_attempt_failed = false;
        bundle = co_await std::move(conn_future).handle_exception([&](auto ep) {
            this_attempt_failed = true;
            return ConnectionBundle{}; // Return empty bundle
        });

        if (!this_attempt_failed) {
            // Connection successful - record connect latency
            auto connect_end = std::chrono::steady_clock::now();
            metrics().record_connect_latency(
                MetricsService::to_seconds(connect_end - ctx->connect_start));
            ctx->connection_failed = false;
            log_proxy.info("[{}] Connection established to backend {} at {}",
                          ctx->request_id, ctx->current_backend, ctx->current_addr);
            break;
        }

        // Record failure for circuit breaker
        _circuit_breaker.record_failure(ctx->current_backend);

        // Short-circuit retries if circuit breaker opened for this backend.
        // Avoids holding the concurrency semaphore during backoff sleep
        // against a confirmed-dead backend (§21.6).
        if (_circuit_breaker.get_state(ctx->current_backend) == CircuitState::OPEN) {
            log_proxy.info("[{}] Circuit breaker OPEN for backend {}, skipping remaining retries",
                           ctx->request_id, ctx->current_backend);
            ++_retries_skipped_circuit_open;
            metrics().record_retry_skipped_circuit_open();
            ctx->connection_failed = true;
            break;
        }

        // Try fallback to a different backend before retrying
        if (ctx->fallback_enabled && ctx->fallback_attempts < ProxyContext::MAX_FALLBACK_ATTEMPTS) {
            auto fallback = get_fallback_backend(ctx->current_backend);
            if (fallback.has_value()) {
                auto fallback_addr = _router.get_backend_address(fallback.value());
                if (fallback_addr.has_value()) {
                    log_proxy.info("[{}] Falling back from backend {} to {}",
                                  ctx->request_id, ctx->current_backend, fallback.value());
                    // Strip prompt_token_ids if the fallback backend doesn't support them.
                    // The body may contain tokens injected for the original vLLM target;
                    // sending them to a non-vLLM fallback (e.g., Ollama) would cause 400s.
                    if (ctx->endpoint == "/v1/completions" &&
                        !_router.backend_supports_token_ids(fallback.value())) {
                        ctx->forwarded_body = RequestRewriter::strip_prompt_token_ids(ctx->forwarded_body);
                        log_proxy.debug("[{}] Stripped prompt_token_ids for fallback backend {} (supports_token_ids=false)",
                                       ctx->request_id, fallback.value());
                    }
                    ctx->current_backend = fallback.value();
                    ctx->current_addr = fallback_addr.value();
                    ctx->fallback_attempts++;
                    // Don't increment retry_attempt for fallback - this is a different backend
                    continue;
                }
            }
        }

        // No fallback available - check if we should retry same backend
        if (ctx->retry_attempt < ctx->retry_config.max_retries) {
            log_proxy.warn("[{}] Connection attempt {} failed, retrying in {}ms",
                ctx->request_id, ctx->retry_attempt + 1, ctx->current_backoff.count());

            // Wait with exponential backoff before retry
            co_await seastar::sleep(ctx->current_backoff);

            // Increase backoff for next attempt (with cap)
            auto next_backoff = std::chrono::milliseconds(
                static_cast<int64_t>(ctx->current_backoff.count() * ctx->retry_config.backoff_multiplier));
            ctx->current_backoff = std::min(next_backoff, ctx->retry_config.max_backoff);
        } else {
            log_proxy.warn("[{}] Connection failed after {} retries and {} fallbacks",
                ctx->request_id, ctx->retry_config.max_retries + 1, ctx->fallback_attempts);
            ctx->connection_failed = true;
        }

        ctx->retry_attempt++;
    }

    co_return bundle;
}

// Send HTTP request to backend
future<bool> HttpController::send_backend_request(ProxyContext* ctx, ConnectionBundle* bundle) {
    // Check request timeout before sending
    if (lowres_clock::now() >= ctx->request_deadline) {
        log_proxy.warn("[{}] Request timeout before sending", ctx->request_id);
        ctx->timed_out = true;
        bundle->is_valid = false;
        co_return false;
    }

    // Build HTTP request headers with sanitized values to prevent CRLF injection.
    // Host is derived from the target address (not hardcoded to localhost).
    // Headers and body are written separately to avoid doubling memory for large payloads.
    sstring host_value = to_sstring(ctx->current_addr);
    sstring safe_request_id = sstring(sanitize_header_value(ctx->request_id));

    sstring http_headers =
        "POST " + sstring(ctx->endpoint) + " HTTP/1.1\r\n"
        "Host: " + host_value + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + to_sstring(ctx->forwarded_body.size()) + "\r\n"
        "X-Request-ID: " + safe_request_id + "\r\n";

    // Add traceparent header if tracing is enabled (sanitize to prevent injection)
    if (!ctx->backend_traceparent.empty()) {
        http_headers += "traceparent: " + sstring(sanitize_header_value(ctx->backend_traceparent)) + "\r\n";
    }

    // Inject X-Ranvier-Prefix-Hash header for push-based cache eviction.
    // When cache_events.inject_prefix_hash_header is true and the request was
    // routed via prefix mode with available tokens, compute and add the hash.
    // Backends store this opaque value and echo it back on eviction events.
    if (_config.cache_events.enabled && _config.cache_events.inject_prefix_hash_header &&
        _config.is_prefix_mode() && !ctx->tokens.empty()) {
        size_t prefix_len = std::min(ctx->tokens.size(), _config.prefix_token_length);
        uint64_t prefix_hash = hash_prefix(ctx->tokens.data(), prefix_len,
                                            _config.block_alignment);
        char hex[PREFIX_HASH_HEX_BUF_SIZE];
        encode_prefix_hash_hex(prefix_hash, hex);
        http_headers += "X-Ranvier-Prefix-Hash: ";
        http_headers += hex;
        http_headers += "\r\n";
    }

    http_headers += "Connection: keep-alive\r\n\r\n";

    log_proxy.info("[{}] Sending request to backend ({} bytes)",
                  ctx->request_id, ctx->forwarded_body.size());

    // Send headers and body separately to avoid concatenating large payloads
    std::exception_ptr rethrow_exception;
    try {
        co_await bundle->out.write(http_headers);
        co_await bundle->out.write(ctx->forwarded_body);
        co_await bundle->out.flush();
        co_return true;
    } catch (...) {
        auto err_type = classify_connection_error(std::current_exception());
        if (err_type != ConnectionErrorType::NONE) {
            log_proxy.warn("[{}] Backend write failed: {} - closing connection",
                           ctx->request_id, connection_error_to_string(err_type));
            bundle->is_valid = false;
            _circuit_breaker.record_failure(ctx->current_backend);
            metrics().record_connection_error();
            ctx->connection_error = true;
            co_return false;
        }
        // Non-connection error - rethrow
        throw;
    }
}

// Stream response from backend to client
future<> HttpController::stream_backend_response(
    ProxyContext* ctx,
    ConnectionBundle* bundle,
    output_stream<char>* client_out) {

    StreamParser parser;
    std::exception_ptr rethrow_exception;

    while (true) {
        // Check request timeout before each read
        if (lowres_clock::now() >= ctx->request_deadline) {
            log_proxy.warn("[{}] Request timeout after {}s",
                          ctx->request_id, ctx->request_timeout.count());
            ctx->timed_out = true;
            bundle->is_valid = false;
            break;
        }

        // Read with per-chunk timeout (use remaining time, capped at per-chunk limit)
        static constexpr auto kStreamChunkReadTimeout = std::chrono::seconds(30);
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            ctx->request_deadline - lowres_clock::now());
        auto read_timeout = std::min(remaining, kStreamChunkReadTimeout);
        auto read_deadline = lowres_clock::now() + read_timeout;

        bool read_failed = false;
        temporary_buffer<char> chunk;

        try {
            auto read_future = with_timeout(read_deadline, bundle->in.read());
            chunk = co_await std::move(read_future).handle_exception([&](auto ep) {
                // Check for connection errors first
                auto err_type = classify_connection_error(ep);
                if (err_type != ConnectionErrorType::NONE) {
                    log_proxy.warn("[{}] Backend read failed: {}",
                                   ctx->request_id, connection_error_to_string(err_type));
                    ctx->connection_error = true;
                } else {
                    log_proxy.warn("[{}] Read timeout waiting for backend response", ctx->request_id);
                }
                read_failed = true;
                return temporary_buffer<char>{}; // Return empty buffer
            });
        } catch (...) {
            // Catch any exceptions that weren't handled by handle_exception
            auto err_type = classify_connection_error(std::current_exception());
            if (err_type != ConnectionErrorType::NONE) {
                log_proxy.warn("[{}] Backend read failed: {}",
                               ctx->request_id, connection_error_to_string(err_type));
                ctx->connection_error = true;
                read_failed = true;
                bundle->is_valid = false;
            } else {
                // Non-connection error - save for rethrow after cleanup
                rethrow_exception = std::current_exception();
            }
        }

        // Handle non-connection errors outside catch block
        if (rethrow_exception) {
            ctx->stream_closed = true;
            std::rethrow_exception(rethrow_exception);
        }

        if (read_failed) {
            if (ctx->connection_error) {
                _circuit_breaker.record_failure(ctx->current_backend);
            } else {
                ctx->timed_out = true;
                bundle->is_valid = false;
            }
            break;
        }

        // EOF logic
        if (chunk.empty()) {
            log_proxy.info("[{}] Backend response complete (EOF after {} chunks)",
                          ctx->request_id, ctx->chunks_received);
            bundle->is_valid = false;
            break;
        }
        ctx->chunks_received++;
        log_proxy.debug("[{}] Received chunk #{} ({} bytes)",
                       ctx->request_id, ctx->chunks_received, chunk.size());

        auto res = parser.push(std::move(chunk));

        // Check for parsing errors (malformed chunked encoding, size limits, etc.)
        if (res.has_error) {
            log_proxy.warn("[{}] Stream parsing error: {}", ctx->request_id, res.error_message);
            bundle->is_valid = false;
            metrics().record_failure();
            co_await write_client_error(client_out,
                sstring("Backend response parsing error: ") + sstring(res.error_message),
                ctx->client_expects_streaming);
            break;
        }

        // If backend signaled Connection: close (or HTTP/1.0 without keep-alive),
        // mark bundle invalid so it is NOT returned to the connection pool.
        // Without this, the pooled connection is dead and the next request gets an
        // immediate EOF, triggering the stale-connection retry path and doubling latency.
        if (res.connection_close) {
            log_proxy.debug("[{}] Backend sent Connection: close, will not repool",
                            ctx->request_id);
            bundle->is_valid = false;
        }

        // Snooping Logic - record success and learn route
        if (res.header_snoop_success) {
            // Backend responded successfully - record for circuit breaker
            _circuit_breaker.record_success(ctx->current_backend);

            // Record response latency (time to first byte)
            if (!ctx->response_latency_recorded) {
                auto response_time = std::chrono::steady_clock::now();
                auto first_byte_latency = MetricsService::to_seconds(response_time - ctx->connect_start);
                // Record in legacy histogram
                metrics().record_response_latency(first_byte_latency);
                // Record in per-backend histogram for GPU model comparison
                metrics().record_first_byte_latency_by_id(ctx->current_backend, first_byte_latency);
                ctx->response_latency_recorded = true;
                log_proxy.info("[{}] First byte received from backend {} (latency: {:.3f}s)",
                                ctx->request_id, ctx->current_backend, first_byte_latency);
            }

            // Learn route in the ART for future prefix matching
            // Use prefix_boundaries for multi-depth routing, or prefix_boundary for single-depth
            if (_config.should_learn_routes() && ctx->tokens.size() >= _config.min_token_length) {
                // Route learning is best-effort; don't fail the request if it fails
                if (!ctx->prefix_boundaries.empty() && _config.enable_multi_depth_routing) {
                    // Multi-depth routing: store routes at multiple boundaries
                    (void)_router.learn_route_global_multi(ctx->tokens, ctx->current_backend, ctx->request_id, ctx->prefix_boundaries)
                        .handle_exception([request_id = ctx->request_id](auto) {
                            log_proxy.debug("[{}] Multi-depth route learning failed (non-fatal)", request_id);
                        });
                    // Persistence for multi-depth (dedup handled internally per-boundary)
                    if (_persistence) {
                        _persistence->queue_save_route(ctx->tokens, ctx->current_backend);
                    }
                } else {
                    // Single-depth routing: use prefix_boundary or default
                    // Skip persistence if route was deduplicated (§21.7)
                    (void)_router.learn_route_global(ctx->tokens, ctx->current_backend, ctx->request_id, ctx->prefix_boundary)
                        .then([this, tokens = ctx->tokens, backend = ctx->current_backend](bool is_new_route) {
                            if (is_new_route && _persistence) {
                                _persistence->queue_save_route(tokens, backend);
                            }
                        })
                        .handle_exception([request_id = ctx->request_id](auto) {
                            log_proxy.debug("[{}] Route learning failed (non-fatal)", request_id);
                        });
                }
            }
        }

        // Write to client with broken pipe handling.
        // Flush after every write to ensure SSE events reach the client promptly.
        // At typical LLM token rates (30-100 tok/s), tokens arrive as individual
        // TCP segments, so this is effectively one flush per token. When the backend
        // bursts, multiple events land in one TCP segment and StreamParser::push()
        // concatenates them into a single res.data — natural batching.
        //
        // A timer-based flush (e.g., every 10ms) could reduce steady-state syscalls
        // but adds per-request timer complexity for a marginal gain. The simpler
        // per-read flush is correct and safe. Do NOT skip intermediate flushes —
        // small SSE events (10-50 bytes) never fill the 8KB output buffer, causing
        // multi-second delivery stalls (see #280 revert).
        if (!res.data.empty()) {
            try {
                co_await client_out->write(res.data);
                co_await client_out->flush();
                ctx->bytes_written_to_client += res.data.size();
            } catch (...) {
                auto err_type = classify_connection_error(std::current_exception());
                if (err_type != ConnectionErrorType::NONE) {
                    log_proxy.warn("[{}] Client write failed: {} - client disconnected",
                                   ctx->request_id, connection_error_to_string(err_type));
                    bundle->is_valid = false;
                    ctx->client_disconnected = true;
                    break;
                }
                // Non-connection error - save for rethrow after cleanup
                rethrow_exception = std::current_exception();
            }

            if (rethrow_exception) {
                ctx->stream_closed = true;
                std::rethrow_exception(rethrow_exception);
            }
        }

        if (res.done) {
            log_proxy.debug("[{}] Stream complete", ctx->request_id);
            break;
        }
    }
}

// ---------------------------------------------------------
// PROXY HANDLER
// ---------------------------------------------------------
future<std::unique_ptr<seastar::http::reply>> HttpController::handle_proxy(
    std::unique_ptr<seastar::http::request> req,
    std::unique_ptr<seastar::http::reply> rep,
    std::string endpoint) {
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
    request_span.set_attribute("http.url", std::string(endpoint));
    request_span.set_attribute("ranvier.request_id", request_id);

    // Check if we're draining - reject new requests with 503
    if (_draining) {
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

    // Backpressure: early concurrency rejection (non-queue path only).
    // When priority queue is disabled, try_get_units here avoids wasting CPU
    // on tokenization/routing when the server is at capacity.
    // When enabled, defer to acquire_concurrency_slot() after priority extraction.
    std::optional<seastar::semaphore_units<>> early_units;
    if (!_config.backpressure.enable_priority_queue) {
        auto sem_check = seastar::try_get_units(_request_semaphore, 1);
        if (!sem_check) {
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
        early_units = std::move(*sem_check);
    }

    // Backpressure: check persistence queue depth (skipped when persistence is not active)
    if (_persistence_backpressure_active && is_persistence_backpressured()) {
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

    // NOTE: P2C shard selection is computed but cross-shard dispatch is not yet
    // implemented (requires foreign_ptr plumbing per Hard Rule #14). Requests are
    // always processed on the local shard. Track local dispatch for future metrics.
    ++_requests_local_dispatch;

    // RAII guard ensures active request counter is decremented even if exception thrown
    // Guard is released before entering the lambda, which takes over responsibility
    ActiveRequestGuard active_request_guard(metrics());

    // Request body size limit check — before any body processing to prevent memory exhaustion (§4.4)
    if (_config.max_request_body_bytes > 0) {
        size_t body_size = get_request_body_size(*req);
        if (body_size > _config.max_request_body_bytes) {
            ++_requests_rejected_body_size;
            metrics().record_body_size_rejection();
            log_proxy.warn("[{}] Request rejected - body size {} exceeds limit {}",
                           request_id, body_size, _config.max_request_body_bytes);
            rep->set_status(static_cast<seastar::http::reply::status_type>(413));
            rep->add_header("X-Request-ID", request_id);
            rep->write_body("json", "{\"error\": \"Request body too large\", \"max_bytes\": " +
                           std::to_string(_config.max_request_body_bytes) + "}");
            co_return std::move(rep);
        }
    }

    // Zero-copy body access: use string_view for tokenization and parsing,
    // only create string copy when we need to forward/modify the body
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    std::string_view body_view(req->content.data(), req->content.size());
#pragma GCC diagnostic pop
    std::string client_ip = get_client_ip(*req);

    log_proxy.info("[{}] Request received from {} ({} bytes)",
                   request_id, client_ip, body_view.size());

    // 1. Validation
    if (!_tokenizer.local().is_loaded()) {
        log_proxy.warn("[{}] Tokenizer not loaded", request_id);
        metrics().record_failure();
        // active_request_guard destructor will decrement counter
        rep->add_header("X-Request-ID", request_id);
        rep->write_body("json", "{\"error\": \"Tokenizer not loaded\"}");
        co_return std::move(rep);
    }

    // Phase-snapshot timing: capture a single clock read at each phase transition
    // and compute all latency deltas from these snapshots. This reduces per-request
    // steady_clock::now() calls on the hot path (each costs 10-100 CPU cycles
    // depending on clock source). Snapshots: routing_start, post_tokenize,
    // post_route, connect_start, connect_end, first_byte, backend_end.
    auto routing_start = std::chrono::steady_clock::now();

    // Get tokens for routing - either from client or by tokenizing locally
    std::vector<int32_t> tokens;
    bool used_client_tokens = false;
    size_t prefix_boundary = 0;  // Token count of shared prefix (system messages)

    // Text extraction result with boundary metadata — populated during tokenization,
    // reused during boundary detection to avoid redundant JSON re-parsing.
    std::optional<RequestRewriter::TextWithBoundaryInfo> text_extraction;

    // NOTE: tokenization_start collapsed into routing_start (phase-snapshot
    // optimization — only variable declarations between them, ~0 cycles).

    // OPTIMIZATION: Skip tokenization entirely in RANDOM routing mode
    // Random routing ignores tokens completely, so tokenization is wasted work.
    // This saves ~5-6ms per request (Rust FFI + HuggingFace tokenizer overhead).
    bool tokenization_skipped = _config.is_random_mode();
    if (tokenization_skipped) {
        // Skip tokenization - tokens remain empty, router will use random backend selection
        metrics().record_tokenization_skipped();
        log_proxy.debug("[{}] Skipping tokenization (random routing mode)", request_id);
    }

    // Start tokenization span (only do actual work if not in random mode)
    if (!tokenization_skipped) {
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
                    rep->write_body("json", "{\"error\": \"Invalid prompt_token_ids: " + escape_json_string(token_result.error) + "\"}");
                    co_return std::move(rep);
                }
            }
        }

        // If no client tokens, tokenize locally
        if (!used_client_tokens) {
            // Extract text from request body for tokenization (prompt or messages content)
            // This ensures we tokenize exactly what we use for routing, not metadata.
            // Uses extract_text_with_boundary_info() for a single JSON parse that also
            // pre-computes system message boundary metadata, eliminating redundant
            // JSON re-parsing in the boundary detection phase below.
            // Only request formatted_messages when multi-depth routing needs them —
            // skips N per-message string allocations on the common path.
            // The chat template controls message formatting (e.g. llama3, chatml)
            // so that tokenized output aligns with vLLM's apply_chat_template().
            text_extraction = RequestRewriter::extract_text_with_boundary_info(
                body_view, _config.enable_multi_depth_routing, _config.chat_template);
            std::string_view text_to_tokenize = text_extraction.has_value()
                ? std::string_view(text_extraction->text)
                : body_view;

            // Validate input before passing to tokenizer to prevent crashes
            // The tokenizer (Rust FFI) can segfault on malformed input
            // If text was extracted from parsed JSON, UTF-8 is already validated
            // by RapidJSON — skip the redundant O(N) byte-by-byte scan
            bool json_validated = text_extraction.has_value();
            auto validation = TextValidator::validate_for_tokenizer(
                text_to_tokenize, TextValidator::DEFAULT_MAX_LENGTH, json_validated);

            if (!validation.valid) {
                log_proxy.warn("[{}] Input validation failed, falling back to round-robin routing: {}",
                               request_id, validation.error);
                tokenize_span.set_error(std::string("validation_failed: ") + validation.error);
                metrics().record_tokenizer_validation_failure();
                tokens.clear();
            } else {
                try {
                    // Use threaded async tokenization to avoid reactor stalls:
                    //   1. Cache hit: returns immediately (no async overhead)
                    //   2. Thread pool (if enabled): offloads FFI to worker thread
                    //   3. Cross-shard dispatch: P2C to least-loaded shard
                    //   4. Local fallback: blocks reactor (last resort)
                    auto tok_result = co_await _tokenizer.local().encode_threaded_async(text_to_tokenize);
                    tokens = std::move(tok_result.tokens);

                    // Set tracing attributes based on tokenization source
                    if (tok_result.cache_hit) {
                        tokenize_span.set_attribute("ranvier.token_source", "cache");
                    } else if (tok_result.cross_shard) {
                        tokenize_span.set_attribute("ranvier.token_source", "cross_shard");
                        tokenize_span.set_attribute("ranvier.tokenizer_shard",
                                                   static_cast<int64_t>(tok_result.source_shard));
                    } else {
                        tokenize_span.set_attribute("ranvier.token_source", "local");
                    }
                    tokenize_span.set_attribute("ranvier.token_count", static_cast<int64_t>(tokens.size()));

                    // Record cache and cross-shard metrics
                    if (tok_result.cache_hit) {
                        metrics().record_tokenization_cache_hit();
                    } else {
                        metrics().record_tokenization_cache_miss();
                        if (tok_result.cross_shard) {
                            metrics().record_tokenization_cross_shard();
                        }
                    }
                } catch (const std::exception& e) {
                    // Tokenizer failed - log and continue without tokens (fall back to round-robin)
                    log_proxy.warn("[{}] Tokenization failed, falling back to round-robin routing: {}",
                                   request_id, e.what());
                    tokenize_span.set_error(std::string("tokenization_failed: ") + e.what());
                    metrics().record_tokenizer_error();
                    tokens.clear();
                } catch (...) {
                    // Catch any other exceptions (including potential Rust panics via FFI)
                    log_proxy.error("[{}] Tokenization failed with unknown exception, falling back to round-robin routing",
                                    request_id);
                    tokenize_span.set_error("tokenization_failed: unknown exception");
                    metrics().record_tokenizer_error();
                    tokens.clear();
                }
            }
        }
    } // tokenization block ends here (tokenize_span goes out of scope)

    // Phase snapshot: post-primary-tokenize (before boundary detection)
    auto post_primary_tokenize = std::chrono::steady_clock::now();

    // Determine shared prefix boundary for routing
    // Priority: 1) Client-provided prefix_token_count/prefix_boundaries, 2) Automatic detection
    bool prefix_boundary_set = false;
    std::vector<size_t> prefix_boundaries;  // For multi-depth routing (Option C)

    // Option 1a: Check for client-provided prefix_boundaries array (multi-depth)
    if (_config.accept_client_prefix_boundary && _config.enable_multi_depth_routing &&
        !tokens.empty() && !tokenization_skipped) {
        auto client_boundaries = RequestRewriter::extract_prefix_boundaries(body_view);
        if (!client_boundaries.empty()) {
            // Filter valid boundaries (must be < tokens.size())
            for (size_t b : client_boundaries) {
                if (b > 0 && b < tokens.size()) {
                    prefix_boundaries.push_back(b);
                }
            }
            if (!prefix_boundaries.empty()) {
                prefix_boundary = prefix_boundaries.front();  // Use first as primary
                prefix_boundary_set = true;
                metrics().record_prefix_boundary_client();
                log_proxy.debug("[{}] Using client-provided multi-depth boundaries: {} depths",
                                request_id, prefix_boundaries.size());
            }
        }
    }

    // Option 1b: Check for client-provided prefix_token_count (single boundary)
    if (!prefix_boundary_set && _config.accept_client_prefix_boundary &&
        !tokens.empty() && !tokenization_skipped) {
        auto client_prefix = RequestRewriter::extract_prefix_token_count(body_view);
        if (client_prefix.has_value()) {
            size_t count = *client_prefix;
            // Validate: must be positive and less than total tokens
            if (count > 0 && count < tokens.size()) {
                prefix_boundary = count;
                prefix_boundary_set = true;
                metrics().record_prefix_boundary_client();
                log_proxy.debug("[{}] Using client-provided prefix boundary: {} tokens",
                                request_id, prefix_boundary);
            } else {
                log_proxy.debug("[{}] Ignoring invalid client prefix_token_count: {} (tokens={})",
                                request_id, count, tokens.size());
            }
        }
    }

    // Option 2: Automatic message boundary detection
    // Uses pre-computed metadata from text_extraction (populated during tokenization)
    // to avoid redundant JSON re-parsing. Falls back to extract_text_with_boundary_info()
    // if text_extraction was not populated (e.g., client-provided tokens path).
    if (!prefix_boundary_set && _config.enable_prefix_boundary && !tokens.empty() && !tokenization_skipped) {
        // Lazily compute text extraction if not already done (client-tokens path)
        if (!text_extraction.has_value()) {
            text_extraction = RequestRewriter::extract_text_with_boundary_info(
                body_view, _config.enable_multi_depth_routing, _config.chat_template);
        }

        // Build config for boundary detection strategies
        BoundaryDetectionConfig bd_config{
            _config.enable_multi_depth_routing,
            _config.min_prefix_boundary_tokens
        };

        // Strategy 1: Fast marker scan (llama3/chatml — exact boundaries)
        auto boundary_marker = _config.chat_template.message_start_marker();
        if (!boundary_marker.empty() && text_extraction.has_value()) {
            try {
                // Resolve marker token ID (cache hit after first request)
                auto marker_tok = co_await _tokenizer.local().encode_threaded_async(boundary_marker);
                if (marker_tok.tokens.size() == 1) {
                    auto scan_result = detect_boundaries_by_marker_scan(
                        tokens, marker_tok.tokens[0], *text_extraction, bd_config);
                    if (scan_result.detected) {
                        prefix_boundary = scan_result.prefix_boundary;
                        prefix_boundaries = std::move(scan_result.prefix_boundaries);
                        prefix_boundary_set = true;
                        metrics().record_prefix_boundary_used();
                        log_proxy.debug("[{}] Fast boundary scan: {} boundaries, "
                                       "prefix_boundary={} tokens",
                                       request_id, prefix_boundaries.size(),
                                       prefix_boundary);
                    }
                }
            } catch (...) {
                // Marker resolution failed — fall through to next strategy
                log_proxy.debug("[{}] Marker boundary resolution failed: {}",
                               request_id, std::current_exception());
            }
        }

        // Strategy 2: Proportional estimation (none/mistral — approximate)
        if (!prefix_boundary_set && text_extraction.has_value()) {
            auto ratio_result = detect_boundaries_by_char_ratio(
                tokens.size(), *text_extraction, bd_config);
            if (ratio_result.detected) {
                prefix_boundary = ratio_result.prefix_boundary;
                prefix_boundaries = std::move(ratio_result.prefix_boundaries);
                prefix_boundary_set = true;
                metrics().record_prefix_boundary_used();
                log_proxy.debug("[{}] Proportional boundary estimate: {} boundaries, "
                               "prefix_boundary={} tokens",
                               request_id, prefix_boundaries.size(), prefix_boundary);
            }
        }

        // Strategy 3: Per-message tokenization (ultimate fallback when both
        // fast scan and proportional estimation fail or don't apply)

        // For multi-depth routing, calculate boundaries at each message
        // using pre-formatted message strings from the initial JSON parse
        if (!prefix_boundary_set && _config.enable_multi_depth_routing &&
            text_extraction.has_value() && !text_extraction->formatted_messages.empty()) {
            size_t cumulative_tokens = 0;
            bool found_non_system = false;
            size_t system_boundary = 0;

            for (const auto& msg : text_extraction->formatted_messages) {
                size_t msg_token_count = 0;
                try {
                    // Use async tokenization to avoid blocking reactor on cache miss.
                    // Cache hits return immediately (make_ready_future); misses offload
                    // to thread pool or cross-shard, keeping the reactor free.
                    auto msg_tok_result = co_await _tokenizer.local().encode_threaded_async(msg.text);
                    if (msg_tok_result.cache_hit) {
                        metrics().record_tokenization_cache_hit();
                    } else {
                        metrics().record_tokenization_cache_miss();
                    }
                    msg_token_count = msg_tok_result.tokens.size();
                } catch (...) {
                    // Individual message tokenization failed — skip this message
                    log_proxy.debug("[{}] Per-message tokenization failed: {}",
                                   request_id, std::current_exception());
                    continue;
                }

                // Track system boundary: cumulative count BEFORE first non-system message
                if (!found_non_system && !msg.is_system) {
                    system_boundary = cumulative_tokens;
                    found_non_system = true;
                }

                cumulative_tokens += msg_token_count;

                // Store boundary after each message
                if (cumulative_tokens > 0 && cumulative_tokens < tokens.size()) {
                    prefix_boundaries.push_back(cumulative_tokens);
                }
            }

            // If all messages were system messages, system_boundary is at the end
            if (!found_non_system && cumulative_tokens > 0) {
                system_boundary = cumulative_tokens;
            }

            if (!prefix_boundaries.empty()) {
                prefix_boundary = system_boundary;
                if (prefix_boundary == 0 && !prefix_boundaries.empty()) {
                    prefix_boundary = prefix_boundaries.front();
                }
                prefix_boundary_set = true;
                metrics().record_prefix_boundary_used();
                log_proxy.debug("[{}] Identified {} message boundaries for multi-depth routing",
                                request_id, prefix_boundaries.size());
            }
        }

        // Fallback: single boundary from system messages
        // Uses pre-extracted system message metadata instead of re-parsing JSON
        if (!prefix_boundary_set && text_extraction.has_value() &&
            text_extraction->has_system_messages) {
            try {
                // Build the system prefix text for tokenization.
                // When system messages are contiguous at the start, we use the
                // prefix substring of the already-extracted combined text directly
                // (this includes chat-template formatting if configured).
                // Otherwise, use the pre-extracted raw system text as an approximation.
                std::string system_prefix_text;
                if (text_extraction->has_system_prefix) {
                    // Contiguous system messages: use prefix substring directly
                    system_prefix_text = text_extraction->text.substr(
                        0, text_extraction->system_prefix_end);
                } else {
                    // Non-contiguous (interleaved) system messages: use pre-extracted text
                    system_prefix_text = text_extraction->system_text;
                }

                // Use async tokenization to avoid blocking reactor on cache miss.
                // System messages have ~90%+ cache hit rate so this is usually instant.
                auto sys_tok_result = co_await _tokenizer.local().encode_threaded_async(system_prefix_text);
                if (sys_tok_result.cache_hit) {
                    metrics().record_tokenization_cache_hit();
                } else {
                    metrics().record_tokenization_cache_miss();
                }
                const auto& system_tokens = sys_tok_result.tokens;
                // Only use as boundary if system tokens meet minimum threshold
                // and are shorter than full tokens (otherwise it's not a prefix)
                if (system_tokens.size() >= _config.min_prefix_boundary_tokens &&
                    system_tokens.size() < tokens.size()) {
                    prefix_boundary = system_tokens.size();
                    prefix_boundary_set = true;
                    metrics().record_prefix_boundary_used();
                    log_proxy.debug("[{}] Identified shared prefix boundary: {} tokens (system messages)",
                                    request_id, prefix_boundary);
                } else {
                    metrics().record_prefix_boundary_skipped();
                }
            } catch (...) {
                // System message tokenization failed - not fatal, just skip prefix boundary
                metrics().record_prefix_boundary_skipped();
                log_proxy.debug("[{}] System message tokenization failed, using default prefix",
                                request_id);
            }
        } else if (!prefix_boundary_set) {
            // No system messages found or extraction failed
            metrics().record_prefix_boundary_skipped();
        }
    }

    // Phase snapshot: post-tokenize (also serves as ART lookup baseline)
    auto post_tokenize = std::chrono::steady_clock::now();
    metrics().record_tokenization_latency(
        MetricsService::to_seconds(post_tokenize - routing_start));
    // Split metrics: primary tokenization vs boundary detection overhead.
    // If P50≈P99 for tokenization, boundary detection is likely the hidden cost.
    metrics().record_primary_tokenization_latency(
        MetricsService::to_seconds(post_primary_tokenize - routing_start));
    metrics().record_boundary_detection_latency(
        MetricsService::to_seconds(post_tokenize - post_primary_tokenize));

    // Prepare forwarded body based on endpoint:
    // - /v1/completions: vLLM supports prompt_token_ids, forward them for efficiency
    // - /v1/chat/completions: vLLM ignores prompt_token_ids, strip them to avoid warnings
    std::string forwarded_body;
    bool is_completions_endpoint = (endpoint == "/v1/completions");

    if (is_completions_endpoint) {
        // /v1/completions supports prompt_token_ids - forward them for vLLM efficiency
        if (used_client_tokens) {
            // Client provided tokens - keep them in body as-is
            forwarded_body = std::string(body_view);
            log_proxy.debug("[{}] Forwarding client tokens to /v1/completions", request_id);
        } else if (_config.enable_token_forwarding && !tokens.empty()) {
            // Server tokenized - add tokens to body for vLLM
            auto rewrite_result = RequestRewriter::rewrite(body_view, tokens);
            if (rewrite_result.success) {
                forwarded_body = std::move(rewrite_result.body);
                log_proxy.debug("[{}] Added {} token IDs to /v1/completions request",
                               request_id, tokens.size());
            } else {
                forwarded_body = std::string(body_view);
                log_proxy.debug("[{}] Token rewrite failed: {}", request_id, rewrite_result.error);
            }
        } else {
            forwarded_body = std::string(body_view);
        }
    } else {
        // /v1/chat/completions ignores prompt_token_ids - strip to avoid vLLM warnings
        if (used_client_tokens) {
            forwarded_body = RequestRewriter::strip_prompt_token_ids(body_view);
            log_proxy.debug("[{}] Stripped client tokens from /v1/chat/completions request", request_id);
        } else {
            // No tokens to strip - just copy body
            forwarded_body = std::string(body_view);
        }
    }

    // Cost estimation: compute before routing so cost-based routing can use it.
    // estimate_request_cost() only needs text length and max_tokens — both available now.
    auto cost = _config.cost_estimation_enabled
        ? estimate_request_cost(
              text_extraction.has_value() ? text_extraction->text.size() : body_view.size(),
              text_extraction.has_value() ? text_extraction->max_tokens : 0)
        : CostEstimate{};

    if (_config.cost_estimation_enabled && cost.cost_units > 0.0) {
        log_proxy.debug("[{}] Cost estimation (pre-route): input={}, output={}, cost_units={:.1f}",
                       request_id, cost.input_tokens, cost.output_tokens, cost.cost_units);
    }

    BackendId target_id;
    std::string routing_mode_str;

    // Start route lookup span
    {
        auto route_span = TracingService::start_child_span("ranvier.route_lookup");

        // Unified routing decision - all mode logic is encapsulated in RouterService
        // Pass prefix_boundary for consistent hash fallback across cluster nodes
        // Pass estimated_cost for cost-based routing
        // NOTE: art_lookup_start collapsed into post_tokenize (phase-snapshot
        // optimization — body prep between them is ~100ns, well within first
        // histogram bucket of 100μs).
        auto route_result = _router.route_request(tokens, request_id, prefix_boundary,
                                                     cost.cost_units);
        routing_mode_str = route_result.routing_mode;
        route_span.set_attribute("ranvier.routing_mode", route_result.routing_mode);
        route_span.set_attribute("ranvier.cache_hit", route_result.cache_hit);

        if (!route_result.backend_id.has_value()) {
            log_proxy.warn("[{}] {}", request_id, route_result.error_message);
            route_span.set_error(route_result.error_message);
            metrics().record_failure();
            rep->add_header("X-Request-ID", request_id);
            rep->write_body("json", "{\"error\": \"" + escape_json_string(route_result.error_message) + "!\"}");
            co_return std::move(rep);
        }

        target_id = route_result.backend_id.value();
        if (route_result.was_load_redirect) {
            log_proxy.debug("[{}] Load redirect: original backend overloaded (gpu_load={:.2f}), "
                            "redirected to backend {}",
                            request_id, route_result.backend_load_at_decision, target_id);
        }
        if (route_result.was_cost_redirect) {
            log_proxy.debug("[{}] Cost redirect: backend cost_budget={:.1f}, fast_lane={} -> backend {}",
                            request_id, route_result.backend_cost_at_decision,
                            route_result.was_fast_lane, target_id);
        }
        log_proxy.debug("[{}] Route decision: {} mode, cache_hit={} -> backend {}",
                        request_id, route_result.routing_mode, route_result.cache_hit, target_id);

        route_span.set_attribute("ranvier.backend_id", static_cast<int64_t>(target_id));
    } // route_span ends here

    // Speculative load increment: create BackendRequestGuard immediately after routing
    // so that concurrent routing decisions (e.g., thread pool burst completions) see
    // updated load counters. Without this, requests completing tokenization at the same
    // time all see load=0 and pile onto the same backend (transient hot-spot).
    // RAII ensures cleanup on any early-return error path below.
    BackendRequestGuard backend_guard(target_id);

    // Cost budget guard: RAII ensures symmetric reserve/release on ALL exit paths.
    // Created on the routed backend; destructor releases on any error/exception/success.
    CostBudgetGuard cost_guard(target_id, cost.cost_units);

    // Phase snapshot: post-route (replaces separate art_lookup_end and routing_end)
    auto post_route = std::chrono::steady_clock::now();
    metrics().record_art_lookup_latency(
        MetricsService::to_seconds(post_route - post_tokenize));
    metrics().record_routing_latency(
        MetricsService::to_seconds(post_route - routing_start));

    // Circuit breaker check - try fallback if circuit is open
    if (!_circuit_breaker.allow_request(target_id)) {
        log_proxy.info("[{}] Circuit open for backend {}, trying fallback", request_id, target_id);
        metrics().record_circuit_open();
        auto fallback = get_fallback_backend(target_id);
        if (fallback.has_value()) {
            target_id = fallback.value();
            // Move-assign new guards: release old backend's counters, acquire new one's
            backend_guard = BackendRequestGuard(target_id);
            cost_guard = CostBudgetGuard(target_id, cost.cost_units);
            metrics().record_fallback();
            log_proxy.info("[{}] Using fallback backend {}", request_id, target_id);
        } else {
            log_proxy.warn("[{}] All backends unavailable (circuit breaker open)", request_id);
            metrics().record_failure();
            // backend_guard + active_request_guard destructors handle cleanup
            rep->add_header("X-Request-ID", request_id);
            rep->write_body("json", "{\"error\": \"All backends unavailable (circuit breaker open)\"}");
            co_return std::move(rep);
        }
    }

    auto target_addr_opt = _router.get_backend_address(target_id);
    if (!target_addr_opt.has_value()) {
        log_proxy.warn("[{}] Backend {} IP not found", request_id, target_id);
        metrics().record_failure();
        // backend_guard + active_request_guard destructors handle cleanup
        rep->add_header("X-Request-ID", request_id);
        rep->write_body("json", "{\"error\": \"Backend IP not found\"}");
        co_return std::move(rep);
    }
    socket_address target_addr = target_addr_opt.value();
    log_proxy.info("[{}] Routing to backend {} at {}", request_id, target_id, target_addr);

    // Strip prompt_token_ids if the target backend doesn't support them.
    // Token injection (above) runs before routing since tokens are needed for prefix lookup.
    // Now that we know the target backend, strip tokens for non-vLLM backends to avoid
    // 400 rejections from backends that don't recognize prompt_token_ids (e.g., Ollama).
    if (is_completions_endpoint && !_router.backend_supports_token_ids(target_id)) {
        forwarded_body = RequestRewriter::strip_prompt_token_ids(forwarded_body);
        log_proxy.debug("[{}] Stripped prompt_token_ids for backend {} (supports_token_ids=false)",
                       request_id, target_id);
    }

    // 2. Setup Streaming with Timeout, Retry, and Circuit Breaker
    // Capture config for the lambda
    auto connect_timeout = _config.connect_timeout;
    auto request_timeout = _config.request_timeout;
    auto retry_config = _config.retry;
    auto fallback_enabled = _config.circuit_breaker.fallback_enabled;

    // Add debugging headers to response before streaming
    // X-Request-ID: correlation ID for distributed tracing
    // X-Backend-ID: which backend was selected
    // X-Routing-Mode: actual routing mode used (helps catch config mismatches)
    rep->add_header("X-Request-ID", request_id);
    rep->add_header("X-Backend-ID", std::to_string(target_id));
    rep->add_header("X-Routing-Mode", routing_mode_str);

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

    // Cost estimation already computed before routing (see cost variable above).

    // Priority tier extraction: classify request before routing
    PriorityLevel priority = PriorityLevel::NORMAL;
    if (_config.priority_tier_enabled) {
        priority = extract_priority(*req, cost.cost_units, body_view);
        log_proxy.debug("[{}] Priority tier: {}", request_id, priority_level_to_string(priority));
        // Per-priority metrics: count and track active
        auto tier = static_cast<uint8_t>(priority);
        metrics().record_priority_request(tier);
        metrics().increment_priority_active(tier);
    }

    // Intent classification: classify request before routing
    RequestIntent intent = RequestIntent::CHAT;
    if (_config.intent_classification_enabled) {
        intent = classify_intent(endpoint, body_view, _config.intent_classifier);
        log_proxy.debug("[{}] intent={} priority={} cost_units={:.1f}",
                       request_id, intent_to_string(intent),
                       priority_level_to_string(priority), cost.cost_units);
        metrics().record_intent_request(static_cast<uint8_t>(intent));
    }

    // Initialize ProxyContext with all state needed for streaming
    // This struct bundles request state to reduce lambda capture complexity
    // Using unique_ptr: ownership transfers from handle_proxy to the lambda (no sharing)
    auto ctx = std::make_unique<ProxyContext>();
    ctx->request_id = request_id;
    ctx->backend_traceparent = backend_traceparent;
    ctx->routing_mode = routing_mode_str;
    ctx->endpoint = endpoint;
    ctx->forwarded_body = std::move(forwarded_body);
    ctx->tokens = std::move(tokens);
    ctx->prefix_boundary = prefix_boundary;
    ctx->prefix_boundaries = std::move(prefix_boundaries);
    ctx->target_id = target_id;
    ctx->target_addr = target_addr;
    ctx->current_backend = target_id;
    ctx->current_addr = target_addr;
    ctx->request_start = request_start;
    ctx->connect_timeout = connect_timeout;
    ctx->request_timeout = request_timeout;
    ctx->retry_config = retry_config;
    ctx->fallback_enabled = fallback_enabled;
    ctx->shard_metrics_active = shard_metrics_active;
    ctx->request_deadline = lowres_clock::now() + request_timeout;
    ctx->estimated_input_tokens = cost.input_tokens;
    ctx->estimated_output_tokens = cost.output_tokens;
    ctx->estimated_cost_units = cost.cost_units;
    ctx->priority = priority;
    ctx->intent = intent;

    // Detect whether client expects streaming (SSE) or non-streaming (JSON) response.
    // OpenAI-compatible APIs use "stream":true/false in the request body.
    // If "stream":false is explicitly present, the client expects a JSON response.
    // Otherwise default to streaming (SSE) for backwards compatibility.
    {
        bool has_stream_false =
            body_view.find("\"stream\":false") != std::string_view::npos ||
            body_view.find("\"stream\": false") != std::string_view::npos;
        ctx->client_expects_streaming = !has_stream_false;
    }

    // Capture User-Agent for fair scheduling (used by RequestScheduler)
    {
        auto ua_it = req->_headers.find("User-Agent");
        if (ua_it != req->_headers.end()) {
            ctx->user_agent = ua_it->second;
        }
    }

    // Agent identification (paused agents enter the queue instead of being rejected)
    // Runs after extract_priority() — additive identification and metrics.
    // TODO: Agent config priority override is deferred; extract_priority() remains unchanged.
    if (_agent_registry) {
        auto agent_id = _agent_registry->identify_agent(*req);
        if (agent_id) {
            _agent_registry->record_request(*agent_id);
            ctx->agent_id = *agent_id;
            // NOTE: paused agents are no longer rejected here.
            // They enter the queue and are skipped during dequeue.
            // When resumed, queued requests drain naturally.
            log_proxy.debug("[{}] Agent identified: {}", request_id, *agent_id);
        }
    }

    // Acquire concurrency slot (direct semaphore or priority queue path)
    auto slot = co_await acquire_concurrency_slot(
        priority, request_id, ctx->user_agent, ctx->agent_id,
        request_start, std::move(early_units));
    if (slot.rejected) {
        rep->set_status(seastar::http::reply::status_type::service_unavailable);
        rep->add_header("X-Request-ID", request_id);
        rep->add_header("Retry-After", std::to_string(_config.backpressure.retry_after_seconds));
        rep->write_body("json", "{\"error\": \"" + slot.rejection_reason + "\"}");
        if (_config.priority_tier_enabled) {
            metrics().decrement_priority_active(static_cast<uint8_t>(priority));
        }
        co_return std::move(rep);
    }

    // Move gate_holder, semaphore_units, backend_guard, and cost_guard into the lambda to keep them alive during streaming.
    // This ensures the gate stays held, the semaphore slot is occupied, the backend load tracking
    // is properly maintained, and cost budget is released when streaming completes (any exit path).
    // BackendRequestGuard destructor decrements active_requests; CostBudgetGuard destructor releases cost budget.
    // Use SSE content type for streaming requests, JSON for non-streaming (e.g. Ollama non-stream)
    auto response_content_type = ctx->client_expects_streaming ? "text/event-stream" : "json";
    rep->write_body(response_content_type, [this, ctx = std::move(ctx), gate_holder = std::move(gate_holder), semaphore_units = std::move(*slot.units), backend_guard = std::move(backend_guard), cost_guard = std::move(cost_guard)](output_stream<char> client_out) mutable -> future<> {

        // Phase 1: Establish backend connection with retry and fallback
        ConnectionBundle bundle = co_await establish_backend_connection(ctx.get());

        if (ctx->connection_failed) {
            log_proxy.error("[{}] Backend connection failed after retries", ctx->request_id);
            metrics().record_failure();
            metrics().decrement_active_requests();
            metrics().decrement_priority_active(static_cast<uint8_t>(ctx->priority));
            if (ctx->shard_metrics_active && shard_load_metrics_initialized()) {
                shard_load_metrics().decrement_active();
            }
            co_await write_client_error(&client_out, "Backend connection failed after retries", ctx->client_expects_streaming);
            co_await client_out.close();
            co_return;
        }

        // Phase 2: Send HTTP request to backend
        bool send_success = co_await send_backend_request(ctx.get(), &bundle);

        if (!send_success) {
            // Handle timeout or connection error during send
            if (ctx->timed_out) {
                metrics().record_timeout();
            } else {
                metrics().record_failure();
            }
            metrics().decrement_active_requests();
            metrics().decrement_priority_active(static_cast<uint8_t>(ctx->priority));
            if (ctx->shard_metrics_active && shard_load_metrics_initialized()) {
                shard_load_metrics().decrement_active();
            }
            co_await bundle.close();
            co_await write_client_error(&client_out,
                ctx->timed_out ? "Request timed out" : "Backend connection lost during request",
                ctx->client_expects_streaming);
            co_await client_out.close();
            co_return;
        }

        // Phase 3: Stream response from backend to client
        std::exception_ptr streaming_exception;
        try {
            co_await stream_backend_response(ctx.get(), &bundle, &client_out);
        } catch (...) {
            // Capture exception - co_await not allowed in catch blocks.
            // Rule #9 note: This exception is rethrown after cleanup below, ensuring
            // proper logging at the call site. We capture here to allow async cleanup.
            streaming_exception = std::current_exception();
        }

        // Handle streaming exception outside catch block (co_await constraint)
        if (streaming_exception) {
            metrics().record_failure();
            metrics().decrement_active_requests();
            metrics().decrement_priority_active(static_cast<uint8_t>(ctx->priority));
            if (ctx->shard_metrics_active && shard_load_metrics_initialized()) {
                shard_load_metrics().decrement_active();
            }
            // Rule #9: Log cleanup failures at debug level (not warn) because:
            // 1. We're already in an error path - the real failure will be logged when rethrown
            // 2. Cleanup failures are expected when streams are already broken
            // 3. Warn-level would create noisy duplicate warnings for single failure events
            try {
                co_await bundle.close();
            } catch (const std::exception& e) {
                log_proxy.debug("[{}] Cleanup: backend connection close failed: {}",
                               ctx->request_id, e.what());
            } catch (...) {
                log_proxy.debug("[{}] Cleanup: backend connection close failed: unknown error",
                               ctx->request_id);
            }
            try {
                co_await client_out.close();
            } catch (const std::exception& e) {
                log_proxy.debug("[{}] Cleanup: client output close failed: {}",
                               ctx->request_id, e.what());
            } catch (...) {
                log_proxy.debug("[{}] Cleanup: client output close failed: unknown error",
                               ctx->request_id);
            }
            std::rethrow_exception(streaming_exception);
        }

        // Phase 3.5: Stale connection retry
        // Detect empty backend response: no error flags set, zero bytes written.
        // This indicates a stale/dead pooled connection. Retry on a fresh connection.
        {
            auto decision = should_retry_stale_connection(
                ctx->timed_out, ctx->connection_error, ctx->client_disconnected,
                ctx->connection_failed, ctx->bytes_written_to_client,
                ctx->stale_retry_attempt, _config.max_stale_retries);

            if (decision.should_retry) {
                ctx->stale_retry_attempt++;
                metrics().record_stale_connection_retry();

                log_proxy.warn("[{}] Empty backend response detected (0 bytes, {} chunks) "
                               "- retrying on fresh connection (attempt {}/{})",
                               ctx->request_id, ctx->chunks_received,
                               ctx->stale_retry_attempt, _config.max_stale_retries);

                // Close the stale connection (do not return to pool)
                bundle.is_valid = false;
                try {
                    co_await bundle.close();
                } catch (...) {
                    log_proxy.trace("[{}] Error closing stale backend connection", ctx->request_id);
                }

                _circuit_breaker.record_failure(ctx->current_backend);

                // Get a fresh connection (bypass pool cache)
                bool retry_connected = false;
                auto retry_connect_deadline = lowres_clock::now() + ctx->connect_timeout;
                try {
                    auto retry_future = with_timeout(retry_connect_deadline,
                        _pool.get_fresh(ctx->current_addr, ctx->request_id));
                    bundle = co_await std::move(retry_future);
                    retry_connected = true;
                } catch (...) {
                    log_proxy.warn("[{}] Fresh connection failed for stale retry: {}",
                                   ctx->request_id, std::current_exception());
                    ctx->connection_failed = true;
                }

                if (retry_connected) {
                    // Reset streaming state for retry
                    ctx->response_latency_recorded = false;
                    ctx->connection_error = false;
                    ctx->timed_out = false;
                    ctx->stream_closed = false;
                    ctx->chunks_received = 0;
                    ctx->connect_start = std::chrono::steady_clock::now();

                    // Re-send the HTTP request on fresh connection
                    bool retry_send_ok = co_await send_backend_request(ctx.get(), &bundle);

                    if (retry_send_ok) {
                        // Re-stream the response
                        std::exception_ptr retry_exception;
                        try {
                            co_await stream_backend_response(ctx.get(), &bundle, &client_out);
                        } catch (...) {
                            retry_exception = std::current_exception();
                        }

                        if (retry_exception) {
                            metrics().record_failure();
                            metrics().decrement_active_requests();
                            metrics().decrement_priority_active(static_cast<uint8_t>(ctx->priority));
                            if (ctx->shard_metrics_active && shard_load_metrics_initialized()) {
                                shard_load_metrics().decrement_active();
                            }
                            try { co_await bundle.close(); } catch (...) {
                                log_proxy.debug("[{}] Bundle close failed during retry cleanup",
                                               ctx->request_id);
                            }
                            try { co_await client_out.close(); } catch (...) {
                                log_proxy.debug("[{}] Client output close failed during retry cleanup",
                                               ctx->request_id);
                            }
                            std::rethrow_exception(retry_exception);
                        }
                        // Fall through to Phase 4 with updated ctx state
                    } else {
                        // Send failed on fresh connection
                        if (!ctx->timed_out) {
                            ctx->connection_error = true;
                        }
                        // Fall through to Phase 4 which handles the error flags
                    }
                }
            }
        }

        // Phase 4: Handle completion status and send appropriate client response
        if (ctx->client_disconnected) {
            log_proxy.info("[{}] Client disconnected mid-stream", ctx->request_id);
        } else if (ctx->timed_out) {
            log_proxy.warn("[{}] Request timed out", ctx->request_id);
            _circuit_breaker.record_failure(ctx->current_backend);
            metrics().record_timeout();
            co_await write_client_error(&client_out, "Request timed out", ctx->client_expects_streaming);
        } else if (ctx->connection_error) {
            log_proxy.warn("[{}] Backend connection error occurred", ctx->request_id);
            metrics().record_connection_error();
            co_await write_client_error(&client_out, "Backend connection lost", ctx->client_expects_streaming);
        } else if (ctx->connection_failed) {
            log_proxy.warn("[{}] Backend connection failed during stale retry", ctx->request_id);
            metrics().record_failure();
            co_await write_client_error(&client_out, "Backend connection failed", ctx->client_expects_streaming);
        } else {
            log_proxy.info("[{}] Request completed successfully", ctx->request_id);
            metrics().record_success();
        }

        // Phase 5: Record metrics and cleanup
        auto backend_end = std::chrono::steady_clock::now();
        record_proxy_completion_metrics(*ctx, backend_end);

        // Cost budget release is handled by CostBudgetGuard destructor (RAII).

        // Return connection to pool or close it
        if (ctx->connection_failed || ctx->connection_error || ctx->timed_out || !bundle.is_valid) {
            bundle.is_valid = false;
            try {
                co_await bundle.close();
            } catch (...) {
                log_proxy.trace("[{}] Error closing backend connection", ctx->request_id);
            }
        } else {
            try {
                _pool.put(std::move(bundle), ctx->request_id);
            } catch (...) {
                log_proxy.trace("[{}] Error returning connection to pool", ctx->request_id);
            }
        }

        // Close client output stream
        if (!ctx->stream_closed) {
            try {
                co_await client_out.close();
            } catch (...) {
                log_proxy.trace("[{}] Error closing client output stream", ctx->request_id);
            }
        }

        // Decrement active request counters
        metrics().decrement_active_requests();
        metrics().decrement_priority_active(static_cast<uint8_t>(ctx->priority));
        if (ctx->shard_metrics_active && shard_load_metrics_initialized()) {
            shard_load_metrics().decrement_active();
            shard_load_metrics().record_request_completed();
        }
    });

    co_return std::move(rep);
}

// ---------------------------------------------------------
// CONTROL PLANE HANDLERS
// ---------------------------------------------------------

future<std::unique_ptr<seastar::http::reply>> HttpController::handle_broadcast_route(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep) {
    // Rule 22: coroutine converts any pre-future throw into a failed future
    sstring id_str = req->get_query_param("backend_id");
    if (id_str.empty()) {
        log_control.warn("POST /admin/routes: missing backend_id parameter");
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Missing backend_id\"}");
        co_return std::move(rep);
    }

    auto backend_id_opt = parse_backend_id(std::string_view(id_str));
    if (!backend_id_opt) {
        log_control.warn("POST /admin/routes: invalid backend_id '{}'", id_str);
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Invalid backend_id: must be a valid integer\"}");
        co_return std::move(rep);
    }
    int backend_id = *backend_id_opt;

    std::vector<int32_t> tokens;
    // Use string_view for zero-copy tokenization
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    std::string_view content_view(req->content.data(), req->content.size());
#pragma GCC diagnostic pop

    // Validate input before passing to tokenizer
    auto validation = TextValidator::validate_for_tokenizer(
        content_view, TextValidator::DEFAULT_MAX_LENGTH);
    if (!validation.valid) {
        log_control.warn("POST /admin/routes: input validation failed for backend {}: {}", backend_id, validation.error);
        metrics().record_tokenizer_validation_failure();
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Input validation failed: " + escape_json_string(validation.error) + "\"}");
        co_return std::move(rep);
    }

    try {
        tokens = _tokenizer.local().encode(content_view);
    } catch (const std::exception& e) {
        log_control.warn("POST /admin/routes: failed to tokenize content for backend {}: {}", backend_id, e.what());
        metrics().record_tokenizer_error();
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Failed to tokenize request content\"}");
        co_return std::move(rep);
    }

    // Check if route will actually be stored (must meet block_alignment minimum)
    size_t token_count = tokens.size();
    size_t aligned_len = (token_count / _config.block_alignment) * _config.block_alignment;

    // If too short, return success with warning (be lenient like the proxy handler)
    if (aligned_len == 0) {
        log_control.info("POST /admin/routes: content too short ({} tokens, need {} for block_alignment), route not stored",
                         token_count, _config.block_alignment);
        std::ostringstream oss;
        oss << "{\"status\": \"ok\", \"backend_id\": " << backend_id
            << ", \"tokens\": " << token_count
            << ", \"stored_tokens\": 0"
            << ", \"warning\": \"Content too short (" << token_count << " tokens), need at least "
            << _config.block_alignment << " for block_alignment. Route not stored.\"}";
        rep->write_body("json", oss.str());
        co_return std::move(rep);
    }

    // Queue route for async persistence (fire-and-forget, non-blocking)
    if (_persistence) {
        _persistence->queue_save_route(tokens, backend_id);
    }

    co_await _router.learn_route_global(tokens, backend_id);

    std::ostringstream oss;
    oss << "{\"status\": \"ok\", \"backend_id\": " << backend_id
        << ", \"tokens\": " << token_count
        << ", \"stored_tokens\": " << aligned_len << "}";
    rep->write_body("json", oss.str());
    co_return std::move(rep);
}

future<std::unique_ptr<seastar::http::reply>> HttpController::handle_broadcast_backend(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep) {
    // Usage: POST /admin/backends?id=1&ip=192.168.4.51&port=11434&weight=100&priority=0&supports_token_ids=true
    // Also supports hostnames: POST /admin/backends?id=1&ip=host.docker.internal&port=11434
    sstring id_str = req->get_query_param("id");
    sstring ip_str = req->get_query_param("ip");
    sstring port_str = req->get_query_param("port");
    sstring weight_str = req->get_query_param("weight");
    sstring priority_str = req->get_query_param("priority");
    sstring supports_token_ids_str = req->get_query_param("supports_token_ids");
    sstring compression_ratio_str = req->get_query_param("compression_ratio");

    // Check for required parameters
    if (id_str.empty() || port_str.empty() || ip_str.empty()) {
        log_control.warn("POST /admin/backends: missing required parameter (id={}, ip={}, port={})",
            id_str.empty() ? "<missing>" : id_str,
            ip_str.empty() ? "<missing>" : ip_str,
            port_str.empty() ? "<missing>" : port_str);
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Missing id, ip, or port\"}");
        co_return std::move(rep);
    }

    auto id_opt = parse_int32(std::string_view(id_str));
    auto port_opt = parse_port(std::string_view(port_str));

    if (!id_opt || !port_opt) {
        log_control.warn("POST /admin/backends: invalid parameter (id={}, port={})",
            id_str, port_str);
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Invalid parameter: id must be integer, port must be 1-65535\"}");
        co_return std::move(rep);
    }

    int id = *id_opt;
    uint16_t port = *port_opt;
    uint32_t weight = 100;
    uint32_t priority = 0;

    if (!weight_str.empty()) {
        auto weight_opt = parse_uint32(std::string_view(weight_str));
        if (!weight_opt) {
            log_control.warn("POST /admin/backends: invalid weight '{}'", weight_str);
            rep->set_status(seastar::http::reply::status_type::bad_request);
            rep->write_body("json", "{\"error\": \"Invalid weight: must be a non-negative integer\"}");
            co_return std::move(rep);
        }
        weight = *weight_opt;
    }

    if (!priority_str.empty()) {
        auto priority_opt = parse_uint32(std::string_view(priority_str));
        if (!priority_opt) {
            log_control.warn("POST /admin/backends: invalid priority '{}'", priority_str);
            rep->set_status(seastar::http::reply::status_type::bad_request);
            rep->write_body("json", "{\"error\": \"Invalid priority: must be a non-negative integer\"}");
            co_return std::move(rep);
        }
        priority = *priority_opt;
    }

    // supports_token_ids: whether this backend supports vLLM's prompt_token_ids field.
    // Default true for backward compatibility. Set to false for non-vLLM backends
    // (e.g., Ollama) to prevent injecting unrecognized fields.
    bool supports_token_ids = true;
    if (!supports_token_ids_str.empty()) {
        auto supports_opt = parse_bool(std::string_view(supports_token_ids_str));
        if (!supports_opt) {
            log_control.warn("POST /admin/backends: invalid supports_token_ids '{}'", supports_token_ids_str);
            rep->set_status(seastar::http::reply::status_type::bad_request);
            rep->write_body("json", "{\"error\": \"Invalid supports_token_ids: must be true/false, 1/0, or yes/no\"}");
            co_return std::move(rep);
        }
        supports_token_ids = *supports_opt;
    }

    // compression_ratio: KV-cache compression ratio (>= 1.0).
    // Falls back to fleet-wide default_compression_ratio from config when not specified.
    double compression_ratio = _config.default_compression_ratio;
    if (!compression_ratio_str.empty()) {
        auto cr_opt = parse_double(std::string_view(compression_ratio_str));
        if (!cr_opt || *cr_opt < 1.0) {
            log_control.warn("POST /admin/backends: invalid compression_ratio '{}'", compression_ratio_str);
            rep->set_status(seastar::http::reply::status_type::bad_request);
            rep->write_body("json", "{\"error\": \"Invalid compression_ratio: must be a number >= 1.0\"}");
            co_return std::move(rep);
        }
        compression_ratio = *cr_opt;
    }

    // Resolve address: supports both direct IP addresses and hostnames
    socket_address addr;
    std::string resolved_ip;
    bool needs_dns_resolution = false;

    // Fast path: Try parsing as direct IP address (most common case)
    try {
        auto ip_string = std::string(ip_str);
        seastar::net::inet_address parsed_addr{ip_string};
        addr = seastar::socket_address(parsed_addr, port);
        resolved_ip = ip_string;
    } catch (const std::exception& e) {
        // Not a valid IP address - this is expected flow for hostnames, not an error.
        // Rule #9 note: Debug level is appropriate since this triggers DNS resolution below.
        log_control.debug("POST /admin/backends: '{}' is not a valid IP ({}), will try DNS",
                         ip_str, e.what());
        needs_dns_resolution = true;
    }

    // Slow path: DNS resolution for hostname (co_await not allowed in catch blocks)
    if (needs_dns_resolution) {
        log_control.debug("POST /admin/backends: '{}' is not a direct IP, attempting DNS resolution", ip_str);

        try {
            auto deadline = seastar::lowres_clock::now()
                + std::chrono::seconds(_config.dns_resolution_timeout_seconds);
            auto hostent = co_await seastar::with_timeout(
                deadline,
                seastar::net::dns::get_host_by_name(std::string(ip_str))
            );

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
            if (hostent.addr_list.empty()) {
                log_control.warn("POST /admin/backends: DNS resolution returned no addresses for '{}'", ip_str);
                rep->set_status(seastar::http::reply::status_type::bad_request);
                rep->write_body("json", "{\"error\": \"DNS resolution returned no addresses for hostname\"}");
                co_return std::move(rep);
            }

            addr = seastar::socket_address(hostent.addr_list[0], port);
            // Convert resolved address back to string for logging/persistence
            std::ostringstream oss;
            oss << hostent.addr_list[0];
            resolved_ip = oss.str();
#pragma GCC diagnostic pop

            log_control.info("POST /admin/backends: resolved hostname '{}' to IP '{}'", ip_str, resolved_ip);

        } catch (const seastar::timed_out_error&) {
            log_control.warn("POST /admin/backends: DNS resolution timed out for '{}' ({}s limit)",
                            ip_str, _config.dns_resolution_timeout_seconds);
            rep->set_status(seastar::http::reply::status_type::bad_request);
            rep->write_body("json", "{\"error\": \"DNS resolution timed out for hostname\"}");
            co_return std::move(rep);
        } catch (const std::exception& e) {
            log_control.warn("POST /admin/backends: failed to resolve '{}': {}", ip_str, e.what());
            rep->set_status(seastar::http::reply::status_type::bad_request);
            rep->write_body("json", "{\"error\": \"Invalid IP address or hostname could not be resolved\"}");
            co_return std::move(rep);
        }
    }

    // Queue backend for async persistence (fire-and-forget, non-blocking)
    // Store the resolved IP, not the hostname, for persistence
    if (_persistence) {
        _persistence->queue_save_backend(id, resolved_ip, port, weight, priority);
    }

    co_await _router.register_backend_global(id, addr, weight, priority, supports_token_ids, compression_ratio);

    // Notify HealthService of compression ratio for compression-aware load scoring.
    // HealthService state lives on shard 0 — must submit_to(0) to avoid cross-shard
    // write (Rule #14). Captures scalars by value (no heap ownership).
    if (_health_service) {
        co_await seastar::smp::submit_to(0, [hs = _health_service, id, compression_ratio] {
            hs->set_backend_compression_ratio(id, compression_ratio);
        });
    }

    log_control.info("Registered Backend {} -> {}:{} (weight={}, priority={}, supports_token_ids={}, compression_ratio={})",
        id, ip_str, port, weight, priority, supports_token_ids, compression_ratio);
    rep->write_body("json", "{\"status\": \"ok\", \"weight\": " + std::to_string(weight) +
        ", \"priority\": " + std::to_string(priority) +
        ", \"supports_token_ids\": " + (supports_token_ids ? "true" : "false") +
        ", \"compression_ratio\": " + std::to_string(compression_ratio) + "}");
    co_return std::move(rep);
}

// ---------------------------------------------------------
// ADMIN DELETE HANDLERS
// ---------------------------------------------------------

future<std::unique_ptr<seastar::http::reply>> HttpController::handle_delete_backend(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep) {
    // Usage: DELETE /admin/backends?id=1
    sstring id_str = req->get_query_param("id");

    if (id_str.empty()) {
        log_control.warn("DELETE /admin/backends: missing id parameter");
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Missing id parameter\"}");
        return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
    }

    auto id_opt = parse_int32(std::string_view(id_str));
    if (!id_opt) {
        log_control.warn("DELETE /admin/backends: invalid id '{}'", id_str);
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Invalid id: must be a valid integer\"}");
        return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
    }
    int id = *id_opt;

    // Queue removal for async persistence (fire-and-forget, non-blocking)
    if (_persistence) {
        _persistence->queue_remove_routes_for_backend(id);
        _persistence->queue_remove_backend(id);
    }

    // Remove from in-memory state across all shards
    return _router.unregister_backend_global(id).then([id, rep = std::move(rep)]() mutable {
        log_control.info("Deleted Backend {} and its persisted routes", id);
        rep->write_body("json", "{\"status\": \"ok\", \"backend_deleted\": " + std::to_string(id) + "}");
        return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
    });
}

future<std::unique_ptr<seastar::http::reply>> HttpController::handle_delete_routes(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep) {
    // Usage: DELETE /admin/routes?backend_id=1
    sstring id_str = req->get_query_param("backend_id");

    if (id_str.empty()) {
        log_control.warn("DELETE /admin/routes: missing backend_id parameter");
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Missing backend_id parameter\"}");
        return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
    }

    auto backend_id_opt = parse_backend_id(std::string_view(id_str));
    if (!backend_id_opt) {
        log_control.warn("DELETE /admin/routes: invalid backend_id '{}'", id_str);
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Invalid backend_id: must be a valid integer\"}");
        return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
    }
    int backend_id = *backend_id_opt;

    // Queue routes removal for async persistence (fire-and-forget, non-blocking)
    if (_persistence) {
        _persistence->queue_remove_routes_for_backend(backend_id);
    }

    // Note: In-memory routes in the RadixTree are not removed immediately.
    // They will be gone after restart. For full removal, we'd need to
    // traverse the entire tree which is expensive.
    log_control.info("Deleted persisted routes for Backend {} (in-memory routes cleared on restart)", backend_id);
    rep->write_body("json", "{\"status\": \"ok\", \"routes_deleted_for_backend\": " + std::to_string(backend_id) + ", \"note\": \"In-memory routes will be cleared on restart\"}");
    return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
}

future<std::unique_ptr<seastar::http::reply>> HttpController::handle_clear_all(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep) {
    // Usage: POST /admin/clear
    // WARNING: This is destructive!

    // Queue clear-all for async persistence (fire-and-forget, non-blocking)
    if (_persistence) {
        _persistence->queue_clear_all();
    }

    log_control.warn("Queued clear of all persisted data (backends and routes). Restart required to clear in-memory state.");
    rep->write_body("json", "{\"status\": \"ok\", \"warning\": \"All persisted data queued for clearing. Restart to clear in-memory state.\"}");
    return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
}

// ---------------------------------------------------------
// API KEY MANAGEMENT
// ---------------------------------------------------------

future<std::unique_ptr<seastar::http::reply>> HttpController::handle_keys_reload(std::unique_ptr<seastar::http::request> req, std::unique_ptr<seastar::http::reply> rep) {
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
        return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
    }

    // Trigger the reload (sends SIGHUP to self for async processing)
    bool triggered = _config_reload_callback();

    if (!triggered) {
        log_control.error("Failed to trigger config reload");
        rep->set_status(seastar::http::reply::status_type::internal_server_error);
        rep->write_body("json", "{\"error\": \"Failed to trigger config reload\"}");
        return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
    }

    log_control.info("Config reload triggered via /admin/keys/reload endpoint");

    // Build response with current key metadata (names only, not values)
    // Uses escape_json_string() from parse_utils.hpp for JSON safety
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
    return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
}

// ---------------------------------------------------------
// GRACEFUL SHUTDOWN
// ---------------------------------------------------------

future<> HttpController::stop() {
    // Guard against double-stop: wait_for_drain() also calls _rate_limiter.stop()
    // and _request_gate.close(). The rate limiter's internal gate cannot be closed
    // twice, so we track whether stop has already been called.
    if (_stopped) {
        co_return;
    }
    _stopped = true;

    // Hard Rule #5: Stop timers before destruction to prevent use-after-free.
    // Hard Rule #6: Metrics lambdas capturing `this` are deregistered when
    // the rate limiter's internal timer is cancelled.
    co_await _rate_limiter.stop();

    // Stop priority queue background fiber (Rule #5: close gate, break CV)
    _queue_cv.broken();
    if (!_queue_gate.is_closed()) {
        co_await _queue_gate.close();
    }

    // Deregister scheduler metrics before member destruction (Rule #6)
    _scheduler_metrics.clear();

    // Deregister agent registry metrics before member destruction (Rule #6)
    _agent_metrics.clear();

    // Deregister cache event metrics before member destruction (Rule #6)
    _cache_event_metrics.clear();

    // Close gate if not already closed by wait_for_drain()
    if (!_request_gate.is_closed()) {
        co_await _request_gate.close();
    }

    log_proxy.debug("HttpController::stop() completed on shard {}", seastar::this_shard_id());
}

void HttpController::start_draining() {
    _draining = true;
    log_proxy.info("Draining started - rejecting new requests");
}

future<> HttpController::wait_for_drain() {
    auto drain_timeout = _config.drain_timeout;
    log_proxy.info("Waiting for in-flight requests to complete (timeout: {}s)", drain_timeout.count());

    // Mark as stopped so HttpController::stop() is a no-op after drain
    _stopped = true;

    // Stop priority queue background fiber first (Rule #5: close gate before destruction).
    // Must happen before _request_gate.close() because the dequeue loop acquires
    // semaphore units which would keep _request_gate holders alive.
    _queue_cv.broken();
    if (!_queue_gate.is_closed()) {
        co_await _queue_gate.close();
    }

    // Deregister scheduler/agent/cache event metrics before member destruction (Rule #6)
    _scheduler_metrics.clear();
    _agent_metrics.clear();
    _cache_event_metrics.clear();

    // Stop rate limiter cleanup timer (Hard Rule #5: close gate before destruction)
    // This ensures no timer callbacks are in-flight when we proceed with shutdown
    co_await _rate_limiter.stop();

    // Close the gate and wait for all holders to be released
    auto gate_close_future = _request_gate.close();

    // Race between gate closing and timeout
    auto deadline = lowres_clock::now() + drain_timeout;

    try {
        co_await with_timeout(deadline, std::move(gate_close_future));
        log_proxy.info("All in-flight requests completed");
    } catch (const seastar::timed_out_error&) {
        log_proxy.warn("Drain timeout reached - some requests may be interrupted");
    }
}

bool HttpController::is_draining() const {
    return _draining;
}

bool HttpController::is_persistence_backpressured() const {
    if (!_config.backpressure.enable_persistence_backpressure || !_persistence) {
        return false;
    }

    // Check if persistence queue is above threshold.
    // queue_depth() reads an atomic shadow counter — zero lock, safe on the reactor.
    size_t max_depth = _persistence->max_queue_depth();
    if (max_depth == 0) {
        return false;  // Avoid division by zero; 0 means unlimited
    }
    size_t current_depth = _persistence->queue_depth();
    double fill_ratio = static_cast<double>(current_depth) / static_cast<double>(max_depth);

    return fill_ratio >= _config.backpressure.persistence_queue_threshold;
}

size_t HttpController::get_request_body_size(const seastar::http::request& req) {
    auto content_length_it = req._headers.find("Content-Length");
    if (content_length_it != req._headers.end()) {
        // Content-Length header present — parse with from_chars (Rule #10)
        const auto& cl_str = content_length_it->second;
        uint64_t cl_value = 0;
        auto [ptr, ec] = std::from_chars(cl_str.data(), cl_str.data() + cl_str.size(), cl_value);
        if (ec == std::errc{} && ptr == cl_str.data() + cl_str.size()) {
            return static_cast<size_t>(cl_value);
        }
    }
    // Chunked/no Content-Length: use actual received content size
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    return req.content.size();
#pragma GCC diagnostic pop
}

uint32_t HttpController::select_target_shard() {
    // NOTE: This method computes the optimal target shard using the P2C algorithm,
    // but actual cross-shard dispatch is NOT yet implemented. The caller always
    // processes on the local shard. This method exists to maintain the load balancer
    // snapshot cache and will be used when smp::submit_to dispatch is added.
    uint32_t local_shard = seastar::this_shard_id();

    // Fast path: load balancing disabled or single shard
    if (!_lb_config.enabled || !_load_balancer || seastar::smp::count <= 1) {
        return local_shard;
    }

    // Update local shard's metrics snapshot in the load balancer cache
    if (shard_load_metrics_initialized()) {
        _load_balancer->local().update_local_snapshot();
    }

    // Compute target shard (result is advisory until dispatch is implemented)
    return _load_balancer->local().select_shard();
}

// ---------------------------------------------------------
// STATE INSPECTION HANDLERS (for rvctl CLI)
// ---------------------------------------------------------

// Default maximum recursion depth for tree dump serialization.
// Prevents stack overflow and multi-MB responses for large trees.
static constexpr int DEFAULT_MAX_DUMP_DEPTH = 32;

// Helper to serialize a DumpNode to JSON with bounded recursion depth.
// When max_depth is reached, children are replaced with a count summary.
static std::string dump_node_to_json(const RadixTree::DumpNode& node, int indent_level = 0, int remaining_depth = DEFAULT_MAX_DUMP_DEPTH) {
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

    // Children array (bounded by remaining_depth)
    ss << inner_indent << "\"children\": [";
    if (!node.children.empty()) {
        if (remaining_depth <= 0) {
            // Depth limit reached — emit count instead of recursing
            ss << "],\n";
            ss << inner_indent << "\"children_truncated\": " << node.children.size() << "\n";
            ss << indent << "}";
            return ss.str();
        }
        ss << "\n";
        for (size_t i = 0; i < node.children.size(); ++i) {
            if (i > 0) ss << ",\n";
            ss << inner_indent << "  {\"edge\": " << node.children[i].first << ", \"node\": ";
            ss << dump_node_to_json(node.children[i].second, indent_level + 2, remaining_depth - 1);
            ss << "}";
        }
        ss << "\n" << inner_indent;
    }
    ss << "]\n";

    ss << indent << "}";
    return ss.str();
}

future<std::unique_ptr<seastar::http::reply>> HttpController::handle_dump_tree(
    std::unique_ptr<seastar::http::request> req,
    std::unique_ptr<seastar::http::reply> rep) {

    // Parse optional max_depth parameter to bound recursion (default: 32)
    int max_depth = DEFAULT_MAX_DUMP_DEPTH;
    sstring depth_str = req->get_query_param("max_depth");
    if (!depth_str.empty()) {
        auto depth_opt = parse_uint32(std::string_view(depth_str));
        static constexpr uint32_t kMaxAllowedDumpDepth = 256;
        if (depth_opt && *depth_opt > 0 && *depth_opt <= kMaxAllowedDumpDepth) {
            max_depth = static_cast<int>(*depth_opt);
        }
    }

    // Check for optional prefix filter (comma-separated token IDs)
    sstring prefix_str = req->get_query_param("prefix");

    std::vector<TokenId> prefix_filter;
    if (!prefix_str.empty()) {
        // Parse comma-separated token IDs
        std::string prefix_string{prefix_str};
        std::istringstream iss{prefix_string};
        std::string token_str;
        while (std::getline(iss, token_str, ',')) {
            auto token_opt = parse_token_id(std::string_view(token_str));
            if (!token_opt) {
                log_control.warn("GET /admin/dump/tree: invalid prefix token '{}'", token_str);
                rep->set_status(seastar::http::reply::status_type::bad_request);
                rep->write_body("json", "{\"error\": \"Invalid prefix token ID\"}");
                return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
            }
            prefix_filter.push_back(*token_opt);
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
        oss << "  \"max_depth\": " << max_depth << ",\n";
        oss << "  \"tree\": " << dump_node_to_json(dump, 1, max_depth) << "\n";
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
            oss << "  \"max_depth\": " << max_depth << ",\n";
            oss << "  \"tree\": " << dump_node_to_json(dump.value(), 1, max_depth) << "\n";
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
    return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
}

future<std::unique_ptr<seastar::http::reply>> HttpController::handle_dump_cluster(
    std::unique_ptr<seastar::http::request> req,
    std::unique_ptr<seastar::http::reply> rep) {

    // GossipService is only on shard 0
    auto* gossip = _router.gossip_service();
    if (!gossip) {
        std::string response = "{\n"
            "  \"error\": \"Cluster mode not enabled\",\n"
            "  \"cluster_enabled\": false\n"
            "}";
        rep->write_body("json", response);
        return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
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
    return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
}

future<std::unique_ptr<seastar::http::reply>> HttpController::handle_dump_backends(
    std::unique_ptr<seastar::http::request> req,
    std::unique_ptr<seastar::http::reply> rep) {

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
        oss << "      \"is_dead\": " << (b.is_dead ? "true" : "false") << ",\n";
        oss << "      \"supports_token_ids\": " << (b.supports_token_ids ? "true" : "false") << ",\n";
        oss << "      \"compression_ratio\": " << b.compression_ratio;
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
    return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
}

future<std::unique_ptr<seastar::http::reply>> HttpController::handle_backend_metrics(
    std::unique_ptr<seastar::http::request> req,
    std::unique_ptr<seastar::http::reply> rep) {

    auto backends = _router.get_all_backend_states();

    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"backends\": [\n";

    for (size_t i = 0; i < backends.size(); ++i) {
        const auto& b = backends[i];

        // Get vLLM metrics from HealthService (if available)
        VLLMMetrics vllm;
        double load_score = 0.0;
        if (_health_service) {
            vllm = _health_service->get_vllm_metrics(b.id);
            load_score = _health_service->get_backend_load(b.id);
        }

        // Compute composite load for this backend
        uint64_t local_active = get_backend_load(b.id);
        uint64_t composite_load = get_composite_backend_load(b.id);

        oss << "    {\n";
        oss << "      \"id\": " << b.id << ",\n";
        oss << "      \"address\": \"" << b.address << ":" << b.port << "\",\n";
        oss << "      \"load_score\": " << load_score << ",\n";
        oss << "      \"composite_load\": " << composite_load << ",\n";
        oss << "      \"local_active_requests\": " << local_active << ",\n";
        // Cost budget tracking
        double cost_budget = get_backend_cost(b.id);
        double cost_max = _config.cost_routing_max_cost;
        double cost_headroom = cost_max > 0.0
            ? ((cost_max - cost_budget) / cost_max) * 100.0
            : 100.0;
        oss << "      \"cost_budget\": " << cost_budget << ",\n";
        oss << "      \"cost_budget_max\": " << cost_max << ",\n";
        oss << "      \"cost_headroom_percent\": " << cost_headroom << ",\n";

        oss << "      \"vllm_metrics\": {\n";
        oss << "        \"valid\": " << (vllm.valid ? "true" : "false");

        if (vllm.valid) {
            auto now = std::chrono::steady_clock::now();
            double scraped_ago = std::chrono::duration<double>(now - vllm.scraped_at).count();
            double gpu_used_gb = vllm.gpu_memory_used_bytes / (1024.0 * 1024.0 * 1024.0);
            double gpu_total_gb = vllm.gpu_memory_total_bytes / (1024.0 * 1024.0 * 1024.0);

            oss << ",\n";
            oss << "        \"num_requests_running\": " << vllm.num_requests_running << ",\n";
            oss << "        \"num_requests_waiting\": " << vllm.num_requests_waiting << ",\n";
            oss << "        \"gpu_cache_usage_percent\": " << vllm.gpu_cache_usage_percent << ",\n";
            oss << "        \"gpu_memory_used_gb\": " << gpu_used_gb << ",\n";
            oss << "        \"gpu_memory_total_gb\": " << gpu_total_gb << ",\n";
            oss << "        \"avg_prompt_throughput\": " << vllm.avg_prompt_throughput << ",\n";
            oss << "        \"avg_generation_throughput\": " << vllm.avg_generation_throughput << ",\n";
            oss << "        \"scraped_seconds_ago\": " << scraped_ago << "\n";
        } else {
            oss << "\n";
        }

        oss << "      }\n";
        oss << "    }";
        if (i < backends.size() - 1) oss << ",";
        oss << "\n";
    }

    oss << "  ]\n";
    oss << "}";

    rep->write_body("json", oss.str());
    return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
}

future<std::unique_ptr<seastar::http::reply>> HttpController::handle_dump_config(
    std::unique_ptr<seastar::http::request> req,
    std::unique_ptr<seastar::http::reply> rep) {

    const auto& lm = _config.local_mode;

    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"local_mode\": {\n";
    oss << "    \"enabled\": " << (lm.enabled ? "true" : "false") << ",\n";
    oss << "    \"clustering_disabled\": " << (lm.disable_clustering ? "true" : "false") << ",\n";
    oss << "    \"persistence_disabled\": " << (lm.disable_persistence ? "true" : "false") << ",\n";
    oss << "    \"auto_discover_backends\": " << (lm.auto_discover_backends ? "true" : "false") << ",\n";
    oss << "    \"discovery_ports\": [";
    for (size_t i = 0; i < lm.discovery_ports.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << lm.discovery_ports[i];
    }
    oss << "]\n";
    oss << "  }\n";
    oss << "}";

    rep->write_body("json", oss.str());
    return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
}

future<std::unique_ptr<seastar::http::reply>> HttpController::handle_drain_backend(
    std::unique_ptr<seastar::http::request> req,
    std::unique_ptr<seastar::http::reply> rep) {

    // Usage: POST /admin/drain?backend_id=1
    sstring id_str = req->get_query_param("backend_id");

    if (id_str.empty()) {
        log_control.warn("POST /admin/drain: missing backend_id parameter");
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Missing backend_id parameter\"}");
        return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
    }

    auto backend_id_opt = parse_backend_id(std::string_view(id_str));
    if (!backend_id_opt) {
        log_control.warn("POST /admin/drain: invalid backend_id '{}'", id_str);
        rep->set_status(seastar::http::reply::status_type::bad_request);
        rep->write_body("json", "{\"error\": \"Invalid backend_id: must be a valid integer\"}");
        return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
    }
    int backend_id = *backend_id_opt;

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
        return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
    });
}

// ---------------------------------------------------------
// HEALTH CHECK HANDLER (public, no auth)
// ---------------------------------------------------------

future<std::unique_ptr<seastar::http::reply>> HttpController::handle_health(
    std::unique_ptr<seastar::http::request> req,
    std::unique_ptr<seastar::http::reply> rep) {

    // Check draining state first (atomic read - lock-free per Anti-Pattern #1)
    if (_draining) {
        rep->set_status(seastar::http::reply::status_type::service_unavailable);
        rep->write_body("json", "{\"status\": \"draining\"}");
        return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
    }

    // Determine quorum status
    // Default to true (has quorum) when cluster mode is not enabled
    bool has_quorum = true;

    auto* gossip = _router.gossip_service();
    if (gossip && gossip->is_enabled()) {
        auto state = gossip->get_cluster_state();
        // ACTIVE state means we have quorum; QUORUM_LOSS means we don't
        has_quorum = (state.quorum_state == "ACTIVE");
    }

    // Return healthy status with quorum info
    // Both quorum=true and quorum=false return 200 OK (still serving requests)
    std::string response = has_quorum
        ? "{\"status\": \"healthy\", \"quorum\": true}"
        : "{\"status\": \"healthy\", \"quorum\": false}";

    rep->write_body("json", response);
    return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
}

// ---------------------------------------------------------
// PRIORITY QUEUE BACKGROUND DEQUEUE LOOP
// ---------------------------------------------------------

// Background fiber that dequeues requests by priority and acquires
// semaphore units on their behalf. Gate-guarded for clean shutdown (Rule #5).
// Inserts maybe_yield() every 64 iterations (Rule #17).
future<> HttpController::process_priority_queue() {
    seastar::gate::holder holder;
    try {
        holder = _queue_gate.hold();
    } catch (const seastar::gate_closed_exception&) {
        co_return;
    }

    uint64_t iterations = 0;
    while (!_queue_gate.is_closed()) {
        auto ctx_opt = _scheduler.dequeue();
        if (!ctx_opt) {
            try {
                co_await _queue_cv.wait();
            } catch (const seastar::broken_condition_variable&) {
                // Shutdown signal — exit cleanly
                break;
            }
            continue;
        }

        auto ctx = std::move(*ctx_opt);

        // Acquire a semaphore unit for this request (may suspend)
        try {
            auto units = co_await seastar::get_units(_request_semaphore, 1);
            // Fulfill the promise so handle_proxy can continue
            ctx->dequeue_promise.set_value(std::move(units));
        } catch (...) {
            // Rule #9: log at warn level
            log_proxy.warn("[{}] Failed to acquire semaphore for dequeued request",
                           ctx->request_id);
            ctx->dequeue_promise.set_exception(std::current_exception());
        }

        if (++iterations % kYieldInterval == 0) {
            co_await seastar::coroutine::maybe_yield();
        }
    }

    // Drain remaining queued contexts (including paused agents): destroy stubs
    // so their promises break, waking any handle_proxy coroutine waiting in
    // acquire_concurrency_slot.  Uses drain_one() instead of dequeue() because
    // dequeue() skips paused agents, which would leak stubs on shutdown.
    while (auto leftover = _scheduler.drain_one()) {
        // Stub is destroyed here → promise breaks → waiter gets broken_promise
        (void)leftover;
    }
}

// Dispatch is not used in the promise-based model (handle_proxy owns the
// response lifecycle). Stub retained for interface completeness.
future<> HttpController::dispatch_proxied_request(
    std::unique_ptr<ProxyContext> /*ctx*/,
    seastar::semaphore_units<> /*units*/) {
    co_return;
}

// ---------------------------------------------------------
// CONCURRENCY SLOT ACQUISITION
// ---------------------------------------------------------

// Unified helper: acquire a semaphore unit either directly or via the
// priority queue scheduler. Keeps handle_proxy() focused on request logic.
future<HttpController::AcquireResult> HttpController::acquire_concurrency_slot(
    PriorityLevel priority,
    std::string request_id,
    std::string user_agent,
    std::string agent_id,
    std::chrono::steady_clock::time_point request_start,
    std::optional<seastar::semaphore_units<>> early_units) {

    // Direct path: units were already acquired early (before tokenization)
    if (early_units) {
        co_return AcquireResult{std::move(*early_units), false, {}};
    }

    // Priority queue path: enqueue a scheduling stub and wait for dequeue.
    auto stub = std::make_unique<ProxyContext>();
    stub->priority = priority;
    stub->request_id = request_id;
    stub->user_agent = user_agent;
    stub->agent_id = agent_id;

    auto dequeue_future = stub->dequeue_promise.get_future();

    if (!_scheduler.enqueue(std::move(stub))) {
        metrics().record_backpressure_rejection();
        log_proxy.warn("[{}] Request rejected - priority queue full (tier={})",
                       request_id, priority_level_to_string(priority));
        co_return AcquireResult{std::nullopt, true,
            "Server overloaded - priority queue full"};
    }
    _queue_cv.signal();

    // Wait for process_priority_queue() to dequeue us and provide a semaphore unit.
    // If shutdown occurs while waiting, the promise is broken — treat as rejection.
    seastar::semaphore_units<> units;
    try {
        units = co_await std::move(dequeue_future);
    } catch (const seastar::broken_promise&) {
        log_proxy.info("[{}] Request rejected - scheduler shutting down", request_id);
        co_return AcquireResult{std::nullopt, true, "Server shutting down"};
    }

    // Record scheduler wait time
    auto wait_duration = std::chrono::steady_clock::now() - request_start;
    metrics().record_scheduler_wait(
        std::chrono::duration<double>(wait_duration).count());

    co_return AcquireResult{std::move(units), false, {}};
}

// ---------------------------------------------------------
// SCHEDULER STATS HANDLER (admin, auth required)
// ---------------------------------------------------------

// =============================================================================
// Agent Registry Admin Handlers
// =============================================================================
// Note: these endpoints are shard-local. Each shard tracks its own agent
// counters independently. Cross-shard aggregation is deferred to a future session.

// Helper: format steady_clock time_point as ISO 8601 UTC string.
// Returns "never" if the time_point is at epoch (agent never seen).
static std::string format_agent_time(std::chrono::steady_clock::time_point tp) {
    if (tp.time_since_epoch().count() == 0) {
        return "never";
    }
    // Convert steady_clock to system_clock for wall-clock formatting
    auto sys_now = std::chrono::system_clock::now();
    auto steady_now = std::chrono::steady_clock::now();
    auto sys_tp = sys_now + std::chrono::duration_cast<std::chrono::system_clock::duration>(tp - steady_now);
    auto time_t_val = std::chrono::system_clock::to_time_t(sys_tp);
    struct tm tm_buf;
    gmtime_r(&time_t_val, &tm_buf);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return std::string(buf);
}

// GET /admin/agents — list all known agents
future<std::unique_ptr<seastar::http::reply>> HttpController::handle_list_agents(
    std::unique_ptr<seastar::http::request> /*req*/,
    std::unique_ptr<seastar::http::reply> rep) {

    if (!_agent_registry) {
        rep->write_body("json", R"({"error":"agent_registry_disabled"})");
        co_return std::move(rep);
    }

    auto agents = _agent_registry->list_agents();
    std::ostringstream oss;
    oss << R"({"agents":[)";
    for (size_t i = 0; i < agents.size(); ++i) {
        const auto& a = agents[i];
        if (i > 0) oss << ",";
        oss << "{"
            << R"("agent_id":")" << a.agent_id << "\""
            << R"(,"display_name":")" << a.display_name << "\""
            << R"(,"pattern":")" << a.pattern << "\""
            << R"(,"default_priority":")" << priority_level_to_string(a.default_priority) << "\""
            << R"(,"allow_pause":)" << (a.allow_pause ? "true" : "false")
            << R"(,"paused":)" << (a.paused ? "true" : "false")
            << R"(,"requests_total":)" << a.requests_total
            << R"(,"requests_paused_rejected":)" << a.requests_paused_rejected
            << R"(,"first_seen":")" << format_agent_time(a.first_seen) << "\""
            << R"(,"last_seen":")" << format_agent_time(a.last_seen) << "\""
            << "}";
    }
    oss << R"(],"total_agents":)" << _agent_registry->agent_count()
        << R"(,"overflow_drops":)" << _agent_registry->overflow_drops()
        << "}";

    rep->write_body("json", oss.str());
    co_return std::move(rep);
}

// GET /admin/agents/stats?agent_id=<id> — single agent detail
future<std::unique_ptr<seastar::http::reply>> HttpController::handle_agent_stats(
    std::unique_ptr<seastar::http::request> req,
    std::unique_ptr<seastar::http::reply> rep) {

    if (!_agent_registry) {
        rep->write_body("json", R"({"error":"agent_registry_disabled"})");
        co_return std::move(rep);
    }

    auto agent_id_param = req->get_query_param("agent_id");
    if (agent_id_param.empty()) {
        rep->set_status(seastar::http::reply::status_type{400});
        rep->write_body("json", R"({"error":"missing agent_id query parameter"})");
        co_return std::move(rep);
    }

    auto info = _agent_registry->get_agent(agent_id_param);
    if (!info) {
        rep->set_status(seastar::http::reply::status_type{404});
        rep->write_body("json", R"({"error":"agent_not_found"})");
        co_return std::move(rep);
    }

    // Include per-agent queue depth from the scheduler
    auto agent_depths = _scheduler.queue_depths_by_agent();
    size_t queued = 0;
    auto depth_it = agent_depths.find(info->agent_id);
    if (depth_it != agent_depths.end()) {
        queued = depth_it->second;
    }

    std::ostringstream oss;
    oss << "{"
        << R"("agent_id":")" << info->agent_id << "\""
        << R"(,"display_name":")" << info->display_name << "\""
        << R"(,"paused":)" << (info->paused ? "true" : "false")
        << R"(,"queued_requests":)" << queued
        << R"(,"requests_total":)" << info->requests_total
        << R"(,"requests_paused_rejected":)" << info->requests_paused_rejected
        << R"(,"first_seen":")" << format_agent_time(info->first_seen) << "\""
        << R"(,"last_seen":")" << format_agent_time(info->last_seen) << "\""
        << "}";

    rep->write_body("json", oss.str());
    co_return std::move(rep);
}

// POST /admin/agents/pause?agent_id=<id> — pause an agent
future<std::unique_ptr<seastar::http::reply>> HttpController::handle_pause_agent(
    std::unique_ptr<seastar::http::request> req,
    std::unique_ptr<seastar::http::reply> rep) {

    if (!_agent_registry) {
        rep->write_body("json", R"({"error":"agent_registry_disabled"})");
        co_return std::move(rep);
    }

    auto agent_id_param = req->get_query_param("agent_id");
    if (agent_id_param.empty()) {
        rep->set_status(seastar::http::reply::status_type{400});
        rep->write_body("json", R"({"error":"missing agent_id query parameter"})");
        co_return std::move(rep);
    }

    auto info = _agent_registry->get_agent(agent_id_param);
    if (!info) {
        rep->set_status(seastar::http::reply::status_type{404});
        rep->write_body("json", R"({"error":"agent_not_found"})");
        co_return std::move(rep);
    }

    if (!info->allow_pause) {
        rep->set_status(seastar::http::reply::status_type{409});
        rep->write_body("json", R"({"error":"agent_pause_not_allowed"})");
        co_return std::move(rep);
    }

    if (info->paused) {
        rep->set_status(seastar::http::reply::status_type{409});
        rep->write_body("json", R"({"error":"agent_already_paused"})");
        co_return std::move(rep);
    }

    _agent_registry->pause_agent(agent_id_param);
    rep->write_body("json", R"({"status":"paused"})");
    co_return std::move(rep);
}

// POST /admin/agents/resume?agent_id=<id> — resume a paused agent
future<std::unique_ptr<seastar::http::reply>> HttpController::handle_resume_agent(
    std::unique_ptr<seastar::http::request> req,
    std::unique_ptr<seastar::http::reply> rep) {

    if (!_agent_registry) {
        rep->write_body("json", R"({"error":"agent_registry_disabled"})");
        co_return std::move(rep);
    }

    auto agent_id_param = req->get_query_param("agent_id");
    if (agent_id_param.empty()) {
        rep->set_status(seastar::http::reply::status_type{400});
        rep->write_body("json", R"({"error":"missing agent_id query parameter"})");
        co_return std::move(rep);
    }

    auto info = _agent_registry->get_agent(agent_id_param);
    if (!info) {
        rep->set_status(seastar::http::reply::status_type{404});
        rep->write_body("json", R"({"error":"agent_not_found"})");
        co_return std::move(rep);
    }

    if (!info->paused) {
        rep->set_status(seastar::http::reply::status_type{409});
        rep->write_body("json", R"({"error":"agent_not_paused"})");
        co_return std::move(rep);
    }

    _agent_registry->resume_agent(agent_id_param);

    // Wake the dequeue loop so previously skipped requests
    // for this agent are processed promptly.
    _queue_cv.signal();

    rep->write_body("json", R"({"status":"resumed"})");
    co_return std::move(rep);
}

future<std::unique_ptr<seastar::http::reply>> HttpController::handle_scheduler_stats(
    std::unique_ptr<seastar::http::request> /*req*/,
    std::unique_ptr<seastar::http::reply> rep) {

    auto depths = _scheduler.queue_depths();
    auto drops = _scheduler.overflow_drops();
    auto agent_depths = _scheduler.queue_depths_by_agent();

    std::ostringstream oss;
    oss << "{"
        << "\"enabled\": " << (_config.backpressure.enable_priority_queue ? "true" : "false")
        << ", \"queue_depths\": {"
        << "\"critical\": " << depths[0]
        << ", \"high\": " << depths[1]
        << ", \"normal\": " << depths[2]
        << ", \"low\": " << depths[3]
        << "}"
        << ", \"queue_depths_by_agent\": {";
    {
        bool first = true;
        for (const auto& [aid, count] : agent_depths) {
            if (!first) oss << ", ";
            oss << "\"" << aid << "\": " << count;
            first = false;
        }
    }
    oss << "}"
        << ", \"total_enqueued\": " << _scheduler.total_enqueued_count()
        << ", \"total_dequeued\": " << _scheduler.total_dequeued_count()
        << ", \"drops_by_priority\": {"
        << "\"critical\": " << drops[0]
        << ", \"high\": " << drops[1]
        << ", \"normal\": " << drops[2]
        << ", \"low\": " << drops[3]
        << "}"
        << ", \"per_agent_drops\": " << _scheduler.per_agent_drops()
        << ", \"agents_tracked\": " << _scheduler.agents_tracked()
        << "}";

    rep->write_body("json", oss.str());
    return make_ready_future<std::unique_ptr<seastar::http::reply>>(std::move(rep));
}

// ============================================================================
// Cache Event Handler (Push-Based Cache Eviction Notifications)
// ============================================================================
//
// Endpoint: POST /v1/cache/events
// Accepts batched cache eviction events from backends. Events use prefix
// hashes (echoed back from X-Ranvier-Prefix-Hash headers) for O(1) lookup.
//
// JSON parsing and validation are handled by parse_cache_events() in
// cache_event_parser.hpp (pure function, testable without Seastar).
// This handler manages auth, metrics, and async eviction dispatch.
//
// Rule #16: Uses coroutine, not .then() chains.
// Rule #22: Coroutine params by value.
// Rule #9: Every catch block logs at warn+.
//
future<std::unique_ptr<seastar::http::reply>> HttpController::handle_cache_events(
    std::unique_ptr<seastar::http::request> req,
    std::unique_ptr<seastar::http::reply> rep) {

    // Auth check: if auth_token is configured, verify Bearer token
    if (!_config.cache_events.auth_token.empty()) {
        auto auth_it = req->_headers.find("Authorization");
        bool authorized = false;
        if (auth_it != req->_headers.end()) {
            auto token = extract_bearer_token(std::string_view(auth_it->second));
            authorized = token.has_value() && *token == _config.cache_events.auth_token;
        }
        if (!authorized) {
            metrics().record_cache_event_auth_failure();
            RouterService::record_cache_event_auth_failure();
            rep->set_status(seastar::http::reply::status_type{401});
            rep->write_body("json", sstring("{\"error\": \"Unauthorized\"}"));
            co_return std::move(rep);
        }
    }

    // Access request body
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    std::string_view body_view(req->content.data(), req->content.size());
#pragma GCC diagnostic pop

    // Parse and validate JSON (pure function — no Seastar, no metrics)
    auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    uint64_t max_age_ms = static_cast<uint64_t>(_config.cache_events.max_event_age_seconds) * 1000;

    auto parsed = parse_cache_events(
        body_view, _config.cache_events.max_events_per_request, max_age_ms, now_ms);

    if (!parsed.ok) {
        metrics().record_cache_event_parse_error();
        RouterService::record_cache_event_parse_error();
        if (!parsed.error.empty()) {
            log_proxy.warn("Cache event parse error: {}", parsed.error);
        }
        rep->set_status(seastar::http::reply::status_type{400});
        rep->write_body("json", sstring("{\"error\": \"" + parsed.error + "\"}"));
        co_return std::move(rep);
    }

    // Dispatch validated events
    uint32_t processed = 0;
    uint32_t evictions_applied = 0;
    uint32_t evictions_stale = 0;
    uint32_t evictions_unknown = 0;

    // Rule #2 exception: sequential co_await is intentional here.
    // Each evict_by_prefix_hash_global() fans out to all shards (~20µs),
    // not external I/O. Loop bounded by max_events_per_request (Rule #4).
    // Parallelizing would create N_events × N_shards SMP messages at once.
    for (const auto& ev : parsed.events) {
        if (ev.age_expired) {
            evictions_stale++;
            metrics().record_cache_event_eviction_stale();
            continue;
        }

        metrics().record_cache_event_received();
        RouterService::record_cache_event_received();
        processed++;

        if (ev.event_type == "evicted") {
            uint32_t evicted = co_await RouterService::evict_by_prefix_hash_global(
                ev.prefix_hash, ev.backend_id, ev.timestamp_ms);

            if (evicted > 0) {
                evictions_applied++;
                metrics().record_cache_event_eviction_applied();

                // Propagate to cluster peers (Phase 2)
                if (_config.cache_events.propagate_via_gossip) {
                    // Rule #22: lambda captures scalars by value for cross-shard
                    // Rule #14: submit to shard 0 where GossipService lives
                    co_await seastar::smp::submit_to(0, [this, prefix_hash = ev.prefix_hash,
                                                          backend_id = ev.backend_id] {
                        if (_router.gossip_service()) {
                            return _router.gossip_service()->broadcast_cache_eviction(
                                prefix_hash, backend_id);
                        }
                        return seastar::make_ready_future<>();
                    });
                }
            } else {
                evictions_unknown++;
                metrics().record_cache_event_eviction_unknown();
            }
        }
        // "loaded" events are Phase 3 — skip silently
    }

    // Build response
    std::ostringstream oss;
    oss << "{\"processed\": " << processed
        << ", \"evictions_applied\": " << evictions_applied
        << ", \"evictions_stale\": " << evictions_stale
        << ", \"evictions_unknown\": " << evictions_unknown
        << "}";

    rep->write_body("json", sstring(oss.str()));
    co_return std::move(rep);
}

} // namespace ranvier
