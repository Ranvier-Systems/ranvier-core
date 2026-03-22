// Ranvier Core - MPSC Ring Buffer Unit Tests
//
// Tests the lock-free multi-producer single-consumer ring buffer used by
// AsyncPersistenceManager to decouple multiple reactor shards (producers)
// from the persistence worker thread (consumer) without any mutex.
//
// Pure C++20 — no Seastar dependency.

#include "mpsc_ring_buffer.hpp"
#include <gtest/gtest.h>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace ranvier;

// =============================================================================
// Test Fixture
// =============================================================================

class MpscRingBufferTest : public ::testing::Test {};

// =============================================================================
// Construction & Capacity
// =============================================================================

TEST_F(MpscRingBufferTest, PowerOfTwoCapacityUnchanged) {
    MpscRingBuffer<int> rb(16);
    EXPECT_EQ(rb.capacity(), 16u);
}

TEST_F(MpscRingBufferTest, NonPowerOfTwoRoundsUp) {
    MpscRingBuffer<int> rb(10);
    // 10 rounds up to 16
    EXPECT_EQ(rb.capacity(), 16u);
}

TEST_F(MpscRingBufferTest, CapacityOfOne) {
    MpscRingBuffer<int> rb(1);
    EXPECT_EQ(rb.capacity(), 1u);
}

TEST_F(MpscRingBufferTest, CapacityOfTwo) {
    MpscRingBuffer<int> rb(2);
    EXPECT_EQ(rb.capacity(), 2u);
}

TEST_F(MpscRingBufferTest, LargeCapacity) {
    MpscRingBuffer<int> rb(100000);
    // 100000 rounds up to 131072
    EXPECT_EQ(rb.capacity(), 131072u);
}

// =============================================================================
// Empty Buffer
// =============================================================================

TEST_F(MpscRingBufferTest, EmptyBufferSizeIsZero) {
    MpscRingBuffer<int> rb(16);
    EXPECT_EQ(rb.size_approx(), 0u);
}

TEST_F(MpscRingBufferTest, TryPopOnEmptyReturnsFalse) {
    MpscRingBuffer<int> rb(16);
    int val = -1;
    EXPECT_FALSE(rb.try_pop(val));
    EXPECT_EQ(val, -1);  // Unchanged
}

TEST_F(MpscRingBufferTest, DrainOnEmptyReturnsZero) {
    MpscRingBuffer<int> rb(16);
    std::vector<int> out;
    EXPECT_EQ(rb.drain(out, 100), 0u);
    EXPECT_TRUE(out.empty());
}

// =============================================================================
// Push / Pop Basics
// =============================================================================

TEST_F(MpscRingBufferTest, PushPopSingleElement) {
    MpscRingBuffer<int> rb(4);
    EXPECT_TRUE(rb.try_push(42));
    EXPECT_EQ(rb.size_approx(), 1u);

    int val = 0;
    EXPECT_TRUE(rb.try_pop(val));
    EXPECT_EQ(val, 42);
    EXPECT_EQ(rb.size_approx(), 0u);
}

TEST_F(MpscRingBufferTest, PushPopMultipleElements) {
    MpscRingBuffer<int> rb(8);
    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE(rb.try_push(std::move(i)));
    }
    EXPECT_EQ(rb.size_approx(), 8u);

    for (int i = 0; i < 8; ++i) {
        int val = -1;
        EXPECT_TRUE(rb.try_pop(val));
        EXPECT_EQ(val, i);
    }
    EXPECT_EQ(rb.size_approx(), 0u);
}

TEST_F(MpscRingBufferTest, FIFOOrdering) {
    MpscRingBuffer<int> rb(16);
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(rb.try_push(std::move(i)));
    }
    for (int i = 0; i < 10; ++i) {
        int val = -1;
        EXPECT_TRUE(rb.try_pop(val));
        EXPECT_EQ(val, i);
    }
}

// =============================================================================
// Full Buffer
// =============================================================================

TEST_F(MpscRingBufferTest, PushToFullReturnsFalse) {
    MpscRingBuffer<int> rb(4);  // capacity = 4
    EXPECT_TRUE(rb.try_push(1));
    EXPECT_TRUE(rb.try_push(2));
    EXPECT_TRUE(rb.try_push(3));
    EXPECT_TRUE(rb.try_push(4));
    EXPECT_FALSE(rb.try_push(5));  // Full
    EXPECT_EQ(rb.size_approx(), 4u);
}

TEST_F(MpscRingBufferTest, PushAfterPopFromFull) {
    MpscRingBuffer<int> rb(4);
    EXPECT_TRUE(rb.try_push(1));
    EXPECT_TRUE(rb.try_push(2));
    EXPECT_TRUE(rb.try_push(3));
    EXPECT_TRUE(rb.try_push(4));
    EXPECT_FALSE(rb.try_push(5));  // Full

    int val = 0;
    EXPECT_TRUE(rb.try_pop(val));
    EXPECT_EQ(val, 1);

    EXPECT_TRUE(rb.try_push(5));  // Now room for one
    EXPECT_EQ(rb.size_approx(), 4u);
}

// =============================================================================
// Wraparound
// =============================================================================

TEST_F(MpscRingBufferTest, IndicesWrapAroundCorrectly) {
    MpscRingBuffer<int> rb(4);
    // Push and pop repeatedly to force wraparound
    for (int round = 0; round < 20; ++round) {
        int v = round;
        EXPECT_TRUE(rb.try_push(std::move(v)));
        int val = -1;
        EXPECT_TRUE(rb.try_pop(val));
        EXPECT_EQ(val, round);
    }
    EXPECT_EQ(rb.size_approx(), 0u);
}

TEST_F(MpscRingBufferTest, FillDrainMultipleRounds) {
    MpscRingBuffer<int> rb(8);
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 8; ++i) {
            int v = round * 100 + i;
            EXPECT_TRUE(rb.try_push(std::move(v)));
        }
        EXPECT_FALSE(rb.try_push(999));  // Full

        for (int i = 0; i < 8; ++i) {
            int val = -1;
            EXPECT_TRUE(rb.try_pop(val));
            EXPECT_EQ(val, round * 100 + i);
        }
        EXPECT_EQ(rb.size_approx(), 0u);
    }
}

// =============================================================================
// Drain
// =============================================================================

TEST_F(MpscRingBufferTest, DrainPartialBatch) {
    MpscRingBuffer<int> rb(16);
    for (int i = 0; i < 10; ++i) {
        int v = i;
        EXPECT_TRUE(rb.try_push(std::move(v)));
    }

    std::vector<int> out;
    EXPECT_EQ(rb.drain(out, 5), 5u);
    EXPECT_EQ(out.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(out[i], i);
    }
    EXPECT_EQ(rb.size_approx(), 5u);
}

TEST_F(MpscRingBufferTest, DrainMoreThanAvailable) {
    MpscRingBuffer<int> rb(16);
    for (int i = 0; i < 3; ++i) {
        int v = i;
        EXPECT_TRUE(rb.try_push(std::move(v)));
    }

    std::vector<int> out;
    EXPECT_EQ(rb.drain(out, 100), 3u);
    EXPECT_EQ(out.size(), 3u);
    EXPECT_EQ(rb.size_approx(), 0u);
}

TEST_F(MpscRingBufferTest, DrainAllElements) {
    MpscRingBuffer<int> rb(16);
    for (int i = 0; i < 10; ++i) {
        int v = i;
        EXPECT_TRUE(rb.try_push(std::move(v)));
    }

    std::vector<int> out;
    EXPECT_EQ(rb.drain_all(out), 10u);
    EXPECT_EQ(out.size(), 10u);
    EXPECT_EQ(rb.size_approx(), 0u);

    // Verify all elements drained
    int val = -1;
    EXPECT_FALSE(rb.try_pop(val));
}

TEST_F(MpscRingBufferTest, DrainAppendsToExistingVector) {
    MpscRingBuffer<int> rb(16);
    int v1 = 10, v2 = 20;
    EXPECT_TRUE(rb.try_push(std::move(v1)));
    EXPECT_TRUE(rb.try_push(std::move(v2)));

    std::vector<int> out = {1, 2, 3};
    EXPECT_EQ(rb.drain(out, 10), 2u);
    ASSERT_EQ(out.size(), 5u);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[1], 2);
    EXPECT_EQ(out[2], 3);
    EXPECT_EQ(out[3], 10);
    EXPECT_EQ(out[4], 20);
}

TEST_F(MpscRingBufferTest, DrainWithWraparound) {
    MpscRingBuffer<int> rb(4);
    // Advance head past 0 to force wraparound during drain
    int v1 = 100, v2 = 200;
    EXPECT_TRUE(rb.try_push(std::move(v1)));
    EXPECT_TRUE(rb.try_push(std::move(v2)));
    int discard = 0;
    EXPECT_TRUE(rb.try_pop(discard));
    EXPECT_TRUE(rb.try_pop(discard));

    // Now push items that will wrap around the buffer
    int v3 = 10, v4 = 20, v5 = 30, v6 = 40;
    EXPECT_TRUE(rb.try_push(std::move(v3)));
    EXPECT_TRUE(rb.try_push(std::move(v4)));
    EXPECT_TRUE(rb.try_push(std::move(v5)));
    EXPECT_TRUE(rb.try_push(std::move(v6)));

    std::vector<int> out;
    EXPECT_EQ(rb.drain_all(out), 4u);
    ASSERT_EQ(out.size(), 4u);
    EXPECT_EQ(out[0], 10);
    EXPECT_EQ(out[1], 20);
    EXPECT_EQ(out[2], 30);
    EXPECT_EQ(out[3], 40);
}

// =============================================================================
// Move-Only Types
// =============================================================================

TEST_F(MpscRingBufferTest, MoveOnlyType) {
    MpscRingBuffer<std::unique_ptr<int>> rb(8);
    EXPECT_TRUE(rb.try_push(std::make_unique<int>(42)));
    EXPECT_TRUE(rb.try_push(std::make_unique<int>(99)));

    std::unique_ptr<int> val;
    EXPECT_TRUE(rb.try_pop(val));
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(*val, 42);

    EXPECT_TRUE(rb.try_pop(val));
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(*val, 99);
}

TEST_F(MpscRingBufferTest, DrainMoveOnlyType) {
    MpscRingBuffer<std::unique_ptr<int>> rb(8);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(rb.try_push(std::make_unique<int>(i * 10)));
    }

    std::vector<std::unique_ptr<int>> out;
    EXPECT_EQ(rb.drain_all(out), 5u);
    ASSERT_EQ(out.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        ASSERT_NE(out[i], nullptr);
        EXPECT_EQ(*out[i], i * 10);
    }
}

TEST_F(MpscRingBufferTest, StringMoveSemantics) {
    MpscRingBuffer<std::string> rb(8);
    std::string s = "hello world this is a long string to avoid SSO";
    EXPECT_TRUE(rb.try_push(std::move(s)));
    EXPECT_TRUE(s.empty());  // Moved-from

    std::string out;
    EXPECT_TRUE(rb.try_pop(out));
    EXPECT_EQ(out, "hello world this is a long string to avoid SSO");
}

// =============================================================================
// Variant Type (matches PersistenceOp usage)
// =============================================================================

TEST_F(MpscRingBufferTest, VariantType) {
    using Op = std::variant<int, std::string, std::vector<int>>;
    MpscRingBuffer<Op> rb(8);

    EXPECT_TRUE(rb.try_push(Op{42}));
    EXPECT_TRUE(rb.try_push(Op{std::string("test")}));
    EXPECT_TRUE(rb.try_push(Op{std::vector<int>{1, 2, 3}}));

    Op val;
    EXPECT_TRUE(rb.try_pop(val));
    EXPECT_EQ(std::get<int>(val), 42);

    EXPECT_TRUE(rb.try_pop(val));
    EXPECT_EQ(std::get<std::string>(val), "test");

    EXPECT_TRUE(rb.try_pop(val));
    EXPECT_EQ(std::get<std::vector<int>>(val), (std::vector<int>{1, 2, 3}));
}

// =============================================================================
// size_approx
// =============================================================================

TEST_F(MpscRingBufferTest, SizeApproxTracksCorrectly) {
    MpscRingBuffer<int> rb(8);
    EXPECT_EQ(rb.size_approx(), 0u);

    int v1 = 1, v2 = 2, v3 = 3;
    rb.try_push(std::move(v1));
    EXPECT_EQ(rb.size_approx(), 1u);

    rb.try_push(std::move(v2));
    rb.try_push(std::move(v3));
    EXPECT_EQ(rb.size_approx(), 3u);

    int val;
    rb.try_pop(val);
    EXPECT_EQ(rb.size_approx(), 2u);

    rb.try_pop(val);
    rb.try_pop(val);
    EXPECT_EQ(rb.size_approx(), 0u);
}

// =============================================================================
// Concurrent: Single Producer, Single Consumer
// =============================================================================

TEST_F(MpscRingBufferTest, ConcurrentSingleProducerSingleConsumer) {
    constexpr size_t kItemCount = 100'000;
    MpscRingBuffer<int> rb(1024);

    std::vector<int> consumed;
    consumed.reserve(kItemCount);

    // Consumer thread
    std::thread consumer([&rb, &consumed]() {
        size_t count = 0;
        while (count < kItemCount) {
            int val;
            if (rb.try_pop(val)) {
                consumed.push_back(val);
                ++count;
            }
        }
    });

    // Producer (this thread)
    for (size_t i = 0; i < kItemCount; ++i) {
        int v = static_cast<int>(i);
        while (!rb.try_push(std::move(v))) {
            // Spin until space available
        }
    }

    consumer.join();

    // Verify all items received in FIFO order
    ASSERT_EQ(consumed.size(), kItemCount);
    for (size_t i = 0; i < kItemCount; ++i) {
        EXPECT_EQ(consumed[i], static_cast<int>(i)) << "Mismatch at index " << i;
    }
}

// =============================================================================
// Concurrent: Multiple Producers, Single Consumer
// =============================================================================

TEST_F(MpscRingBufferTest, ConcurrentMultiProducerSingleConsumer) {
    constexpr int kNumProducers = 8;
    constexpr size_t kItemsPerProducer = 10'000;
    constexpr size_t kTotalItems = kNumProducers * kItemsPerProducer;
    MpscRingBuffer<int> rb(4096);

    std::vector<int> consumed;
    consumed.reserve(kTotalItems);

    // Consumer thread
    std::thread consumer([&rb, &consumed]() {
        size_t count = 0;
        while (count < kTotalItems) {
            int val;
            if (rb.try_pop(val)) {
                consumed.push_back(val);
                ++count;
            }
        }
    });

    // Multiple producer threads
    std::latch start_latch(kNumProducers);
    std::vector<std::thread> producers;
    for (int t = 0; t < kNumProducers; ++t) {
        producers.emplace_back([&rb, &start_latch, t]() {
            start_latch.arrive_and_wait();
            for (size_t i = 0; i < kItemsPerProducer; ++i) {
                int v = t * 1000000 + static_cast<int>(i);
                while (!rb.try_push(std::move(v))) {
                    // Spin until space available
                }
            }
        });
    }

    for (auto& p : producers) {
        p.join();
    }
    consumer.join();

    // Verify all items received (order may be interleaved across producers)
    ASSERT_EQ(consumed.size(), kTotalItems);

    // Verify each producer's items are in order relative to each other
    std::vector<size_t> next_expected(kNumProducers, 0);
    for (int val : consumed) {
        int producer_id = val / 1000000;
        size_t item_idx = val % 1000000;
        ASSERT_GE(producer_id, 0);
        ASSERT_LT(producer_id, kNumProducers);
        EXPECT_EQ(item_idx, next_expected[producer_id])
            << "Producer " << producer_id << " items out of order";
        next_expected[producer_id]++;
    }

    // Verify all items from each producer were received
    for (int t = 0; t < kNumProducers; ++t) {
        EXPECT_EQ(next_expected[t], kItemsPerProducer)
            << "Producer " << t << " missing items";
    }
}

TEST_F(MpscRingBufferTest, ConcurrentMultiProducerWithDrain) {
    constexpr int kNumProducers = 4;
    constexpr size_t kItemsPerProducer = 10'000;
    constexpr size_t kTotalItems = kNumProducers * kItemsPerProducer;
    constexpr size_t kBatchSize = 100;
    MpscRingBuffer<int> rb(2048);

    std::vector<int> consumed;
    consumed.reserve(kTotalItems);

    // Consumer thread using drain()
    std::thread consumer([&rb, &consumed]() {
        while (consumed.size() < kTotalItems) {
            std::vector<int> batch;
            rb.drain(batch, kBatchSize);
            for (auto& v : batch) {
                consumed.push_back(v);
            }
        }
    });

    // Multiple producer threads
    std::latch start_latch(kNumProducers);
    std::vector<std::thread> producers;
    for (int t = 0; t < kNumProducers; ++t) {
        producers.emplace_back([&rb, &start_latch, t]() {
            start_latch.arrive_and_wait();
            for (size_t i = 0; i < kItemsPerProducer; ++i) {
                int v = t * 1000000 + static_cast<int>(i);
                while (!rb.try_push(std::move(v))) {
                    // Spin until space available
                }
            }
        });
    }

    for (auto& p : producers) {
        p.join();
    }
    consumer.join();

    ASSERT_EQ(consumed.size(), kTotalItems);
}
