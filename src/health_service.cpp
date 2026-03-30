#include "health_service.hpp"
#include "logging.hpp"
#include "prometheus_parser.hpp"
#include "router_service.hpp"

#include <cmath>
#include <fmt/format.h>

#include <seastar/core/sleep.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/core/timed_out_error.hh>
#include <seastar/net/api.hh>
#include <seastar/net/inet_address.hh>

using namespace seastar;

namespace ranvier {

// Maximum concurrent health checks to prevent overwhelming backends/network
constexpr size_t HEALTH_MAX_CONCURRENT_CHECKS = 16;

// Maximum response body size for /metrics endpoint (Rule #4)
constexpr size_t METRICS_MAX_RESPONSE_SIZE = 128 * 1024;  // 128KB

HealthService::HealthService(BackendRegistry& registry, HealthServiceConfig config)
    : _registry(registry), _config(config) {
    _backend_vllm_metrics.reserve(MAX_TRACKED_BACKENDS);
}

void HealthService::start() {
    // Register scraping observability metrics (Rule #6: deregistered in stop())
    namespace sm = seastar::metrics;
    _metrics.add_group("ranvier", {
        sm::make_counter("health_vllm_scrapes_total", _vllm_scrapes_total,
            sm::description("Total vLLM metrics scrape attempts")),
        sm::make_counter("health_vllm_scrapes_success", _vllm_scrapes_success,
            sm::description("Successful vLLM metrics scrapes")),
        sm::make_counter("health_vllm_scrapes_failed", _vllm_scrapes_failed,
            sm::description("Failed vLLM metrics scrapes (includes non-vLLM backends)")),
        sm::make_counter("health_vllm_overflow_drops", _metrics_overflow_drops,
            sm::description("vLLM metrics dropped due to MAX_TRACKED_BACKENDS limit")),
        sm::make_gauge("health_vllm_scrape_duration_avg_seconds",
            sm::description("Average vLLM metrics scrape duration in seconds"),
            [this] {
                if (_vllm_scrape_duration_count == 0) return 0.0;
                return _vllm_scrape_duration_sum / static_cast<double>(_vllm_scrape_duration_count);
            }),
    });

    _running = true;
    // Store the loop future for clean shutdown tracking (Rule #5)
    _loop_future = run_loop();
}

future<> HealthService::stop() {
    // Rule #6: Deregister metrics first before any other cleanup
    _metrics.clear();

    _running = false;
    // First close the gate (signals loop to exit and waits for in-flight checks)
    co_await _gate.close();
    // Then await the loop future to ensure clean exit
    co_await std::move(_loop_future);
}

VLLMMetrics HealthService::get_vllm_metrics(BackendId id) const {
    auto it = _backend_vllm_metrics.find(id);
    if (it != _backend_vllm_metrics.end()) {
        return it->second;
    }
    return VLLMMetrics{};  // Default: valid=false
}

double HealthService::get_backend_load(BackendId id) const {
    auto it = _backend_vllm_metrics.find(id);
    if (it != _backend_vllm_metrics.end()) {
        return it->second.load_score();
    }
    return 0.0;  // Optimistic default
}

future<> HealthService::run_loop() {
    // Only run on Core 0 to avoid DDOSing backends
    if (this_shard_id() != 0) co_return;

    // Hold gate for entire loop lifetime (Rule #5: proper lifecycle tracking)
    auto holder = _gate.hold();

    try {
        while (_running) {
            // 1. Get list of backends and resolve addresses
            auto ids = _registry.get_all_backend_ids();

            // Collect backends with valid addresses (Rule #2: no co_await in loops)
            std::vector<std::pair<BackendId, socket_address>> backends_to_check;
            for (auto id : ids) {
                auto addr_opt = _registry.get_backend_address(id);
                if (addr_opt.has_value()) {
                    backends_to_check.emplace_back(id, addr_opt.value());
                }
            }

            // 3. Check health in parallel with concurrency limit
            co_await seastar::max_concurrent_for_each(
                backends_to_check, HEALTH_MAX_CONCURRENT_CHECKS,
                [this](const std::pair<BackendId, socket_address>& backend) -> future<> {
                    auto [id, addr] = backend;
                    try {
                        bool is_alive = co_await check_backend(addr);
                        // 4. Update State (Broadcasts to all cores)
                        co_await _registry.set_backend_status_global(id, is_alive);
                    } catch (const std::exception& e) {
                        log_health.warn("Health check failed for backend {}: {}", id, e.what());
                    }
                });

            // Scrape vLLM metrics (parallel, independent of health check result)
            if (_config.enable_vllm_metrics) {
                co_await scrape_all_vllm_metrics(backends_to_check);

                // Broadcast GPU load scores to all shards for routing decisions.
                // Only broadcast if we have valid metrics to distribute.
                if (!_backend_vllm_metrics.empty()) {
                    try {
                        absl::flat_hash_map<BackendId, double> load_scores;
                        for (const auto& [id, m] : _backend_vllm_metrics) {
                            if (m.valid) {
                                load_scores[id] = m.load_score();
                            }
                        }
                        if (!load_scores.empty()) {
                            co_await RouterService::broadcast_gpu_load(std::move(load_scores));
                        }
                    } catch (const std::exception& e) {
                        // Rule #9: Log at warn level
                        log_health.warn("GPU load broadcast failed: {}", e.what());
                    }
                }
            }

            // 5. Sleep for configured interval
            co_await seastar::sleep(_config.check_interval);
        }
    } catch (const seastar::gate_closed_exception&) {
        // Expected during shutdown - gate closed while loop was running
        log_health.debug("Health check loop exiting: gate closed during shutdown");
    } catch (const std::exception& e) {
        // Rule #9: Log unexpected exceptions at warn level
        log_health.warn("Health check loop exiting unexpectedly: {}", e.what());
    } catch (...) {
        // Rule #9: Log unknown exceptions at warn level
        log_health.warn("Health check loop exiting: unknown exception");
    }
}

future<> HealthService::scrape_all_vllm_metrics(
        const std::vector<std::pair<BackendId, socket_address>>& backends) {
    co_await seastar::max_concurrent_for_each(
        backends, HEALTH_MAX_CONCURRENT_CHECKS,
        [this](const std::pair<BackendId, socket_address>& backend) -> future<> {
            co_await scrape_one_backend(backend.first, backend.second);
        });
}

future<> HealthService::scrape_one_backend(BackendId id, socket_address addr) {
    ++_vllm_scrapes_total;
    auto scrape_start = std::chrono::steady_clock::now();
    try {
        auto metrics = co_await scrape_vllm_metrics(addr);
        if (metrics) {
            store_vllm_metrics(id, std::move(*metrics));
            ++_vllm_scrapes_success;
        } else {
            ++_vllm_scrapes_failed;
        }
    } catch (const std::exception& e) {
        // Not warn — expected for non-vLLM backends
        log_health.debug("vLLM metrics scrape failed for backend {}: {}", id, e.what());
        ++_vllm_scrapes_failed;
    }
    auto duration = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - scrape_start).count();
    _vllm_scrape_duration_sum += duration;
    ++_vllm_scrape_duration_count;
}

future<bool> HealthService::check_backend(socket_address addr) {
    // Use configured timeout for health check connections
    auto deadline = lowres_clock::now() + _config.check_timeout;

    return with_timeout(deadline, seastar::connect(addr))
        .then([](seastar::connected_socket fd) {
            // Success - connection will be closed when fd goes out of scope
            return true;
        })
        .handle_exception([](auto ep) {
            // Timeout, connection refused, network error, etc.
            return false;
        });
}

future<std::optional<VLLMMetrics>>
HealthService::scrape_vllm_metrics(socket_address addr) {
    auto deadline = lowres_clock::now() + _config.vllm_metrics_timeout;

    try {
        // 1. Connect with metrics_timeout
        auto conn = co_await with_timeout(deadline, seastar::connect(addr));

        auto out = conn.output();
        auto in = conn.input();

        // Use exception_ptr to close streams outside catch (co_await not allowed in handlers)
        std::exception_ptr ep;
        sstring response_data;

        try {
            // 2. Write raw HTTP/1.1 GET /metrics request
            auto host_str = fmt::format("{}:{}", addr.addr(), addr.port());
            auto request = fmt::format(
                "GET /metrics HTTP/1.1\r\nHost: {}\r\nConnection: close\r\n\r\n",
                host_str);
            co_await out.write(request);
            co_await out.flush();

            // 3. Read response with deadline (Rule #4: bounded at 128KB)
            while (response_data.size() < METRICS_MAX_RESPONSE_SIZE) {
                auto buf = co_await with_timeout(deadline, in.read());
                if (buf.empty()) break;  // EOF
                response_data.append(buf.get(), buf.size());
            }
        } catch (...) {
            ep = std::current_exception();
        }

        // 4. Close streams in all paths (success, failure, timeout)
        try { co_await out.close(); } catch (...) {}
        try { co_await in.close(); } catch (...) {}

        if (ep) {
            std::rethrow_exception(ep);
        }

        // 5. Parse HTTP status — look for "200"
        if (response_data.size() < 12) {
            co_return std::nullopt;
        }

        auto status_end = response_data.find("\r\n");
        if (status_end == sstring::npos) {
            co_return std::nullopt;
        }

        auto status_line = response_data.substr(0, status_end);
        if (status_line.find("200") == sstring::npos) {
            // Non-200 — backend doesn't expose /metrics or returned error
            co_return std::nullopt;
        }

        // 6. Extract body (after \r\n\r\n)
        auto body_start = response_data.find("\r\n\r\n");
        if (body_start == sstring::npos) {
            co_return std::nullopt;
        }

        std::string_view body(response_data.data() + body_start + 4,
                              response_data.size() - body_start - 4);

        // 7. Parse target metrics using Prometheus text parser
        VLLMMetrics m;

        // Request load (guard against Inf/NaN — UB on integer cast)
        if (auto v = extract_prometheus_metric(body, "vllm:num_requests_running")) {
            if (std::isfinite(*v) && *v >= 0) m.num_requests_running = static_cast<uint32_t>(*v);
        }
        if (auto v = extract_prometheus_metric(body, "vllm:num_requests_waiting")) {
            if (std::isfinite(*v) && *v >= 0) m.num_requests_waiting = static_cast<uint32_t>(*v);
        }

        // KV cache usage
        if (auto v = extract_prometheus_metric(body, "vllm:gpu_cache_usage_perc")) {
            m.gpu_cache_usage_percent = *v;
        }

        // Throughput
        if (auto v = extract_prometheus_metric(body, "vllm:avg_prompt_throughput_toks_per_s")) {
            m.avg_prompt_throughput = *v;
        }
        if (auto v = extract_prometheus_metric(body, "vllm:avg_generation_throughput_toks_per_s")) {
            m.avg_generation_throughput = *v;
        }

        // Lifetime token counters (guard against Inf/NaN — UB on integer cast)
        if (auto v = extract_prometheus_metric(body, "vllm:prompt_tokens_total")) {
            if (std::isfinite(*v) && *v >= 0) m.prompt_tokens_total = static_cast<uint64_t>(*v);
        }
        if (auto v = extract_prometheus_metric(body, "vllm:generation_tokens_total")) {
            if (std::isfinite(*v) && *v >= 0) m.generation_tokens_total = static_cast<uint64_t>(*v);
        }

        // GPU memory (may not be prefixed with vllm:)
        if (auto v = extract_prometheus_metric(body, "gpu_memory_used_bytes")) {
            m.gpu_memory_used_bytes = *v;
        }
        if (auto v = extract_prometheus_metric(body, "gpu_memory_total_bytes")) {
            m.gpu_memory_total_bytes = *v;
        }

        m.scraped_at = std::chrono::steady_clock::now();
        m.valid = true;

        co_return std::make_optional(std::move(m));

    } catch (const seastar::timed_out_error&) {
        // Timeout — expected for non-vLLM backends or slow responses
        co_return std::nullopt;
    } catch (const std::system_error& e) {
        // Connection refused, etc. — expected for non-vLLM backends
        co_return std::nullopt;
    } catch (const std::exception& e) {
        log_health.debug("vLLM metrics scrape exception: {}", e.what());
        co_return std::nullopt;
    }
}

void HealthService::store_vllm_metrics(BackendId id, VLLMMetrics metrics) {
    if (_backend_vllm_metrics.size() >= MAX_TRACKED_BACKENDS
        && !_backend_vllm_metrics.contains(id)) {
        ++_metrics_overflow_drops;
        return;
    }
    _backend_vllm_metrics[id] = std::move(metrics);
}

} // namespace ranvier
