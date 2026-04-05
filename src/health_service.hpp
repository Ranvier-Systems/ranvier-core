#pragma once
#include <chrono>
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/metrics_registration.hh>

#include <absl/container/flat_hash_map.h>

#include "backend_registry.hpp"
#include "vllm_metrics.hpp"

namespace ranvier {

// Health check configuration
struct HealthServiceConfig {
    std::chrono::seconds check_interval{5};
    std::chrono::seconds check_timeout{3};
    uint32_t failure_threshold = 3;
    uint32_t recovery_threshold = 2;

    // vLLM metrics scraping
    bool enable_vllm_metrics = true;                    // Scrape /metrics if available
    std::chrono::milliseconds vllm_metrics_timeout{200}; // Timeout for /metrics fetch
};

class HealthService {
public:
    HealthService(BackendRegistry& registry, HealthServiceConfig config = {});

    // Start the background loop
    void start();

    // Stop cleanly
    seastar::future<> stop();

    // Get latest vLLM metrics for a backend (returns default if not available)
    VLLMMetrics get_vllm_metrics(BackendId id) const;

    // Get load score for a backend (0.0 idle → 1.0 saturated)
    // Returns 0.0 if no vLLM metrics available (optimistic default)
    double get_backend_load(BackendId id) const;

    // Set compression ratio for a backend (called at registration time).
    // Used during load broadcast to compute compression-aware load scores.
    void set_backend_compression_ratio(BackendId id, double compression_ratio);

private:
    BackendRegistry& _registry;
    HealthServiceConfig _config;
    seastar::gate _gate; // Prevents shutdown while checking
    bool _running = false;

    // Track the background loop future for clean shutdown (Rule #5)
    seastar::future<> _loop_future = seastar::make_ready_future<>();

    // The main loop
    seastar::future<> run_loop();

    // Check a single host
    seastar::future<bool> check_backend(seastar::socket_address addr);

    // --- vLLM Metrics Scraping ---

    // Per-backend vLLM metrics (shard 0 only, matches health check loop)
    // Hard Rule #4: bounded by MAX_TRACKED_BACKENDS
    absl::flat_hash_map<BackendId, VLLMMetrics> _backend_vllm_metrics;
    static constexpr size_t MAX_TRACKED_BACKENDS = 256;
    uint64_t _metrics_overflow_drops = 0;

    // Per-backend compression ratio for compression-aware load scoring.
    // Sourced from BackendInfo at registration time; read during load broadcast.
    // Hard Rule #4: bounded by MAX_TRACKED_BACKENDS (same backend pool).
    absl::flat_hash_map<BackendId, double> _backend_compression_ratios;

    // --- Adaptive scrape suppression ---
    // After SCRAPE_FAILURE_SUPPRESSION_THRESHOLD consecutive failures for a
    // backend, stop scraping it (avoids log noise and wasted TCP connections
    // for non-vLLM backends like Ollama). A single successful scrape resets
    // the counter, so backends upgraded to vLLM recover automatically.
    static constexpr uint32_t SCRAPE_FAILURE_SUPPRESSION_THRESHOLD = 3;

    // Per-backend consecutive failure count.
    // Hard Rule #4: bounded by MAX_TRACKED_BACKENDS (same pool of backends).
    absl::flat_hash_map<BackendId, uint32_t> _backend_scrape_failures;

    // Returns true if scraping should be skipped for this backend
    bool is_scrape_suppressed(BackendId id) const;

    // Record a scrape failure; returns the new consecutive failure count
    uint32_t record_scrape_failure(BackendId id);

    // Record a scrape success (resets failure counter)
    void record_scrape_success(BackendId id);

    // Run one metrics scrape pass across all backends (called from run_loop)
    seastar::future<> scrape_all_vllm_metrics(
        const std::vector<std::pair<BackendId, seastar::socket_address>>& backends);

    // Scrape and store metrics for a single backend (with timing + counters)
    seastar::future<> scrape_one_backend(BackendId id, seastar::socket_address addr);

    // Scrape /metrics from a backend, parse vLLM Prometheus metrics
    seastar::future<std::optional<VLLMMetrics>>
        scrape_vllm_metrics(seastar::socket_address addr);

    // Store scraped metrics with bounds checking
    void store_vllm_metrics(BackendId id, VLLMMetrics metrics);

    // --- Scraping Observability Metrics (Rule #6: deregistered in stop()) ---
    seastar::metrics::metric_groups _metrics;
    uint64_t _vllm_scrapes_total = 0;
    uint64_t _vllm_scrapes_success = 0;
    uint64_t _vllm_scrapes_failed = 0;
    uint64_t _vllm_scrapes_suppressed = 0;

    // Scrape duration histogram (using simple sum/count for now)
    double _vllm_scrape_duration_sum = 0.0;
    uint64_t _vllm_scrape_duration_count = 0;
};

}  // namespace ranvier
