// Ranvier Core - Metrics Service
//
// Provides Prometheus metrics for monitoring Ranvier performance.
// Includes histograms for latency tracking and counters for request stats.

#pragma once

#include "types.hpp"

#include <chrono>
#include <seastar/core/metrics.hh>
#include <seastar/core/metrics_registration.hh>

namespace ranvier {

// Histogram bucket boundaries for latency measurements (in seconds)
// Covers range from 1ms to 5 minutes for LLM inference workloads
inline std::vector<double> latency_buckets() {
    return {
        0.001,   // 1ms
        0.005,   // 5ms
        0.01,    // 10ms
        0.025,   // 25ms
        0.05,    // 50ms
        0.1,     // 100ms
        0.25,    // 250ms
        0.5,     // 500ms
        1.0,     // 1s
        2.5,     // 2.5s
        5.0,     // 5s
        10.0,    // 10s
        30.0,    // 30s
        60.0,    // 1min
        120.0,   // 2min
        300.0    // 5min
    };
}

// Thread-local metrics for the HTTP controller
// Each shard has its own counters/histograms (shared-nothing model)
class MetricsService {
public:
    MetricsService() {
        // Register metrics group
        _metrics.add_group("ranvier", {
            // Request counters
            seastar::metrics::make_counter("http_requests_total", _requests_total,
                seastar::metrics::description("Total number of HTTP requests received")),

            seastar::metrics::make_counter("http_requests_success", _requests_success,
                seastar::metrics::description("Total number of successful HTTP requests")),

            seastar::metrics::make_counter("http_requests_failed", _requests_failed,
                seastar::metrics::description("Total number of failed HTTP requests")),

            seastar::metrics::make_counter("http_requests_timeout", _requests_timeout,
                seastar::metrics::description("Total number of timed out HTTP requests")),

            seastar::metrics::make_counter("http_requests_rate_limited", _requests_rate_limited,
                seastar::metrics::description("Total number of rate-limited HTTP requests")),

            seastar::metrics::make_counter("circuit_breaker_opens", _circuit_opens,
                seastar::metrics::description("Total number of circuit breaker opens")),

            seastar::metrics::make_counter("fallback_attempts", _fallback_attempts,
                seastar::metrics::description("Total number of fallback routing attempts")),

            // Latency histograms
            seastar::metrics::make_histogram("http_request_duration_seconds",
                seastar::metrics::description("HTTP request duration in seconds"),
                latency_buckets(),
                [this] { return _request_latency.get_histogram(); }),

            seastar::metrics::make_histogram("backend_connect_duration_seconds",
                seastar::metrics::description("Backend connection establishment duration in seconds"),
                latency_buckets(),
                [this] { return _connect_latency.get_histogram(); }),

            seastar::metrics::make_histogram("backend_response_duration_seconds",
                seastar::metrics::description("Time to first byte from backend in seconds"),
                latency_buckets(),
                [this] { return _response_latency.get_histogram(); }),

            seastar::metrics::make_histogram("backend_total_duration_seconds",
                seastar::metrics::description("Total backend request duration in seconds"),
                latency_buckets(),
                [this] { return _backend_total_latency.get_histogram(); })
        });
    }

    // Record a request received
    void record_request() { _requests_total++; }

    // Record request outcome
    void record_success() { _requests_success++; }
    void record_failure() { _requests_failed++; }
    void record_timeout() { _requests_timeout++; }
    void record_rate_limited() { _requests_rate_limited++; }

    // Record circuit breaker events
    void record_circuit_open() { _circuit_opens++; }
    void record_fallback() { _fallback_attempts++; }

    // Record latencies (in seconds)
    void record_request_latency(double seconds) {
        _request_latency.add(seconds);
    }

    void record_connect_latency(double seconds) {
        _connect_latency.add(seconds);
    }

    void record_response_latency(double seconds) {
        _response_latency.add(seconds);
    }

    void record_backend_total_latency(double seconds) {
        _backend_total_latency.add(seconds);
    }

    // Helper to convert chrono duration to seconds
    template<typename Duration>
    static double to_seconds(Duration d) {
        return std::chrono::duration<double>(d).count();
    }

private:
    seastar::metrics::metric_groups _metrics;

    // Counters
    uint64_t _requests_total = 0;
    uint64_t _requests_success = 0;
    uint64_t _requests_failed = 0;
    uint64_t _requests_timeout = 0;
    uint64_t _requests_rate_limited = 0;
    uint64_t _circuit_opens = 0;
    uint64_t _fallback_attempts = 0;

    // Histogram accumulators
    seastar::metrics::histogram _request_latency;
    seastar::metrics::histogram _connect_latency;
    seastar::metrics::histogram _response_latency;
    seastar::metrics::histogram _backend_total_latency;
};

// Global thread-local metrics instance
inline thread_local MetricsService* g_metrics = nullptr;

// Initialize metrics for this shard (call once per shard)
inline void init_metrics() {
    if (!g_metrics) {
        g_metrics = new MetricsService();
    }
}

// Get the metrics instance for this shard
inline MetricsService& metrics() {
    return *g_metrics;
}

}  // namespace ranvier
