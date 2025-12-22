// Ranvier Core - Rate Limiter
//
// Token bucket rate limiter for controlling request rates per client IP.
// Each client gets a bucket that refills at a configured rate.

#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

namespace ranvier {

struct RateLimiterConfig {
    bool enabled = false;
    uint32_t requests_per_second = 100;
    uint32_t burst_size = 50;
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
    explicit RateLimiter(RateLimiterConfig config)
        : _config(config) {}

    // Check if request is allowed and consume a token if so
    // Returns true if allowed, false if rate limited
    bool allow(const std::string& client_ip) {
        if (!_config.enabled) {
            return true;
        }

        auto now = std::chrono::steady_clock::now();
        auto it = _buckets.find(client_ip);

        if (it == _buckets.end()) {
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
    // Call periodically (e.g., every minute)
    void cleanup(std::chrono::seconds max_idle = std::chrono::seconds(300)) {
        auto now = std::chrono::steady_clock::now();
        for (auto it = _buckets.begin(); it != _buckets.end(); ) {
            auto idle = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_refill);
            if (idle > max_idle) {
                it = _buckets.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Get current bucket count (for metrics/debugging)
    size_t bucket_count() const { return _buckets.size(); }

    bool is_enabled() const { return _config.enabled; }

private:
    RateLimiterConfig _config;
    std::unordered_map<std::string, TokenBucket> _buckets;
};

}  // namespace ranvier
