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

#include "config.hpp"
#include "logging.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

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

// Crypto operation statistics
struct CryptoOpStats {
    uint64_t total_ops = 0;              // Total crypto operations
    uint64_t offloaded_ops = 0;          // Operations offloaded to thread pool
    uint64_t inline_ops = 0;             // Operations run inline on reactor
    uint64_t stalls_avoided = 0;         // Offloads that avoided potential stalls
    uint64_t stall_warnings = 0;         // Operations that exceeded stall threshold
    uint64_t handshakes_offloaded = 0;   // Handshake operations offloaded
    uint64_t symmetric_ops_inline = 0;   // Symmetric ops run inline
    uint64_t thread_pool_queue_depth = 0; // Current queue depth
    uint64_t thread_pool_peak_depth = 0; // Peak queue depth seen
};

// Configuration for the crypto offloader
struct CryptoOffloaderConfig {
    // Number of worker threads in the pool
    size_t thread_pool_size = 2;

    // Threshold for offloading based on data size (bytes)
    // Operations on data larger than this are more likely to be offloaded
    size_t size_threshold_bytes = 1024;

    // Threshold for considering an operation a "stall" (microseconds)
    // Operations exceeding this duration trigger stall warnings
    uint64_t stall_threshold_us = 500;

    // Threshold for predicted latency to trigger offloading (microseconds)
    // If predicted latency exceeds this, operation is offloaded
    uint64_t offload_latency_threshold_us = 100;

    // Maximum queue depth before rejecting new operations
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

// Forward declaration
class CryptoOffloader;

// Result holder for async crypto operations
template<typename T>
class CryptoOpResult {
public:
    CryptoOpResult() = default;
    explicit CryptoOpResult(T value) : _value(std::move(value)), _has_value(true) {}

    bool has_value() const { return _has_value; }
    T& value() { return _value; }
    const T& value() const { return _value; }

    void set_value(T value) {
        _value = std::move(value);
        _has_value = true;
    }

    void set_error(std::string error) {
        _error = std::move(error);
        _has_error = true;
    }

    bool has_error() const { return _has_error; }
    const std::string& error() const { return _error; }

private:
    T _value{};
    std::string _error;
    bool _has_value = false;
    bool _has_error = false;
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

    // Check if the offloader is running
    bool is_running() const { return _running.load(std::memory_order_acquire); }

    // Wrap a crypto operation with adaptive offloading
    //
    // The operation will be executed either:
    // - Inline on the reactor thread (for fast symmetric ops)
    // - On a background thread (for slow asymmetric ops or large data)
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

    // Wrap a crypto operation that returns void
    template<typename Func>
    seastar::future<> wrap_crypto_op_void(
        CryptoOpType op_type,
        size_t data_size,
        Func&& func);

    // Get current statistics (thread-safe snapshot)
    CryptoOpStats get_stats() const;

    // Get current queue depth
    size_t queue_depth() const { return _queue_depth.load(std::memory_order_relaxed); }

    // Predict whether an operation should be offloaded
    // Returns true if the operation should be offloaded to thread pool
    bool should_offload(CryptoOpType op_type, size_t data_size) const;

    // Register metrics with Seastar metrics system
    void register_metrics(seastar::metrics::metric_groups& metrics);

private:
    CryptoOffloaderConfig _config;
    std::atomic<bool> _running{false};
    std::atomic<bool> _stopping{false};

    // Metrics for tracking in-flight async operations
    std::atomic<size_t> _queue_depth{0};

    // Statistics (atomic for thread-safe updates)
    mutable std::atomic<uint64_t> _total_ops{0};
    mutable std::atomic<uint64_t> _offloaded_ops{0};
    mutable std::atomic<uint64_t> _inline_ops{0};
    mutable std::atomic<uint64_t> _stalls_avoided{0};
    mutable std::atomic<uint64_t> _stall_warnings{0};
    mutable std::atomic<uint64_t> _handshakes_offloaded{0};
    mutable std::atomic<uint64_t> _symmetric_ops_inline{0};
    mutable std::atomic<size_t> _peak_queue_depth{0};

    // Worker thread function (not used in seastar::async mode)
    void worker_loop();

    // Submit work using seastar::async
    // Returns a future that resolves when the work is complete
    seastar::future<> submit_work(std::function<void()> task);

    // Estimate operation latency in microseconds
    uint64_t estimate_latency_us(CryptoOpType op_type, size_t data_size) const;

    // Update peak queue depth
    void update_peak_depth();
};

//------------------------------------------------------------------------------
// Template Implementation
//------------------------------------------------------------------------------

template<typename Func>
seastar::future<std::invoke_result_t<Func>> CryptoOffloader::wrap_crypto_op(
    CryptoOpType op_type,
    size_t data_size,
    Func&& func) {

    using ResultType = std::invoke_result_t<Func>;

    ++_total_ops;

    // Check if we should offload this operation
    if (!should_offload(op_type, data_size)) {
        // Run inline on reactor thread with timing
        ++_inline_ops;

        if (op_type == CryptoOpType::SYMMETRIC_ENCRYPT ||
            op_type == CryptoOpType::SYMMETRIC_DECRYPT) {
            ++_symmetric_ops_inline;
        }

        auto start = std::chrono::steady_clock::now();

        try {
            ResultType result = func();

            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (static_cast<uint64_t>(elapsed_us) > _config.stall_threshold_us) {
                ++_stall_warnings;
                log_crypto_offloader.warn(
                    "Inline crypto op exceeded stall threshold: {}μs (threshold: {}μs, type: {}, size: {})",
                    elapsed_us, _config.stall_threshold_us, static_cast<int>(op_type), data_size);
            }

            return seastar::make_ready_future<ResultType>(std::move(result));
        } catch (...) {
            return seastar::make_exception_future<ResultType>(std::current_exception());
        }
    }

    // Offload to thread pool
    ++_offloaded_ops;

    if (op_type == CryptoOpType::HANDSHAKE_INITIATE ||
        op_type == CryptoOpType::HANDSHAKE_CONTINUE) {
        ++_handshakes_offloaded;
    }

    // Create promise/future pair for result
    auto result_holder = seastar::make_lw_shared<CryptoOpResult<ResultType>>();
    auto exception_holder = seastar::make_lw_shared<std::exception_ptr>();

    // Capture function by value for thread safety
    auto captured_func = std::forward<Func>(func);

    auto start_time = std::chrono::steady_clock::now();
    uint64_t stall_threshold = _config.stall_threshold_us;
    std::atomic<uint64_t>* stalls_avoided_ptr = &_stalls_avoided;

    return submit_work([result_holder, exception_holder, captured_func = std::move(captured_func),
                       start_time, stall_threshold, stalls_avoided_ptr]() mutable {
        try {
            ResultType result = captured_func();
            result_holder->set_value(std::move(result));

            // Check if we avoided a stall (operation took longer than threshold)
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start_time).count();

            if (static_cast<uint64_t>(elapsed_us) > stall_threshold) {
                ++(*stalls_avoided_ptr);
            }
        } catch (...) {
            *exception_holder = std::current_exception();
        }
    }).then([result_holder, exception_holder]() -> seastar::future<ResultType> {
        if (*exception_holder) {
            return seastar::make_exception_future<ResultType>(*exception_holder);
        }
        if (result_holder->has_error()) {
            return seastar::make_exception_future<ResultType>(
                std::runtime_error(result_holder->error()));
        }
        return seastar::make_ready_future<ResultType>(std::move(result_holder->value()));
    });
}

template<typename Func>
seastar::future<> CryptoOffloader::wrap_crypto_op_void(
    CryptoOpType op_type,
    size_t data_size,
    Func&& func) {

    ++_total_ops;

    // Check if we should offload this operation
    if (!should_offload(op_type, data_size)) {
        // Run inline on reactor thread with timing
        ++_inline_ops;

        if (op_type == CryptoOpType::SYMMETRIC_ENCRYPT ||
            op_type == CryptoOpType::SYMMETRIC_DECRYPT) {
            ++_symmetric_ops_inline;
        }

        auto start = std::chrono::steady_clock::now();

        try {
            func();

            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (static_cast<uint64_t>(elapsed_us) > _config.stall_threshold_us) {
                ++_stall_warnings;
                log_crypto_offloader.warn(
                    "Inline crypto op (void) exceeded stall threshold: {}μs (threshold: {}μs, type: {}, size: {})",
                    elapsed_us, _config.stall_threshold_us, static_cast<int>(op_type), data_size);
            }

            return seastar::make_ready_future<>();
        } catch (...) {
            return seastar::make_exception_future<>(std::current_exception());
        }
    }

    // Offload to thread pool
    ++_offloaded_ops;

    if (op_type == CryptoOpType::HANDSHAKE_INITIATE ||
        op_type == CryptoOpType::HANDSHAKE_CONTINUE) {
        ++_handshakes_offloaded;
    }

    auto exception_holder = seastar::make_lw_shared<std::exception_ptr>();
    auto captured_func = std::forward<Func>(func);

    auto start_time = std::chrono::steady_clock::now();
    uint64_t stall_threshold = _config.stall_threshold_us;
    std::atomic<uint64_t>* stalls_avoided_ptr = &_stalls_avoided;

    return submit_work([exception_holder, captured_func = std::move(captured_func),
                       start_time, stall_threshold, stalls_avoided_ptr]() mutable {
        try {
            captured_func();

            // Check if we avoided a stall
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start_time).count();

            if (static_cast<uint64_t>(elapsed_us) > stall_threshold) {
                ++(*stalls_avoided_ptr);
            }
        } catch (...) {
            *exception_holder = std::current_exception();
        }
    }).then([exception_holder]() -> seastar::future<> {
        if (*exception_holder) {
            return seastar::make_exception_future<>(*exception_holder);
        }
        return seastar::make_ready_future<>();
    });
}

}  // namespace ranvier
