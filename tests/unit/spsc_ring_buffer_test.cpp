// Ranvier Core - SPSC Ring Buffer Unit Tests
//
// Tests the lock-free single-producer single-consumer ring buffer used by
// AsyncPersistenceManager to decouple the reactor thread (producer) from
// the persistence worker thread (consumer) without any mutex.
//
// Pure C++20 — no Seastar dependency.

#include "spsc_ring_buffer.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace ranvier;

// =============================================================================
// Test Fixture
// =============================================================================

class SpscRingBufferTest : public ::testing::Test {};

// =============================================================================
// Construction & Capacity
// =============================================================================

TEST_F(SpscRingBufferTest, PowerOfTwoCapacityUnchanged) {
    SpscRingBuffer<int> rb(16);
    // Usable capacity is power_of_two - 1 (sentinel slot)
    EXPECT_EQ(rb.capacity(), 15u);
}

TEST_F(SpscRingBufferTest, NonPowerOfTwoRoundsUp) {
    SpscRingBuffer<int> rb(10);
    // 10 rounds up to 16, usable = 15
    EXPECT_EQ(rb.capacity(), 15u);
}

TEST_F(SpscRingBufferTest, CapacityOfOne) {
    SpscRingBuffer<int> rb(1);
    // 1 is power of two, usable = 0 — degenerate but safe
    EXPECT_EQ(rb.capacity(), 0u);
}

TEST_F(SpscRingBufferTest, CapacityOfTwo) {
    SpscRingBuffer<int> rb(2);
    EXPECT_EQ(rb.capacity(), 1u);
}

TEST_F(SpscRingBufferTest, LargeCapacity) {
    SpscRingBuffer<int> rb(100000);
    // 100000 rounds up to 131072, usable = 131071
    EXPECT_EQ(rb.capacity(), 131071u);
}

// =============================================================================
// Empty Buffer
// =============================================================================

TEST_F(SpscRingBufferTest, EmptyBufferSizeIsZero) {
    SpscRingBuffer<int> rb(16);
    EXPECT_EQ(rb.size_approx(), 0u);
}

TEST_F(SpscRingBufferTest, TryPopOnEmptyReturnsFalse) {
    SpscRingBuffer<int> rb(16);
    int val = -1;
    EXPECT_FALSE(rb.try_pop(val));
    EXPECT_EQ(val, -1);  // Unchanged
}

TEST_F(SpscRingBufferTest, DrainOnEmptyReturnsZero) {
    SpscRingBuffer<int> rb(16);
    std::vector<int> out;
    EXPECT_EQ(rb.drain(out, 100), 0u);
    EXPECT_TRUE(out.empty());
}

// =============================================================================
// Push / Pop Basics
// =============================================================================

TEST_F(SpscRingBufferTest, PushPopSingleElement) {
    SpscRingBuffer<int> rb(4);
    EXPECT_TRUE(rb.try_push(42));
    EXPECT_EQ(rb.size_approx(), 1u);

    int val = 0;
    EXPECT_TRUE(rb.try_pop(val));
    EXPECT_EQ(val, 42);
    EXPECT_EQ(rb.size_approx(), 0u);
}

TEST_F(SpscRingBufferTest, PushPopMultipleElements) {
    SpscRingBuffer<int> rb(8);
    for (int i = 0; i < 7; ++i) {  // capacity = 7
        EXPECT_TRUE(rb.try_push(std::move(i)));
    }
    EXPECT_EQ(rb.size_approx(), 7u);

    for (int i = 0; i < 7; ++i) {
        int val = -1;
        EXPECT_TRUE(rb.try_pop(val));
        EXPECT_EQ(val, i);
    }
    EXPECT_EQ(rb.size_approx(), 0u);
}

TEST_F(SpscRingBufferTest, FIFOOrdering) {
    SpscRingBuffer<int> rb(16);
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

TEST_F(SpscRingBufferTest, PushToFullReturnsFalse) {
    SpscRingBuffer<int> rb(4);  // usable capacity = 3
    EXPECT_TRUE(rb.try_push(1));
    EXPECT_TRUE(rb.try_push(2));
    EXPECT_TRUE(rb.try_push(3));
    EXPECT_FALSE(rb.try_push(4));  // Full
    EXPECT_EQ(rb.size_approx(), 3u);
}

TEST_F(SpscRingBufferTest, PushAfterPopFromFull) {
    SpscRingBuffer<int> rb(4);  // usable capacity = 3
    EXPECT_TRUE(rb.try_push(1));
    EXPECT_TRUE(rb.try_push(2));
    EXPECT_TRUE(rb.try_push(3));
    EXPECT_FALSE(rb.try_push(4));  // Full

    int val = 0;
    EXPECT_TRUE(rb.try_pop(val));
    EXPECT_EQ(val, 1);

    EXPECT_TRUE(rb.try_push(4));  // Now room for one
    EXPECT_EQ(rb.size_approx(), 3u);
}

// =============================================================================
// Wraparound
// =============================================================================

TEST_F(SpscRingBufferTest, IndicesWrapAroundCorrectly) {
    SpscRingBuffer<int> rb(4);  // usable capacity = 3
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

TEST_F(SpscRingBufferTest, FillDrainMultipleRounds) {
    SpscRingBuffer<int> rb(8);  // usable capacity = 7
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 7; ++i) {
            int v = round * 100 + i;
            EXPECT_TRUE(rb.try_push(std::move(v)));
        }
        EXPECT_FALSE(rb.try_push(999));  // Full

        for (int i = 0; i < 7; ++i) {
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

TEST_F(SpscRingBufferTest, DrainPartialBatch) {
    SpscRingBuffer<int> rb(16);
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

TEST_F(SpscRingBufferTest, DrainMoreThanAvailable) {
    SpscRingBuffer<int> rb(16);
    for (int i = 0; i < 3; ++i) {
        int v = i;
        EXPECT_TRUE(rb.try_push(std::move(v)));
    }

    std::vector<int> out;
    EXPECT_EQ(rb.drain(out, 100), 3u);
    EXPECT_EQ(out.size(), 3u);
    EXPECT_EQ(rb.size_approx(), 0u);
}

TEST_F(SpscRingBufferTest, DrainAllElements) {
    SpscRingBuffer<int> rb(16);
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

TEST_F(SpscRingBufferTest, DrainAppendsToExistingVector) {
    SpscRingBuffer<int> rb(16);
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

TEST_F(SpscRingBufferTest, DrainWithWraparound) {
    SpscRingBuffer<int> rb(4);  // usable capacity = 3
    // Advance head past 0 to force wraparound during drain
    int v1 = 100, v2 = 200;
    EXPECT_TRUE(rb.try_push(std::move(v1)));
    EXPECT_TRUE(rb.try_push(std::move(v2)));
    int discard = 0;
    EXPECT_TRUE(rb.try_pop(discard));
    EXPECT_TRUE(rb.try_pop(discard));

    // Now head=2, tail=2. Push 3 items to wrap tail around.
    int v3 = 10, v4 = 20, v5 = 30;
    EXPECT_TRUE(rb.try_push(std::move(v3)));
    EXPECT_TRUE(rb.try_push(std::move(v4)));
    EXPECT_TRUE(rb.try_push(std::move(v5)));

    std::vector<int> out;
    EXPECT_EQ(rb.drain_all(out), 3u);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0], 10);
    EXPECT_EQ(out[1], 20);
    EXPECT_EQ(out[2], 30);
}

// =============================================================================
// Move-Only Types
// =============================================================================

TEST_F(SpscRingBufferTest, MoveOnlyType) {
    SpscRingBuffer<std::unique_ptr<int>> rb(8);
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

TEST_F(SpscRingBufferTest, DrainMoveOnlyType) {
    SpscRingBuffer<std::unique_ptr<int>> rb(8);
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

TEST_F(SpscRingBufferTest, StringMoveSemantics) {
    SpscRingBuffer<std::string> rb(8);
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

TEST_F(SpscRingBufferTest, VariantType) {
    using Op = std::variant<int, std::string, std::vector<int>>;
    SpscRingBuffer<Op> rb(8);

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

TEST_F(SpscRingBufferTest, SizeApproxTracksCorrectly) {
    SpscRingBuffer<int> rb(8);
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
// Concurrent SPSC (single producer thread, single consumer thread)
// =============================================================================

TEST_F(SpscRingBufferTest, ConcurrentProducerConsumer) {
    constexpr size_t kItemCount = 100'000;
    SpscRingBuffer<int> rb(1024);

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

TEST_F(SpscRingBufferTest, ConcurrentProducerConsumerWithDrain) {
    constexpr size_t kItemCount = 50'000;
    constexpr size_t kBatchSize = 100;
    SpscRingBuffer<int> rb(1024);

    std::vector<int> consumed;
    consumed.reserve(kItemCount);

    // Consumer thread using drain()
    std::thread consumer([&rb, &consumed]() {
        while (consumed.size() < kItemCount) {
            std::vector<int> batch;
            rb.drain(batch, kBatchSize);
            for (auto& v : batch) {
                consumed.push_back(v);
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

    ASSERT_EQ(consumed.size(), kItemCount);
    for (size_t i = 0; i < kItemCount; ++i) {
        EXPECT_EQ(consumed[i], static_cast<int>(i)) << "Mismatch at index " << i;
    }
}
