// Ranvier Core - Rate Limiter Concurrency Stress Tests
//
// Exercises concurrent access to BasicRateLimiter from multiple threads
// using C++20 std::latch/std::barrier.
//
// IMPORTANT: BasicRateLimiter's _buckets map (std::unordered_map) is NOT
// thread-safe. In production, each Seastar shard has its own rate limiter
// instance — no cross-shard sharing. Most tests give each thread its own
// limiter to verify no global state interference. The one shared-instance
// test (OverflowCountAtomicUnderContention) pre-fills the map so the
// concurrent phase only reads the map + increments an atomic counter.

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
    //
    // Uses TestClock frozen at epoch — no thread advances time — so the
    // token-bucket refill never runs during the 100-request burst. With
    // the default steady_clock the 1ms refill window can race the burst
    // under sanitiser slowdown and the assertion flakes.
    TestClock::reset();
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

            BasicRateLimiter<TestClock> limiter(config);

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
    // make requests with new IPs. All hit the overflow path (map read-only,
    // atomic counter increment). Validates atomic overflow_count accuracy.
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

TEST_F(RateLimiterConcurrencyTest, ConcurrentAllowPerThreadLimiters) {
    // Each thread has its own rate limiter instance (matching the Seastar
    // shared-nothing model). Validates that concurrent creation and use
    // of independent limiters has no cross-instance interference.
    std::latch start_latch(kNumThreads);
    std::atomic<int> correct_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&start_latch, &correct_count, t]() {
            start_latch.arrive_and_wait();

            RateLimiterConfig config;
            config.enabled = true;
            config.requests_per_second = 1000;
            config.burst_size = 10;
            BasicRateLimiter<> limiter(config);

            for (int i = 0; i < kOpsPerThread; ++i) {
                std::string ip = "client_" + std::to_string(i);
                limiter.allow(ip);
            }

            // Each limiter independently tracks its own buckets
            if (limiter.bucket_count() == static_cast<size_t>(kOpsPerThread) &&
                limiter.overflow_count() == 0u) {
                correct_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(correct_count.load(), kNumThreads);
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

    // Reader threads: continuously read atomic counters only.
    // NOTE: bucket_count() calls _buckets.size() which is NOT safe to read
    // concurrently with cleanup's _buckets.erase(). Only the truly atomic
    // accessors (overflow_count, buckets_cleaned_total) are safe here.
    for (int t = 0; t < kNumThreads - 1; ++t) {
        threads.emplace_back([&limiter, &start_latch, &running]() {
            start_latch.arrive_and_wait();
            while (running.load(std::memory_order_relaxed)) {
                [[maybe_unused]] auto overflow = limiter.overflow_count();
                [[maybe_unused]] auto cleaned = limiter.buckets_cleaned_total();
                [[maybe_unused]] auto enabled = limiter.is_enabled();
            }
        });
    }

    // Cleanup thread: runs cleanup which modifies _buckets (not thread-safe)
    threads.emplace_back([&limiter, &start_latch, &running]() {
        start_latch.arrive_and_wait();
        size_t cleaned = limiter.cleanup(std::chrono::seconds(0));
        EXPECT_EQ(cleaned, 1000u);
        running.store(false, std::memory_order_relaxed);
    });

    for (auto& thread : threads) {
        thread.join();
    }

    // Safe to read non-atomic accessor after all threads joined
    EXPECT_EQ(limiter.bucket_count(), 0u);
}

// =============================================================================
// Barrier-Synchronized Allow and Drain
// =============================================================================

TEST_F(RateLimiterConcurrencyTest, BarrierSynchronizedBurstDrain) {
    // Each thread owns its own rate limiter (matching Seastar shared-nothing).
    // Phase 1: exhaust burst. Barrier. Phase 2: verify rate limiting.
    constexpr int kClientsPerThread = 100;
    std::barrier sync_barrier(kNumThreads);
    std::atomic<int> correctly_limited{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&sync_barrier, &correctly_limited, t]() {
            RateLimiterConfig config;
            config.enabled = true;
            config.requests_per_second = 1;  // Very slow refill
            config.burst_size = 5;
            BasicRateLimiter<TestClock> limiter(config);

            // Phase 1: exhaust burst for this thread's clients
            for (int c = 0; c < kClientsPerThread; ++c) {
                std::string ip = "client_" + std::to_string(c);
                for (int b = 0; b < 5; ++b) {
                    limiter.allow(ip);
                }
            }

            sync_barrier.arrive_and_wait();

            // Phase 2: verify all clients are rate-limited
            int limited_count = 0;
            for (int c = 0; c < kClientsPerThread; ++c) {
                std::string ip = "client_" + std::to_string(c);
                if (!limiter.allow(ip)) {
                    limited_count++;
                }
            }

            if (limited_count == kClientsPerThread) {
                correctly_limited.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(correctly_limited.load(), kNumThreads);
}

// =============================================================================
// Config Update Concurrent Read
// =============================================================================

TEST_F(RateLimiterConcurrencyTest, ConcurrentConfigUpdatePerThread) {
    // Each thread creates a limiter and rapidly toggles config.
    // Validates update_config does not corrupt internal state.
    std::latch start_latch(kNumThreads);
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&start_latch, &success_count]() {
            start_latch.arrive_and_wait();

            RateLimiterConfig config;
            config.enabled = true;
            config.requests_per_second = 100;
            config.burst_size = 50;
            BasicRateLimiter<> limiter(config);

            // Create some buckets
            for (int i = 0; i < 10; ++i) {
                limiter.allow("client_" + std::to_string(i));
            }

            // Rapidly toggle config
            for (int i = 0; i < 1000; ++i) {
                RateLimiterConfig new_config;
                new_config.enabled = (i % 2 == 0);
                new_config.requests_per_second = 100 + (i % 50);
                new_config.burst_size = 10 + (i % 40);
                limiter.update_config(new_config);
            }

            // Buckets should be preserved across config updates
            if (limiter.bucket_count() == 10u) {
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
// Stress: Many Clients Same Bucket
// =============================================================================

TEST_F(RateLimiterConcurrencyTest, ConcurrentSameBucketPerThreadLimiter) {
    // Each thread hammers a single client IP on its own limiter.
    // Validates that high-frequency allow() on one bucket produces
    // correct rate limiting behavior.
    std::latch start_latch(kNumThreads);
    std::atomic<int> threads_correct{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&start_latch, &threads_correct]() {
            start_latch.arrive_and_wait();

            RateLimiterConfig config;
            config.enabled = true;
            config.requests_per_second = 100;
            config.burst_size = 50;
            BasicRateLimiter<> limiter(config);

            int allowed = 0;
            int denied = 0;
            for (int i = 0; i < kOpsPerThread; ++i) {
                if (limiter.allow("shared_client")) {
                    allowed++;
                } else {
                    denied++;
                }
            }

            // First burst_size requests allowed, rest should be mostly denied
            // (some may refill during wall-clock time, so just check both > 0)
            if (allowed > 0 && denied > 0 &&
                allowed + denied == kOpsPerThread) {
                threads_correct.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(threads_correct.load(), kNumThreads);
}
