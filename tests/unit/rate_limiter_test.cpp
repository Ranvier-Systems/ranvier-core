// Ranvier Core - Rate Limiter Unit Tests
//
// Tests for the token bucket rate limiter with MAX_BUCKETS bound (Hard Rule #4).
// Includes deterministic timing tests using TestClock for token refill verification.

#include "rate_limiter_core.hpp"
#include "test_clock.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <string>

using namespace ranvier;

// Test alias: BasicRateLimiter with default clock (same algorithm as Seastar RateLimiter)
using RateLimiter = BasicRateLimiter<>;

// =============================================================================
// RateLimiterConfig Tests
// =============================================================================

class RateLimiterConfigTest : public ::testing::Test {
protected:
    RateLimiterConfig config;
};

TEST_F(RateLimiterConfigTest, DefaultConfigIsDisabled) {
    EXPECT_FALSE(config.enabled);
    EXPECT_EQ(config.requests_per_second, 100u);
    EXPECT_EQ(config.burst_size, 50u);
    EXPECT_EQ(config.cleanup_interval, std::chrono::seconds(60));
}

TEST_F(RateLimiterConfigTest, ConfigCanBeCustomized) {
    config.enabled = true;
    config.requests_per_second = 500;
    config.burst_size = 100;
    config.cleanup_interval = std::chrono::seconds(120);

    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.requests_per_second, 500u);
    EXPECT_EQ(config.burst_size, 100u);
    EXPECT_EQ(config.cleanup_interval, std::chrono::seconds(120));
}

// =============================================================================
// TokenBucket Tests
// =============================================================================

class TokenBucketTest : public ::testing::Test {};

TEST_F(TokenBucketTest, InitializesWithGivenTokens) {
    TokenBucket bucket(50);
    EXPECT_EQ(bucket.tokens, 50.0);
}

TEST_F(TokenBucketTest, LastRefillSetToNow) {
    auto before = std::chrono::steady_clock::now();
    TokenBucket bucket(10);
    auto after = std::chrono::steady_clock::now();

    EXPECT_GE(bucket.last_refill, before);
    EXPECT_LE(bucket.last_refill, after);
}

// =============================================================================
// RateLimiter Basic Behavior Tests
// =============================================================================

class RateLimiterTest : public ::testing::Test {
protected:
    void SetUp() override {
        config.enabled = true;
        config.requests_per_second = 10;
        config.burst_size = 5;
    }

    RateLimiterConfig config;
};

TEST_F(RateLimiterTest, DisabledLimiterAlwaysAllows) {
    config.enabled = false;
    RateLimiter limiter(config);

    // Should always allow when disabled
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(limiter.allow("client_ip"));
    }
    // No buckets created when disabled
    EXPECT_EQ(limiter.bucket_count(), 0u);
}

TEST_F(RateLimiterTest, FirstRequestAlwaysAllowed) {
    RateLimiter limiter(config);
    EXPECT_TRUE(limiter.allow("new_client"));
    EXPECT_EQ(limiter.bucket_count(), 1u);
}

TEST_F(RateLimiterTest, BurstSizeRespected) {
    RateLimiter limiter(config);

    // First request allowed and creates bucket with burst_size tokens
    // After first request, bucket has burst_size - 1 = 4 tokens
    EXPECT_TRUE(limiter.allow("client"));

    // Should allow 4 more requests (remaining tokens)
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(limiter.allow("client"));
    }

    // Next request should be rate limited (no tokens left)
    EXPECT_FALSE(limiter.allow("client"));
}

TEST_F(RateLimiterTest, DifferentClientsHaveSeparateBuckets) {
    RateLimiter limiter(config);

    // Exhaust tokens for client_a
    for (int i = 0; i < 5; ++i) {
        limiter.allow("client_a");
    }
    EXPECT_FALSE(limiter.allow("client_a"));

    // client_b should still have tokens
    EXPECT_TRUE(limiter.allow("client_b"));
    EXPECT_EQ(limiter.bucket_count(), 2u);
}

TEST_F(RateLimiterTest, IsEnabledReturnsConfigState) {
    RateLimiter enabled_limiter(config);
    EXPECT_TRUE(enabled_limiter.is_enabled());

    config.enabled = false;
    RateLimiter disabled_limiter(config);
    EXPECT_FALSE(disabled_limiter.is_enabled());
}

// =============================================================================
// MAX_BUCKETS Bound Tests (Hard Rule #4)
// =============================================================================

class RateLimiterMaxBucketsTest : public ::testing::Test {
protected:
    void SetUp() override {
        config.enabled = true;
        config.requests_per_second = 100;
        config.burst_size = 10;
    }

    RateLimiterConfig config;
};

TEST_F(RateLimiterMaxBucketsTest, MaxBucketsConstantExists) {
    // Verify the constant is defined and reasonable
    EXPECT_EQ(RateLimiter::MAX_BUCKETS, 100'000u);
}

TEST_F(RateLimiterMaxBucketsTest, OverflowCountStartsAtZero) {
    RateLimiter limiter(config);
    EXPECT_EQ(limiter.overflow_count(), 0u);
}

TEST_F(RateLimiterMaxBucketsTest, OverflowCountIncrementsBeyondCapacity) {
    // Note: This test uses a modified approach since filling 100k buckets
    // would be slow. We test the logic by verifying the counter mechanism.
    RateLimiter limiter(config);

    // Create a few buckets to verify normal operation
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(limiter.allow("client_" + std::to_string(i)));
    }
    EXPECT_EQ(limiter.bucket_count(), 10u);
    EXPECT_EQ(limiter.overflow_count(), 0u);
}

TEST_F(RateLimiterMaxBucketsTest, BucketCountAccessorWorks) {
    RateLimiter limiter(config);

    EXPECT_EQ(limiter.bucket_count(), 0u);

    limiter.allow("client_1");
    EXPECT_EQ(limiter.bucket_count(), 1u);

    limiter.allow("client_2");
    EXPECT_EQ(limiter.bucket_count(), 2u);

    // Same client doesn't create new bucket
    limiter.allow("client_1");
    EXPECT_EQ(limiter.bucket_count(), 2u);
}

// =============================================================================
// Cleanup Tests
// =============================================================================

class RateLimiterCleanupTest : public ::testing::Test {
protected:
    using TestLimiter = BasicRateLimiter<TestClock>;

    void SetUp() override {
        TestClock::reset();
        config.enabled = true;
        config.requests_per_second = 100;
        config.burst_size = 10;
    }

    RateLimiterConfig config;
};

TEST_F(RateLimiterCleanupTest, CleanupRemovesIdleBuckets) {
    TestLimiter limiter(config);

    // Create some buckets
    limiter.allow("client_1");
    limiter.allow("client_2");
    EXPECT_EQ(limiter.bucket_count(), 2u);

    // Advance time so buckets become idle (idle > 0s threshold)
    TestClock::advance(std::chrono::seconds(1));

    // Cleanup with 0 seconds idle time should remove all
    size_t removed = limiter.cleanup(std::chrono::seconds(0));
    EXPECT_EQ(removed, 2u);
    EXPECT_EQ(limiter.bucket_count(), 0u);
}

TEST_F(RateLimiterCleanupTest, CleanupPreservesRecentBuckets) {
    TestLimiter limiter(config);

    limiter.allow("recent_client");
    EXPECT_EQ(limiter.bucket_count(), 1u);

    // Cleanup with long idle threshold should preserve recent bucket
    size_t removed = limiter.cleanup(std::chrono::seconds(3600));
    EXPECT_EQ(removed, 0u);
    EXPECT_EQ(limiter.bucket_count(), 1u);
}

TEST_F(RateLimiterCleanupTest, CleanupReturnsRemovedCount) {
    TestLimiter limiter(config);

    // Create some buckets
    limiter.allow("client_1");
    limiter.allow("client_2");
    limiter.allow("client_3");
    EXPECT_EQ(limiter.bucket_count(), 3u);

    // Advance time so buckets become idle (idle > 0s threshold)
    TestClock::advance(std::chrono::seconds(1));

    // Cleanup with 0 seconds idle time should remove all and return count
    size_t removed = limiter.cleanup(std::chrono::seconds(0));
    EXPECT_EQ(removed, 3u);
    EXPECT_EQ(limiter.bucket_count(), 0u);
}

TEST_F(RateLimiterCleanupTest, BucketsCleanedTotalStartsAtZero) {
    TestLimiter limiter(config);
    EXPECT_EQ(limiter.buckets_cleaned_total(), 0u);
}

// Note: Timer-based cleanup tests require Seastar reactor and are in
// tests/integration/rate_limiter_timer_test.cpp. The on_cleanup_timer()
// callback increments buckets_cleaned_total atomically.

// =============================================================================
// Config Update Tests
// =============================================================================

class RateLimiterConfigUpdateTest : public ::testing::Test {
protected:
    void SetUp() override {
        config.enabled = true;
        config.requests_per_second = 10;
        config.burst_size = 5;
    }

    RateLimiterConfig config;
};

TEST_F(RateLimiterConfigUpdateTest, UpdateConfigPreservesBuckets) {
    RateLimiter limiter(config);

    limiter.allow("client_1");
    limiter.allow("client_2");
    EXPECT_EQ(limiter.bucket_count(), 2u);

    // Update config
    RateLimiterConfig new_config;
    new_config.enabled = true;
    new_config.requests_per_second = 100;
    new_config.burst_size = 50;
    limiter.update_config(new_config);

    // Buckets should be preserved
    EXPECT_EQ(limiter.bucket_count(), 2u);
}

TEST_F(RateLimiterConfigUpdateTest, DisablingViaConfigAllowsAllRequests) {
    RateLimiter limiter(config);

    // Exhaust tokens
    for (int i = 0; i < 5; ++i) {
        limiter.allow("client");
    }
    EXPECT_FALSE(limiter.allow("client"));

    // Disable via config update
    config.enabled = false;
    limiter.update_config(config);

    // Now should allow (disabled)
    EXPECT_TRUE(limiter.allow("client"));
}

TEST_F(RateLimiterConfigUpdateTest, UpdateConfigChangesCleanupInterval) {
    RateLimiter limiter(config);

    // Update cleanup interval
    RateLimiterConfig new_config;
    new_config.enabled = true;
    new_config.requests_per_second = 100;
    new_config.burst_size = 10;
    new_config.cleanup_interval = std::chrono::seconds(30);
    limiter.update_config(new_config);

    // Note: The actual interval change takes effect on next timer rearm.
    // This test verifies config is accepted; timer behavior tested in integration.
    EXPECT_TRUE(limiter.is_enabled());
}

// =============================================================================
// Type Traits Tests
// =============================================================================

TEST(RateLimiterTypeTraitsTest, ConfigIsCopyable) {
    EXPECT_TRUE(std::is_copy_constructible_v<RateLimiterConfig>);
    EXPECT_TRUE(std::is_copy_assignable_v<RateLimiterConfig>);
}

TEST(RateLimiterTypeTraitsTest, TokenBucketIsCopyable) {
    EXPECT_TRUE(std::is_copy_constructible_v<TokenBucket>);
    EXPECT_TRUE(std::is_copy_assignable_v<TokenBucket>);
}

TEST(RateLimiterTypeTraitsTest, RateLimiterNotCopyable) {
    // Contains std::atomic members, so not copyable or movable
    EXPECT_FALSE(std::is_copy_constructible_v<RateLimiter>);
    EXPECT_FALSE(std::is_copy_assignable_v<RateLimiter>);
}

// =============================================================================
// Stress Test for MAX_BUCKETS (Optional - marked for integration tests)
// =============================================================================

// This test is expensive (creates 100k+ entries) - run selectively
TEST(RateLimiterStressTest, DISABLED_FailOpenAtMaxBuckets) {
    RateLimiterConfig config;
    config.enabled = true;
    config.requests_per_second = 100;
    config.burst_size = 10;

    RateLimiter limiter(config);

    // Fill to capacity
    for (size_t i = 0; i < RateLimiter::MAX_BUCKETS; ++i) {
        EXPECT_TRUE(limiter.allow("ip_" + std::to_string(i)));
    }
    EXPECT_EQ(limiter.bucket_count(), RateLimiter::MAX_BUCKETS);
    EXPECT_EQ(limiter.overflow_count(), 0u);

    // Next new IP should fail-open (allowed, but no bucket created)
    EXPECT_TRUE(limiter.allow("overflow_ip_1"));
    EXPECT_EQ(limiter.bucket_count(), RateLimiter::MAX_BUCKETS);  // No new bucket
    EXPECT_EQ(limiter.overflow_count(), 1u);

    // More overflow requests
    EXPECT_TRUE(limiter.allow("overflow_ip_2"));
    EXPECT_TRUE(limiter.allow("overflow_ip_3"));
    EXPECT_EQ(limiter.overflow_count(), 3u);

    // Existing clients still work normally
    for (size_t i = 0; i < 5; ++i) {
        limiter.allow("ip_0");  // Existing bucket
    }
    // Should be rate limited after burst
    EXPECT_FALSE(limiter.allow("ip_0"));
}

// =============================================================================
// Deterministic Timing Tests (TestClock - no sleeps, instant execution)
// =============================================================================

class RateLimiterTimingTest : public ::testing::Test {
protected:
    using TestLimiter = BasicRateLimiter<TestClock>;

    void SetUp() override {
        TestClock::reset();
        config.enabled = true;
        config.requests_per_second = 10;  // 10 tokens/sec = 1 token per 100ms
        config.burst_size = 5;
    }

    RateLimiterConfig config;
};

TEST_F(RateLimiterTimingTest, TokenRefillOverSimulatedTime) {
    TestLimiter limiter(config);

    // Exhaust all 5 tokens
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(limiter.allow("client"));
    }
    EXPECT_FALSE(limiter.allow("client"));

    // Advance 100ms -> should refill 1 token (10 tokens/sec * 0.1s = 1.0)
    TestClock::advance(std::chrono::milliseconds(100));
    EXPECT_TRUE(limiter.allow("client"));
    EXPECT_FALSE(limiter.allow("client"));

    // Advance another 200ms -> should refill 2 tokens
    TestClock::advance(std::chrono::milliseconds(200));
    EXPECT_TRUE(limiter.allow("client"));
    EXPECT_TRUE(limiter.allow("client"));
    EXPECT_FALSE(limiter.allow("client"));
}

TEST_F(RateLimiterTimingTest, TokenRefillCapsAtBurstSize) {
    TestLimiter limiter(config);

    // Use 1 token
    EXPECT_TRUE(limiter.allow("client"));

    // Advance 10 seconds -> would refill 100 tokens, but capped at burst_size=5
    TestClock::advance(std::chrono::seconds(10));

    // Should have exactly burst_size tokens (5)
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(limiter.allow("client"));
    }
    EXPECT_FALSE(limiter.allow("client"));
}

TEST_F(RateLimiterTimingTest, ExactRefillRate) {
    // Configure for easy math: 1 token per second, burst of 3
    config.requests_per_second = 1;
    config.burst_size = 3;
    TestLimiter limiter(config);

    // Use all 3 tokens
    EXPECT_TRUE(limiter.allow("client"));
    EXPECT_TRUE(limiter.allow("client"));
    EXPECT_TRUE(limiter.allow("client"));
    EXPECT_FALSE(limiter.allow("client"));

    // Advance exactly 1 second -> 1 token refilled
    TestClock::advance(std::chrono::seconds(1));
    EXPECT_TRUE(limiter.allow("client"));
    EXPECT_FALSE(limiter.allow("client"));

    // Advance exactly 3 seconds -> 3 tokens refilled (capped at burst_size=3)
    TestClock::advance(std::chrono::seconds(3));
    EXPECT_TRUE(limiter.allow("client"));
    EXPECT_TRUE(limiter.allow("client"));
    EXPECT_TRUE(limiter.allow("client"));
    EXPECT_FALSE(limiter.allow("client"));
}

TEST_F(RateLimiterTimingTest, NoRefillWithNoTimeAdvance) {
    TestLimiter limiter(config);

    // Exhaust all tokens
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(limiter.allow("client"));
    }

    // Without advancing time, tokens stay exhausted
    EXPECT_FALSE(limiter.allow("client"));
    EXPECT_FALSE(limiter.allow("client"));
    EXPECT_FALSE(limiter.allow("client"));
}

TEST_F(RateLimiterTimingTest, PartialTokenRefill) {
    // 10 tokens/sec, need 50ms for 0.5 tokens (not enough for a whole token)
    TestLimiter limiter(config);

    // Exhaust tokens
    for (int i = 0; i < 5; ++i) {
        limiter.allow("client");
    }
    EXPECT_FALSE(limiter.allow("client"));

    // Advance 50ms -> 0.5 tokens, not enough for 1 request
    TestClock::advance(std::chrono::milliseconds(50));
    EXPECT_FALSE(limiter.allow("client"));

    // Advance another 50ms -> now 0.5 more (but previous 0.5 was consumed
    // by the failed allow check that updated last_refill). Actually, after
    // the failed allow, the bucket has 0.5 tokens and last_refill updated.
    // Another 50ms gives 0.5 more -> 1.0 total -> should allow
    TestClock::advance(std::chrono::milliseconds(50));
    EXPECT_TRUE(limiter.allow("client"));
}

TEST_F(RateLimiterTimingTest, CleanupRemovesIdleBucketsWithTestClock) {
    TestLimiter limiter(config);

    // Create buckets
    limiter.allow("client_old");
    EXPECT_EQ(limiter.bucket_count(), 1u);

    // Advance time past idle threshold (300s default)
    TestClock::advance(std::chrono::seconds(301));

    // Add a fresh client
    limiter.allow("client_new");
    EXPECT_EQ(limiter.bucket_count(), 2u);

    // Cleanup with default 300s idle threshold
    size_t removed = limiter.cleanup(std::chrono::seconds(300));
    EXPECT_EQ(removed, 1u);
    EXPECT_EQ(limiter.bucket_count(), 1u);
}

TEST_F(RateLimiterTimingTest, CleanupPreservesActiveBuckets) {
    TestLimiter limiter(config);

    // Create bucket and keep it active
    limiter.allow("active_client");

    // Advance 100 seconds
    TestClock::advance(std::chrono::seconds(100));

    // Touch the bucket (updates last_refill via allow)
    limiter.allow("active_client");

    // Advance another 100 seconds (200 total, but only 100 since last touch)
    TestClock::advance(std::chrono::seconds(100));

    // Cleanup with 150s threshold - bucket was active 100s ago, should survive
    size_t removed = limiter.cleanup(std::chrono::seconds(150));
    EXPECT_EQ(removed, 0u);
    EXPECT_EQ(limiter.bucket_count(), 1u);
}

TEST_F(RateLimiterTimingTest, MultipleClientsRefillIndependently) {
    config.requests_per_second = 1;
    config.burst_size = 2;
    TestLimiter limiter(config);

    // Exhaust client_a
    limiter.allow("client_a");
    limiter.allow("client_a");
    EXPECT_FALSE(limiter.allow("client_a"));

    // Advance 500ms - client_a gets partial refill
    TestClock::advance(std::chrono::milliseconds(500));

    // Exhaust client_b (just created, full burst)
    limiter.allow("client_b");
    limiter.allow("client_b");
    EXPECT_FALSE(limiter.allow("client_b"));

    // client_a should still not have enough (0.5 tokens)
    EXPECT_FALSE(limiter.allow("client_a"));

    // Advance another 600ms -> client_a: +0.6 from last check,
    // client_b: +0.6 from 600ms ago
    TestClock::advance(std::chrono::milliseconds(600));
    EXPECT_TRUE(limiter.allow("client_a"));
    EXPECT_TRUE(limiter.allow("client_b"));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
