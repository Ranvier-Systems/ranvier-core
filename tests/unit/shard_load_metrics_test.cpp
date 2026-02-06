// Ranvier Core - Shard Load Metrics Unit Tests
//
// Tests for per-shard atomic counters, RAII guards, load scoring,
// and snapshot semantics used by the load-aware routing system.
// Uses stub headers for Seastar (tests/stubs/) to keep this a pure-C++ test.

#include "shard_load_metrics.hpp"
#include <gtest/gtest.h>
#include <cstdint>
#include <type_traits>

using namespace ranvier;

// =============================================================================
// ShardLoadMetrics Basic Counter Tests
// =============================================================================

class ShardLoadMetricsTest : public ::testing::Test {
protected:
    ShardLoadMetrics metrics;
};

TEST_F(ShardLoadMetricsTest, InitialCountersAreZero) {
    EXPECT_EQ(metrics.active_requests(), 0u);
    EXPECT_EQ(metrics.queued_requests(), 0u);
    EXPECT_EQ(metrics.total_requests(), 0u);
}

TEST_F(ShardLoadMetricsTest, IncrementDecrementActive) {
    metrics.increment_active();
    EXPECT_EQ(metrics.active_requests(), 1u);
    metrics.increment_active();
    EXPECT_EQ(metrics.active_requests(), 2u);
    metrics.decrement_active();
    EXPECT_EQ(metrics.active_requests(), 1u);
    metrics.decrement_active();
    EXPECT_EQ(metrics.active_requests(), 0u);
}

TEST_F(ShardLoadMetricsTest, IncrementDecrementQueued) {
    metrics.increment_queued();
    EXPECT_EQ(metrics.queued_requests(), 1u);
    metrics.increment_queued();
    EXPECT_EQ(metrics.queued_requests(), 2u);
    metrics.decrement_queued();
    EXPECT_EQ(metrics.queued_requests(), 1u);
    metrics.decrement_queued();
    EXPECT_EQ(metrics.queued_requests(), 0u);
}

TEST_F(ShardLoadMetricsTest, RecordRequestCompleted) {
    metrics.record_request_completed();
    EXPECT_EQ(metrics.total_requests(), 1u);
    metrics.record_request_completed();
    metrics.record_request_completed();
    EXPECT_EQ(metrics.total_requests(), 3u);
}

TEST_F(ShardLoadMetricsTest, CountersAreIndependent) {
    metrics.increment_active();
    metrics.increment_queued();
    metrics.record_request_completed();
    EXPECT_EQ(metrics.active_requests(), 1u);
    EXPECT_EQ(metrics.queued_requests(), 1u);
    EXPECT_EQ(metrics.total_requests(), 1u);
}

TEST_F(ShardLoadMetricsTest, UnderflowWrapsUnsigned) {
    // uint64_t atomic fetch_sub wraps around — no protection
    metrics.decrement_active();
    EXPECT_NE(metrics.active_requests(), 0u);
    // Wrap back to zero
    metrics.increment_active();
    EXPECT_EQ(metrics.active_requests(), 0u);
}

// =============================================================================
// ActiveRequestGuard Tests
// =============================================================================

class ActiveRequestGuardTest : public ::testing::Test {
protected:
    ShardLoadMetrics metrics;
};

TEST_F(ActiveRequestGuardTest, IncrementsOnConstruction) {
    EXPECT_EQ(metrics.active_requests(), 0u);
    {
        ShardLoadMetrics::ActiveRequestGuard guard(metrics);
        EXPECT_EQ(metrics.active_requests(), 1u);
    }
}

TEST_F(ActiveRequestGuardTest, DecrementsOnDestruction) {
    {
        ShardLoadMetrics::ActiveRequestGuard guard(metrics);
        EXPECT_EQ(metrics.active_requests(), 1u);
    }
    EXPECT_EQ(metrics.active_requests(), 0u);
}

TEST_F(ActiveRequestGuardTest, MultipleGuardsStack) {
    {
        ShardLoadMetrics::ActiveRequestGuard g1(metrics);
        EXPECT_EQ(metrics.active_requests(), 1u);
        {
            ShardLoadMetrics::ActiveRequestGuard g2(metrics);
            EXPECT_EQ(metrics.active_requests(), 2u);
        }
        EXPECT_EQ(metrics.active_requests(), 1u);
    }
    EXPECT_EQ(metrics.active_requests(), 0u);
}

TEST_F(ActiveRequestGuardTest, IsNonCopyable) {
    EXPECT_FALSE(std::is_copy_constructible_v<ShardLoadMetrics::ActiveRequestGuard>);
    EXPECT_FALSE(std::is_copy_assignable_v<ShardLoadMetrics::ActiveRequestGuard>);
}

TEST_F(ActiveRequestGuardTest, IsNonMovable) {
    EXPECT_FALSE(std::is_move_constructible_v<ShardLoadMetrics::ActiveRequestGuard>);
    EXPECT_FALSE(std::is_move_assignable_v<ShardLoadMetrics::ActiveRequestGuard>);
}

// =============================================================================
// QueuedRequestGuard Tests
// =============================================================================

class QueuedRequestGuardTest : public ::testing::Test {
protected:
    ShardLoadMetrics metrics;
};

TEST_F(QueuedRequestGuardTest, IncrementsOnConstruction) {
    EXPECT_EQ(metrics.queued_requests(), 0u);
    {
        ShardLoadMetrics::QueuedRequestGuard guard(metrics);
        EXPECT_EQ(metrics.queued_requests(), 1u);
    }
}

TEST_F(QueuedRequestGuardTest, DecrementsOnDestruction) {
    {
        ShardLoadMetrics::QueuedRequestGuard guard(metrics);
        EXPECT_EQ(metrics.queued_requests(), 1u);
    }
    EXPECT_EQ(metrics.queued_requests(), 0u);
}

TEST_F(QueuedRequestGuardTest, MultipleGuardsStack) {
    {
        ShardLoadMetrics::QueuedRequestGuard g1(metrics);
        EXPECT_EQ(metrics.queued_requests(), 1u);
        {
            ShardLoadMetrics::QueuedRequestGuard g2(metrics);
            EXPECT_EQ(metrics.queued_requests(), 2u);
        }
        EXPECT_EQ(metrics.queued_requests(), 1u);
    }
    EXPECT_EQ(metrics.queued_requests(), 0u);
}

TEST_F(QueuedRequestGuardTest, IsNonCopyable) {
    EXPECT_FALSE(std::is_copy_constructible_v<ShardLoadMetrics::QueuedRequestGuard>);
    EXPECT_FALSE(std::is_copy_assignable_v<ShardLoadMetrics::QueuedRequestGuard>);
}

TEST_F(QueuedRequestGuardTest, IsNonMovable) {
    EXPECT_FALSE(std::is_move_constructible_v<ShardLoadMetrics::QueuedRequestGuard>);
    EXPECT_FALSE(std::is_move_assignable_v<ShardLoadMetrics::QueuedRequestGuard>);
}

// =============================================================================
// ShardLoadSnapshot / load_score Tests
// =============================================================================

class ShardLoadSnapshotTest : public ::testing::Test {};

TEST_F(ShardLoadSnapshotTest, LoadScoreFormula) {
    ShardLoadSnapshot snap;
    snap.active_requests = 5;
    snap.queued_requests = 3;
    // score = active * 2.0 + queued = 5 * 2.0 + 3 = 13.0
    EXPECT_DOUBLE_EQ(snap.load_score(), 13.0);
}

TEST_F(ShardLoadSnapshotTest, LoadScoreZeroWhenEmpty) {
    ShardLoadSnapshot snap;
    snap.active_requests = 0;
    snap.queued_requests = 0;
    EXPECT_DOUBLE_EQ(snap.load_score(), 0.0);
}

TEST_F(ShardLoadSnapshotTest, LoadScoreActiveOnlyWeightedDouble) {
    ShardLoadSnapshot snap;
    snap.active_requests = 10;
    snap.queued_requests = 0;
    EXPECT_DOUBLE_EQ(snap.load_score(), 20.0);
}

TEST_F(ShardLoadSnapshotTest, LoadScoreQueuedOnlyWeightedSingle) {
    ShardLoadSnapshot snap;
    snap.active_requests = 0;
    snap.queued_requests = 7;
    EXPECT_DOUBLE_EQ(snap.load_score(), 7.0);
}

TEST_F(ShardLoadSnapshotTest, SnapshotCapturesPointInTime) {
    ShardLoadMetrics metrics;
    metrics.increment_active();
    metrics.increment_active();
    metrics.increment_queued();
    metrics.record_request_completed();

    auto snap = metrics.snapshot();
    EXPECT_EQ(snap.active_requests, 2u);
    EXPECT_EQ(snap.queued_requests, 1u);
    EXPECT_EQ(snap.total_requests, 1u);

    // Mutate after snapshot — snapshot unchanged
    metrics.increment_active();
    EXPECT_EQ(snap.active_requests, 2u);
    EXPECT_EQ(metrics.active_requests(), 3u);
}

TEST_F(ShardLoadSnapshotTest, SnapshotHasTimestamp) {
    ShardLoadMetrics metrics;
    auto before = std::chrono::steady_clock::now();
    auto snap = metrics.snapshot();
    auto after = std::chrono::steady_clock::now();

    EXPECT_GE(snap.timestamp, before);
    EXPECT_LE(snap.timestamp, after);
}

// =============================================================================
// Thread-local Lifecycle Tests
// =============================================================================

class ShardLoadMetricsLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        cleanup_shard_load_metrics();
    }
    void TearDown() override {
        cleanup_shard_load_metrics();
    }
};

TEST_F(ShardLoadMetricsLifecycleTest, NotInitializedByDefault) {
    EXPECT_FALSE(shard_load_metrics_initialized());
}

TEST_F(ShardLoadMetricsLifecycleTest, InitCreatesInstance) {
    init_shard_load_metrics();
    EXPECT_TRUE(shard_load_metrics_initialized());
}

TEST_F(ShardLoadMetricsLifecycleTest, AccessorLazyInitializes) {
    EXPECT_FALSE(shard_load_metrics_initialized());
    auto& m = shard_load_metrics();
    EXPECT_TRUE(shard_load_metrics_initialized());
    EXPECT_EQ(m.active_requests(), 0u);
}

TEST_F(ShardLoadMetricsLifecycleTest, CleanupDestroysInstance) {
    init_shard_load_metrics();
    EXPECT_TRUE(shard_load_metrics_initialized());
    cleanup_shard_load_metrics();
    EXPECT_FALSE(shard_load_metrics_initialized());
}
