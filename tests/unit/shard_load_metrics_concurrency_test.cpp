// Ranvier Core - Shard Load Metrics Concurrency Tests
//
// Validates shard-local (thread-per-core) metrics isolation under concurrent
// execution. Each thread creates its own ShardLoadMetrics instance, matching
// Seastar's shared-nothing model where each shard owns its own counters.
// Tests verify that concurrent shard activity produces correct per-shard
// results with no cross-shard interference.

#include "shard_load_metrics.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <cmath>
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
};

// =============================================================================
// Per-Shard Increment/Decrement Tests
// =============================================================================

TEST_F(ShardLoadMetricsConcurrencyTest, ConcurrentIncrementActive) {
    // Each shard (thread) increments its own active_requests counter.
    // Final value per shard must equal kOpsPerThread.
    std::latch start_latch(kNumThreads);
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&start_latch, &success_count]() {
            ShardLoadMetrics metrics;
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                metrics.increment_active();
            }
            if (metrics.active_requests() == static_cast<uint64_t>(kOpsPerThread)) {
                success_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(success_count.load(), kNumThreads);
}

TEST_F(ShardLoadMetricsConcurrencyTest, ConcurrentIncrementDecrement) {
    // Each shard increments then decrements. Net per shard = 0.
    std::latch start_latch(kNumThreads);
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&start_latch, &success_count]() {
            ShardLoadMetrics metrics;
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                metrics.increment_active();
            }
            for (int i = 0; i < kOpsPerThread; ++i) {
                metrics.decrement_active();
            }
            if (metrics.active_requests() == 0u) {
                success_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(success_count.load(), kNumThreads);
}

TEST_F(ShardLoadMetricsConcurrencyTest, ConcurrentIncrementQueued) {
    std::latch start_latch(kNumThreads);
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&start_latch, &success_count]() {
            ShardLoadMetrics metrics;
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                metrics.increment_queued();
            }
            if (metrics.queued_requests() == static_cast<uint64_t>(kOpsPerThread)) {
                success_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(success_count.load(), kNumThreads);
}

TEST_F(ShardLoadMetricsConcurrencyTest, ConcurrentIncrementDecrementQueued) {
    std::latch start_latch(kNumThreads);
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&start_latch, &success_count]() {
            ShardLoadMetrics metrics;
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                metrics.increment_queued();
            }
            for (int i = 0; i < kOpsPerThread; ++i) {
                metrics.decrement_queued();
            }
            if (metrics.queued_requests() == 0u) {
                success_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(success_count.load(), kNumThreads);
}

TEST_F(ShardLoadMetricsConcurrencyTest, ConcurrentRecordRequestCompleted) {
    std::latch start_latch(kNumThreads);
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&start_latch, &success_count]() {
            ShardLoadMetrics metrics;
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                metrics.record_request_completed();
            }
            if (metrics.total_requests() == static_cast<uint64_t>(kOpsPerThread)) {
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
// Mixed Counter Per-Shard Access
// =============================================================================

TEST_F(ShardLoadMetricsConcurrencyTest, ConcurrentMixedCounterOperations) {
    // Each shard operates on one counter type. All shards run simultaneously.
    // Validates no interference between shards or between counter fields.
    std::latch start_latch(kNumThreads);
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&start_latch, &success_count, t]() {
            ShardLoadMetrics metrics;
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                switch (t % 3) {
                    case 0: metrics.increment_active(); break;
                    case 1: metrics.increment_queued(); break;
                    case 2: metrics.record_request_completed(); break;
                }
            }

            bool ok = false;
            switch (t % 3) {
                case 0: ok = (metrics.active_requests() == static_cast<uint64_t>(kOpsPerThread)); break;
                case 1: ok = (metrics.queued_requests() == static_cast<uint64_t>(kOpsPerThread)); break;
                case 2: ok = (metrics.total_requests() == static_cast<uint64_t>(kOpsPerThread)); break;
            }
            // Also verify untouched counters remain zero
            switch (t % 3) {
                case 0: ok = ok && (metrics.queued_requests() == 0u) && (metrics.total_requests() == 0u); break;
                case 1: ok = ok && (metrics.active_requests() == 0u) && (metrics.total_requests() == 0u); break;
                case 2: ok = ok && (metrics.active_requests() == 0u) && (metrics.queued_requests() == 0u); break;
            }
            if (ok) {
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
// RAII Guard Per-Shard Tests
// =============================================================================

TEST_F(ShardLoadMetricsConcurrencyTest, ConcurrentActiveRequestGuards) {
    // Each shard creates and destroys guards. After all iterations, counter = 0.
    std::latch start_latch(kNumThreads);
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&start_latch, &success_count]() {
            ShardLoadMetrics metrics;
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                ShardLoadMetrics::ActiveRequestGuard guard(metrics);
                // Guard is destroyed at end of each iteration
            }
            if (metrics.active_requests() == 0u) {
                success_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(success_count.load(), kNumThreads);
}

TEST_F(ShardLoadMetricsConcurrencyTest, ConcurrentQueuedRequestGuards) {
    std::latch start_latch(kNumThreads);
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&start_latch, &success_count]() {
            ShardLoadMetrics metrics;
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                ShardLoadMetrics::QueuedRequestGuard guard(metrics);
            }
            if (metrics.queued_requests() == 0u) {
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
// Snapshot Per-Shard Test
// =============================================================================

TEST_F(ShardLoadMetricsConcurrencyTest, SnapshotDuringConcurrentUpdates) {
    // Each shard modifies its own counters and takes snapshots.
    // Validates snapshot() returns consistent data under per-shard access.
    std::latch start_latch(kNumThreads);
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&start_latch, &success_count]() {
            ShardLoadMetrics metrics;
            start_latch.arrive_and_wait();

            for (int i = 0; i < kOpsPerThread; ++i) {
                metrics.increment_active();
                metrics.increment_queued();
                metrics.record_request_completed();

                auto snap = metrics.snapshot();
                // Snapshot must be consistent (no torn reads on this shard)
                if (std::isnan(snap.load_score()) || std::isinf(snap.load_score())) {
                    return;  // Fail: corrupted snapshot
                }
                // active and queued grow monotonically in this loop
                if (snap.active_requests != static_cast<uint64_t>(i + 1)) {
                    return;
                }

                metrics.decrement_active();
                metrics.decrement_queued();
            }

            // After loop: active=0, queued=0, total=kOpsPerThread
            if (metrics.active_requests() == 0u &&
                metrics.queued_requests() == 0u &&
                metrics.total_requests() == static_cast<uint64_t>(kOpsPerThread)) {
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
// Barrier-Synchronized Phases
// =============================================================================

TEST_F(ShardLoadMetricsConcurrencyTest, BarrierSynchronizedIncrementDecrement) {
    // Phase 1: all shards increment their own counters. Barrier. Verify.
    // Phase 2: all shards decrement. Barrier. Verify zero.
    constexpr int kPhaseOps = 1000;
    std::barrier sync_barrier(kNumThreads);
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&sync_barrier, &success_count]() {
            ShardLoadMetrics metrics;

            // Phase 1: increment
            for (int i = 0; i < kPhaseOps; ++i) {
                metrics.increment_active();
            }
            sync_barrier.arrive_and_wait();

            // Verify phase 1
            if (metrics.active_requests() != static_cast<uint64_t>(kPhaseOps)) {
                return;
            }
            sync_barrier.arrive_and_wait();

            // Phase 2: decrement
            for (int i = 0; i < kPhaseOps; ++i) {
                metrics.decrement_active();
            }
            sync_barrier.arrive_and_wait();

            // Verify phase 2
            if (metrics.active_requests() == 0u) {
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
