// Ranvier Core - Shard Load Metrics
//
// Per-shard load metrics for the shard-aware load balancer.
// Each shard maintains its own metrics (thread-local, lock-free).
// The load balancer queries these metrics to make routing decisions.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include <seastar/core/smp.hh>
#include <seastar/core/future.hh>

namespace ranvier {

// Lightweight snapshot of shard load state
// Used for cross-shard metric collection (must be trivially copyable)
struct ShardLoadSnapshot {
    uint32_t shard_id = 0;
    uint64_t active_requests = 0;      // Currently processing requests
    uint64_t queued_requests = 0;      // Requests waiting for semaphore
    uint64_t total_requests = 0;       // Total requests handled (for rate estimation)
    std::chrono::steady_clock::time_point timestamp;

    // Compute a load score for P2C comparison
    // Lower score = less loaded = preferred
    double load_score() const {
        // Weight active requests more heavily than queued
        // Active requests are consuming CPU; queued are just waiting
        return static_cast<double>(active_requests) * 2.0 +
               static_cast<double>(queued_requests);
    }
};

// Per-shard load metrics tracker
// Thread-local singleton pattern for lock-free access
class ShardLoadMetrics {
public:
    ShardLoadMetrics()
        : _shard_id(seastar::this_shard_id())
        , _active_requests(0)
        , _queued_requests(0)
        , _total_requests(0) {}

    // RAII guard for tracking active requests
    class ActiveRequestGuard {
    public:
        explicit ActiveRequestGuard(ShardLoadMetrics& metrics) : _metrics(metrics) {
            _metrics.increment_active();
        }
        ~ActiveRequestGuard() {
            _metrics.decrement_active();
        }
        // Non-copyable, non-movable
        ActiveRequestGuard(const ActiveRequestGuard&) = delete;
        ActiveRequestGuard& operator=(const ActiveRequestGuard&) = delete;
        ActiveRequestGuard(ActiveRequestGuard&&) = delete;
        ActiveRequestGuard& operator=(ActiveRequestGuard&&) = delete;
    private:
        ShardLoadMetrics& _metrics;
    };

    // RAII guard for tracking queued requests
    class QueuedRequestGuard {
    public:
        explicit QueuedRequestGuard(ShardLoadMetrics& metrics) : _metrics(metrics) {
            _metrics.increment_queued();
        }
        ~QueuedRequestGuard() {
            _metrics.decrement_queued();
        }
        // Non-copyable, non-movable
        QueuedRequestGuard(const QueuedRequestGuard&) = delete;
        QueuedRequestGuard& operator=(const QueuedRequestGuard&) = delete;
        QueuedRequestGuard(QueuedRequestGuard&&) = delete;
        QueuedRequestGuard& operator=(QueuedRequestGuard&&) = delete;
    private:
        ShardLoadMetrics& _metrics;
    };

    // Manual increment/decrement for active requests
    void increment_active() {
        _active_requests.fetch_add(1, std::memory_order_relaxed);
    }
    void decrement_active() {
        _active_requests.fetch_sub(1, std::memory_order_relaxed);
    }

    // Manual increment/decrement for queued requests
    void increment_queued() {
        _queued_requests.fetch_add(1, std::memory_order_relaxed);
    }
    void decrement_queued() {
        _queued_requests.fetch_sub(1, std::memory_order_relaxed);
    }

    // Record a completed request (for throughput tracking)
    void record_request_completed() {
        _total_requests.fetch_add(1, std::memory_order_relaxed);
    }

    // Get current metrics
    uint64_t active_requests() const {
        return _active_requests.load(std::memory_order_relaxed);
    }
    uint64_t queued_requests() const {
        return _queued_requests.load(std::memory_order_relaxed);
    }
    uint64_t total_requests() const {
        return _total_requests.load(std::memory_order_relaxed);
    }
    uint32_t shard_id() const {
        return _shard_id;
    }

    // Create a snapshot for cross-shard communication
    ShardLoadSnapshot snapshot() const {
        return ShardLoadSnapshot{
            _shard_id,
            _active_requests.load(std::memory_order_relaxed),
            _queued_requests.load(std::memory_order_relaxed),
            _total_requests.load(std::memory_order_relaxed),
            std::chrono::steady_clock::now()
        };
    }

private:
    uint32_t _shard_id;
    std::atomic<uint64_t> _active_requests;
    std::atomic<uint64_t> _queued_requests;
    std::atomic<uint64_t> _total_requests;
};

// Thread-local shard load metrics instance
inline thread_local std::unique_ptr<ShardLoadMetrics> g_shard_load_metrics = nullptr;

// Initialize shard load metrics for this shard (call once per shard)
inline void init_shard_load_metrics() {
    if (!g_shard_load_metrics) {
        g_shard_load_metrics = std::make_unique<ShardLoadMetrics>();
    }
}

// Get the shard load metrics instance for this shard
// Rule #3: Null-guard all pointer accessors - lazy initialization ensures never null
inline ShardLoadMetrics& shard_load_metrics() {
    if (!g_shard_load_metrics) {
        g_shard_load_metrics = std::make_unique<ShardLoadMetrics>();
    }
    return *g_shard_load_metrics;
}

// Check if shard load metrics are initialized
inline bool shard_load_metrics_initialized() {
    return g_shard_load_metrics != nullptr;

}

// Cleanup shard load metrics (call during shutdown if needed)
inline void cleanup_shard_load_metrics() {
    g_shard_load_metrics.reset();
}

}  // namespace ranvier
