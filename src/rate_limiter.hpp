// Ranvier Core - Rate Limiter (Seastar Service)
//
// Full Seastar service wrapper around BasicRateLimiter.
// Adds cleanup timer, gate lifecycle, Prometheus metrics, and logging.
//
// Pure algorithm lives in rate_limiter_core.hpp (no Seastar dependencies).
// Unit tests include rate_limiter_core.hpp directly.
//
// Hard Rule #5 compliance: Timer callbacks use gate guards to prevent
// use-after-free during shutdown. stop() closes gate before cancelling timer.

#pragma once

#include "rate_limiter_core.hpp"

#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/core/timer.hh>
#include <seastar/util/log.hh>

namespace ranvier {

// Logger for rate limiter warnings
inline seastar::logger log_rate_limiter("ranvier.rate_limiter");

// Per-shard rate limiter with Seastar service lifecycle.
// Inherits pure algorithm from BasicRateLimiter, adds timer-based cleanup,
// gate-guarded shutdown, and Prometheus metrics.
class RateLimiter : public BasicRateLimiter<> {
public:
    explicit RateLimiter(RateLimiterConfig config)
        : BasicRateLimiter<>(config) {}

    // Start the cleanup timer (call after construction, before serving requests)
    // Hard Rule #5: Timer uses gate guard pattern for safe shutdown
    void start() {
        if (!is_enabled()) {
            return;
        }

        _cleanup_timer.set_callback([this] { on_cleanup_timer(); });
        _cleanup_timer.arm(cleanup_interval());
        log_rate_limiter.info("Rate limiter cleanup timer started (interval: {}s)",
                              cleanup_interval().count());
    }

    // Stop the cleanup timer and wait for in-flight callbacks
    // Hard Rule #5: Cancel timer, then close gate to wait for in-flight callbacks
    // Returns future that resolves when safe to destroy
    seastar::future<> stop() {
        // Rule #6: Deregister metrics first to prevent lambda use-after-free
        _metrics.clear();

        // Cancel timer to prevent new callbacks from being scheduled
        _cleanup_timer.cancel();

        // Close gate and wait for any in-flight on_cleanup_timer() to complete
        return _timer_gate.close().then([] {
            log_rate_limiter.info("Rate limiter cleanup timer stopped");
        });
    }

    // Register Prometheus metrics
    void register_metrics() {
        namespace sm = seastar::metrics;
        _metrics.add_group("ranvier_rate_limiter", {
            sm::make_counter("overflow_total",
                [this] { return overflow_count(); },
                sm::description("Number of requests that bypassed rate limiting due to bucket overflow (fail-open)")),
            sm::make_counter("buckets_cleaned_total",
                [this] { return buckets_cleaned_total(); },
                sm::description("Total number of stale buckets removed by automatic cleanup")),
            sm::make_gauge("bucket_count",
                [this] { return static_cast<double>(bucket_count()); },
                sm::description("Current number of active rate limit buckets")),
            sm::make_gauge("bucket_capacity",
                [] { return static_cast<double>(MAX_BUCKETS); },
                sm::description("Maximum number of rate limit buckets allowed")),
        });
    }

    // Rule #6: Deregister metrics before destruction to prevent use-after-free
    void clear_metrics() {
        _metrics.clear();
    }

private:
    // Timer callback - invoked by Seastar reactor
    // Hard Rule #5: Must acquire gate holder before accessing any member state
    void on_cleanup_timer() {
        // Try to acquire gate holder - fails if gate is closed (shutdown in progress)
        seastar::gate::holder holder;
        try {
            holder = _timer_gate.hold();
        } catch (const seastar::gate_closed_exception&) {
            // Shutdown in progress - exit without rearming or accessing state
            return;
        }

        // Rule #18: cleanup_async() is a coroutine — handle returned future.
        // Gate holder moved into coroutine chain for full async lifetime (Rule #5).
        (void)cleanup_async().handle_exception([](auto ep) {
            try { std::rethrow_exception(ep); }
            catch (const std::exception& e) {
                log_rate_limiter.warn("Rate limiter cleanup failed: {}", e.what());
            }
        }).finally([holder = std::move(holder)] {});
    }

    // Rule #17: Yielding cleanup for up to 100K buckets.
    // Replaces synchronous cleanup() call with a coroutine that yields
    // every 128 iterations to avoid reactor stalls.
    seastar::future<> cleanup_async() {
        auto now = std::chrono::steady_clock::now();

        auto refill_period = std::chrono::seconds(
            config().burst_size / std::max(config().requests_per_second, 1u));
        auto max_idle = std::max(refill_period * 2, std::chrono::seconds(60));

        size_t removed = 0;
        size_t iterations = 0;
        for (auto it = buckets().begin(); it != buckets().end(); ) {
            auto idle = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_refill);
            if (idle > max_idle) {
                it = buckets().erase(it);
                ++removed;
            } else {
                ++it;
            }

            if (++iterations % 128 == 0) {
                co_await seastar::coroutine::maybe_yield();
            }
        }

        if (removed > 0) {
            add_buckets_cleaned(removed);
            log_rate_limiter.debug("Cleaned {} stale rate limit buckets ({} remaining)",
                                   removed, bucket_count());
        }

        // Rearm timer for next cleanup cycle
        _cleanup_timer.arm(cleanup_interval());
    }

    // Cleanup timer (Hard Rule #5: uses gate guard pattern)
    seastar::timer<> _cleanup_timer;
    seastar::gate _timer_gate;

    // Prometheus metrics (Rule #6: clear in stop() before gate close)
    seastar::metrics::metric_groups _metrics;
};

}  // namespace ranvier
