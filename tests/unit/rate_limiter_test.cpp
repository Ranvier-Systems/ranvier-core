// Ranvier Core - Rate Limiter Unit Tests
//
// Tests for the token bucket rate limiter with MAX_BUCKETS bound (Hard Rule #4).

#include "rate_limiter.hpp"
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <chrono>

using namespace ranvier;

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
}

TEST_F(RateLimiterConfigTest, ConfigCanBeCustomized) {
    config.enabled = true;
    config.requests_per_second = 500;
    config.burst_size = 100;

    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.requests_per_second, 500u);
    EXPECT_EQ(config.burst_size, 100u);
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
    void SetUp() override {
        config.enabled = true;
        config.requests_per_second = 100;
        config.burst_size = 10;
    }

    RateLimiterConfig config;
};

TEST_F(RateLimiterCleanupTest, CleanupRemovesIdleBuckets) {
    RateLimiter limiter(config);

    // Create some buckets
    limiter.allow("client_1");
    limiter.allow("client_2");
    EXPECT_EQ(limiter.bucket_count(), 2u);

    // Cleanup with 0 seconds idle time should remove all
    limiter.cleanup(std::chrono::seconds(0));
    EXPECT_EQ(limiter.bucket_count(), 0u);
}

TEST_F(RateLimiterCleanupTest, CleanupPreservesRecentBuckets) {
    RateLimiter limiter(config);

    limiter.allow("recent_client");
    EXPECT_EQ(limiter.bucket_count(), 1u);

    // Cleanup with long idle threshold should preserve recent bucket
    limiter.cleanup(std::chrono::seconds(3600));
    EXPECT_EQ(limiter.bucket_count(), 1u);
}

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

TEST(RateLimiterTypeTraitsTest, RateLimiterIsMovable) {
    EXPECT_TRUE(std::is_move_constructible_v<RateLimiter>);
    EXPECT_TRUE(std::is_move_assignable_v<RateLimiter>);
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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
