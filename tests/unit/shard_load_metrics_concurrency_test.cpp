// Ranvier Core - Shard Load Metrics Concurrency Stress Tests
//
// Exercises concurrent access to ShardLoadMetrics atomic counters
// from multiple threads using C++20 std::latch and std::barrier.
// Validates that atomic increment/decrement operations maintain
// correct totals under contention, and that RAII guards remain
// balanced across concurrent scope exits.

#include "shard_load_metrics.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <cstdint>
#include <latch>
#include <barrier>
#include <thread>
#include <vector>

using namespace ranvier;

// =============================================================================
// Test Fixture
// =============================================================================

class ShardLoadMetricsConcurrencyTest : public ::testing::Test {
protected:
    static constexpr int kNumThreads = 8;
    static constexpr int kOpsPerThread = 10'000;

    ShardLoadMetrics metrics;
};

// =============================================================================
// Concurrent Increment/Decrement Tests
// =============================================================================

TEST_F(ShardLoadMetricsConcurrencyTest, ConcurrentIncrementActive) {
    // All threads increment active_requests concurrently.
    // Final value must equal kNumThreads * kOpsPerThread.
    std::latch start_latch(kNumThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([this, &start_latch]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                metrics.increment_active();
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(metrics.active_requests(),
              static_cast<uint64_t>(kNumThreads) * kOpsPerThread);
}

TEST_F(ShardLoadMetricsConcurrencyTest, ConcurrentIncrementDecrement) {
    // Each thread increments kOpsPerThread times then decrements kOpsPerThread
    // times. Net effect per thread = 0. Final counter must be 0.
    std::latch start_latch(kNumThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([this, &start_latch]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                metrics.increment_active();
            }
            for (int i = 0; i < kOpsPerThread; ++i) {
                metrics.decrement_active();
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(metrics.active_requests(), 0u);
}

TEST_F(ShardLoadMetricsConcurrencyTest, ConcurrentIncrementQueued) {
    std::latch start_latch(kNumThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([this, &start_latch]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                metrics.increment_queued();
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(metrics.queued_requests(),
              static_cast<uint64_t>(kNumThreads) * kOpsPerThread);
}

TEST_F(ShardLoadMetricsConcurrencyTest, ConcurrentIncrementDecrementQueued) {
    std::latch start_latch(kNumThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([this, &start_latch]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                metrics.increment_queued();
            }
            for (int i = 0; i < kOpsPerThread; ++i) {
                metrics.decrement_queued();
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(metrics.queued_requests(), 0u);
}

TEST_F(ShardLoadMetricsConcurrencyTest, ConcurrentRecordRequestCompleted) {
    std::latch start_latch(kNumThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([this, &start_latch]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                metrics.record_request_completed();
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(metrics.total_requests(),
              static_cast<uint64_t>(kNumThreads) * kOpsPerThread);
}

// =============================================================================
// Mixed Counter Concurrent Access
// =============================================================================

TEST_F(ShardLoadMetricsConcurrencyTest, ConcurrentMixedCounterOperations) {
    // Different threads operate on different counters simultaneously.
    // Validates no interference between independent atomic fields.
    std::latch start_latch(kNumThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([this, &start_latch, t]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                switch (t % 3) {
                    case 0: metrics.increment_active(); break;
                    case 1: metrics.increment_queued(); break;
                    case 2: metrics.record_request_completed(); break;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Threads are split across 3 operations (8 threads, ~2-3 per op)
    uint64_t expected_per_op = 0;
    for (int t = 0; t < kNumThreads; ++t) {
        if (t % 3 == 0) expected_per_op++;
    }
    // Each group has expected_per_op threads, each doing kOpsPerThread ops
    uint64_t active_threads = 0, queued_threads = 0, total_threads = 0;
    for (int t = 0; t < kNumThreads; ++t) {
        switch (t % 3) {
            case 0: active_threads++; break;
            case 1: queued_threads++; break;
            case 2: total_threads++; break;
        }
    }

    EXPECT_EQ(metrics.active_requests(), active_threads * kOpsPerThread);
    EXPECT_EQ(metrics.queued_requests(), queued_threads * kOpsPerThread);
    EXPECT_EQ(metrics.total_requests(), total_threads * kOpsPerThread);
}

// =============================================================================
// RAII Guard Concurrency Tests
// =============================================================================

TEST_F(ShardLoadMetricsConcurrencyTest, ConcurrentActiveRequestGuards) {
    // Each thread creates and destroys ActiveRequestGuard kOpsPerThread times.
    // After all threads complete, active_requests must be 0.
    std::latch start_latch(kNumThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([this, &start_latch]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                ShardLoadMetrics::ActiveRequestGuard guard(metrics);
                // Guard is destroyed at end of each iteration
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(metrics.active_requests(), 0u);
}

TEST_F(ShardLoadMetricsConcurrencyTest, ConcurrentQueuedRequestGuards) {
    std::latch start_latch(kNumThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([this, &start_latch]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                ShardLoadMetrics::QueuedRequestGuard guard(metrics);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(metrics.queued_requests(), 0u);
}

// =============================================================================
// Snapshot Under Contention
// =============================================================================

TEST_F(ShardLoadMetricsConcurrencyTest, SnapshotDuringConcurrentUpdates) {
    // Writers continuously modify counters while a reader takes snapshots.
    // Validates that snapshot() does not crash or return corrupted data.
    std::atomic<bool> running{true};
    std::latch start_latch(kNumThreads + 1);  // +1 for reader
    std::vector<std::thread> threads;

    // Writer threads
    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([this, &start_latch, &running]() {
            start_latch.arrive_and_wait();
            while (running.load(std::memory_order_relaxed)) {
                metrics.increment_active();
                metrics.increment_queued();
                metrics.record_request_completed();
                metrics.decrement_active();
                metrics.decrement_queued();
            }
        });
    }

    // Reader thread: take snapshots and validate them
    std::atomic<uint64_t> snapshot_count{0};
    threads.emplace_back([this, &start_latch, &running, &snapshot_count]() {
        start_latch.arrive_and_wait();
        while (running.load(std::memory_order_relaxed)) {
            auto snap = metrics.snapshot();
            // Snapshot values should be non-negative (no corruption)
            // Since these are uint64_t, they're always >= 0, but
            // load_score() should not produce NaN or inf
            EXPECT_FALSE(std::isnan(snap.load_score()));
            EXPECT_FALSE(std::isinf(snap.load_score()));
            snapshot_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Let threads run briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running.store(false, std::memory_order_relaxed);

    for (auto& thread : threads) {
        thread.join();
    }

    // Verify we actually took some snapshots
    EXPECT_GT(snapshot_count.load(), 0u);
}

// =============================================================================
// Barrier-Synchronized Phases
// =============================================================================

TEST_F(ShardLoadMetricsConcurrencyTest, BarrierSynchronizedIncrementDecrement) {
    // Phase 1: all threads increment. Barrier. Check sum.
    // Phase 2: all threads decrement. Barrier. Check zero.
    constexpr int kPhaseOps = 1000;
    std::barrier sync_barrier(kNumThreads);
    std::vector<std::thread> threads;
    std::atomic<bool> phase1_ok{true};

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([this, &sync_barrier, &phase1_ok, t]() {
            // Phase 1: increment
            for (int i = 0; i < kPhaseOps; ++i) {
                metrics.increment_active();
            }
            sync_barrier.arrive_and_wait();

            // After phase 1, exactly kNumThreads * kPhaseOps active
            if (t == 0) {
                uint64_t expected = static_cast<uint64_t>(kNumThreads) * kPhaseOps;
                if (metrics.active_requests() != expected) {
                    phase1_ok.store(false, std::memory_order_relaxed);
                }
            }
            sync_barrier.arrive_and_wait();

            // Phase 2: decrement
            for (int i = 0; i < kPhaseOps; ++i) {
                metrics.decrement_active();
            }
            sync_barrier.arrive_and_wait();
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_TRUE(phase1_ok.load());
    EXPECT_EQ(metrics.active_requests(), 0u);
}

// =============================================================================
// Thread-local Lifecycle Under Concurrent Access
// =============================================================================

TEST_F(ShardLoadMetricsConcurrencyTest, ConcurrentAccessToThreadLocalAccessor) {
    // Multiple threads access the thread-local shard_load_metrics() accessor.
    // Each thread gets its own thread-local instance (no cross-thread sharing).
    // Validates no crashes from concurrent lazy initialization.
    std::latch start_latch(kNumThreads);
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&start_latch, &success_count]() {
            start_latch.arrive_and_wait();
            // Each thread gets its own thread-local instance
            auto& m = shard_load_metrics();
            m.increment_active();
            m.increment_queued();
            m.record_request_completed();
            m.decrement_active();
            m.decrement_queued();

            auto snap = m.snapshot();
            if (snap.active_requests == 0 && snap.queued_requests == 0 &&
                snap.total_requests == 1) {
                success_count.fetch_add(1, std::memory_order_relaxed);
            }

            cleanup_shard_load_metrics();
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(success_count.load(), kNumThreads);
}
