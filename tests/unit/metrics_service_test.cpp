// Ranvier Core - Metrics Service Unit Tests
//
// Tests for histogram bucket logic, bounded container enforcement (Rule #4),
// cache-hit-ratio divide-by-zero protection, counter recording, and prefix
// skip length averaging. Uses stub headers for Seastar (tests/stubs/).

#include "metrics_service.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstdint>

// Stub for the forward-declared function in metrics_service.hpp.
// In production this lives in router_service.cpp.
namespace ranvier {
uint64_t get_backend_load(BackendId) { return 0; }
}

using namespace ranvier;

// =============================================================================
// Histogram Bucket Definition Tests
// =============================================================================

class HistogramBucketsTest : public ::testing::Test {};

TEST_F(HistogramBucketsTest, LatencyBucketsAreSorted) {
    auto buckets = latency_buckets();
    EXPECT_TRUE(std::is_sorted(buckets.begin(), buckets.end()));
}

TEST_F(HistogramBucketsTest, LatencyBucketsAllPositive) {
    for (double b : latency_buckets()) {
        EXPECT_GT(b, 0.0);
    }
}

TEST_F(HistogramBucketsTest, LatencyBucketsCount) {
    EXPECT_EQ(latency_buckets().size(), 16u);
}

TEST_F(HistogramBucketsTest, LatencyBucketsRange) {
    auto buckets = latency_buckets();
    EXPECT_DOUBLE_EQ(buckets.front(), 0.001);   // 1ms
    EXPECT_DOUBLE_EQ(buckets.back(), 300.0);     // 5min
}

TEST_F(HistogramBucketsTest, RoutingLatencyBucketsAreSorted) {
    auto buckets = routing_latency_buckets();
    EXPECT_TRUE(std::is_sorted(buckets.begin(), buckets.end()));
}

TEST_F(HistogramBucketsTest, RoutingLatencyBucketsCount) {
    EXPECT_EQ(routing_latency_buckets().size(), 10u);
}

TEST_F(HistogramBucketsTest, RoutingLatencyBucketsRange) {
    auto buckets = routing_latency_buckets();
    EXPECT_DOUBLE_EQ(buckets.front(), 0.0001);   // 100μs
    EXPECT_DOUBLE_EQ(buckets.back(), 0.1);        // 100ms
}

TEST_F(HistogramBucketsTest, BackendLatencyBucketsAreSorted) {
    auto buckets = backend_latency_buckets();
    EXPECT_TRUE(std::is_sorted(buckets.begin(), buckets.end()));
}

TEST_F(HistogramBucketsTest, BackendLatencyBucketsCount) {
    EXPECT_EQ(backend_latency_buckets().size(), 13u);
}

TEST_F(HistogramBucketsTest, BackendLatencyBucketsRange) {
    auto buckets = backend_latency_buckets();
    EXPECT_DOUBLE_EQ(buckets.front(), 0.05);   // 50ms
    EXPECT_DOUBLE_EQ(buckets.back(), 10.0);    // 10s
}

TEST_F(HistogramBucketsTest, TotalRequestLatencyBucketsAreSorted) {
    auto buckets = total_request_latency_buckets();
    EXPECT_TRUE(std::is_sorted(buckets.begin(), buckets.end()));
}

TEST_F(HistogramBucketsTest, TotalRequestLatencyBucketsCount) {
    EXPECT_EQ(total_request_latency_buckets().size(), 14u);
}

TEST_F(HistogramBucketsTest, TotalRequestLatencyBucketsRange) {
    auto buckets = total_request_latency_buckets();
    EXPECT_DOUBLE_EQ(buckets.front(), 0.01);   // 10ms
    EXPECT_DOUBLE_EQ(buckets.back(), 300.0);   // 5min
}

// =============================================================================
// MetricsService Counter Tests
// =============================================================================

class MetricsServiceCounterTest : public ::testing::Test {
protected:
    MetricsService svc;
};

TEST_F(MetricsServiceCounterTest, CacheCountersStartAtZero) {
    EXPECT_EQ(svc.get_cache_hits(), 0u);
    EXPECT_EQ(svc.get_cache_misses(), 0u);
}

TEST_F(MetricsServiceCounterTest, CacheHitsMissesIncrement) {
    svc.record_cache_hit();
    svc.record_cache_hit();
    svc.record_cache_miss();
    EXPECT_EQ(svc.get_cache_hits(), 2u);
    EXPECT_EQ(svc.get_cache_misses(), 1u);
}

TEST_F(MetricsServiceCounterTest, ActiveRequestsIncrementDecrement) {
    EXPECT_EQ(svc.get_active_requests(), 0u);
    svc.increment_active_requests();
    svc.increment_active_requests();
    EXPECT_EQ(svc.get_active_requests(), 2u);
    svc.decrement_active_requests();
    EXPECT_EQ(svc.get_active_requests(), 1u);
}

TEST_F(MetricsServiceCounterTest, RadixTreeLookupCounters) {
    EXPECT_EQ(svc.get_radix_tree_lookup_hits(), 0u);
    EXPECT_EQ(svc.get_radix_tree_lookup_misses(), 0u);
    svc.record_radix_tree_lookup_hit();
    svc.record_radix_tree_lookup_hit();
    svc.record_radix_tree_lookup_miss();
    EXPECT_EQ(svc.get_radix_tree_lookup_hits(), 2u);
    EXPECT_EQ(svc.get_radix_tree_lookup_misses(), 1u);
}

TEST_F(MetricsServiceCounterTest, TokenizationCacheCounters) {
    svc.record_tokenization_cache_hit();
    svc.record_tokenization_cache_miss();
    svc.record_tokenization_cross_shard();
    EXPECT_EQ(svc.get_tokenization_cache_hits(), 1u);
    EXPECT_EQ(svc.get_tokenization_cache_misses(), 1u);
    EXPECT_EQ(svc.get_tokenization_cross_shard(), 1u);
}

TEST_F(MetricsServiceCounterTest, LoadAwareFallbackIncrementsBothCounters) {
    svc.record_load_aware_fallback();
    EXPECT_EQ(svc.get_load_aware_fallbacks(), 1u);
    EXPECT_EQ(svc.get_cache_miss_due_to_load(), 1u);
}

// =============================================================================
// Cache Hit Ratio Tests (divide-by-zero protection)
// =============================================================================

class CacheHitRatioTest : public ::testing::Test {
protected:
    MetricsService svc;
};

TEST_F(CacheHitRatioTest, ZeroHitsZeroMissesGivesZero) {
    // The ratio lambda: total == 0 → return 0.0
    uint64_t total = svc.get_cache_hits() + svc.get_cache_misses();
    EXPECT_EQ(total, 0u);
    // Verify the logic manually since the lambda is inside the constructor
    double ratio = (total == 0) ? 0.0
        : static_cast<double>(svc.get_cache_hits()) / static_cast<double>(total);
    EXPECT_DOUBLE_EQ(ratio, 0.0);
}

TEST_F(CacheHitRatioTest, AllHitsGivesOne) {
    svc.record_cache_hit();
    svc.record_cache_hit();
    svc.record_cache_hit();
    uint64_t total = svc.get_cache_hits() + svc.get_cache_misses();
    double ratio = static_cast<double>(svc.get_cache_hits()) / static_cast<double>(total);
    EXPECT_DOUBLE_EQ(ratio, 1.0);
}

TEST_F(CacheHitRatioTest, AllMissesGivesZero) {
    svc.record_cache_miss();
    svc.record_cache_miss();
    uint64_t total = svc.get_cache_hits() + svc.get_cache_misses();
    double ratio = static_cast<double>(svc.get_cache_hits()) / static_cast<double>(total);
    EXPECT_DOUBLE_EQ(ratio, 0.0);
}

TEST_F(CacheHitRatioTest, MixedHitsMisses) {
    svc.record_cache_hit();   // 1 hit
    svc.record_cache_miss();  // 1 miss
    svc.record_cache_hit();   // 2 hits
    svc.record_cache_miss();  // 2 misses
    uint64_t total = svc.get_cache_hits() + svc.get_cache_misses();
    double ratio = static_cast<double>(svc.get_cache_hits()) / static_cast<double>(total);
    EXPECT_DOUBLE_EQ(ratio, 0.5);
}

// =============================================================================
// Average Prefix Skip Length Tests
// =============================================================================

class PrefixSkipTest : public ::testing::Test {
protected:
    MetricsService svc;
};

TEST_F(PrefixSkipTest, ZeroCountGivesZeroAverage) {
    EXPECT_DOUBLE_EQ(svc.get_average_prefix_skip_length(), 0.0);
}

TEST_F(PrefixSkipTest, SingleRecordGivesExactValue) {
    svc.record_prefix_skip(10);
    EXPECT_DOUBLE_EQ(svc.get_average_prefix_skip_length(), 10.0);
}

TEST_F(PrefixSkipTest, MultipleRecordsGiveCorrectAverage) {
    svc.record_prefix_skip(4);
    svc.record_prefix_skip(6);
    svc.record_prefix_skip(8);
    // Average = (4 + 6 + 8) / 3 = 6.0
    EXPECT_DOUBLE_EQ(svc.get_average_prefix_skip_length(), 6.0);
}

TEST_F(PrefixSkipTest, ZeroLengthSkipRecorded) {
    svc.record_prefix_skip(0);
    svc.record_prefix_skip(10);
    // Average = (0 + 10) / 2 = 5.0
    EXPECT_DOUBLE_EQ(svc.get_average_prefix_skip_length(), 5.0);
}

// =============================================================================
// Bounded Container Tests (Rule #4: MAX_TRACKED_BACKENDS = 10000)
// =============================================================================

class BoundedBackendMetricsTest : public ::testing::Test {
protected:
    MetricsService svc;
};

TEST_F(BoundedBackendMetricsTest, OverflowCounterStartsAtZero) {
    EXPECT_EQ(svc.get_backend_metrics_overflow(), 0u);
}

TEST_F(BoundedBackendMetricsTest, RecordingNewBackendDoesNotOverflow) {
    svc.record_backend_latency_by_id(1, 0.5);
    EXPECT_EQ(svc.get_backend_metrics_overflow(), 0u);
}

TEST_F(BoundedBackendMetricsTest, SameBackendDoesNotCountMultipleTimes) {
    for (int i = 0; i < 100; ++i) {
        svc.record_backend_latency_by_id(42, 0.1 * i);
    }
    EXPECT_EQ(svc.get_backend_metrics_overflow(), 0u);
}

TEST_F(BoundedBackendMetricsTest, OverflowAfterMaxBackends) {
    // Fill up to MAX_TRACKED_BACKENDS (10000)
    for (BackendId id = 0; id < 10000; ++id) {
        svc.record_backend_latency_by_id(id, 0.1);
    }
    EXPECT_EQ(svc.get_backend_metrics_overflow(), 0u);

    // Next new backend triggers overflow
    svc.record_backend_latency_by_id(10000, 0.1);
    EXPECT_EQ(svc.get_backend_metrics_overflow(), 1u);

    // Additional new backends keep incrementing overflow
    svc.record_backend_latency_by_id(10001, 0.1);
    EXPECT_EQ(svc.get_backend_metrics_overflow(), 2u);
}

TEST_F(BoundedBackendMetricsTest, ExistingBackendStillWorksAfterOverflow) {
    for (BackendId id = 0; id < 10000; ++id) {
        svc.record_backend_latency_by_id(id, 0.1);
    }
    // Overflow one
    svc.record_backend_latency_by_id(99999, 0.1);
    EXPECT_EQ(svc.get_backend_metrics_overflow(), 1u);

    // Existing backend (id=0) still works without overflow
    svc.record_backend_latency_by_id(0, 0.2);
    EXPECT_EQ(svc.get_backend_metrics_overflow(), 1u);
}

// =============================================================================
// to_seconds Static Helper Tests
// =============================================================================

class ToSecondsTest : public ::testing::Test {};

TEST_F(ToSecondsTest, MillisecondsToSeconds) {
    auto dur = std::chrono::milliseconds(1500);
    EXPECT_DOUBLE_EQ(MetricsService::to_seconds(dur), 1.5);
}

TEST_F(ToSecondsTest, MicrosecondsToSeconds) {
    auto dur = std::chrono::microseconds(500);
    EXPECT_DOUBLE_EQ(MetricsService::to_seconds(dur), 0.0005);
}

TEST_F(ToSecondsTest, ZeroDuration) {
    auto dur = std::chrono::milliseconds(0);
    EXPECT_DOUBLE_EQ(MetricsService::to_seconds(dur), 0.0);
}

TEST_F(ToSecondsTest, SecondsToSeconds) {
    auto dur = std::chrono::seconds(42);
    EXPECT_DOUBLE_EQ(MetricsService::to_seconds(dur), 42.0);
}

// =============================================================================
// MetricsService Lifecycle Tests
// =============================================================================

class MetricsLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        stop_metrics();
    }
    void TearDown() override {
        stop_metrics();
    }
};

TEST_F(MetricsLifecycleTest, LazyInitialization) {
    auto& m = metrics();
    EXPECT_EQ(m.get_cache_hits(), 0u);
}

TEST_F(MetricsLifecycleTest, StopAndReinit) {
    auto& m1 = metrics();
    m1.record_cache_hit();
    EXPECT_EQ(m1.get_cache_hits(), 1u);

    stop_metrics();

    // New instance starts fresh
    auto& m2 = metrics();
    EXPECT_EQ(m2.get_cache_hits(), 0u);
}
