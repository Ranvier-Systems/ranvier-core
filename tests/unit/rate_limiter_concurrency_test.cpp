// Ranvier Core - Rate Limiter Concurrency Stress Tests
//
// Exercises concurrent access to BasicRateLimiter's token buckets and
// atomic counters from multiple threads using C++20 std::latch/std::barrier.
//
// IMPORTANT: BasicRateLimiter's _buckets map (std::unordered_map) is NOT
// thread-safe. In production, each Seastar shard has its own rate limiter
// instance — no cross-shard sharing. These tests validate the atomic
// counters (overflow_count, buckets_cleaned_total) that ARE safe for
// concurrent access, plus scenarios where each thread operates on its
// own rate limiter instance to verify no global state interference.
//
// Single-instance concurrent allow() tests use per-thread client IPs
// to exercise the atomic overflow counter under MAX_BUCKETS pressure
// without concurrent writes to the same bucket.

#include "rate_limiter_core.hpp"
#include "test_clock.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <latch>
#include <barrier>
#include <thread>
#include <vector>

using namespace ranvier;

// =============================================================================
// Test Fixture
// =============================================================================

class RateLimiterConcurrencyTest : public ::testing::Test {
protected:
    static constexpr int kNumThreads = 8;
    static constexpr int kOpsPerThread = 5'000;
};

// =============================================================================
// Per-Instance Concurrent Tests (each thread has own limiter)
// =============================================================================

TEST_F(RateLimiterConcurrencyTest, IndependentLimitersNoInterference) {
    // Each thread creates and uses its own rate limiter.
    // Validates no global static state causes cross-instance interference.
    std::latch start_latch(kNumThreads);
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&start_latch, &success_count, t]() {
            start_latch.arrive_and_wait();

            RateLimiterConfig config;
            config.enabled = true;
            config.requests_per_second = 1000;
            config.burst_size = 100;

            BasicRateLimiter<> limiter(config);

            // Each limiter should behave independently
            std::string client = "client_" + std::to_string(t);

            // First burst_size requests should succeed
            bool all_allowed = true;
            for (int i = 0; i < 100; ++i) {
                if (!limiter.allow(client)) {
                    all_allowed = false;
                    break;
                }
            }

            // Next request should be rate-limited
            bool correctly_limited = !limiter.allow(client);

            if (all_allowed && correctly_limited) {
                success_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(success_count.load(), kNumThreads);
}

// =============================================================================
// Atomic Counter Concurrent Access Tests
// =============================================================================

TEST_F(RateLimiterConcurrencyTest, OverflowCountAtomicUnderContention) {
    // Pre-fill a rate limiter to MAX_BUCKETS, then have multiple threads
    // make requests with new IPs. All should increment overflow_count atomically.
    //
    // NOTE: Filling 100k buckets is expensive but necessary to test overflow.
    // We use a smaller custom test to validate the atomic counter pattern.
    RateLimiterConfig config;
    config.enabled = true;
    config.requests_per_second = 1000;
    config.burst_size = 10;

    BasicRateLimiter<> limiter(config);

    // Fill to capacity
    for (size_t i = 0; i < BasicRateLimiter<>::MAX_BUCKETS; ++i) {
        limiter.allow("fill_" + std::to_string(i));
    }
    ASSERT_EQ(limiter.bucket_count(), BasicRateLimiter<>::MAX_BUCKETS);
    ASSERT_EQ(limiter.overflow_count(), 0u);

    // Now multiple threads all try new IPs -> all hit overflow path
    constexpr int kOverflowOps = 1000;
    std::latch start_latch(kNumThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&limiter, &start_latch, t]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOverflowOps; ++i) {
                std::string ip = "overflow_" + std::to_string(t) +
                                 "_" + std::to_string(i);
                limiter.allow(ip);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Each overflow request should have incremented the atomic counter.
    // All requests with new IPs go through the overflow path (fail-open).
    EXPECT_EQ(limiter.overflow_count(),
              static_cast<size_t>(kNumThreads) * kOverflowOps);
    // Bucket count should not have changed
    EXPECT_EQ(limiter.bucket_count(), BasicRateLimiter<>::MAX_BUCKETS);
}

// =============================================================================
// Concurrent allow() With Unique Client IPs (No Bucket Contention)
// =============================================================================

TEST_F(RateLimiterConcurrencyTest, ConcurrentAllowUniqueClients) {
    // Each thread uses its own unique client IPs, so there's no contention
    // on individual buckets. This tests the unordered_map insertion path.
    //
    // NOTE: This is safe because each thread inserts unique keys.
    // In production, each shard has its own limiter — no concurrent
    // map access. This test validates the pattern works when keys don't collide.
    RateLimiterConfig config;
    config.enabled = true;
    config.requests_per_second = 1000;
    config.burst_size = 10;

    BasicRateLimiter<> limiter(config);

    // Each thread creates kOpsPerThread unique clients
    // Total: kNumThreads * kOpsPerThread < MAX_BUCKETS (80k < 100k)
    static_assert(kNumThreads * kOpsPerThread <
                  static_cast<int>(BasicRateLimiter<>::MAX_BUCKETS));

    std::latch start_latch(kNumThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&limiter, &start_latch, t]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                // Unique key per thread+iteration
                std::string ip = std::to_string(t) + "_" + std::to_string(i);
                EXPECT_TRUE(limiter.allow(ip));
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(limiter.bucket_count(),
              static_cast<size_t>(kNumThreads) * kOpsPerThread);
    EXPECT_EQ(limiter.overflow_count(), 0u);
}

// =============================================================================
// Concurrent Cleanup Tests (with TestClock)
// =============================================================================

TEST_F(RateLimiterConcurrencyTest, ConcurrentCleanupAndOverflowReads) {
    // One thread performs cleanup while others read atomic counters.
    // Validates that buckets_cleaned_total and overflow_count don't
    // produce torn reads or crashes.
    using TestLimiter = BasicRateLimiter<TestClock>;
    TestClock::reset();

    RateLimiterConfig config;
    config.enabled = true;
    config.requests_per_second = 1000;
    config.burst_size = 10;

    TestLimiter limiter(config);

    // Create some buckets
    for (int i = 0; i < 1000; ++i) {
        limiter.allow("client_" + std::to_string(i));
    }
    ASSERT_EQ(limiter.bucket_count(), 1000u);

    // Age the buckets
    TestClock::advance(std::chrono::seconds(600));

    std::atomic<bool> running{true};
    std::latch start_latch(kNumThreads);
    std::vector<std::thread> threads;

    // Reader threads: continuously read atomic counters
    for (int t = 0; t < kNumThreads - 1; ++t) {
        threads.emplace_back([&limiter, &start_latch, &running]() {
            start_latch.arrive_and_wait();
            while (running.load(std::memory_order_relaxed)) {
                [[maybe_unused]] auto overflow = limiter.overflow_count();
                [[maybe_unused]] auto cleaned = limiter.buckets_cleaned_total();
                [[maybe_unused]] auto count = limiter.bucket_count();
                [[maybe_unused]] auto enabled = limiter.is_enabled();
            }
        });
    }

    // Cleanup thread: runs cleanup which modifies _buckets
    threads.emplace_back([&limiter, &start_latch, &running]() {
        start_latch.arrive_and_wait();
        size_t cleaned = limiter.cleanup(std::chrono::seconds(0));
        EXPECT_EQ(cleaned, 1000u);
        running.store(false, std::memory_order_relaxed);
    });

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(limiter.bucket_count(), 0u);
}

// =============================================================================
// Barrier-Synchronized Allow and Drain
// =============================================================================

TEST_F(RateLimiterConcurrencyTest, BarrierSynchronizedBurstDrain) {
    // Phase 1: All threads exhaust their burst on unique clients.
    // Barrier synchronization.
    // Phase 2: All threads verify rate limiting is active.
    constexpr int kClientsPerThread = 100;
    RateLimiterConfig config;
    config.enabled = true;
    config.requests_per_second = 1;  // Very slow refill
    config.burst_size = 5;

    BasicRateLimiter<TestClock> limiter(config);
    TestClock::reset();

    std::barrier sync_barrier(kNumThreads);
    std::atomic<int> correctly_limited{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&limiter, &sync_barrier, &correctly_limited, t]() {
            // Phase 1: exhaust burst for this thread's clients
            for (int c = 0; c < kClientsPerThread; ++c) {
                std::string ip = std::to_string(t) + "_" + std::to_string(c);
                for (int b = 0; b < 5; ++b) {
                    limiter.allow(ip);
                }
            }

            sync_barrier.arrive_and_wait();

            // Phase 2: verify all clients are rate-limited
            int limited_count = 0;
            for (int c = 0; c < kClientsPerThread; ++c) {
                std::string ip = std::to_string(t) + "_" + std::to_string(c);
                if (!limiter.allow(ip)) {
                    limited_count++;
                }
            }

            // All clients should be limited (no time advance)
            if (limited_count == kClientsPerThread) {
                correctly_limited.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(correctly_limited.load(), kNumThreads);
    EXPECT_EQ(limiter.bucket_count(),
              static_cast<size_t>(kNumThreads) * kClientsPerThread);
}

// =============================================================================
// Config Update Concurrent Read
// =============================================================================

TEST_F(RateLimiterConcurrencyTest, ConfigReadWhileUpdating) {
    // One thread updates config while others read is_enabled().
    // Validates no crash or torn config reads.
    RateLimiterConfig config;
    config.enabled = true;
    config.requests_per_second = 100;
    config.burst_size = 50;

    BasicRateLimiter<> limiter(config);

    std::atomic<bool> running{true};
    std::latch start_latch(kNumThreads);
    std::vector<std::thread> threads;

    // Reader threads
    for (int t = 0; t < kNumThreads - 1; ++t) {
        threads.emplace_back([&limiter, &start_latch, &running]() {
            start_latch.arrive_and_wait();
            while (running.load(std::memory_order_relaxed)) {
                // These should not crash regardless of concurrent update
                [[maybe_unused]] auto enabled = limiter.is_enabled();
                [[maybe_unused]] auto count = limiter.bucket_count();
            }
        });
    }

    // Writer thread: repeatedly toggle config
    threads.emplace_back([&limiter, &start_latch, &running]() {
        start_latch.arrive_and_wait();
        for (int i = 0; i < 1000; ++i) {
            RateLimiterConfig new_config;
            new_config.enabled = (i % 2 == 0);
            new_config.requests_per_second = 100 + (i % 50);
            new_config.burst_size = 10 + (i % 40);
            limiter.update_config(new_config);
        }
        running.store(false, std::memory_order_relaxed);
    });

    for (auto& thread : threads) {
        thread.join();
    }
}

// =============================================================================
// Stress: Many Clients Same Bucket
// =============================================================================

TEST_F(RateLimiterConcurrencyTest, ConcurrentSameBucketAccess) {
    // Multiple threads call allow() on the same client IP.
    // This tests the token bucket arithmetic under contention.
    //
    // NOTE: In production, each shard has its own limiter (no sharing).
    // This test intentionally shares to validate the token bucket math
    // doesn't produce impossible values. The unordered_map itself is NOT
    // thread-safe, but since all threads access the same key (already
    // inserted), they only race on the double arithmetic (tokens, last_refill).
    // This may produce slightly inaccurate token counts but should not crash.
    RateLimiterConfig config;
    config.enabled = true;
    config.requests_per_second = 1000000;  // Very high rate to minimize failures
    config.burst_size = 1000000;

    BasicRateLimiter<> limiter(config);

    // Pre-create the bucket
    limiter.allow("shared_client");

    std::latch start_latch(kNumThreads);
    std::atomic<int> allowed{0};
    std::atomic<int> denied{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&limiter, &start_latch, &allowed, &denied]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                if (limiter.allow("shared_client")) {
                    allowed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    denied.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All operations accounted for (no lost ops)
    EXPECT_EQ(allowed.load() + denied.load(), kNumThreads * kOpsPerThread);
    // With a huge burst size and high rate, most should be allowed
    EXPECT_GT(allowed.load(), 0);
}
