// Ranvier Core - Async Persistence Concurrency Stress Tests
//
// Exercises concurrent enqueue operations on AsyncPersistenceManager's
// mutex-protected queue from multiple threads using C++20 std::latch.
// Validates queue depth accounting, backpressure enforcement, and
// mixed operation type handling under heavy contention.

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "async_persistence.hpp"
#include "mocks/mock_persistence_store.hpp"
#include <atomic>
#include <latch>
#include <thread>
#include <vector>

using namespace ranvier;
using ::testing::NiceMock;

// =============================================================================
// Test Fixture
// =============================================================================

class AsyncPersistenceConcurrencyTest : public ::testing::Test {
protected:
    static constexpr int kNumThreads = 8;
    static constexpr int kOpsPerThread = 5'000;

    std::unique_ptr<AsyncPersistenceManager> create_manager(size_t max_queue_depth) {
        AsyncPersistenceConfig config;
        config.max_queue_depth = max_queue_depth;
        auto store = std::make_unique<NiceMock<MockPersistenceStore>>();
        store->open("/tmp/test");
        return std::make_unique<AsyncPersistenceManager>(config, std::move(store));
    }
};

// =============================================================================
// Concurrent Enqueue Tests
// =============================================================================

TEST_F(AsyncPersistenceConcurrencyTest, ConcurrentEnqueueSaveRoute) {
    // All threads enqueue SaveRouteOp concurrently.
    // Total enqueued must equal kNumThreads * kOpsPerThread.
    auto manager = create_manager(kNumThreads * kOpsPerThread + 1000);
    std::latch start_latch(kNumThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&manager, &start_latch, t]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                std::vector<TokenId> tokens = {
                    static_cast<TokenId>(t * 100000 + i),
                    static_cast<TokenId>(t * 100000 + i + 1)
                };
                manager->queue_save_route(tokens, static_cast<BackendId>(t));
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(manager->queue_depth(),
              static_cast<size_t>(kNumThreads) * kOpsPerThread);
    EXPECT_EQ(manager->operations_dropped(), 0u);
}

TEST_F(AsyncPersistenceConcurrencyTest, ConcurrentEnqueueMixedOps) {
    // Threads enqueue different operation types concurrently.
    auto manager = create_manager(kNumThreads * kOpsPerThread + 1000);
    std::latch start_latch(kNumThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&manager, &start_latch, t]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                switch (i % 4) {
                    case 0: {
                        std::vector<TokenId> tokens = {
                            static_cast<TokenId>(t * 100000 + i)
                        };
                        manager->queue_save_route(tokens, static_cast<BackendId>(t));
                        break;
                    }
                    case 1:
                        manager->queue_save_backend(
                            static_cast<BackendId>(t * 1000 + i),
                            "10.0." + std::to_string(t) + "." + std::to_string(i % 256),
                            static_cast<uint16_t>(8080 + (i % 100)),
                            100, 0);
                        break;
                    case 2:
                        manager->queue_remove_backend(
                            static_cast<BackendId>(t * 1000 + i));
                        break;
                    case 3:
                        manager->queue_remove_routes_for_backend(
                            static_cast<BackendId>(t));
                        break;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(manager->queue_depth(),
              static_cast<size_t>(kNumThreads) * kOpsPerThread);
    EXPECT_EQ(manager->operations_dropped(), 0u);
}

// =============================================================================
// Concurrent Backpressure Tests
// =============================================================================

TEST_F(AsyncPersistenceConcurrencyTest, ConcurrentBackpressureEnforcement) {
    // Queue is smaller than total ops. Validates that:
    //   queue_depth + operations_dropped == total_attempted
    constexpr size_t kMaxQueue = 1000;
    auto manager = create_manager(kMaxQueue);
    std::latch start_latch(kNumThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&manager, &start_latch, t]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                std::vector<TokenId> tokens = {
                    static_cast<TokenId>(t * 100000 + i)
                };
                manager->queue_save_route(tokens, static_cast<BackendId>(t));
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    size_t total_attempted = static_cast<size_t>(kNumThreads) * kOpsPerThread;
    size_t queued = manager->queue_depth();
    size_t dropped = manager->operations_dropped();

    // Invariant: queued + dropped == total_attempted
    EXPECT_EQ(queued + dropped, total_attempted);

    // Queue should be at capacity
    EXPECT_LE(queued, kMaxQueue);

    // Some operations must have been dropped
    EXPECT_GT(dropped, 0u);
}

TEST_F(AsyncPersistenceConcurrencyTest, BackpressureIndicatorConsistent) {
    // With a tiny queue, is_backpressured() should be true after filling.
    constexpr size_t kMaxQueue = 50;
    auto manager = create_manager(kMaxQueue);
    std::latch start_latch(kNumThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&manager, &start_latch, t]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < 100; ++i) {
                std::vector<TokenId> tokens = {static_cast<TokenId>(t * 1000 + i)};
                manager->queue_save_route(tokens, static_cast<BackendId>(t));
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_TRUE(manager->is_backpressured());
    EXPECT_LE(manager->queue_depth(), kMaxQueue);
}

// =============================================================================
// Concurrent Reader/Writer Tests
// =============================================================================

TEST_F(AsyncPersistenceConcurrencyTest, ConcurrentQueueDepthReading) {
    // Writers enqueue while readers poll queue_depth().
    // Validates the atomic _queue_size counter stays consistent.
    auto manager = create_manager(kNumThreads * kOpsPerThread + 1000);
    std::atomic<bool> running{true};
    std::latch start_latch(kNumThreads + 2);  // writers + 2 readers
    std::vector<std::thread> threads;

    // Writer threads
    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&manager, &start_latch, t]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                std::vector<TokenId> tokens = {static_cast<TokenId>(t * 100000 + i)};
                manager->queue_save_route(tokens, static_cast<BackendId>(t));
            }
        });
    }

    // Reader threads: continuously read queue_depth and track peak value
    std::atomic<size_t> max_depth_seen{0};
    std::atomic<uint64_t> reads_performed{0};
    for (int r = 0; r < 2; ++r) {
        threads.emplace_back([&manager, &start_latch, &running,
                              &max_depth_seen, &reads_performed]() {
            start_latch.arrive_and_wait();
            while (running.load(std::memory_order_relaxed)) {
                size_t depth = manager->queue_depth();
                size_t current_max = max_depth_seen.load(std::memory_order_relaxed);
                while (depth > current_max &&
                       !max_depth_seen.compare_exchange_weak(
                           current_max, depth, std::memory_order_relaxed)) {
                    current_max = max_depth_seen.load(std::memory_order_relaxed);
                }
                reads_performed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Wait for writers
    for (int t = 0; t < kNumThreads; ++t) {
        threads[t].join();
    }
    running.store(false, std::memory_order_relaxed);

    // Wait for readers
    for (size_t t = kNumThreads; t < threads.size(); ++t) {
        threads[t].join();
    }

    EXPECT_EQ(manager->queue_depth(),
              static_cast<size_t>(kNumThreads) * kOpsPerThread);
    EXPECT_GT(reads_performed.load(), 0u);
}

// =============================================================================
// ClearAll Under Contention
// =============================================================================

TEST_F(AsyncPersistenceConcurrencyTest, ClearAllDuringConcurrentEnqueue) {
    // Writers enqueue while one thread calls queue_clear_all().
    // After completion, queue should contain only ClearAllOp (depth=1)
    // or whatever was enqueued after the clear.
    auto manager = create_manager(100000);
    std::latch start_latch(kNumThreads + 1);
    std::vector<std::thread> threads;

    // Writer threads
    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&manager, &start_latch, t]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < 1000; ++i) {
                std::vector<TokenId> tokens = {static_cast<TokenId>(t * 10000 + i)};
                manager->queue_save_route(tokens, static_cast<BackendId>(t));
            }
        });
    }

    // Clear thread: waits for some enqueues then clears
    threads.emplace_back([&manager, &start_latch]() {
        start_latch.arrive_and_wait();
        // Let some enqueues happen
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        manager->queue_clear_all();
    });

    for (auto& thread : threads) {
        thread.join();
    }

    // After clear + remaining enqueues, the queue state is valid.
    // At least the ClearAllOp should be in the queue
    EXPECT_GE(manager->queue_depth(), 1u);
    // Queue depth must not exceed configured limit
    EXPECT_LE(manager->queue_depth(), manager->max_queue_depth());
}

// =============================================================================
// Stress: High Contention Short Queue
// =============================================================================

TEST_F(AsyncPersistenceConcurrencyTest, HighContentionSmallQueue) {
    // Many threads fight over a very small queue to maximize mutex contention.
    constexpr size_t kTinyQueue = 10;
    constexpr int kHighOps = 10'000;
    auto manager = create_manager(kTinyQueue);
    std::latch start_latch(kNumThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&manager, &start_latch, t]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < kHighOps; ++i) {
                std::vector<TokenId> tokens = {static_cast<TokenId>(t * 100000 + i)};
                manager->queue_save_route(tokens, static_cast<BackendId>(t));
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    size_t total = static_cast<size_t>(kNumThreads) * kHighOps;
    EXPECT_EQ(manager->queue_depth() + manager->operations_dropped(), total);
    EXPECT_LE(manager->queue_depth(), kTinyQueue);
    EXPECT_GT(manager->operations_dropped(), 0u);
}

// =============================================================================
// Close During Concurrent Enqueue (Shutdown Stress)
// =============================================================================

TEST_F(AsyncPersistenceConcurrencyTest, CloseDuringConcurrentEnqueue) {
    // Writers enqueue while one thread calls close() on the manager.
    // After close(), is_open() returns false, so queue_save_route silently
    // drops operations. Validates no crash or data corruption when the
    // store is closed mid-enqueue.
    auto manager = create_manager(100000);
    std::latch start_latch(kNumThreads + 1);  // writers + closer
    std::vector<std::thread> threads;
    std::atomic<uint64_t> enqueue_attempts{0};

    // Writer threads: enqueue continuously
    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&manager, &start_latch, &enqueue_attempts, t]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < 5000; ++i) {
                std::vector<TokenId> tokens = {
                    static_cast<TokenId>(t * 100000 + i)
                };
                manager->queue_save_route(tokens, static_cast<BackendId>(t));
                enqueue_attempts.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Closer thread: wait briefly then close the store
    threads.emplace_back([&manager, &start_latch]() {
        start_latch.arrive_and_wait();
        // Let some enqueues happen
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        manager->close();
    });

    for (auto& thread : threads) {
        thread.join();
    }

    // After close, some ops were enqueued (before close) and some were dropped
    // (after close when is_open() returns false). Total should be consistent.
    size_t queued = manager->queue_depth();
    size_t dropped = manager->operations_dropped();
    uint64_t attempted = enqueue_attempts.load();

    // Queued + dropped + silently-skipped (is_open check) == total attempts
    // We can't track the silently-skipped ones, but we can verify:
    EXPECT_LE(queued + dropped, attempted);
    // The store should be closed
    EXPECT_FALSE(manager->is_open());
}

TEST_F(AsyncPersistenceConcurrencyTest, ConcurrentOperationsProcessedCounter) {
    // Verify the _ops_processed and _ops_dropped atomic counters are accurate
    // when multiple threads are enqueuing (some hitting backpressure).
    constexpr size_t kMediumQueue = 5000;
    auto manager = create_manager(kMediumQueue);
    std::latch start_latch(kNumThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&manager, &start_latch, t]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < kOpsPerThread; ++i) {
                std::vector<TokenId> tokens = {
                    static_cast<TokenId>(t * 100000 + i)
                };
                manager->queue_save_route(tokens, static_cast<BackendId>(t));
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    size_t total_attempted = static_cast<size_t>(kNumThreads) * kOpsPerThread;
    size_t queued = manager->queue_depth();
    size_t dropped = manager->operations_dropped();

    // Invariant: queued + dropped == total attempted
    EXPECT_EQ(queued + dropped, total_attempted);
    // operations_processed should be 0 (no flush timer running)
    EXPECT_EQ(manager->operations_processed(), 0u);
}

// =============================================================================
// Large Token Vector Under Contention
// =============================================================================

TEST_F(AsyncPersistenceConcurrencyTest, ConcurrentLargeTokenVectors) {
    // Each thread enqueues large token vectors to stress memory allocation
    // under contention.
    constexpr int kLargeOps = 500;
    constexpr size_t kTokenVectorSize = 1000;
    auto manager = create_manager(kNumThreads * kLargeOps + 100);
    std::latch start_latch(kNumThreads);
    std::vector<std::thread> threads;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&manager, &start_latch, t]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < kLargeOps; ++i) {
                std::vector<TokenId> tokens(kTokenVectorSize);
                for (size_t j = 0; j < kTokenVectorSize; ++j) {
                    tokens[j] = static_cast<TokenId>(t * 1000000 + i * 1000 + j);
                }
                manager->queue_save_route(tokens, static_cast<BackendId>(t));
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(manager->queue_depth(),
              static_cast<size_t>(kNumThreads) * kLargeOps);
    EXPECT_EQ(manager->operations_dropped(), 0u);
}
