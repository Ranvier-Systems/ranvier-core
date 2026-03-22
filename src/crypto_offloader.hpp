// Ranvier Core - Adaptive Crypto Offloader
//
// Provides adaptive offloading of cryptographic operations using seastar::async
// to maintain sub-millisecond reactor responsiveness during heavy cluster operations.
//
// Key features:
// - Uses seastar::async for CPU-intensive crypto work
// - Threshold-based offloading: symmetric ops run on-shard, asymmetric ops offload
// - Non-blocking futures that resolve when background work completes
// - Stall tracking metrics to measure offloading effectiveness
//
// Design rationale:
// OpenSSL RSA/ECDH handshakes can take 1-10ms, far exceeding Seastar's 0.5ms task quota.
// AES-GCM symmetric operations are typically 1-50μs for small packets.
// This class adaptively routes operations based on predicted latency.

#pragma once

#include "logging.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <type_traits>

#include <seastar/core/future.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/shared_ptr.hh>

namespace ranvier {

// Logger for crypto offloader
inline seastar::logger log_crypto_offloader("ranvier.crypto.offloader");

// Crypto operation type - determines offloading strategy
enum class CryptoOpType : uint8_t {
    // Symmetric operations (AES-GCM) - fast, typically run on-shard
    SYMMETRIC_ENCRYPT,
    SYMMETRIC_DECRYPT,

    // Asymmetric operations (RSA/ECDH) - slow, always offload
    HANDSHAKE_INITIATE,      // SSL_connect initial
    HANDSHAKE_CONTINUE,      // SSL_do_handshake continuation

    // Mixed/unknown - use size-based heuristics
    UNKNOWN
};

// Convert CryptoOpType to string for logging
inline const char* crypto_op_type_name(CryptoOpType type) {
    switch (type) {
        case CryptoOpType::SYMMETRIC_ENCRYPT: return "symmetric_encrypt";
        case CryptoOpType::SYMMETRIC_DECRYPT: return "symmetric_decrypt";
        case CryptoOpType::HANDSHAKE_INITIATE: return "handshake_initiate";
        case CryptoOpType::HANDSHAKE_CONTINUE: return "handshake_continue";
        case CryptoOpType::UNKNOWN: return "unknown";
        default: return "invalid";
    }
}

// Crypto operation statistics
struct CryptoOpStats {
    uint64_t total_ops = 0;              // Total crypto operations
    uint64_t offloaded_ops = 0;          // Operations offloaded to seastar::async
    uint64_t inline_ops = 0;             // Operations run inline on reactor
    uint64_t stalls_avoided = 0;         // Offloads that avoided potential stalls
    uint64_t stall_warnings = 0;         // Operations that exceeded stall threshold
    uint64_t handshakes_offloaded = 0;   // Handshake operations offloaded
    uint64_t symmetric_ops_inline = 0;   // Symmetric ops run inline
    uint64_t queue_depth = 0;            // Current in-flight async ops
    uint64_t queue_peak = 0;             // Peak in-flight async ops
    uint64_t queue_rejected = 0;         // Operations rejected due to queue limit
};

// Configuration for the crypto offloader
struct CryptoOffloaderConfig {
    // Threshold for offloading based on data size (bytes)
    // Operations on data larger than this are more likely to be offloaded
    size_t size_threshold_bytes = 1024;

    // Threshold for considering an operation a "stall" (microseconds)
    // Operations exceeding this duration trigger stall warnings
    uint64_t stall_threshold_us = 500;

    // Threshold for predicted latency to trigger offloading (microseconds)
    // If predicted latency exceeds this, operation is offloaded
    uint64_t offload_latency_threshold_us = 100;

    // Maximum number of in-flight async operations
    // New operations are run inline when this limit is reached
    size_t max_queue_depth = 1024;

    // Enable/disable adaptive offloading
    bool enabled = true;

    // Force offload all operations (for testing/debugging)
    bool force_offload = false;

    // Always run symmetric operations inline (override size threshold)
    bool symmetric_always_inline = true;

    // Always offload handshake operations
    bool handshake_always_offload = true;
};

// Adaptive Crypto Offloader
//
// Uses seastar::async for offloading expensive crypto operations.
// Adaptive logic decides whether to run operations on the reactor thread
// or offload them to a Seastar thread context.
class CryptoOffloader {
public:
    explicit CryptoOffloader(const CryptoOffloaderConfig& config = CryptoOffloaderConfig{});
    ~CryptoOffloader();

    // Non-copyable, non-movable
    CryptoOffloader(const CryptoOffloader&) = delete;
    CryptoOffloader& operator=(const CryptoOffloader&) = delete;
    CryptoOffloader(CryptoOffloader&&) = delete;
    CryptoOffloader& operator=(CryptoOffloader&&) = delete;

    // Start the offloader (must be called before any operations)
    void start();

    // Stop the offloader
    void stop();

    // Check if the offloader is running and enabled
    bool is_running() const {
        return _running.load(std::memory_order_acquire) && _config.enabled;
    }

    // Wrap a crypto operation with adaptive offloading
    //
    // The operation will be executed either:
    // - Inline on the reactor thread (for fast symmetric ops)
    // - In a seastar::async context (for slow asymmetric ops or large data)
    //
    // Returns a future that resolves when the operation completes.
    //
    // Example usage:
    //   auto result = co_await offloader.wrap_crypto_op(
    //       CryptoOpType::SYMMETRIC_ENCRYPT,
    //       data.size(),
    //       [&]() { return session->encrypt(data); }
    //   );
    template<typename Func>
    seastar::future<std::invoke_result_t<Func>> wrap_crypto_op(
        CryptoOpType op_type,
        size_t data_size,
        Func&& func);

    // Get current statistics (thread-safe snapshot)
    CryptoOpStats get_stats() const;

    // Get current queue depth
    size_t queue_depth() const { return _queue_depth.load(std::memory_order_relaxed); }

    // Predict whether an operation should be offloaded
    // Returns true if the operation should be offloaded
    bool should_offload(CryptoOpType op_type, size_t data_size) const;

    // Register metrics with Seastar metrics system
    void register_metrics(seastar::metrics::metric_groups& metrics);

    // Get the configuration
    const CryptoOffloaderConfig& config() const { return _config; }

private:
    CryptoOffloaderConfig _config;
    std::atomic<bool> _running{false};

    // Metrics for tracking in-flight async operations
    std::atomic<size_t> _queue_depth{0};
    std::atomic<size_t> _peak_queue_depth{0};

    // Statistics (atomic for thread-safe updates)
    mutable std::atomic<uint64_t> _total_ops{0};
    mutable std::atomic<uint64_t> _offloaded_ops{0};
    mutable std::atomic<uint64_t> _inline_ops{0};
    mutable std::atomic<uint64_t> _stalls_avoided{0};
    mutable std::atomic<uint64_t> _stall_warnings{0};
    mutable std::atomic<uint64_t> _handshakes_offloaded{0};
    mutable std::atomic<uint64_t> _symmetric_ops_inline{0};
    mutable std::atomic<uint64_t> _queue_rejected{0};

    // Estimate operation latency in microseconds
    uint64_t estimate_latency_us(CryptoOpType op_type, size_t data_size) const;

    // Update peak queue depth atomically
    void update_peak_depth();

    // Execute operation inline with timing instrumentation
    template<typename Func>
    seastar::future<std::invoke_result_t<Func>> execute_inline(
        CryptoOpType op_type,
        size_t data_size,
        Func&& func);

    // Execute operation in seastar::async context
    template<typename Func>
    seastar::future<std::invoke_result_t<Func>> execute_offloaded(
        CryptoOpType op_type,
        Func&& func);
};

//------------------------------------------------------------------------------
// Template Implementation
//------------------------------------------------------------------------------

template<typename Func>
seastar::future<std::invoke_result_t<Func>> CryptoOffloader::execute_inline(
    CryptoOpType op_type,
    size_t data_size,
    Func&& func) {

    using ResultType = std::invoke_result_t<Func>;

    ++_inline_ops;

    if (op_type == CryptoOpType::SYMMETRIC_ENCRYPT ||
        op_type == CryptoOpType::SYMMETRIC_DECRYPT) {
        ++_symmetric_ops_inline;
    }

    auto start = std::chrono::steady_clock::now();

    try {
        if constexpr (std::is_void_v<ResultType>) {
            func();

            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (static_cast<uint64_t>(elapsed_us) > _config.stall_threshold_us) {
                ++_stall_warnings;
                log_crypto_offloader.warn(
                    "Inline crypto op exceeded stall threshold: {}μs (threshold: {}μs, type: {}, size: {})",
                    elapsed_us, _config.stall_threshold_us, crypto_op_type_name(op_type), data_size);
            }

            return seastar::make_ready_future<>();
        } else {
            ResultType result = func();

            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (static_cast<uint64_t>(elapsed_us) > _config.stall_threshold_us) {
                ++_stall_warnings;
                log_crypto_offloader.warn(
                    "Inline crypto op exceeded stall threshold: {}μs (threshold: {}μs, type: {}, size: {})",
                    elapsed_us, _config.stall_threshold_us, crypto_op_type_name(op_type), data_size);
            }

            return seastar::make_ready_future<ResultType>(std::move(result));
        }
    } catch (...) {
        if constexpr (std::is_void_v<ResultType>) {
            return seastar::make_exception_future<>(std::current_exception());
        } else {
            return seastar::make_exception_future<ResultType>(std::current_exception());
        }
    }
}

template<typename Func>
seastar::future<std::invoke_result_t<Func>> CryptoOffloader::execute_offloaded(
    CryptoOpType op_type,
    Func&& func) {

    using ResultType = std::invoke_result_t<Func>;

    ++_offloaded_ops;

    if (op_type == CryptoOpType::HANDSHAKE_INITIATE ||
        op_type == CryptoOpType::HANDSHAKE_CONTINUE) {
        ++_handshakes_offloaded;
    }

    // Check queue depth limit
    size_t current_depth = _queue_depth.fetch_add(1, std::memory_order_relaxed);
    if (current_depth >= _config.max_queue_depth) {
        _queue_depth.fetch_sub(1, std::memory_order_relaxed);
        ++_queue_rejected;
        --_offloaded_ops;  // Correct the count since we're not actually offloading

        log_crypto_offloader.warn(
            "Crypto offloader queue full ({}/{}), running inline",
            current_depth, _config.max_queue_depth);

        // Fall back to inline execution
        ++_inline_ops;
        try {
            if constexpr (std::is_void_v<ResultType>) {
                func();
                return seastar::make_ready_future<>();
            } else {
                return seastar::make_ready_future<ResultType>(func());
            }
        } catch (...) {
            if constexpr (std::is_void_v<ResultType>) {
                return seastar::make_exception_future<>(std::current_exception());
            } else {
                return seastar::make_exception_future<ResultType>(std::current_exception());
            }
        }
    }

    update_peak_depth();

    auto start_time = std::chrono::steady_clock::now();
    uint64_t stall_threshold = _config.stall_threshold_us;
    auto* stalls_avoided_ptr = &_stalls_avoided;
    auto* queue_depth_ptr = &_queue_depth;

    // Use seastar::async to run the operation in a Seastar thread context
    return seastar::async([func = std::forward<Func>(func),
                          start_time, stall_threshold, stalls_avoided_ptr]() mutable -> ResultType {
        if constexpr (std::is_void_v<ResultType>) {
            func();

            // Check if we avoided a stall
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (static_cast<uint64_t>(elapsed_us) > stall_threshold) {
                stalls_avoided_ptr->fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            ResultType result = func();

            // Check if we avoided a stall
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (static_cast<uint64_t>(elapsed_us) > stall_threshold) {
                stalls_avoided_ptr->fetch_add(1, std::memory_order_relaxed);
            }

            return result;
        }
    }).finally([queue_depth_ptr] {
        queue_depth_ptr->fetch_sub(1, std::memory_order_relaxed);
    });
}

template<typename Func>
seastar::future<std::invoke_result_t<Func>> CryptoOffloader::wrap_crypto_op(
    CryptoOpType op_type,
    size_t data_size,
    Func&& func) {

    ++_total_ops;

    // Check if we should offload this operation
    if (!should_offload(op_type, data_size)) {
        return execute_inline(op_type, data_size, std::forward<Func>(func));
    }

    return execute_offloaded(op_type, std::forward<Func>(func));
}

}  // namespace ranvier
