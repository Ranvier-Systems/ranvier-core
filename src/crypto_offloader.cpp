// Ranvier Core - Adaptive Crypto Offloader Implementation
//
// Uses seastar::async for offloading expensive cryptographic operations
// to avoid reactor stalls while maintaining Seastar's cooperative model.

#include "crypto_offloader.hpp"

#include <algorithm>

#include <seastar/core/thread.hh>
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

    log_crypto_offloader.info("Starting crypto offloader (seastar::async mode)");
    log_crypto_offloader.info("Offload thresholds: size={}B, latency={}μs, stall={}μs",
                              _config.size_threshold_bytes,
                              _config.offload_latency_threshold_us,
                              _config.stall_threshold_us);

    // No thread pool needed - we use seastar::async which runs in Seastar's
    // internal thread pool and integrates with the cooperative scheduler
    log_crypto_offloader.info("Crypto offloader started");
}

void CryptoOffloader::stop() {
    if (!_running.exchange(false, std::memory_order_acq_rel)) {
        return;  // Already stopped
    }

    log_crypto_offloader.info("Stopping crypto offloader...");

    // No cleanup needed - seastar::async futures are managed by Seastar
    _stopping.store(true, std::memory_order_release);

    log_crypto_offloader.info("Crypto offloader stopped");
}

void CryptoOffloader::worker_loop() {
    // Not used in seastar::async mode
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

    // Track queue depth for metrics
    _queue_depth.fetch_add(1, std::memory_order_relaxed);
    update_peak_depth();

    // Use seastar::async to run the task in a Seastar thread
    // This allows blocking operations without stalling the reactor
    return seastar::async([this, task = std::move(task)]() mutable {
        try {
            task();
        } catch (...) {
            // Re-throw to propagate to the future
            throw;
        }
    }).finally([this] {
        _queue_depth.fetch_sub(1, std::memory_order_relaxed);
    });
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
            seastar::metrics::description("Crypto operations offloaded to seastar::async")),

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
            seastar::metrics::description("Current number of in-flight async operations")),

        seastar::metrics::make_gauge(
            "crypto_offloader_queue_peak",
            [this] { return static_cast<double>(_peak_queue_depth.load(std::memory_order_relaxed)); },
            seastar::metrics::description("Peak number of in-flight async operations")),

        seastar::metrics::make_gauge(
            "crypto_offloader_enabled",
            [this] { return _config.enabled && _running.load(std::memory_order_relaxed) ? 1.0 : 0.0; },
            seastar::metrics::description("Whether crypto offloader is enabled and running"))
    });
}

}  // namespace ranvier
