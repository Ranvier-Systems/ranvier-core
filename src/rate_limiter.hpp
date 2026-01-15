// Ranvier Core - Rate Limiter
//
// Token bucket rate limiter for controlling request rates per client IP.
// Each client gets a bucket that refills at a configured rate.
//
// Hard Rule #4 compliance: MAX_BUCKETS bounds the _buckets map to prevent
// memory exhaustion from attackers generating requests from unique source IPs.
// When at capacity, fail-open (allow request) to maintain availability.
//
// Hard Rule #5 compliance: Timer callbacks use gate guards to prevent
// use-after-free during shutdown. stop() closes gate before cancelling timer.

#pragma once

#include <atomic>
#include <chrono>
#include <mutex>  // for std::call_once / std::once_flag only
#include <string>
#include <unordered_map>

#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/core/timer.hh>
#include <seastar/util/log.hh>

namespace ranvier {

// Logger for rate limiter warnings
inline seastar::logger log_rate_limiter("ranvier.rate_limiter");

struct RateLimiterConfig {
    bool enabled = false;
    uint32_t requests_per_second = 100;
    uint32_t burst_size = 50;
    std::chrono::seconds cleanup_interval{60};  // How often to run cleanup timer
};

// Token bucket for a single client
struct TokenBucket {
    double tokens;
    std::chrono::steady_clock::time_point last_refill;

    TokenBucket(uint32_t initial_tokens)
        : tokens(initial_tokens)
        , last_refill(std::chrono::steady_clock::now()) {}
};

// Per-shard rate limiter (no locks needed in Seastar's shared-nothing model)
class RateLimiter {
public:
    // Hard Rule #4: Every growing container must have an explicit MAX_SIZE
    // 100k buckets * ~56 bytes/bucket ≈ 5.6 MB max memory per shard
    static constexpr size_t MAX_BUCKETS = 100'000;

    explicit RateLimiter(RateLimiterConfig config)
        : _config(config)
        , _cleanup_interval(config.cleanup_interval)
        , _overflow_count(0)
        , _buckets_cleaned_total(0) {}

    // Start the cleanup timer (call after construction, before serving requests)
    // Hard Rule #5: Timer uses gate guard pattern for safe shutdown
    void start() {
        if (!_config.enabled) {
            return;
        }

        _cleanup_timer.set_callback([this] { on_cleanup_timer(); });
        _cleanup_timer.arm(_cleanup_interval);
        log_rate_limiter.info("Rate limiter cleanup timer started (interval: {}s)",
                              _cleanup_interval.count());
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

    // Check if request is allowed and consume a token if so
    // Returns true if allowed, false if rate limited
    bool allow(const std::string& client_ip) {
        if (!_config.enabled) {
            return true;
        }

        auto now = std::chrono::steady_clock::now();
        auto it = _buckets.find(client_ip);

        if (it == _buckets.end()) {
            // Hard Rule #4: Check MAX_BUCKETS before inserting
            if (_buckets.size() >= MAX_BUCKETS) {
                // Fail-open: allow request without creating bucket
                _overflow_count.fetch_add(1, std::memory_order_relaxed);

                // Log warning once per process lifetime
                std::call_once(_overflow_logged, [] {
                    log_rate_limiter.warn(
                        "Rate limiter bucket overflow: MAX_BUCKETS ({}) reached. "
                        "Failing open for new clients. This may indicate an attack "
                        "or misconfiguration.", MAX_BUCKETS);
                });

                return true;
            }

            // New client - create bucket with burst_size tokens
            _buckets.emplace(client_ip, TokenBucket(_config.burst_size));

            // First request always allowed
            auto iter = _buckets.find(client_ip);
            if (iter != _buckets.end()) {
                auto& bucket = iter->second;
                bucket.tokens -= 1.0;
            }
            return true;
        }

        auto& bucket = it->second;

        // Refill tokens based on time elapsed
        auto elapsed = std::chrono::duration<double>(now - bucket.last_refill).count();
        double refill = elapsed * _config.requests_per_second;
        bucket.tokens = std::min(static_cast<double>(_config.burst_size), bucket.tokens + refill);
        bucket.last_refill = now;

        // Check if we have tokens available
        if (bucket.tokens >= 1.0) {
            bucket.tokens -= 1.0;
            return true;
        }

        return false;
    }

    // Clean up old entries to prevent memory growth
    // Returns the number of buckets removed
    size_t cleanup(std::chrono::seconds max_idle = std::chrono::seconds(300)) {
        auto now = std::chrono::steady_clock::now();
        size_t removed = 0;
        for (auto it = _buckets.begin(); it != _buckets.end(); ) {
            auto idle = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_refill);
            if (idle > max_idle) {
                it = _buckets.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

    // Get current bucket count (for metrics/debugging)
    size_t bucket_count() const { return _buckets.size(); }

    bool is_enabled() const { return _config.enabled; }

    // Lock-free overflow counter accessor (Rule #1: metrics must be lock-free)
    size_t overflow_count() const {
        return _overflow_count.load(std::memory_order_relaxed);
    }

    // Lock-free buckets cleaned counter accessor (Rule #1: metrics must be lock-free)
    size_t buckets_cleaned_total() const {
        return _buckets_cleaned_total.load(std::memory_order_relaxed);
    }

    // Register Prometheus metrics
    void register_metrics() {
        namespace sm = seastar::metrics;
        _metrics.add_group("ranvier_rate_limiter", {
            sm::make_counter("overflow_total",
                [this] { return _overflow_count.load(std::memory_order_relaxed); },
                sm::description("Number of requests that bypassed rate limiting due to bucket overflow (fail-open)")),
            sm::make_counter("buckets_cleaned_total",
                [this] { return _buckets_cleaned_total.load(std::memory_order_relaxed); },
                sm::description("Total number of stale buckets removed by automatic cleanup")),
            sm::make_gauge("bucket_count",
                [this] { return static_cast<double>(_buckets.size()); },
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

    // Hot-reload: Update configuration at runtime
    void update_config(const RateLimiterConfig& config) {
        _config = config;
        _cleanup_interval = config.cleanup_interval;
        // Note: Existing buckets are preserved to maintain rate limit state
        // Timer interval change takes effect on next rearm
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

        // Gate held - safe to access members
        // Calculate max idle time as 2x the refill period
        // A bucket is "stale" if no requests for 2x the time needed to fully refill
        auto refill_period = std::chrono::seconds(
            _config.burst_size / std::max(_config.requests_per_second, 1u));
        auto max_idle = std::max(refill_period * 2, std::chrono::seconds(60));

        size_t cleaned = cleanup(max_idle);
        if (cleaned > 0) {
            _buckets_cleaned_total.fetch_add(cleaned, std::memory_order_relaxed);
            log_rate_limiter.debug("Cleaned {} stale rate limit buckets ({} remaining)",
                                   cleaned, _buckets.size());
        }

        // Rearm timer for next cleanup cycle
        // This happens while gate holder is still valid
        _cleanup_timer.arm(_cleanup_interval);
    }

    RateLimiterConfig _config;
    std::unordered_map<std::string, TokenBucket> _buckets;

    // Cleanup timer (Hard Rule #5: uses gate guard pattern)
    seastar::timer<> _cleanup_timer;
    seastar::gate _timer_gate;
    std::chrono::seconds _cleanup_interval;

    // Overflow tracking (Rule #4: bounded container overflow detection)
    std::atomic<size_t> _overflow_count;
    inline static std::once_flag _overflow_logged;  // Log once per process

    // Cleanup tracking (Rule #1: lock-free counter for metrics)
    std::atomic<size_t> _buckets_cleaned_total;

    // Prometheus metrics (Rule #6: clear in stop() before gate close)
    seastar::metrics::metric_groups _metrics;
};

}  // namespace ranvier
