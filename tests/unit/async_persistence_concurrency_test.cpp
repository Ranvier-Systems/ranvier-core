// Ranvier Core - Async Persistence Concurrency Stress Tests
//
// Exercises the SPSC (single-producer single-consumer) queue contract of
// AsyncPersistenceManager. The reactor thread is the sole producer; a
// separate reader thread may observe queue_depth() (atomic counter).
//
// Tests validate queue depth accounting, backpressure enforcement, and
// mixed operation type handling under high throughput from a single producer.

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "async_persistence.hpp"
#include "mocks/mock_persistence_store.hpp"
#include <atomic>
#include <thread>
#include <vector>

using namespace ranvier;
using ::testing::NiceMock;

// =============================================================================
// Test Fixture
// =============================================================================

class AsyncPersistenceConcurrencyTest : public ::testing::Test {
protected:
    static constexpr int kTotalOps = 40'000;

    std::unique_ptr<AsyncPersistenceManager> create_manager(size_t max_queue_depth) {
        AsyncPersistenceConfig config;
        config.max_queue_depth = max_queue_depth;
        auto store = std::make_unique<NiceMock<MockPersistenceStore>>();
        store->open("/tmp/test");
        return std::make_unique<AsyncPersistenceManager>(config, std::move(store));
    }
};

// =============================================================================
// Single-Producer Enqueue Tests
// =============================================================================

TEST_F(AsyncPersistenceConcurrencyTest, ConcurrentEnqueueSaveRoute) {
    // Single producer enqueues SaveRouteOps.
    // Total enqueued must equal kTotalOps.
    auto manager = create_manager(kTotalOps + 1000);

    for (int i = 0; i < kTotalOps; ++i) {
        std::vector<TokenId> tokens = {
            static_cast<TokenId>(i),
            static_cast<TokenId>(i + 1)
        };
        manager->queue_save_route(tokens, static_cast<BackendId>(i / 5000));
    }

    EXPECT_EQ(manager->queue_depth(), static_cast<size_t>(kTotalOps));
    EXPECT_EQ(manager->operations_dropped(), 0u);
}

TEST_F(AsyncPersistenceConcurrencyTest, ConcurrentEnqueueMixedOps) {
    // Single producer enqueues different operation types.
    auto manager = create_manager(kTotalOps + 1000);

    for (int i = 0; i < kTotalOps; ++i) {
        switch (i % 4) {
            case 0: {
                std::vector<TokenId> tokens = {
                    static_cast<TokenId>(i)
                };
                manager->queue_save_route(tokens, static_cast<BackendId>(i / 1000));
                break;
            }
            case 1:
                manager->queue_save_backend(
                    static_cast<BackendId>(i),
                    "10.0." + std::to_string(i / 256) + "." + std::to_string(i % 256),
                    static_cast<uint16_t>(8080 + (i % 100)),
                    100, 0);
                break;
            case 2:
                manager->queue_remove_backend(static_cast<BackendId>(i));
                break;
            case 3:
                manager->queue_remove_routes_for_backend(static_cast<BackendId>(i / 1000));
                break;
        }
    }

    EXPECT_EQ(manager->queue_depth(), static_cast<size_t>(kTotalOps));
    EXPECT_EQ(manager->operations_dropped(), 0u);
}

// =============================================================================
// Backpressure Tests
// =============================================================================

TEST_F(AsyncPersistenceConcurrencyTest, ConcurrentBackpressureEnforcement) {
    // Queue is smaller than total ops. Validates that:
    //   queue_depth + operations_dropped == total_attempted
    constexpr size_t kMaxQueue = 1000;
    auto manager = create_manager(kMaxQueue);

    for (int i = 0; i < kTotalOps; ++i) {
        std::vector<TokenId> tokens = {
            static_cast<TokenId>(i)
        };
        manager->queue_save_route(tokens, static_cast<BackendId>(i / 5000));
    }

    size_t total_attempted = static_cast<size_t>(kTotalOps);
    size_t queued = manager->queue_depth();
    size_t dropped = manager->operations_dropped();

    // Invariant: queued + dropped == total_attempted
    EXPECT_EQ(queued + dropped, total_attempted);

    // Queue should be at capacity
    EXPECT_EQ(queued, kMaxQueue);

    // Remaining operations must have been dropped
    EXPECT_EQ(dropped, total_attempted - kMaxQueue);
}

TEST_F(AsyncPersistenceConcurrencyTest, BackpressureIndicatorConsistent) {
    // With a tiny queue, is_backpressured() should be true after filling.
    constexpr size_t kMaxQueue = 50;
    auto manager = create_manager(kMaxQueue);

    for (int i = 0; i < 800; ++i) {
        std::vector<TokenId> tokens = {static_cast<TokenId>(i)};
        manager->queue_save_route(tokens, static_cast<BackendId>(i / 100));
    }

    EXPECT_TRUE(manager->is_backpressured());
    EXPECT_EQ(manager->queue_depth(), kMaxQueue);
}

// =============================================================================
// Reader/Writer Tests (reader thread polls atomic queue_depth)
// =============================================================================

TEST_F(AsyncPersistenceConcurrencyTest, ConcurrentQueueDepthReading) {
    // Single producer enqueues while reader threads poll queue_depth().
    // Validates the atomic _queue_size counter stays consistent.
    auto manager = create_manager(kTotalOps + 1000);
    std::atomic<bool> running{true};
    std::atomic<size_t> max_depth_seen{0};
    std::atomic<uint64_t> reads_performed{0};

    // Reader threads: continuously read queue_depth (atomic, safe cross-thread)
    std::vector<std::thread> readers;
    for (int r = 0; r < 2; ++r) {
        readers.emplace_back([&manager, &running,
                              &max_depth_seen, &reads_performed]() {
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

    // Single producer enqueues on this thread
    for (int i = 0; i < kTotalOps; ++i) {
        std::vector<TokenId> tokens = {static_cast<TokenId>(i)};
        manager->queue_save_route(tokens, static_cast<BackendId>(i / 5000));
    }

    running.store(false, std::memory_order_relaxed);
    for (auto& t : readers) {
        t.join();
    }

    EXPECT_EQ(manager->queue_depth(), static_cast<size_t>(kTotalOps));
    EXPECT_GT(reads_performed.load(), 0u);
}

// =============================================================================
// ClearAll
// =============================================================================

TEST_F(AsyncPersistenceConcurrencyTest, ClearAllDuringEnqueue) {
    // Single producer enqueues, then calls queue_clear_all() mid-stream.
    // After clear, queue should contain ClearAllOp plus anything enqueued after.
    auto manager = create_manager(100000);

    // Enqueue some ops
    for (int i = 0; i < 1000; ++i) {
        std::vector<TokenId> tokens = {static_cast<TokenId>(i)};
        manager->queue_save_route(tokens, static_cast<BackendId>(i / 100));
    }
    EXPECT_EQ(manager->queue_depth(), 1000u);

    // Clear all — drains ring buffer and enqueues ClearAllOp
    manager->queue_clear_all();
    EXPECT_EQ(manager->queue_depth(), 1u);

    // Enqueue more after clear
    for (int i = 0; i < 500; ++i) {
        std::vector<TokenId> tokens = {static_cast<TokenId>(i + 10000)};
        manager->queue_save_route(tokens, static_cast<BackendId>(i / 100));
    }
    EXPECT_EQ(manager->queue_depth(), 501u);  // ClearAllOp + 500 new ops
}

// =============================================================================
// Stress: High Throughput Small Queue
// =============================================================================

TEST_F(AsyncPersistenceConcurrencyTest, HighContentionSmallQueue) {
    // Single producer pushes many ops into a very small queue.
    // Validates backpressure accounting under high drop rate.
    constexpr size_t kTinyQueue = 10;
    constexpr int kHighOps = 80'000;
    auto manager = create_manager(kTinyQueue);

    for (int i = 0; i < kHighOps; ++i) {
        std::vector<TokenId> tokens = {static_cast<TokenId>(i)};
        manager->queue_save_route(tokens, static_cast<BackendId>(i / 10000));
    }

    size_t total = static_cast<size_t>(kHighOps);
    EXPECT_EQ(manager->queue_depth() + manager->operations_dropped(), total);
    EXPECT_EQ(manager->queue_depth(), kTinyQueue);
    EXPECT_EQ(manager->operations_dropped(), total - kTinyQueue);
}

TEST_F(AsyncPersistenceConcurrencyTest, CloseDuringEnqueue) {
    // Single producer enqueues, then close() is called on the store.
    // After close(), is_open() returns false, so queue_save_route silently
    // drops operations. Validates no crash or data corruption.
    auto manager = create_manager(100000);

    // Enqueue some ops
    for (int i = 0; i < 2000; ++i) {
        std::vector<TokenId> tokens = {static_cast<TokenId>(i)};
        manager->queue_save_route(tokens, static_cast<BackendId>(i / 500));
    }
    size_t queued_before_close = manager->queue_depth();
    EXPECT_EQ(queued_before_close, 2000u);

    // Close the store
    manager->close();
    EXPECT_FALSE(manager->is_open());

    // Further enqueues should be silently skipped (is_open check)
    for (int i = 0; i < 1000; ++i) {
        std::vector<TokenId> tokens = {static_cast<TokenId>(i + 10000)};
        manager->queue_save_route(tokens, static_cast<BackendId>(i / 500));
    }

    // Queue depth unchanged — new ops were silently dropped
    EXPECT_EQ(manager->queue_depth(), queued_before_close);
    EXPECT_EQ(manager->operations_dropped(), 0u);  // Not backpressure drops
}

TEST_F(AsyncPersistenceConcurrencyTest, ConcurrentOperationsProcessedCounter) {
    // Verify the _ops_dropped atomic counter is accurate when hitting backpressure.
    constexpr size_t kMediumQueue = 5000;
    auto manager = create_manager(kMediumQueue);

    for (int i = 0; i < kTotalOps; ++i) {
        std::vector<TokenId> tokens = {
            static_cast<TokenId>(i)
        };
        manager->queue_save_route(tokens, static_cast<BackendId>(i / 5000));
    }

    size_t total_attempted = static_cast<size_t>(kTotalOps);
    size_t queued = manager->queue_depth();
    size_t dropped = manager->operations_dropped();

    // Invariant: queued + dropped == total attempted
    EXPECT_EQ(queued + dropped, total_attempted);
    // operations_processed should be 0 (no flush timer running)
    EXPECT_EQ(manager->operations_processed(), 0u);
}

// =============================================================================
// Large Token Vector Stress
// =============================================================================

TEST_F(AsyncPersistenceConcurrencyTest, ConcurrentLargeTokenVectors) {
    // Single producer enqueues large token vectors to stress memory allocation.
    constexpr int kLargeOps = 4000;
    constexpr size_t kTokenVectorSize = 1000;
    auto manager = create_manager(kLargeOps + 100);

    for (int i = 0; i < kLargeOps; ++i) {
        std::vector<TokenId> tokens(kTokenVectorSize);
        for (size_t j = 0; j < kTokenVectorSize; ++j) {
            tokens[j] = static_cast<TokenId>(i * 1000 + j);
        }
        manager->queue_save_route(tokens, static_cast<BackendId>(i / 500));
    }

    EXPECT_EQ(manager->queue_depth(), static_cast<size_t>(kLargeOps));
    EXPECT_EQ(manager->operations_dropped(), 0u);
}
