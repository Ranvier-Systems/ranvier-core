// Ranvier Core - Metrics Service
//
// Provides Prometheus metrics for monitoring Ranvier performance.
// Includes histograms for latency tracking and counters for request stats.
// Supports per-backend metrics for comparing GPU model performance.

#pragma once

#include "types.hpp"

#include <chrono>
#include <functional>
#include <unordered_map>
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

// Optimized routing latency buckets: 10μs to 100ms
// Designed for microsecond-scale routing decisions
inline std::vector<double> routing_latency_buckets() {
    return {
        0.00001,  // 10μs
        0.000025, // 25μs
        0.00005,  // 50μs
        0.0001,   // 100μs
        0.00025,  // 250μs
        0.0005,   // 500μs
        0.001,    // 1ms
        0.0025,   // 2.5ms
        0.005,    // 5ms
        0.01,     // 10ms
        0.025,    // 25ms
        0.05,     // 50ms
        0.1       // 100ms
    };
}

// Backend latency buckets: 50ms to 10s
// Optimized for LLM prefill/generation workloads (second-scale inference)
inline std::vector<double> backend_latency_buckets() {
    return {
        0.05,    // 50ms
        0.1,     // 100ms
        0.25,    // 250ms
        0.5,     // 500ms
        0.75,    // 750ms
        1.0,     // 1s
        1.5,     // 1.5s
        2.0,     // 2s
        3.0,     // 3s
        4.0,     // 4s
        5.0,     // 5s
        7.5,     // 7.5s
        10.0     // 10s
    };
}

// Total request latency buckets: full end-to-end time
// Covers the complete request lifecycle from ingress to completion
inline std::vector<double> total_request_latency_buckets() {
    return {
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
    struct MetricHistogram {
        seastar::metrics::histogram data;

        // Initialize with boundaries
        MetricHistogram(const std::vector<double>& boundaries) {
            data.sample_count = 0;
            data.sample_sum = 0;
            for (double b : boundaries) {
                data.buckets.push_back({b, 0});
            }
        }

        void record(double value) {
            // 1. Update the global totals
            data.sample_count++;
            data.sample_sum += value;

            // 2. Update the individual buckets
            // In a cumulative histogram, a value of 0.05s belongs in
            // the 0.05 bucket, the 0.1 bucket, the 0.5 bucket, etc.
            for (auto& bucket : data.buckets) {
                if (value <= bucket.upper_bound) {
                    bucket.count++;
                }
            }
        }
    };

    // Per-backend histogram storage for labeled metrics
    struct BackendMetrics {
        MetricHistogram latency;  // ranvier_backend_latency_seconds
        MetricHistogram first_byte_latency;
        bool registered = false;

        BackendMetrics()
            : latency(backend_latency_buckets())
            , first_byte_latency(backend_latency_buckets()) {}
    };

public:
    MetricsService() {
        // Register metrics group with core metrics
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

            seastar::metrics::make_counter("http_requests_connection_error", _requests_connection_error,
                seastar::metrics::description("Total number of requests failed due to connection errors (broken pipe, reset)")),

            seastar::metrics::make_counter("circuit_breaker_opens", _circuit_opens,
                seastar::metrics::description("Total number of circuit breaker opens")),

            seastar::metrics::make_counter("fallback_attempts", _fallback_attempts,
                seastar::metrics::description("Total number of fallback routing attempts")),

            seastar::metrics::make_counter("http_requests_backpressure_rejected", _requests_backpressure,
                seastar::metrics::description("Total number of requests rejected due to backpressure (concurrency or persistence)")),

            // Legacy latency histograms (for backwards compatibility)
            seastar::metrics::make_histogram("http_request_duration_seconds",
                seastar::metrics::description("HTTP request duration in seconds"),
                [this] { return _request_latency.data; }),

            seastar::metrics::make_histogram("backend_connect_duration_seconds",
                seastar::metrics::description("Backend connection establishment duration in seconds"),
                [this] { return _connect_latency.data; }),

            seastar::metrics::make_histogram("backend_response_duration_seconds",
                seastar::metrics::description("Time to first byte from backend in seconds"),
                [this] { return _response_latency.data; }),

            seastar::metrics::make_histogram("backend_total_duration_seconds",
                seastar::metrics::description("Total backend request duration in seconds"),
                [this] { return _backend_total_latency.data; }),

            // New advanced histograms with optimized buckets
            seastar::metrics::make_histogram("router_routing_latency_seconds",
                seastar::metrics::description("Router lookup/decision latency in seconds (10μs-100ms buckets)"),
                [this] { return _routing_latency.data; }),

            seastar::metrics::make_histogram("router_backend_latency_seconds",
                seastar::metrics::description("Backend processing latency for LLM inference in seconds (50ms-10s buckets)"),
                [this] { return _router_backend_latency.data; }),

            seastar::metrics::make_histogram("router_request_total_latency_seconds",
                seastar::metrics::description("Full end-to-end request latency in seconds"),
                [this] { return _router_total_latency.data; }),

            // Active proxy requests gauge
            seastar::metrics::make_gauge("active_proxy_requests", _active_requests,
                seastar::metrics::description("Current number of in-flight proxy requests")),

            // Cache hit ratio gauge: hits / (hits + misses)
            // Returns 0.0 when no requests have been made (divide-by-zero protection)
            // Useful for detecting KV-cache thrashing before it impacts user experience
            seastar::metrics::make_gauge("cache_hit_ratio",
                seastar::metrics::description("Ratio of cache hits to total cache lookups (0.0-1.0). Returns 0.0 when cluster first starts."),
                [this] {
                    uint64_t total = _cache_hits + _cache_misses;
                    if (total == 0) {
                        return 0.0;  // Avoid divide-by-zero on startup
                    }
                    return static_cast<double>(_cache_hits) / static_cast<double>(total);
                })
        });
    }

    // Record a request received
    void record_request() { _requests_total++; }

    // Record request outcome
    void record_success() { _requests_success++; }
    void record_failure() { _requests_failed++; }
    void record_timeout() { _requests_timeout++; }
    void record_rate_limited() { _requests_rate_limited++; }
    void record_connection_error() { _requests_connection_error++; }

    // Cache hit/miss tracking for ranvier_cache_hit_ratio gauge
    // These are shard-local (lock-free) for hot path efficiency
    void record_cache_hit() { _cache_hits++; }
    void record_cache_miss() { _cache_misses++; }
    uint64_t get_cache_hits() const { return _cache_hits; }
    uint64_t get_cache_misses() const { return _cache_misses; }

    // Record circuit breaker events
    void record_circuit_open() { _circuit_opens++; }
    void record_fallback() { _fallback_attempts++; }

    // Record backpressure rejections (503 due to concurrency or persistence limits)
    void record_backpressure_rejection() { _requests_backpressure++; }

    // Active request tracking
    void increment_active_requests() { _active_requests++; }
    void decrement_active_requests() { _active_requests--; }
    uint64_t get_active_requests() const { return _active_requests; }

    // Record latencies (in seconds) - legacy methods for backwards compatibility
    void record_request_latency(double seconds) {
        _request_latency.record(seconds);
    }

    void record_connect_latency(double seconds) {
        _connect_latency.record(seconds);
    }

    void record_response_latency(double seconds) {
        _response_latency.record(seconds);
    }

    void record_backend_total_latency(double seconds) {
        _backend_total_latency.record(seconds);
    }

    // New advanced histogram recording methods

    // Record routing decision latency (time to lookup and select backend)
    void record_routing_latency(double seconds) {
        _routing_latency.record(seconds);
    }

    // Record backend processing latency (optimized for LLM inference timescales)
    void record_router_backend_latency(double seconds) {
        _router_backend_latency.record(seconds);
    }

    // Record total end-to-end request latency
    void record_router_total_latency(double seconds) {
        _router_total_latency.record(seconds);
    }

    // Record latency with backend_id label for GPU model comparison
    // This populates ranvier_backend_latency_seconds{backend_id="X"}
    // Shard-local for lock-free hot path efficiency
    void record_backend_latency_by_id(BackendId backend_id, double seconds) {
        auto& backend_metrics = get_or_create_backend_metrics(backend_id);
        backend_metrics.latency.record(seconds);
        // Also record in the aggregate histogram
        _router_backend_latency.record(seconds);
    }

    // Record first-byte latency with backend_id label
    void record_first_byte_latency_by_id(BackendId backend_id, double seconds) {
        auto& backend_metrics = get_or_create_backend_metrics(backend_id);
        backend_metrics.first_byte_latency.record(seconds);
    }

    // Helper to convert chrono duration to seconds
    template<typename Duration>
    static double to_seconds(Duration d) {
        return std::chrono::duration<double>(d).count();
    }

private:
    seastar::metrics::metric_groups _metrics;
    seastar::metrics::metric_groups _backend_metrics;

    // Counters
    uint64_t _requests_total = 0;
    uint64_t _requests_success = 0;
    uint64_t _requests_failed = 0;
    uint64_t _requests_timeout = 0;
    uint64_t _requests_rate_limited = 0;
    uint64_t _requests_connection_error = 0;
    uint64_t _requests_backpressure = 0;
    uint64_t _circuit_opens = 0;
    uint64_t _fallback_attempts = 0;

    // Cache hit/miss counters for ranvier_cache_hit_ratio gauge
    // Shard-local for lock-free hot path performance
    uint64_t _cache_hits = 0;
    uint64_t _cache_misses = 0;

    // Active requests gauge
    uint64_t _active_requests = 0;

    // Legacy histogram accumulators
    MetricHistogram _request_latency{latency_buckets()};
    MetricHistogram _connect_latency{latency_buckets()};
    MetricHistogram _response_latency{latency_buckets()};
    MetricHistogram _backend_total_latency{latency_buckets()};

    // New advanced histogram accumulators with optimized buckets
    MetricHistogram _routing_latency{routing_latency_buckets()};
    MetricHistogram _router_backend_latency{backend_latency_buckets()};
    MetricHistogram _router_total_latency{total_request_latency_buckets()};

    // Per-backend metrics for GPU model comparison
    std::unordered_map<BackendId, BackendMetrics> _per_backend_metrics;

    // Get or create per-backend metrics and register with Seastar
    // Metrics are shard-local (lock-free) to maintain hot path efficiency
    BackendMetrics& get_or_create_backend_metrics(BackendId backend_id) {
        auto it = _per_backend_metrics.find(backend_id);
        if (it != _per_backend_metrics.end()) {
            return it->second;
        }

        // Create new backend metrics
        auto& metrics = _per_backend_metrics[backend_id];

        // Register per-backend histograms with Seastar metrics
        // Uses backend_id label to segment data for identifying slow GPUs in the cluster
        std::string backend_id_str = std::to_string(backend_id);
        _backend_metrics.add_group("ranvier", {
            // Per-backend latency histogram for GPU model comparison (e.g., H100 vs A100)
            // Allows operators to identify slow GPUs in the cluster
            seastar::metrics::make_histogram("backend_latency_seconds",
                seastar::metrics::description("Backend processing latency in seconds, segmented by backend_id. Use to identify slow GPUs in the cluster."),
                {{"backend_id", backend_id_str}},
                [&metrics] { return metrics.latency.data; }),

            seastar::metrics::make_histogram("backend_first_byte_latency_seconds",
                seastar::metrics::description("Time to first byte from backend in seconds, segmented by backend_id."),
                {{"backend_id", backend_id_str}},
                [&metrics] { return metrics.first_byte_latency.data; })
        });

        metrics.registered = true;
        return metrics;
    }
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
