// Ranvier Core - Metrics Helpers
//
// Reusable metrics infrastructure: histogram bucket definitions, MetricHistogram
// accumulator, and the bounded per-backend metrics map pattern. These are generic
// patterns not specific to Ranvier's domain counters.
//
// For Ranvier-specific counters (tokenization, ART, prefix boundary, etc.),
// see metrics_service.hpp.

#pragma once

#include "types.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <seastar/core/metrics.hh>
#include <seastar/core/metrics_registration.hh>

namespace ranvier {

// =============================================================================
// Histogram Bucket Definitions
// =============================================================================

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

// Optimized routing latency buckets: 100μs to 100ms
// Note: Seastar's Prometheus exporter truncates very small values (< 0.0001)
// to 0.000000, so we start at 100μs to ensure proper bucket boundaries.
inline std::vector<double> routing_latency_buckets() {
    return {
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

// =============================================================================
// MetricHistogram — Cumulative Histogram Accumulator
// =============================================================================

// Seastar-compatible histogram accumulator with pre-defined bucket boundaries.
// Records values into cumulative buckets and tracks sample count/sum.
struct MetricHistogram {
    seastar::metrics::histogram data;

    // Initialize with boundaries
    MetricHistogram(const std::vector<double>& boundaries) {
        data.sample_count = 0;
        data.sample_sum = 0;
        for (double b : boundaries) {
            data.buckets.push_back({0, b});
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

// =============================================================================
// BackendMetrics — Per-Backend Histogram Storage
// =============================================================================

// Maximum number of unique backends to track metrics for (Rule #4: bounded containers)
// Prevents OOM from malicious/buggy backends flooding unique IDs
inline constexpr size_t MAX_TRACKED_BACKENDS = 10000;

// Per-backend histogram storage for labeled metrics
struct BackendMetrics {
    MetricHistogram latency;  // ranvier_backend_latency_seconds
    MetricHistogram first_byte_latency;
    bool registered = false;

    BackendMetrics()
        : latency(backend_latency_buckets())
        , first_byte_latency(backend_latency_buckets()) {}
};

// =============================================================================
// ApiKeyMetrics — Per-API-Key Counter / Histogram Storage
// =============================================================================
// Per-shard slot for the api_key label table. One ApiKeyMetrics per distinct
// observed (or pre-registered) label, plus three sentinels (_unauthenticated,
// _invalid, _overflow). Bounded by AttributionConfig::max_label_cardinality.
//
// All fields are shard-local plain integers / doubles (Hard Rule #1 — no
// atomics, no mutexes; metrics scrape reads the values lock-free). Stored
// behind unique_ptr in the owning map so lambda captures of &slot stay valid
// across rehashes.
struct ApiKeyMetrics {
    std::string label;  // sanitised label value used as Prometheus label

    // Counters (Memo §6.1)
    uint64_t requests_total = 0;
    uint64_t requests_success = 0;
    uint64_t requests_failed = 0;
    uint64_t requests_timeout = 0;
    uint64_t requests_rate_limited = 0;

    // Aggregate sums for tokens/cost (counters; histograms not currently
    // present in the codebase). Prometheus operators can derive rate / sum
    // queries off these.
    uint64_t input_tokens_sum = 0;
    uint64_t output_tokens_sum = 0;
    double   cost_units_sum = 0.0;

    // Histograms (Memo §6.1)
    MetricHistogram request_duration;       // ranvier_http_request_duration_seconds
    MetricHistogram router_total_latency;   // ranvier_router_request_total_latency_seconds

    bool registered = false;

    ApiKeyMetrics()
        : request_duration(latency_buckets())
        , router_total_latency(total_request_latency_buckets()) {}
};

}  // namespace ranvier
