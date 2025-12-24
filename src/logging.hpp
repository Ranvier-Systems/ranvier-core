#pragma once

#include <seastar/util/log.hh>
#include <seastar/core/smp.hh>

#include <chrono>
#include <atomic>
#include <string>
#include <fmt/format.h>

namespace ranvier {

// Non-blocking request ID generator for distributed tracing
// Format: "req-{shard}-{timestamp_hex}-{sequence_hex}"
// - Shard ID ensures uniqueness across Seastar shards
// - High-resolution timestamp provides temporal ordering
// - Atomic sequence counter ensures uniqueness within same microsecond
inline std::string generate_request_id() {
    // Thread-local state for lock-free operation (Seastar shard-safe)
    static thread_local std::atomic<uint64_t> sequence_counter{0};
    static thread_local auto epoch = std::chrono::steady_clock::now();

    auto now = std::chrono::steady_clock::now();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
        now - epoch).count();
    auto seq = sequence_counter.fetch_add(1, std::memory_order_relaxed);

    return fmt::format("req-{}-{:012x}-{:06x}",
                       seastar::this_shard_id(), micros, seq & 0xFFFFFF);
}

// Extract request ID from incoming HTTP headers
// Checks X-Request-ID first, then X-Correlation-ID
// Returns empty string if neither header is present
inline std::string extract_request_id(const auto& headers) {
    // Check X-Request-ID first
    auto req_id_it = headers.find("X-Request-ID");
    if (req_id_it != headers.end() && !req_id_it->second.empty()) {
        return std::string(req_id_it->second);
    }

    // Fall back to X-Correlation-ID
    auto corr_id_it = headers.find("X-Correlation-ID");
    if (corr_id_it != headers.end() && !corr_id_it->second.empty()) {
        return std::string(corr_id_it->second);
    }

    return "";
}

// Ranvier loggers - one per component for fine-grained control
//
// Log levels (from seastar/util/log.hh):
//   trace, debug, info, warn, error
//
// Usage:
//   ranvier::log_router.info("Cache hit for backend {}", backend_id);
//   ranvier::log_proxy.debug("Forwarding {} bytes", size);
//   ranvier::log_health.warn("Backend {} is DOWN", id);

// Main application logger
inline seastar::logger log_main("ranvier");

// Router/routing decisions
inline seastar::logger log_router("ranvier.router");

// HTTP proxy operations
inline seastar::logger log_proxy("ranvier.proxy");

// Health check service
inline seastar::logger log_health("ranvier.health");

// Control plane operations
inline seastar::logger log_control("ranvier.control");

// Connection pool
inline seastar::logger log_pool("ranvier.pool");

} // namespace ranvier
