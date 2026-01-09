// Ranvier Core - Adaptive Crypto Offloader Implementation
//
// Thread pool-based offloading for expensive cryptographic operations

#include "crypto_offloader.hpp"

#include <algorithm>

#include <seastar/core/reactor.hh>
#include <seastar/core/future-util.hh>

namespace ranvier {

CryptoOffloader::CryptoOffloader(const CryptoOffloaderConfig& config)
    : _config(config) {
}

CryptoOffloader::~CryptoOffloader() {
    stop();
}

void CryptoOffloader::start() {
    if (_running.exchange(true, std::memory_order_acq_rel)) {
        return;  // Already running
    }

    if (!_config.enabled) {
        log_crypto_offloader.info("Crypto offloader disabled");
        return;
    }

    log_crypto_offloader.info("Starting crypto offloader with {} worker threads", _config.thread_pool_size);
    log_crypto_offloader.info("Offload thresholds: size={}B, latency={}μs, stall={}μs",
                              _config.size_threshold_bytes,
                              _config.offload_latency_threshold_us,
                              _config.stall_threshold_us);

    // Create worker threads
    _workers.reserve(_config.thread_pool_size);
    for (size_t i = 0; i < _config.thread_pool_size; ++i) {
        _workers.emplace_back([this]() {
            worker_loop();
        });
    }

    log_crypto_offloader.info("Crypto offloader started");
}

void CryptoOffloader::stop() {
    if (!_running.exchange(false, std::memory_order_acq_rel)) {
        return;  // Already stopped
    }

    log_crypto_offloader.info("Stopping crypto offloader...");

    // Signal workers to stop
    _stopping.store(true, std::memory_order_release);

    // Wake up all workers
    _queue_cv.notify_all();

    // Wait for workers to finish
    for (auto& worker : _workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    _workers.clear();

    // Clear any remaining work items
    {
        std::lock_guard<std::mutex> lock(_queue_mutex);
        while (!_work_queue.empty()) {
            auto& item = _work_queue.front();
            // Complete with broken promise (will throw on the reactor side)
            item.completion.set_exception(std::runtime_error("Crypto offloader stopped"));
            _work_queue.pop();
        }
    }

    _stopping.store(false, std::memory_order_release);
    log_crypto_offloader.info("Crypto offloader stopped");
}

void CryptoOffloader::worker_loop() {
    log_crypto_offloader.debug("Worker thread started");

    while (true) {
        WorkItem item;

        {
            std::unique_lock<std::mutex> lock(_queue_mutex);

            // Wait for work or stop signal
            _queue_cv.wait(lock, [this] {
                return _stopping.load(std::memory_order_acquire) || !_work_queue.empty();
            });

            // Check stop condition
            if (_stopping.load(std::memory_order_acquire) && _work_queue.empty()) {
                break;
            }

            if (_work_queue.empty()) {
                continue;
            }

            // Get work item
            item = std::move(_work_queue.front());
            _work_queue.pop();
            _queue_depth.fetch_sub(1, std::memory_order_relaxed);
        }

        // Execute the task outside the lock
        try {
            item.task();
            // Signal completion on the reactor thread
            // We use submit_to() to safely complete the promise from a foreign thread
            seastar::alien::submit_to(seastar::engine().alien(), 0, [p = std::move(item.completion)]() mutable {
                p.set_value();
            });
        } catch (...) {
            auto ex = std::current_exception();
            seastar::alien::submit_to(seastar::engine().alien(), 0, [p = std::move(item.completion), ex]() mutable {
                p.set_exception(ex);
            });
        }
    }

    log_crypto_offloader.debug("Worker thread exiting");
}

seastar::future<> CryptoOffloader::submit_work(std::function<void()> task) {
    // Check if we're running
    if (!_running.load(std::memory_order_acquire) || !_config.enabled) {
        // Fall back to running inline if offloader not running
        try {
            task();
            return seastar::make_ready_future<>();
        } catch (...) {
            return seastar::make_exception_future<>(std::current_exception());
        }
    }

    // Check queue depth
    size_t current_depth = _queue_depth.load(std::memory_order_relaxed);
    if (current_depth >= _config.max_queue_depth) {
        log_crypto_offloader.warn("Crypto offloader queue full ({}/{}), running inline",
                                  current_depth, _config.max_queue_depth);
        try {
            task();
            return seastar::make_ready_future<>();
        } catch (...) {
            return seastar::make_exception_future<>(std::current_exception());
        }
    }

    // Create work item with promise
    WorkItem item;
    item.task = std::move(task);
    auto fut = item.completion.get_future();

    {
        std::lock_guard<std::mutex> lock(_queue_mutex);
        _work_queue.push(std::move(item));
        _queue_depth.fetch_add(1, std::memory_order_relaxed);
        update_peak_depth();
    }

    // Wake up a worker
    _queue_cv.notify_one();

    return fut;
}

bool CryptoOffloader::should_offload(CryptoOpType op_type, size_t data_size) const {
    // If disabled, never offload
    if (!_config.enabled) {
        return false;
    }

    // If not running, can't offload
    if (!_running.load(std::memory_order_acquire)) {
        return false;
    }

    // Force offload mode (for testing)
    if (_config.force_offload) {
        return true;
    }

    // Handshake operations are always expensive (RSA/ECDH key exchange)
    // They typically take 1-10ms which far exceeds reactor task quota
    if (_config.handshake_always_offload) {
        if (op_type == CryptoOpType::HANDSHAKE_INITIATE ||
            op_type == CryptoOpType::HANDSHAKE_CONTINUE) {
            return true;
        }
    }

    // Symmetric operations on small data are fast (AES-GCM is ~1-5μs for <1KB)
    if (_config.symmetric_always_inline) {
        if ((op_type == CryptoOpType::SYMMETRIC_ENCRYPT ||
             op_type == CryptoOpType::SYMMETRIC_DECRYPT) &&
            data_size <= _config.size_threshold_bytes) {
            return false;
        }
    }

    // Use latency estimation for other cases
    uint64_t estimated_latency = estimate_latency_us(op_type, data_size);
    return estimated_latency > _config.offload_latency_threshold_us;
}

uint64_t CryptoOffloader::estimate_latency_us(CryptoOpType op_type, size_t data_size) const {
    // Latency estimates based on typical OpenSSL performance
    // These are conservative estimates for modern hardware

    switch (op_type) {
        case CryptoOpType::SYMMETRIC_ENCRYPT:
        case CryptoOpType::SYMMETRIC_DECRYPT:
            // AES-256-GCM: ~1GB/s on modern CPUs
            // So 1KB takes ~1μs, 1MB takes ~1ms
            // Add 5μs baseline for function call overhead
            return 5 + (data_size / 1024);  // ~1μs per KB

        case CryptoOpType::HANDSHAKE_INITIATE:
            // Initial handshake involves RSA/ECDHE key generation
            // RSA-2048: ~1-2ms, ECDHE P-256: ~0.5ms
            // Conservative estimate: 2000μs
            return 2000;

        case CryptoOpType::HANDSHAKE_CONTINUE:
            // Handshake continuation involves signature verification
            // RSA-2048 verify: ~0.1ms, ECDSA P-256 verify: ~0.2ms
            // Plus potential certificate chain validation
            // Conservative estimate: 500μs
            return 500;

        case CryptoOpType::UNKNOWN:
        default:
            // Unknown operations: use size-based heuristic
            // Assume worst case of 10μs per KB
            return 10 + (data_size * 10 / 1024);
    }
}

void CryptoOffloader::update_peak_depth() {
    size_t current = _queue_depth.load(std::memory_order_relaxed);
    size_t peak = _peak_queue_depth.load(std::memory_order_relaxed);

    while (current > peak) {
        if (_peak_queue_depth.compare_exchange_weak(peak, current, std::memory_order_relaxed)) {
            break;
        }
    }
}

CryptoOpStats CryptoOffloader::get_stats() const {
    CryptoOpStats stats;
    stats.total_ops = _total_ops.load(std::memory_order_relaxed);
    stats.offloaded_ops = _offloaded_ops.load(std::memory_order_relaxed);
    stats.inline_ops = _inline_ops.load(std::memory_order_relaxed);
    stats.stalls_avoided = _stalls_avoided.load(std::memory_order_relaxed);
    stats.stall_warnings = _stall_warnings.load(std::memory_order_relaxed);
    stats.handshakes_offloaded = _handshakes_offloaded.load(std::memory_order_relaxed);
    stats.symmetric_ops_inline = _symmetric_ops_inline.load(std::memory_order_relaxed);
    stats.thread_pool_queue_depth = _queue_depth.load(std::memory_order_relaxed);
    stats.thread_pool_peak_depth = _peak_queue_depth.load(std::memory_order_relaxed);
    return stats;
}

void CryptoOffloader::register_metrics(seastar::metrics::metric_groups& metrics) {
    metrics.add_group("ranvier", {
        seastar::metrics::make_counter(
            "crypto_offloader_total_ops",
            [this] { return _total_ops.load(std::memory_order_relaxed); },
            seastar::metrics::description("Total crypto operations processed")),

        seastar::metrics::make_counter(
            "crypto_offloader_offloaded_ops",
            [this] { return _offloaded_ops.load(std::memory_order_relaxed); },
            seastar::metrics::description("Crypto operations offloaded to thread pool")),

        seastar::metrics::make_counter(
            "crypto_offloader_inline_ops",
            [this] { return _inline_ops.load(std::memory_order_relaxed); },
            seastar::metrics::description("Crypto operations run inline on reactor")),

        seastar::metrics::make_counter(
            "crypto_offloader_stalls_avoided",
            [this] { return _stalls_avoided.load(std::memory_order_relaxed); },
            seastar::metrics::description("Reactor stalls avoided by offloading (ops that exceeded threshold)")),

        seastar::metrics::make_counter(
            "crypto_offloader_stall_warnings",
            [this] { return _stall_warnings.load(std::memory_order_relaxed); },
            seastar::metrics::description("Inline operations that exceeded stall threshold")),

        seastar::metrics::make_counter(
            "crypto_offloader_handshakes_offloaded",
            [this] { return _handshakes_offloaded.load(std::memory_order_relaxed); },
            seastar::metrics::description("DTLS handshake operations offloaded")),

        seastar::metrics::make_counter(
            "crypto_offloader_symmetric_inline",
            [this] { return _symmetric_ops_inline.load(std::memory_order_relaxed); },
            seastar::metrics::description("Symmetric encryption operations run inline")),

        seastar::metrics::make_gauge(
            "crypto_offloader_queue_depth",
            [this] { return static_cast<double>(_queue_depth.load(std::memory_order_relaxed)); },
            seastar::metrics::description("Current thread pool queue depth")),

        seastar::metrics::make_gauge(
            "crypto_offloader_queue_peak",
            [this] { return static_cast<double>(_peak_queue_depth.load(std::memory_order_relaxed)); },
            seastar::metrics::description("Peak thread pool queue depth")),

        seastar::metrics::make_gauge(
            "crypto_offloader_enabled",
            [this] { return _config.enabled && _running.load(std::memory_order_relaxed) ? 1.0 : 0.0; },
            seastar::metrics::description("Whether crypto offloader is enabled and running"))
    });
}

}  // namespace ranvier
