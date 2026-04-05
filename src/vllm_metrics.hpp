#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace ranvier {

// Per-backend vLLM metrics scraped from Prometheus /metrics endpoint.
// Standalone header — no Seastar or health_service dependencies.
struct VLLMMetrics {
    // Request load
    uint32_t num_requests_running = 0;     // Active requests on GPU
    uint32_t num_requests_waiting = 0;     // Queued in vLLM scheduler

    // GPU memory
    double gpu_memory_used_bytes = 0.0;
    double gpu_memory_total_bytes = 0.0;

    // KV cache
    double gpu_cache_usage_percent = 0.0;  // 0.0 to 1.0

    // Throughput
    double avg_prompt_throughput = 0.0;    // Tokens/sec (prefill)
    double avg_generation_throughput = 0.0; // Tokens/sec (decode)

    // Lifetime token counters
    uint64_t prompt_tokens_total = 0;
    uint64_t generation_tokens_total = 0;

    // Freshness
    std::chrono::steady_clock::time_point scraped_at;

    // Whether this struct has been populated with real data
    bool valid = false;

    // Compute effective cache pressure adjusted for KV-cache compression.
    // compression_ratio >= 1.0: a backend with 6x compression at 50% raw usage
    // has effective_cache_pressure = 0.083 (enormous headroom), while a
    // non-compressed backend at 50% has 0.5.
    //
    // compression_ratio must be >= 1.0; values < 1.0 are clamped to 1.0.
    double effective_cache_pressure(double compression_ratio) const {
        double ratio = std::max(compression_ratio, 1.0);
        return gpu_cache_usage_percent / ratio;
    }

    // Compute a 0.0–1.0 load score for routing decisions.
    // Composite of request queue depth and KV cache pressure.
    // compression_ratio adjusts cache pressure for backends with KV-cache
    // compression (default 1.0 = no compression, backward compatible).
    double load_score(double compression_ratio = 1.0) const {
        if (!valid) return 0.0;

        // Primary signal: request saturation
        // (running + waiting) / (running + 1) — ratio > 1.0 means queuing
        double request_pressure = (num_requests_running + num_requests_waiting > 0)
            ? static_cast<double>(num_requests_running + num_requests_waiting)
              / static_cast<double>(num_requests_running + 1)
            : 0.0;

        // Secondary signal: KV cache pressure (compression-adjusted)
        double cache_pressure = effective_cache_pressure(compression_ratio);

        // Weighted blend — request queue is the dominant signal
        double score = 0.7 * std::min(request_pressure / 3.0, 1.0)
                     + 0.3 * cache_pressure;
        return std::min(score, 1.0);
    }
};

}  // namespace ranvier
