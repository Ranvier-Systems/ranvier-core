// Ranvier Core - Rate Limiter Core Algorithm
//
// Token bucket rate limiter for controlling request rates per client IP.
// Pure algorithm - no Seastar dependencies. See rate_limiter.hpp for the
// full Seastar service wrapper with timer, gate, and metrics.
//
// Clock injection: Template parameter defaults to std::chrono::steady_clock
// for zero-overhead production use. Tests use TestClock for deterministic timing.
//
// Hard Rule #4 compliance: MAX_BUCKETS bounds the _buckets map to prevent
// memory exhaustion from attackers generating requests from unique source IPs.
// When at capacity, fail-open (allow request) to maintain availability.

#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>

namespace ranvier {

struct RateLimiterConfig {
    bool enabled = false;
    uint32_t requests_per_second = 100;
    uint32_t burst_size = 50;
    std::chrono::seconds cleanup_interval{60};  // How often to run cleanup timer
};

// Token bucket for a single client
// Clock parameter allows injecting a test clock for deterministic timing
template<typename Clock = std::chrono::steady_clock>
struct BasicTokenBucket {
    double tokens;
    typename Clock::time_point last_refill;

    BasicTokenBucket(uint32_t initial_tokens)
        : tokens(initial_tokens)
        , last_refill(Clock::now()) {}
};

// Backward-compatible alias: production code uses TokenBucket unchanged
using TokenBucket = BasicTokenBucket<>;

// Per-shard rate limiter core algorithm.
// Clock parameter allows injecting a test clock for deterministic timing.
// Production default (steady_clock) is resolved at compile time with zero overhead.
template<typename Clock = std::chrono::steady_clock>
class BasicRateLimiter {
public:
    // Hard Rule #4: Every growing container must have an explicit MAX_SIZE
    // 100k buckets * ~56 bytes/bucket ≈ 5.6 MB max memory per shard
    static constexpr size_t MAX_BUCKETS = 100'000;

    explicit BasicRateLimiter(RateLimiterConfig config)
        : _config(config)
        , _cleanup_interval(config.cleanup_interval)
        , _overflow_count(0)
        , _buckets_cleaned_total(0) {}

    // Check if request is allowed and consume a token if so
    // Returns true if allowed, false if rate limited
    bool allow(const std::string& client_ip) {
        if (!_config.enabled) {
            return true;
        }

        auto now = Clock::now();
        auto it = _buckets.find(client_ip);

        if (it == _buckets.end()) {
            // Hard Rule #4: Check MAX_BUCKETS before inserting
            if (_buckets.size() >= MAX_BUCKETS) {
                // Fail-open: allow request without creating bucket
                _overflow_count.fetch_add(1, std::memory_order_relaxed);
                return true;
            }

            // New client - create bucket with burst_size tokens
            _buckets.emplace(client_ip, BasicTokenBucket<Clock>(_config.burst_size));

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
        auto now = Clock::now();
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

    // Hot-reload: Update configuration at runtime
    void update_config(const RateLimiterConfig& config) {
        _config = config;
        _cleanup_interval = config.cleanup_interval;
        // Note: Existing buckets are preserved to maintain rate limit state
        // Timer interval change takes effect on next rearm
    }

protected:
    // Accessors for derived Seastar service class (rate_limiter.hpp)
    const RateLimiterConfig& config() const { return _config; }
    std::chrono::seconds cleanup_interval() const { return _cleanup_interval; }
    void add_buckets_cleaned(size_t n) {
        _buckets_cleaned_total.fetch_add(n, std::memory_order_relaxed);
    }

private:
    RateLimiterConfig _config;
    std::unordered_map<std::string, BasicTokenBucket<Clock>> _buckets;
    std::chrono::seconds _cleanup_interval;

    // Overflow tracking (Rule #4: bounded container overflow detection)
    std::atomic<size_t> _overflow_count;

    // Cleanup tracking (Rule #1: lock-free counter for metrics)
    std::atomic<size_t> _buckets_cleaned_total;
};

}  // namespace ranvier
