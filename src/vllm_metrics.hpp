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

    // Compute a 0.0–1.0 load score for routing decisions.
    // Composite of request queue depth and KV cache pressure.
    double load_score() const {
        if (!valid) return 0.0;

        // Primary signal: request saturation
        // (running + waiting) / (running + 1) — ratio > 1.0 means queuing
        double request_pressure = (num_requests_running + num_requests_waiting > 0)
            ? static_cast<double>(num_requests_running + num_requests_waiting)
              / static_cast<double>(num_requests_running + 1)
            : 0.0;

        // Secondary signal: KV cache pressure
        double cache_pressure = gpu_cache_usage_percent;

        // Weighted blend — request queue is the dominant signal
        double score = 0.7 * std::min(request_pressure / 3.0, 1.0)
                     + 0.3 * cache_pressure;
        return std::min(score, 1.0);
    }
};

}  // namespace ranvier
