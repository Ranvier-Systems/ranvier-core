/**
 * Unit tests for Route Batching
 *
 * Tests the route batching mechanism used to prevent "SMP storms" when
 * receiving high volumes of remote route announcements via gossip.
 *
 * These tests verify:
 * - PendingRemoteRoute struct correctly stores route data
 * - RouteBatchConfig has appropriate default values
 * - Batching logic correctly triggers flushes at threshold
 * - Buffer behavior under various scenarios
 */

#include <gtest/gtest.h>
#include <vector>
#include <chrono>

// Include the types we need
#include "types.hpp"

using namespace ranvier;

// =============================================================================
// Replicated Types (to avoid Seastar dependencies in unit tests)
// =============================================================================

// Mirror of PendingRemoteRoute from router_service.hpp
struct PendingRemoteRoute {
    std::vector<int32_t> tokens;
    BackendId backend;
};

// Mirror of RouteBatchConfig from router_service.hpp
struct RouteBatchConfig {
    static constexpr size_t MAX_BATCH_SIZE = 100;
    static constexpr size_t MAX_BUFFER_SIZE = 10000;
    static constexpr size_t OVERFLOW_DROP_COUNT = 1000;
    static constexpr std::chrono::milliseconds DEFAULT_FLUSH_INTERVAL{10};
};

// =============================================================================
// PendingRemoteRoute Tests
// =============================================================================

class PendingRemoteRouteTest : public ::testing::Test {
protected:
    std::vector<int32_t> make_tokens(size_t count) {
        std::vector<int32_t> tokens;
        tokens.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            tokens.push_back(static_cast<int32_t>(i + 1));
        }
        return tokens;
    }
};

TEST_F(PendingRemoteRouteTest, CanStoreEmptyTokens) {
    PendingRemoteRoute route{{}, 42};

    EXPECT_TRUE(route.tokens.empty());
    EXPECT_EQ(route.backend, 42);
}

TEST_F(PendingRemoteRouteTest, CanStoreSingleToken) {
    PendingRemoteRoute route{{123}, 1};

    ASSERT_EQ(route.tokens.size(), 1);
    EXPECT_EQ(route.tokens[0], 123);
    EXPECT_EQ(route.backend, 1);
}

TEST_F(PendingRemoteRouteTest, CanStoreTypicalPrefixLength) {
    // Typical prefix length is 128 tokens
    auto tokens = make_tokens(128);
    PendingRemoteRoute route{tokens, 99};

    EXPECT_EQ(route.tokens.size(), 128);
    EXPECT_EQ(route.backend, 99);
    EXPECT_EQ(route.tokens[0], 1);
    EXPECT_EQ(route.tokens[127], 128);
}

TEST_F(PendingRemoteRouteTest, CanStoreLargePrefixLength) {
    // Maximum expected prefix length is 256 tokens
    auto tokens = make_tokens(256);
    PendingRemoteRoute route{tokens, 0};

    EXPECT_EQ(route.tokens.size(), 256);
    EXPECT_EQ(route.backend, 0);
}

TEST_F(PendingRemoteRouteTest, MoveConstructorWorks) {
    auto tokens = make_tokens(10);
    PendingRemoteRoute original{tokens, 5};

    PendingRemoteRoute moved{std::move(original)};

    EXPECT_EQ(moved.tokens.size(), 10);
    EXPECT_EQ(moved.backend, 5);
    // Original should be empty after move
    EXPECT_TRUE(original.tokens.empty());
}

// =============================================================================
// RouteBatchConfig Tests
// =============================================================================

TEST(RouteBatchConfigTest, MaxBatchSizeIsReasonable) {
    // MAX_BATCH_SIZE should be between 10 and 1000 for practical use
    EXPECT_GE(RouteBatchConfig::MAX_BATCH_SIZE, 10);
    EXPECT_LE(RouteBatchConfig::MAX_BATCH_SIZE, 1000);

    // Current value should be 100
    EXPECT_EQ(RouteBatchConfig::MAX_BATCH_SIZE, 100);
}

TEST(RouteBatchConfigTest, FlushIntervalIsReasonable) {
    // FLUSH_INTERVAL should be between 1ms and 1000ms
    EXPECT_GE(RouteBatchConfig::DEFAULT_FLUSH_INTERVAL.count(), 1);
    EXPECT_LE(RouteBatchConfig::DEFAULT_FLUSH_INTERVAL.count(), 1000);

    // Current value should be 10ms
    EXPECT_EQ(RouteBatchConfig::DEFAULT_FLUSH_INTERVAL.count(), 10);
}

TEST(RouteBatchConfigTest, FlushIntervalEnsuresBoundedLatency) {
    // At 10ms flush interval with 100 route batch size:
    // - Worst case latency for a single route is 10ms (timer fires)
    // - This is acceptable for cluster route sync
    EXPECT_LE(RouteBatchConfig::DEFAULT_FLUSH_INTERVAL.count(), 100);
}

// =============================================================================
// Batching Behavior Simulation Tests
// =============================================================================

class BatchingBehaviorTest : public ::testing::Test {
protected:
    std::vector<PendingRemoteRoute> buffer;

    void SetUp() override {
        buffer.clear();
        buffer.reserve(RouteBatchConfig::MAX_BATCH_SIZE);
    }

    // Simulates learn_route_remote behavior
    bool add_route(std::vector<int32_t> tokens, BackendId backend) {
        buffer.push_back(PendingRemoteRoute{std::move(tokens), backend});
        return buffer.size() >= RouteBatchConfig::MAX_BATCH_SIZE;
    }

    // Simulates flush_route_batch behavior
    std::vector<PendingRemoteRoute> flush() {
        auto batch = std::move(buffer);
        buffer.clear();
        return batch;
    }

    std::vector<int32_t> make_tokens(size_t count) {
        std::vector<int32_t> tokens;
        for (size_t i = 0; i < count; ++i) {
            tokens.push_back(static_cast<int32_t>(i));
        }
        return tokens;
    }
};

TEST_F(BatchingBehaviorTest, BufferStartsEmpty) {
    EXPECT_TRUE(buffer.empty());
}

TEST_F(BatchingBehaviorTest, SingleRouteDoesNotTriggerFlush) {
    bool should_flush = add_route({1, 2, 3}, 1);

    EXPECT_FALSE(should_flush);
    EXPECT_EQ(buffer.size(), 1);
}

TEST_F(BatchingBehaviorTest, RoutesAccumulateInBuffer) {
    for (int i = 0; i < 50; ++i) {
        add_route(make_tokens(10), i);
    }

    EXPECT_EQ(buffer.size(), 50);
}

TEST_F(BatchingBehaviorTest, FlushTriggersAtMaxBatchSize) {
    // Add routes up to MAX_BATCH_SIZE - 1
    for (size_t i = 0; i < RouteBatchConfig::MAX_BATCH_SIZE - 1; ++i) {
        bool should_flush = add_route(make_tokens(5), static_cast<BackendId>(i));
        EXPECT_FALSE(should_flush) << "Unexpected flush at " << i;
    }

    // The MAX_BATCH_SIZE-th route should trigger flush
    bool should_flush = add_route(make_tokens(5), 999);
    EXPECT_TRUE(should_flush);
    EXPECT_EQ(buffer.size(), RouteBatchConfig::MAX_BATCH_SIZE);
}

TEST_F(BatchingBehaviorTest, FlushReturnsAllBufferedRoutes) {
    for (int i = 0; i < 25; ++i) {
        add_route({i}, i);
    }

    auto batch = flush();

    EXPECT_EQ(batch.size(), 25);
    EXPECT_TRUE(buffer.empty());

    // Verify batch contents
    for (int i = 0; i < 25; ++i) {
        EXPECT_EQ(batch[i].tokens.size(), 1);
        EXPECT_EQ(batch[i].tokens[0], i);
        EXPECT_EQ(batch[i].backend, i);
    }
}

TEST_F(BatchingBehaviorTest, FlushClearsBuffer) {
    add_route({1, 2, 3}, 1);
    add_route({4, 5, 6}, 2);

    flush();

    EXPECT_TRUE(buffer.empty());
}

TEST_F(BatchingBehaviorTest, BufferCanBeReusedAfterFlush) {
    // First batch
    add_route({1}, 1);
    add_route({2}, 2);
    auto batch1 = flush();
    EXPECT_EQ(batch1.size(), 2);

    // Second batch
    add_route({3}, 3);
    auto batch2 = flush();
    EXPECT_EQ(batch2.size(), 1);
    EXPECT_EQ(batch2[0].tokens[0], 3);
}

TEST_F(BatchingBehaviorTest, EmptyFlushReturnsEmptyBatch) {
    auto batch = flush();

    EXPECT_TRUE(batch.empty());
}

TEST_F(BatchingBehaviorTest, PreAllocationPreventsReallocations) {
    // buffer.reserve() was called in SetUp
    void* initial_data = buffer.data();

    // Add up to reserved capacity
    for (size_t i = 0; i < RouteBatchConfig::MAX_BATCH_SIZE; ++i) {
        add_route({static_cast<int32_t>(i)}, static_cast<BackendId>(i));
    }

    // Data pointer should not have changed (no reallocation)
    // Note: This test may be flaky if reserve doesn't allocate immediately
    // but it documents the expected behavior
    EXPECT_EQ(buffer.data(), initial_data);
}

// =============================================================================
// Performance Characteristics Tests
// =============================================================================

TEST(PerformanceCharacteristicsTest, BatchReducesSmpMessages) {
    // Document the SMP message reduction
    constexpr size_t routes_per_second = 1000;
    constexpr size_t shards = 64;
    constexpr size_t batch_size = RouteBatchConfig::MAX_BATCH_SIZE;

    // Without batching: routes × shards messages
    size_t messages_without_batching = routes_per_second * shards;

    // With batching: (routes / batch_size) × shards messages
    size_t batches_per_second = (routes_per_second + batch_size - 1) / batch_size;
    size_t messages_with_batching = batches_per_second * shards;

    // Verify significant reduction
    EXPECT_LT(messages_with_batching, messages_without_batching / 10);

    // Document actual values
    EXPECT_EQ(messages_without_batching, 64000);
    EXPECT_EQ(messages_with_batching, 640);  // 10 batches × 64 shards
}

TEST(PerformanceCharacteristicsTest, WorstCaseLatencyIsBounded) {
    // Worst case: a single route arrives just after a flush
    // It must wait for the next timer tick
    auto worst_case_latency = RouteBatchConfig::DEFAULT_FLUSH_INTERVAL;

    // Should be at most 100ms for acceptable UX
    EXPECT_LE(worst_case_latency.count(), 100);
}
