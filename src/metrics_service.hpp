// Ranvier Core - Metrics Service
//
// Provides Prometheus metrics for monitoring Ranvier performance.
// Includes histograms for latency tracking and counters for request stats.
// Supports per-backend metrics for comparing GPU model performance.
//
// For reusable histogram/bucket infrastructure, see metrics_helpers.hpp.

#pragma once

#include "health_service.hpp"
#include "metrics_helpers.hpp"

#include <array>
#include <chrono>

namespace ranvier {

// Forward declaration for load-aware routing metrics
// Defined in router_service.cpp - reads shard-local BackendInfo::active_requests
uint64_t get_backend_load(BackendId id);

// Thread-local metrics for the HTTP controller
// Each shard has its own counters/histograms (shared-nothing model)
class MetricsService {
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

            seastar::metrics::make_counter("stale_connection_retries_total", _stale_connection_retries,
                seastar::metrics::description("Retries triggered by empty backend response on stale pooled connection")),

            seastar::metrics::make_counter("circuit_breaker_opens", _circuit_opens,
                seastar::metrics::description("Total number of circuit breaker opens")),

            seastar::metrics::make_counter("retries_skipped_circuit_open_total", _retries_skipped_circuit_open,
                seastar::metrics::description("Connection retries skipped because circuit breaker was open")),

            seastar::metrics::make_counter("circuit_breaker_circuits_removed_total", _circuits_removed,
                seastar::metrics::description("Total number of circuit breaker entries removed when backends were deregistered (Rule #4: bounded container cleanup)")),

            seastar::metrics::make_counter("fallback_attempts", _fallback_attempts,
                seastar::metrics::description("Total number of fallback routing attempts")),

            seastar::metrics::make_counter("http_requests_backpressure_rejected", _requests_backpressure,
                seastar::metrics::description("Total number of requests rejected due to backpressure (concurrency or persistence)")),

            seastar::metrics::make_counter("requests_rejected_body_size_total", _requests_rejected_body_size,
                seastar::metrics::description("Total number of requests rejected due to request body size exceeding limit (HTTP 413)")),

            seastar::metrics::make_counter("stream_parser_size_limit_rejections", _stream_parser_size_rejections,
                seastar::metrics::description("Total number of connections rejected due to stream parser accumulator size limit (Slowloris defense)")),

            seastar::metrics::make_counter("backend_metrics_overflow", _backend_metrics_overflow,
                seastar::metrics::description("Times backend metrics limit was reached, new backends ignored (Rule #4: bounded containers)")),

            seastar::metrics::make_counter("tokenizer_validation_failures", _tokenizer_validation_failures,
                seastar::metrics::description("Total number of requests with invalid input (UTF-8, null bytes, length) that failed validation before tokenization")),

            seastar::metrics::make_counter("tokenizer_errors", _tokenizer_errors,
                seastar::metrics::description("Total number of tokenizer errors (exceptions during encode)")),

            seastar::metrics::make_counter("tokenization_skipped", _tokenization_skipped,
                seastar::metrics::description("Total number of requests where tokenization was skipped (random routing mode)")),

            seastar::metrics::make_counter("tokenization_cache_hits", _tokenization_cache_hits,
                seastar::metrics::description("Total number of tokenization cache hits (avoided FFI calls)")),

            seastar::metrics::make_counter("tokenization_cache_misses", _tokenization_cache_misses,
                seastar::metrics::description("Total number of tokenization cache misses (required FFI calls)")),

            seastar::metrics::make_counter("tokenization_cross_shard", _tokenization_cross_shard,
                seastar::metrics::description("Total number of tokenization cache misses dispatched to other shards via P2C")),

            seastar::metrics::make_counter("prefix_boundary_used", _prefix_boundary_used,
                seastar::metrics::description("Total number of requests where system message prefix boundary was identified and used for routing")),

            seastar::metrics::make_counter("prefix_boundary_skipped", _prefix_boundary_skipped,
                seastar::metrics::description("Total number of requests where prefix boundary was skipped (no system messages, too short, or disabled)")),

            seastar::metrics::make_counter("prefix_boundary_client", _prefix_boundary_client,
                seastar::metrics::description("Total number of requests where client-provided prefix_token_count was used for routing")),

            // Load-aware routing metrics
            seastar::metrics::make_counter("routing_load_aware_fallbacks_total", _load_aware_fallbacks,
                seastar::metrics::description("Total number of requests diverted to less-loaded backends due to queue depth exceeding threshold")),

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

            seastar::metrics::make_histogram("router_tokenization_latency_seconds",
                seastar::metrics::description("Tokenization latency in seconds (10μs-100ms buckets)"),
                [this] { return _tokenization_latency.data; }),

            seastar::metrics::make_histogram("router_primary_tokenization_latency_seconds",
                seastar::metrics::description("Primary prompt tokenization latency (excludes boundary detection)"),
                [this] { return _primary_tokenization_latency.data; }),

            seastar::metrics::make_histogram("router_boundary_detection_latency_seconds",
                seastar::metrics::description("Prefix boundary detection latency (message tokenization for boundaries)"),
                [this] { return _boundary_detection_latency.data; }),

            seastar::metrics::make_histogram("router_art_lookup_latency_seconds",
                seastar::metrics::description("ART radix tree lookup latency in seconds (10μs-100ms buckets)"),
                [this] { return _art_lookup_latency.data; }),

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
                }),

            // Per-priority request counters and active gauges
            seastar::metrics::make_gauge("proxy_requests_by_priority",
                seastar::metrics::description("Total proxy requests by priority tier"),
                {{"priority", "critical"}},
                [this] { return static_cast<double>(_requests_by_priority[0]); }),
            seastar::metrics::make_gauge("proxy_requests_by_priority",
                seastar::metrics::description("Total proxy requests by priority tier"),
                {{"priority", "high"}},
                [this] { return static_cast<double>(_requests_by_priority[1]); }),
            seastar::metrics::make_gauge("proxy_requests_by_priority",
                seastar::metrics::description("Total proxy requests by priority tier"),
                {{"priority", "normal"}},
                [this] { return static_cast<double>(_requests_by_priority[2]); }),
            seastar::metrics::make_gauge("proxy_requests_by_priority",
                seastar::metrics::description("Total proxy requests by priority tier"),
                {{"priority", "low"}},
                [this] { return static_cast<double>(_requests_by_priority[3]); }),

            seastar::metrics::make_gauge("proxy_active_by_priority",
                seastar::metrics::description("Active proxy requests by priority tier"),
                {{"priority", "critical"}},
                [this] { return static_cast<double>(_active_by_priority[0]); }),
            seastar::metrics::make_gauge("proxy_active_by_priority",
                seastar::metrics::description("Active proxy requests by priority tier"),
                {{"priority", "high"}},
                [this] { return static_cast<double>(_active_by_priority[1]); }),
            seastar::metrics::make_gauge("proxy_active_by_priority",
                seastar::metrics::description("Active proxy requests by priority tier"),
                {{"priority", "normal"}},
                [this] { return static_cast<double>(_active_by_priority[2]); }),
            seastar::metrics::make_gauge("proxy_active_by_priority",
                seastar::metrics::description("Active proxy requests by priority tier"),
                {{"priority", "low"}},
                [this] { return static_cast<double>(_active_by_priority[3]); }),

            // ================================================================
            // Radix Tree Path Compression Metric
            // Note: Lookup hit/miss counters, node counts, and slab utilization
            // are registered in RouterService where the RadixTree and NodeSlab
            // are accessible. This gauge aggregates path compression efficiency.
            // ================================================================

            // Per-intent request counters (intent classification)
            seastar::metrics::make_gauge("proxy_requests_by_intent",
                seastar::metrics::description("Total proxy requests by classified intent"),
                {{"intent", "autocomplete"}},
                [this] { return static_cast<double>(_requests_by_intent[0]); }),
            seastar::metrics::make_gauge("proxy_requests_by_intent",
                seastar::metrics::description("Total proxy requests by classified intent"),
                {{"intent", "chat"}},
                [this] { return static_cast<double>(_requests_by_intent[1]); }),
            seastar::metrics::make_gauge("proxy_requests_by_intent",
                seastar::metrics::description("Total proxy requests by classified intent"),
                {{"intent", "edit"}},
                [this] { return static_cast<double>(_requests_by_intent[2]); }),

            // Scheduler wait time: time requests spend queued in priority scheduler
            seastar::metrics::make_histogram("scheduler_wait_seconds",
                seastar::metrics::description("Time requests spend in priority queue before dequeue (seconds)"),
                [this] { return _scheduler_wait_latency.data; }),

            // Average prefix skip length: measures path compression effectiveness
            // Higher values indicate more efficient tree structure (fewer nodes traversed per lookup)
            // This is the running average of tokens skipped via path compression during lookups
            seastar::metrics::make_gauge("radix_tree_average_prefix_skip_length",
                seastar::metrics::description("Average tokens skipped per lookup via path compression. Higher = better tree structure."),
                [this] { return get_average_prefix_skip_length(); }),

            // Prefix cache hits bucketed by backend compression tier.
            // Tiers: "none" (ratio == 1.0), "moderate" (1.0 < ratio < 4.0), "high" (ratio >= 4.0).
            // Uses make_gauge with lambdas (same pattern as proxy_requests_by_priority)
            // because Seastar's make_counter with labels requires a lambda, not a variable reference.
            seastar::metrics::make_gauge("prefix_hits_by_compression_tier",
                seastar::metrics::description("Cache hits on backends with no KV-cache compression (ratio == 1.0)"),
                {{"compression_tier", "none"}},
                [this] { return static_cast<double>(_prefix_hits_tier_none); }),
            seastar::metrics::make_gauge("prefix_hits_by_compression_tier",
                seastar::metrics::description("Cache hits on backends with moderate KV-cache compression (1.0 < ratio < 4.0)"),
                {{"compression_tier", "moderate"}},
                [this] { return static_cast<double>(_prefix_hits_tier_moderate); }),
            seastar::metrics::make_gauge("prefix_hits_by_compression_tier",
                seastar::metrics::description("Cache hits on backends with high KV-cache compression (ratio >= 4.0)"),
                {{"compression_tier", "high"}},
                [this] { return static_cast<double>(_prefix_hits_tier_high); })
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
    void record_stale_connection_retry() { _stale_connection_retries++; }

    // Cache hit/miss tracking for ranvier_cache_hit_ratio gauge
    // These are shard-local (lock-free) for hot path efficiency
    void record_cache_hit() { _cache_hits++; }
    void record_cache_miss() { _cache_misses++; }
    uint64_t get_cache_hits() const { return _cache_hits; }
    uint64_t get_cache_misses() const { return _cache_misses; }

    // Radix tree lookup hit/miss tracking
    // Hits: lookup found a valid Backend route
    // Misses: lookup failed to find any matching route
    void record_radix_tree_lookup_hit() { _radix_tree_lookup_hits++; }
    void record_radix_tree_lookup_miss() { _radix_tree_lookup_misses++; }
    uint64_t get_radix_tree_lookup_hits() const { return _radix_tree_lookup_hits; }
    uint64_t get_radix_tree_lookup_misses() const { return _radix_tree_lookup_misses; }

    // Prefix skip length tracking for path compression efficiency
    // Records the length of prefixes skipped during tree traversal
    void record_prefix_skip(size_t length) {
        _prefix_skip_length_sum += length;
        _prefix_skip_count++;
    }
    double get_average_prefix_skip_length() const {
        if (_prefix_skip_count == 0) return 0.0;
        return static_cast<double>(_prefix_skip_length_sum) / static_cast<double>(_prefix_skip_count);
    }

    // Record circuit breaker events
    void record_circuit_open() { _circuit_opens++; }
    void record_retry_skipped_circuit_open() { _retries_skipped_circuit_open++; }
    void record_circuit_removed() { _circuits_removed++; }
    void record_fallback() { _fallback_attempts++; }

    // Record backpressure rejections (503 due to concurrency or persistence limits)
    void record_backpressure_rejection() { _requests_backpressure++; }

    // Record request body size limit rejections (HTTP 413)
    void record_body_size_rejection() { _requests_rejected_body_size++; }

    // Record stream parser size limit rejections (Slowloris defense)
    void record_stream_parser_size_rejection() { _stream_parser_size_rejections++; }

    // Record tokenizer validation failure (invalid UTF-8, null bytes, etc.)
    void record_tokenizer_validation_failure() { _tokenizer_validation_failures++; }

    // Record tokenizer error (exception during encode)
    void record_tokenizer_error() { _tokenizer_errors++; }

    // Record tokenization skipped (random routing mode optimization)
    void record_tokenization_skipped() { _tokenization_skipped++; }

    // Tokenization cache hit/miss tracking
    // These are shard-local (lock-free) for hot path efficiency
    void record_tokenization_cache_hit() { _tokenization_cache_hits++; }
    void record_tokenization_cache_miss() { _tokenization_cache_misses++; }
    void record_tokenization_cross_shard() { _tokenization_cross_shard++; }
    uint64_t get_tokenization_cache_hits() const { return _tokenization_cache_hits; }
    uint64_t get_tokenization_cache_misses() const { return _tokenization_cache_misses; }
    uint64_t get_tokenization_cross_shard() const { return _tokenization_cross_shard; }

    // Record prefix boundary detection results
    void record_prefix_boundary_used() { _prefix_boundary_used++; }
    void record_prefix_boundary_skipped() { _prefix_boundary_skipped++; }
    void record_prefix_boundary_client() { _prefix_boundary_client++; }

    // Load-aware routing metrics
    // Records when a request is diverted to a less-loaded backend
    void record_load_aware_fallback() {
        _load_aware_fallbacks++;
    }
    uint64_t get_load_aware_fallbacks() const { return _load_aware_fallbacks; }

    // Record a prefix cache hit bucketed by backend compression tier.
    // compression_ratio: the selected backend's KV-cache compression ratio (>= 1.0).
    // Tiers: "none" (== 1.0), "moderate" (1.0 < ratio < 4.0), "high" (>= 4.0).
    void record_prefix_hit_by_compression_tier(double compression_ratio) {
        if (compression_ratio >= 4.0) {
            _prefix_hits_tier_high++;
        } else if (compression_ratio > 1.0) {
            _prefix_hits_tier_moderate++;
        } else {
            _prefix_hits_tier_none++;
        }
    }
    uint64_t get_prefix_hits_tier_none() const { return _prefix_hits_tier_none; }
    uint64_t get_prefix_hits_tier_moderate() const { return _prefix_hits_tier_moderate; }
    uint64_t get_prefix_hits_tier_high() const { return _prefix_hits_tier_high; }

    // Per-priority tier metrics (shard-local counters, no atomics)
    void record_priority_request(uint8_t tier) {
        if (tier < 4) _requests_by_priority[tier]++;
    }
    void increment_priority_active(uint8_t tier) {
        if (tier < 4) _active_by_priority[tier]++;
    }
    void decrement_priority_active(uint8_t tier) {
        if (tier < 4) _active_by_priority[tier]--;
    }

    // Per-intent metrics (shard-local counters, no atomics)
    void record_intent_request(uint8_t intent) {
        if (intent < 3) _requests_by_intent[intent]++;
    }

    // Scheduler wait time histogram (time between enqueue and dequeue)
    void record_scheduler_wait(double seconds) {
        _scheduler_wait_latency.record(seconds);
    }

    // Get overflow count for backend metrics limit (for monitoring)
    uint64_t get_backend_metrics_overflow() const { return _backend_metrics_overflow; }

    // Active request tracking
    void increment_active_requests() { _active_requests++; }
    void decrement_active_requests() { _active_requests--; }
    uint64_t get_active_requests() const { return _active_requests; }

    // Counter accessors (for dashboard stats aggregation)
    uint64_t get_requests_total() const { return _requests_total; }
    uint64_t get_requests_failed() const { return _requests_failed; }

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

    // Record tokenization latency (time to tokenize request body)
    void record_tokenization_latency(double seconds) {
        _tokenization_latency.record(seconds);
    }

    // Record primary prompt tokenization latency (excludes boundary detection)
    void record_primary_tokenization_latency(double seconds) {
        _primary_tokenization_latency.record(seconds);
    }

    // Record prefix boundary detection latency (message tokenization for boundaries)
    void record_boundary_detection_latency(double seconds) {
        _boundary_detection_latency.record(seconds);
    }

    // Record ART radix tree lookup latency (time to find route)
    void record_art_lookup_latency(double seconds) {
        _art_lookup_latency.record(seconds);
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
        auto* bm = get_or_create_backend_metrics(backend_id);
        if (bm) {
            bm->latency.record(seconds);
        }
        // Always record in the aggregate histogram regardless of overflow
        _router_backend_latency.record(seconds);
    }

    // Record first-byte latency with backend_id label
    void record_first_byte_latency_by_id(BackendId backend_id, double seconds) {
        auto* bm = get_or_create_backend_metrics(backend_id);
        if (bm) {
            bm->first_byte_latency.record(seconds);
        }
    }

    // Set HealthService pointer for vLLM metrics gauge lambdas.
    // Called during init; null-safe (gauges return 0 when unset).
    void set_health_service(HealthService* hs) { _health_service = hs; }

    // Helper to convert chrono duration to seconds
    template<typename Duration>
    static double to_seconds(Duration d) {
        return std::chrono::duration<double>(d).count();
    }

private:
    seastar::metrics::metric_groups _metrics;
    seastar::metrics::metric_groups _backend_metrics;

    // HealthService pointer for vLLM gauge lambdas (nullable, not owned)
    HealthService* _health_service = nullptr;

    // Counters
    uint64_t _requests_total = 0;
    uint64_t _requests_success = 0;
    uint64_t _requests_failed = 0;
    uint64_t _requests_timeout = 0;
    uint64_t _requests_rate_limited = 0;
    uint64_t _requests_connection_error = 0;
    uint64_t _stale_connection_retries = 0;
    uint64_t _requests_backpressure = 0;
    uint64_t _requests_rejected_body_size = 0;
    uint64_t _circuit_opens = 0;
    uint64_t _retries_skipped_circuit_open = 0;
    uint64_t _circuits_removed = 0;
    uint64_t _fallback_attempts = 0;
    uint64_t _stream_parser_size_rejections = 0;
    uint64_t _backend_metrics_overflow = 0;  // Times backend metrics limit was hit (Rule #4)
    uint64_t _tokenizer_validation_failures = 0;  // Input validation failures before tokenization
    uint64_t _tokenizer_errors = 0;  // Tokenizer exceptions during encode()
    uint64_t _tokenization_skipped = 0;  // Tokenization skipped in random routing mode
    uint64_t _tokenization_cache_hits = 0;    // Tokenization cache hits (avoided FFI)
    uint64_t _tokenization_cache_misses = 0;  // Tokenization cache misses (required FFI)
    uint64_t _tokenization_cross_shard = 0;   // Cache misses dispatched to other shards via P2C
    uint64_t _prefix_boundary_used = 0;  // System message prefix boundary was used
    uint64_t _prefix_boundary_skipped = 0;  // Prefix boundary skipped (no system messages, too short, disabled)
    uint64_t _prefix_boundary_client = 0;  // Client-provided prefix_token_count was used

    // Load-aware routing counters
    uint64_t _load_aware_fallbacks = 0;  // Requests diverted due to backend load

    // Prefix hit counters by compression tier (shard-local, no atomics — Rule #1)
    uint64_t _prefix_hits_tier_none = 0;      // compression_ratio == 1.0
    uint64_t _prefix_hits_tier_moderate = 0;  // 1.0 < compression_ratio < 4.0
    uint64_t _prefix_hits_tier_high = 0;      // compression_ratio >= 4.0

    // Per-priority tier counters (shard-local, no atomics — Hard Rule #0/#1)
    std::array<uint64_t, 4> _requests_by_priority = {0, 0, 0, 0};  // [CRITICAL, HIGH, NORMAL, LOW]
    std::array<uint64_t, 4> _active_by_priority = {0, 0, 0, 0};    // Active gauge per tier

    // Per-intent counters (shard-local, no atomics)
    std::array<uint64_t, 3> _requests_by_intent = {0, 0, 0};  // [AUTOCOMPLETE, CHAT, EDIT]

    // Cache hit/miss counters for ranvier_cache_hit_ratio gauge
    // Shard-local for lock-free hot path performance
    uint64_t _cache_hits = 0;
    uint64_t _cache_misses = 0;

    // Radix tree lookup counters for efficiency tracking
    // Hits: lookup found a valid Backend
    // Misses: lookup failed to find a route
    uint64_t _radix_tree_lookup_hits = 0;
    uint64_t _radix_tree_lookup_misses = 0;

    // Prefix skip length accumulator for path compression efficiency
    // sum / count = average prefix skip length
    uint64_t _prefix_skip_length_sum = 0;
    uint64_t _prefix_skip_count = 0;

    // Active requests gauge
    uint64_t _active_requests = 0;

    // Legacy histogram accumulators
    MetricHistogram _request_latency{latency_buckets()};
    MetricHistogram _connect_latency{latency_buckets()};
    MetricHistogram _response_latency{latency_buckets()};
    MetricHistogram _backend_total_latency{latency_buckets()};

    // New advanced histogram accumulators with optimized buckets
    MetricHistogram _routing_latency{routing_latency_buckets()};
    MetricHistogram _tokenization_latency{routing_latency_buckets()};
    MetricHistogram _primary_tokenization_latency{routing_latency_buckets()};
    MetricHistogram _boundary_detection_latency{routing_latency_buckets()};
    MetricHistogram _art_lookup_latency{routing_latency_buckets()};
    MetricHistogram _router_backend_latency{backend_latency_buckets()};
    MetricHistogram _router_total_latency{total_request_latency_buckets()};

    // Scheduler wait time histogram (enqueue → dequeue latency)
    MetricHistogram _scheduler_wait_latency{latency_buckets()};

    // Per-backend metrics for GPU model comparison
    std::unordered_map<BackendId, BackendMetrics> _per_backend_metrics;

    // Get or create per-backend metrics and register with Seastar
    // Metrics are shard-local (lock-free) to maintain hot path efficiency
    // Rule #4: Bounded container - rejects new backends beyond MAX_TRACKED_BACKENDS
    // Returns nullptr when the limit is reached (caller skips per-backend recording)
    BackendMetrics* get_or_create_backend_metrics(BackendId backend_id) {
        auto it = _per_backend_metrics.find(backend_id);
        if (it != _per_backend_metrics.end()) {
            return &it->second;
        }

        // Bounds check: reject new backends if limit reached (Rule #4)
        // Increment overflow counter and return nullptr — caller skips per-backend
        // recording while aggregate histograms still capture the data
        if (_per_backend_metrics.size() >= MAX_TRACKED_BACKENDS) {
            ++_backend_metrics_overflow;
            return nullptr;
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
                [&metrics] { return metrics.first_byte_latency.data; }),

            // Per-backend active requests gauge for load-aware routing observability
            // Shows current in-flight requests to each backend (Rule #1: lock-free atomic read)
            seastar::metrics::make_gauge("backend_active_requests",
                seastar::metrics::description("Current number of in-flight requests to backend. Use for load-aware routing observability."),
                {{"backend_id", backend_id_str}},
                [backend_id] { return static_cast<double>(get_backend_load(backend_id)); }),

            // Per-backend vLLM metrics gauges (read from HealthService on shard 0)
            // Returns 0/default when HealthService is not set or backend has no vLLM data
            seastar::metrics::make_gauge("backend_vllm_requests_running",
                seastar::metrics::description("Active requests on GPU (from vLLM /metrics)"),
                {{"backend_id", backend_id_str}},
                [this, backend_id] {
                    if (!_health_service) return 0.0;
                    return static_cast<double>(_health_service->get_vllm_metrics(backend_id).num_requests_running);
                }),

            seastar::metrics::make_gauge("backend_vllm_requests_waiting",
                seastar::metrics::description("Queued requests in vLLM scheduler (from /metrics)"),
                {{"backend_id", backend_id_str}},
                [this, backend_id] {
                    if (!_health_service) return 0.0;
                    return static_cast<double>(_health_service->get_vllm_metrics(backend_id).num_requests_waiting);
                }),

            seastar::metrics::make_gauge("backend_vllm_cache_usage",
                seastar::metrics::description("GPU KV cache usage 0.0-1.0 (from vLLM /metrics)"),
                {{"backend_id", backend_id_str}},
                [this, backend_id] {
                    if (!_health_service) return 0.0;
                    return _health_service->get_vllm_metrics(backend_id).gpu_cache_usage_percent;
                }),

            seastar::metrics::make_gauge("backend_vllm_load_score",
                seastar::metrics::description("Composite load score 0.0-1.0 (from vLLM /metrics)"),
                {{"backend_id", backend_id_str}},
                [this, backend_id] {
                    if (!_health_service) return 0.0;
                    return _health_service->get_backend_load(backend_id);
                }),

            seastar::metrics::make_gauge("backend_vllm_prompt_throughput",
                seastar::metrics::description("Average prompt throughput tokens/sec (from vLLM /metrics)"),
                {{"backend_id", backend_id_str}},
                [this, backend_id] {
                    if (!_health_service) return 0.0;
                    return _health_service->get_vllm_metrics(backend_id).avg_prompt_throughput;
                }),

            seastar::metrics::make_gauge("backend_vllm_generation_throughput",
                seastar::metrics::description("Average generation throughput tokens/sec (from vLLM /metrics)"),
                {{"backend_id", backend_id_str}},
                [this, backend_id] {
                    if (!_health_service) return 0.0;
                    return _health_service->get_vllm_metrics(backend_id).avg_generation_throughput;
                }),

            // Per-backend effective cache capacity/usage (compression-aware)
            seastar::metrics::make_gauge("backend_effective_cache_capacity",
                seastar::metrics::description("Raw GPU memory * compression_ratio (bytes). Higher = more effective KV-cache room."),
                {{"backend_id", backend_id_str}},
                [this, backend_id] {
                    if (!_health_service) return 0.0;
                    return _health_service->get_backend_effective_cache_capacity(backend_id);
                }),

            seastar::metrics::make_gauge("backend_effective_cache_usage",
                seastar::metrics::description("Raw cache usage / compression_ratio (bytes). Lower = more effective headroom."),
                {{"backend_id", backend_id_str}},
                [this, backend_id] {
                    if (!_health_service) return 0.0;
                    return _health_service->get_backend_effective_cache_usage(backend_id);
                })
        });

        metrics.registered = true;
        return &metrics;
    }

public:
    // Stop the metrics service - MUST be called during shutdown (Rule #6).
    // Deregisters all metrics lambdas to prevent use-after-free when
    // Prometheus scrapes arrive after shutdown begins.
    //
    // Order is critical:
    //   1. Clear _metrics (deregisters all lambdas capturing `this`)
    //   2. Clear _backend_metrics (deregisters per-backend lambdas)
    //   3. Clear _per_backend_metrics (destroys BackendMetrics objects)
    //
    // After stop() returns, no metrics lambda can access `this`.
    void stop() {
        // FIRST: Deregister all metrics before any other cleanup.
        // This removes all lambdas from Prometheus scrape path.
        _metrics.clear();
        _backend_metrics.clear();

        // Now safe to clear internal state
        _per_backend_metrics.clear();
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
// Rule #3: Null-guard all pointer accessors - lazy initialization ensures never null
inline MetricsService& metrics() {
    if (!g_metrics) {
        g_metrics = new MetricsService();
    }
    return *g_metrics;
}

// Stop and cleanup metrics for this shard (call during shutdown).
// MUST be called before destruction to deregister metrics lambdas (Rule #6).
// After this call, Prometheus scrapes cannot access any metrics state.
inline void stop_metrics() {
    if (g_metrics) {
        g_metrics->stop();
        delete g_metrics;
        g_metrics = nullptr;
    }
}

}  // namespace ranvier
