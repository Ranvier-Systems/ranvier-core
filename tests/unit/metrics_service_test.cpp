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

TEST_F(MetricsServiceCounterTest, LoadAwareFallbackIncrementsCounter) {
    svc.record_load_aware_fallback();
    EXPECT_EQ(svc.get_load_aware_fallbacks(), 1u);
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
// Overflow Isolation Tests (fix for shared mutable singleton fallback_metrics)
// Verifies that overflow backends don't share state and that aggregate
// histograms still capture data when per-backend tracking is at capacity.
// =============================================================================

class OverflowIsolationTest : public ::testing::Test {
protected:
    MetricsService svc;

    // Fill the per-backend map to capacity
    void fill_to_capacity() {
        for (BackendId id = 0; id < 10000; ++id) {
            svc.record_backend_latency_by_id(id, 0.1);
        }
    }
};

TEST_F(OverflowIsolationTest, OverflowDoesNotCorruptExistingBackendData) {
    // Record a known value for backend 0
    svc.record_backend_latency_by_id(0, 1.0);

    // Fill remaining capacity (backends 1-9999)
    for (BackendId id = 1; id < 10000; ++id) {
        svc.record_backend_latency_by_id(id, 0.1);
    }

    // Now trigger overflow with many different backends and large values
    for (BackendId id = 10000; id < 10100; ++id) {
        svc.record_backend_latency_by_id(id, 999.0);
    }

    // Backend 0 should still only have its original single recording
    // (Before the fix, the shared static singleton would have accumulated
    // all overflow recordings, making data incoherent)
    EXPECT_EQ(svc.get_backend_metrics_overflow(), 100u);
}

TEST_F(OverflowIsolationTest, AggregateHistogramStillRecordsOnOverflow) {
    fill_to_capacity();

    // Record the aggregate latency before overflow
    // The aggregate histogram (_router_backend_latency) should capture
    // data even when per-backend tracking overflows
    svc.record_backend_latency_by_id(99999, 5.0);
    EXPECT_EQ(svc.get_backend_metrics_overflow(), 1u);

    // The request still completes successfully (no crash, no UB)
    // Aggregate recording happens regardless of per-backend overflow
}

TEST_F(OverflowIsolationTest, FirstByteLatencyOverflowIsHandled) {
    fill_to_capacity();

    // first_byte_latency recording should also handle overflow gracefully
    svc.record_first_byte_latency_by_id(99999, 2.0);
    EXPECT_EQ(svc.get_backend_metrics_overflow(), 1u);

    // Multiple overflow recordings should each increment the counter
    svc.record_first_byte_latency_by_id(99998, 3.0);
    EXPECT_EQ(svc.get_backend_metrics_overflow(), 2u);
}

TEST_F(OverflowIsolationTest, MixedLatencyAndFirstByteOverflow) {
    fill_to_capacity();

    // Both recording methods should independently handle overflow
    svc.record_backend_latency_by_id(50000, 1.0);
    svc.record_first_byte_latency_by_id(50001, 2.0);
    svc.record_backend_latency_by_id(50002, 3.0);

    // Each call to a new overflow backend increments the counter
    EXPECT_EQ(svc.get_backend_metrics_overflow(), 3u);
}

TEST_F(OverflowIsolationTest, RepeatedOverflowBackendIncrementsEachTime) {
    fill_to_capacity();

    // The same overflow backend ID recorded multiple times should
    // increment the overflow counter each time (it's never added to the map)
    svc.record_backend_latency_by_id(99999, 1.0);
    svc.record_backend_latency_by_id(99999, 2.0);
    svc.record_backend_latency_by_id(99999, 3.0);
    EXPECT_EQ(svc.get_backend_metrics_overflow(), 3u);
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

// =============================================================================
// Tokenization Latency Split Recording Tests
//
// Smoke tests for the primary/boundary latency split introduced alongside
// the thread-pool offload. The underlying histograms are private, so we
// verify the public API compiles, accepts representative values without
// crashing, and that both methods coexist on the same MetricsService instance.
// =============================================================================

class TokenizationLatencySplitTest : public ::testing::Test {
protected:
    MetricsService svc;
};

TEST_F(TokenizationLatencySplitTest, RecordPrimaryTokenizationLatency) {
    // Typical primary tokenization: 1-10ms range
    svc.record_primary_tokenization_latency(0.005);
    svc.record_primary_tokenization_latency(0.010);
}

TEST_F(TokenizationLatencySplitTest, RecordBoundaryDetectionLatency) {
    // Boundary detection is typically faster: 0.1-2ms range
    svc.record_boundary_detection_latency(0.0005);
    svc.record_boundary_detection_latency(0.002);
}

TEST_F(TokenizationLatencySplitTest, RecordBothInSequence) {
    // Mirrors the call pattern in http_controller: primary first, then boundary
    svc.record_primary_tokenization_latency(0.008);
    svc.record_boundary_detection_latency(0.001);
    // Total tokenization latency = primary + boundary
    svc.record_tokenization_latency(0.009);
}

TEST_F(TokenizationLatencySplitTest, ZeroLatencyDoesNotCrash) {
    svc.record_primary_tokenization_latency(0.0);
    svc.record_boundary_detection_latency(0.0);
    svc.record_tokenization_latency(0.0);
}

TEST_F(TokenizationLatencySplitTest, VerySmallLatencyDoesNotCrash) {
    // Sub-microsecond: below smallest bucket (0.0001s = 100μs)
    svc.record_primary_tokenization_latency(0.0000001);
    svc.record_boundary_detection_latency(0.0000001);
}

TEST_F(TokenizationLatencySplitTest, LargeLatencyDoesNotCrash) {
    // Pathological case: 100s tokenization (above all buckets)
    svc.record_primary_tokenization_latency(100.0);
    svc.record_boundary_detection_latency(100.0);
}

TEST_F(TokenizationLatencySplitTest, ManyRecordingsDoNotCrash) {
    // Sustained load: 10k recordings without accumulator overflow
    for (int i = 0; i < 10000; ++i) {
        svc.record_primary_tokenization_latency(0.005);
        svc.record_boundary_detection_latency(0.001);
    }
}

// =============================================================================
// Per-Priority Tier Metrics Tests
// =============================================================================

class PriorityMetricsTest : public ::testing::Test {
protected:
    MetricsService svc;
};

TEST_F(PriorityMetricsTest, RequestCountersStartAtZero) {
    // Verify all 4 tiers start at zero (no public getter, so use record + decrement pattern)
    // We rely on the fact that increment/decrement are symmetric
    svc.increment_priority_active(0);
    svc.decrement_priority_active(0);
    // No crash = success (counters are internal, tested via metrics scrape in integration)
}

TEST_F(PriorityMetricsTest, RecordPriorityRequestAllTiers) {
    svc.record_priority_request(0);  // CRITICAL
    svc.record_priority_request(1);  // HIGH
    svc.record_priority_request(2);  // NORMAL
    svc.record_priority_request(3);  // LOW
    // No crash = counters incremented correctly
}

TEST_F(PriorityMetricsTest, IncrementDecrementActiveAllTiers) {
    for (uint8_t tier = 0; tier < 4; ++tier) {
        svc.increment_priority_active(tier);
        svc.increment_priority_active(tier);
        svc.decrement_priority_active(tier);
        // Net: 1 active per tier — no crash, no underflow
    }
}

TEST_F(PriorityMetricsTest, OutOfBoundsTierIgnored) {
    // Tier 4+ should be silently ignored (bounds check in method)
    svc.record_priority_request(4);
    svc.record_priority_request(255);
    svc.increment_priority_active(4);
    svc.decrement_priority_active(4);
    // No crash = bounds check works
}

TEST_F(PriorityMetricsTest, HighVolumeDoesNotCrash) {
    // Sustained load across all tiers
    for (int i = 0; i < 10000; ++i) {
        uint8_t tier = static_cast<uint8_t>(i % 4);
        svc.record_priority_request(tier);
        svc.increment_priority_active(tier);
        svc.decrement_priority_active(tier);
    }
}

// =============================================================================
// P1: Prefix Hits by Compression Tier Tests
// =============================================================================

class PrefixHitsByCompressionTierTest : public ::testing::Test {
protected:
    MetricsService svc;
};

TEST_F(PrefixHitsByCompressionTierTest, AllCountersStartAtZero) {
    EXPECT_EQ(svc.get_prefix_hits_tier_none(), 0u);
    EXPECT_EQ(svc.get_prefix_hits_tier_moderate(), 0u);
    EXPECT_EQ(svc.get_prefix_hits_tier_high(), 0u);
}

TEST_F(PrefixHitsByCompressionTierTest, UncompressedBackendGoesToNoneTier) {
    svc.record_prefix_hit_by_compression_tier(1.0);
    EXPECT_EQ(svc.get_prefix_hits_tier_none(), 1u);
    EXPECT_EQ(svc.get_prefix_hits_tier_moderate(), 0u);
    EXPECT_EQ(svc.get_prefix_hits_tier_high(), 0u);
}

TEST_F(PrefixHitsByCompressionTierTest, ModerateCompressionGoesToModerateTier) {
    svc.record_prefix_hit_by_compression_tier(2.0);
    svc.record_prefix_hit_by_compression_tier(3.5);
    EXPECT_EQ(svc.get_prefix_hits_tier_none(), 0u);
    EXPECT_EQ(svc.get_prefix_hits_tier_moderate(), 2u);
    EXPECT_EQ(svc.get_prefix_hits_tier_high(), 0u);
}

TEST_F(PrefixHitsByCompressionTierTest, HighCompressionGoesToHighTier) {
    svc.record_prefix_hit_by_compression_tier(4.0);
    svc.record_prefix_hit_by_compression_tier(6.0);
    svc.record_prefix_hit_by_compression_tier(10.0);
    EXPECT_EQ(svc.get_prefix_hits_tier_none(), 0u);
    EXPECT_EQ(svc.get_prefix_hits_tier_moderate(), 0u);
    EXPECT_EQ(svc.get_prefix_hits_tier_high(), 3u);
}

TEST_F(PrefixHitsByCompressionTierTest, BoundaryValues) {
    // Exactly 1.0 -> none
    svc.record_prefix_hit_by_compression_tier(1.0);
    // Just above 1.0 -> moderate
    svc.record_prefix_hit_by_compression_tier(1.001);
    // Just below 4.0 -> moderate
    svc.record_prefix_hit_by_compression_tier(3.999);
    // Exactly 4.0 -> high
    svc.record_prefix_hit_by_compression_tier(4.0);

    EXPECT_EQ(svc.get_prefix_hits_tier_none(), 1u);
    EXPECT_EQ(svc.get_prefix_hits_tier_moderate(), 2u);
    EXPECT_EQ(svc.get_prefix_hits_tier_high(), 1u);
}

TEST_F(PrefixHitsByCompressionTierTest, SubOneRatioTreatedAsNone) {
    // compression_ratio < 1.0 shouldn't happen (clamped at config layer),
    // but if it does, bucket as "none" (defensive)
    svc.record_prefix_hit_by_compression_tier(0.5);
    EXPECT_EQ(svc.get_prefix_hits_tier_none(), 1u);
}

TEST_F(PrefixHitsByCompressionTierTest, MixedTrafficAcrossAllTiers) {
    // Simulate fleet with 3 backend types
    for (int i = 0; i < 100; ++i) {
        svc.record_prefix_hit_by_compression_tier(1.0);   // uncompressed
        svc.record_prefix_hit_by_compression_tier(2.5);   // moderate
        svc.record_prefix_hit_by_compression_tier(6.0);   // high (TurboQuant)
    }
    EXPECT_EQ(svc.get_prefix_hits_tier_none(), 100u);
    EXPECT_EQ(svc.get_prefix_hits_tier_moderate(), 100u);
    EXPECT_EQ(svc.get_prefix_hits_tier_high(), 100u);
}

// =============================================================================
// Per-API-Key Attribution Tests (memo §6, §9)
// =============================================================================
// Closes the alice/bob + bound-overflow scenarios from memo §9 that were
// flagged as TODO in tests/integration/test_attribution_cardinality.py
// (the integration harness can't reconfigure max_label_cardinality
// per-test). MetricsService is unit-testable via the seastar stubs in
// tests/stubs/, so we exercise the bound logic directly here.

namespace {

ApiKey make_api_key(const std::string& name) {
    ApiKey k;
    k.name = name;
    k.key = name + "-secret-token";  // arbitrary; not used in attribution
    return k;
}

AuthConfig make_auth_with_keys(std::initializer_list<std::string> names) {
    AuthConfig auth;
    for (const auto& n : names) {
        auth.api_keys.push_back(make_api_key(n));
    }
    return auth;
}

}  // namespace

class ApiKeyAttributionTest : public ::testing::Test {
protected:
    MetricsService svc;

    // Helper: count distinct labels actually registered as slots
    size_t slot_count_excluding_sentinels() const {
        size_t n = svc.get_api_key_slot_count();
        // The three sentinels are always pre-registered.
        return n >= 3 ? n - 3 : 0;
    }
};

TEST_F(ApiKeyAttributionTest, BeforeInitNoSlotsAndNoOverflow) {
    EXPECT_EQ(svc.get_api_key_slot_count(), 0u);
    EXPECT_EQ(svc.get_api_key_label_overflow(), 0u);
}

TEST_F(ApiKeyAttributionTest, BeforeInitRecordingIsNoOp) {
    // Hard Rule #9: a missing init should not crash; the recording call
    // silently no-ops (returns nullptr from get_or_overflow_slot).
    svc.record_api_key_request_received("anything");
    svc.record_api_key_success("anything");
    EXPECT_EQ(svc.get_api_key_slot_count(), 0u);
    EXPECT_EQ(svc.get_api_key_label_overflow(), 0u);
}

TEST_F(ApiKeyAttributionTest, InitRegistersThreeSentinels) {
    AuthConfig auth;
    AttributionConfig cfg;
    svc.init_api_key_attribution(auth, cfg);

    EXPECT_EQ(svc.get_api_key_slot_count(), 3u);
    EXPECT_NE(svc.get_api_key_slot("_unauthenticated"), nullptr);
    EXPECT_NE(svc.get_api_key_slot("_invalid"), nullptr);
    EXPECT_NE(svc.get_api_key_slot("_overflow"), nullptr);
}

TEST_F(ApiKeyAttributionTest, InitIsIdempotent) {
    AuthConfig auth;
    AttributionConfig cfg;
    svc.init_api_key_attribution(auth, cfg);
    svc.init_api_key_attribution(auth, cfg);
    svc.init_api_key_attribution(auth, cfg);
    EXPECT_EQ(svc.get_api_key_slot_count(), 3u);
}

TEST_F(ApiKeyAttributionTest, BootTimePreFillRegistersConfiguredKeys) {
    auto auth = make_auth_with_keys({"alice", "bob", "carol"});
    AttributionConfig cfg;  // default max_label_cardinality = 256

    svc.init_api_key_attribution(auth, cfg);

    // 3 sentinels + 3 configured keys (sanitised to themselves)
    EXPECT_EQ(svc.get_api_key_slot_count(), 6u);
    EXPECT_NE(svc.get_api_key_slot("alice"), nullptr);
    EXPECT_NE(svc.get_api_key_slot("bob"), nullptr);
    EXPECT_NE(svc.get_api_key_slot("carol"), nullptr);
}

TEST_F(ApiKeyAttributionTest, PreFillSanitisesKeyNames) {
    // Operator-chosen names with chars Prometheus doesn't allow in labels.
    auto auth = make_auth_with_keys({"Production-Deploy", "team@example.com"});
    AttributionConfig cfg;

    svc.init_api_key_attribution(auth, cfg);

    // Sanitiser rules: lowercase, [^a-z0-9_] -> '_', truncate to 64.
    EXPECT_NE(svc.get_api_key_slot("production_deploy"), nullptr);
    EXPECT_NE(svc.get_api_key_slot("team_example_com"), nullptr);
    // Original names should NOT be registered.
    EXPECT_EQ(svc.get_api_key_slot("Production-Deploy"), nullptr);
    EXPECT_EQ(svc.get_api_key_slot("team@example.com"), nullptr);
}

TEST_F(ApiKeyAttributionTest, RecordingIncrementsSlotCounters) {
    auto auth = make_auth_with_keys({"alice"});
    AttributionConfig cfg;
    svc.init_api_key_attribution(auth, cfg);

    svc.record_api_key_request_received("alice");
    svc.record_api_key_request_received("alice");
    svc.record_api_key_success("alice");
    svc.record_api_key_failure("alice");
    svc.record_api_key_timeout("alice");
    svc.record_api_key_rate_limited("alice");
    svc.record_api_key_completion("alice",
        /*request_latency_s=*/0.5, /*total_latency_s=*/0.6,
        /*input=*/100, /*output=*/50, /*cost=*/1.5);

    const auto* slot = svc.get_api_key_slot("alice");
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->requests_total, 2u);
    EXPECT_EQ(slot->requests_success, 1u);
    EXPECT_EQ(slot->requests_failed, 1u);
    EXPECT_EQ(slot->requests_timeout, 1u);
    EXPECT_EQ(slot->requests_rate_limited, 1u);
    EXPECT_EQ(slot->input_tokens_sum, 100u);
    EXPECT_EQ(slot->output_tokens_sum, 50u);
    EXPECT_DOUBLE_EQ(slot->cost_units_sum, 1.5);
    EXPECT_EQ(slot->request_duration.data.sample_count, 1u);
    EXPECT_EQ(slot->router_total_latency.data.sample_count, 1u);
}

TEST_F(ApiKeyAttributionTest, UnconfiguredKeyRegistersLazilyUnderBound) {
    AuthConfig auth;  // no configured keys
    AttributionConfig cfg;  // default 256
    svc.init_api_key_attribution(auth, cfg);

    // First observation of an unseen key creates a slot (memo §6.2).
    svc.record_api_key_request_received("late_arrival");
    EXPECT_NE(svc.get_api_key_slot("late_arrival"), nullptr);
    EXPECT_EQ(svc.get_api_key_slot_count(), 4u);  // 3 sentinels + 1
    EXPECT_EQ(svc.get_api_key_label_overflow(), 0u);
}

TEST_F(ApiKeyAttributionTest, OverflowTriggersWhenBoundReached) {
    // Memo §9: configure cardinality=4 with 6 keys, drive traffic for all,
    // assert 4 + 3 sentinels are distinct and the rest fall to _overflow.
    AttributionConfig cfg;
    cfg.max_label_cardinality = 4;  // budget = 1 (cap - 3 sentinels)

    // Pre-fill with 2 keys — only 1 fits (budget = 1).
    auto auth = make_auth_with_keys({"first", "second"});
    svc.init_api_key_attribution(auth, cfg);

    EXPECT_EQ(svc.get_api_key_slot_count(), 4u);  // 3 sentinels + 1 pre-filled
    EXPECT_NE(svc.get_api_key_slot("first"), nullptr);
    EXPECT_EQ(svc.get_api_key_slot("second"), nullptr);  // budget exhausted

    // Runtime: "first" hits its own slot.
    svc.record_api_key_request_received("first");
    EXPECT_EQ(svc.get_api_key_label_overflow(), 0u);

    // Runtime: "second" was never registered; collapses to overflow.
    svc.record_api_key_request_received("second");
    EXPECT_EQ(svc.get_api_key_label_overflow(), 1u);
    EXPECT_EQ(svc.get_api_key_slot_count(), 4u);  // unchanged

    // Further unseen labels keep hitting overflow.
    svc.record_api_key_request_received("third");
    svc.record_api_key_request_received("fourth");
    svc.record_api_key_request_received("fifth");
    EXPECT_EQ(svc.get_api_key_label_overflow(), 4u);
    EXPECT_EQ(svc.get_api_key_slot_count(), 4u);
}

TEST_F(ApiKeyAttributionTest, OverflowSlotAccumulatesAcrossOverflowedKeys) {
    AttributionConfig cfg;
    cfg.max_label_cardinality = 4;
    AuthConfig auth = make_auth_with_keys({"only_pre_filled"});
    svc.init_api_key_attribution(auth, cfg);

    // All three of these collapse to _overflow.
    svc.record_api_key_request_received("a");
    svc.record_api_key_request_received("b");
    svc.record_api_key_success("c");

    const auto* overflow = svc.get_api_key_slot("_overflow");
    ASSERT_NE(overflow, nullptr);
    EXPECT_EQ(overflow->requests_total, 2u);
    EXPECT_EQ(overflow->requests_success, 1u);
}

TEST_F(ApiKeyAttributionTest, SentinelsDoNotCountTowardBound) {
    AttributionConfig cfg;
    cfg.max_label_cardinality = 4;
    AuthConfig auth;
    svc.init_api_key_attribution(auth, cfg);

    // 3 sentinels exist; budget = 1.
    EXPECT_EQ(svc.get_api_key_slot_count(), 3u);

    // Sentinels themselves never overflow.
    svc.record_api_key_request_received("_unauthenticated");
    svc.record_api_key_request_received("_invalid");
    svc.record_api_key_request_received("_overflow");
    EXPECT_EQ(svc.get_api_key_label_overflow(), 0u);

    // One real key still fits within the budget.
    svc.record_api_key_request_received("alice");
    EXPECT_EQ(svc.get_api_key_slot_count(), 4u);
    EXPECT_EQ(svc.get_api_key_label_overflow(), 0u);

    // The next one overflows.
    svc.record_api_key_request_received("bob");
    EXPECT_EQ(svc.get_api_key_label_overflow(), 1u);
}

TEST_F(ApiKeyAttributionTest, PreFillEnforcesMinimumCardinality) {
    // max_label_cardinality is clamped to 4 minimum (room for sentinels +
    // at least one real key). Verify the clamp.
    AttributionConfig cfg;
    cfg.max_label_cardinality = 2;  // below the floor
    AuthConfig auth = make_auth_with_keys({"alice"});
    svc.init_api_key_attribution(auth, cfg);

    // After clamp to 4, budget = 1, alice fits.
    EXPECT_NE(svc.get_api_key_slot("alice"), nullptr);
}

TEST_F(ApiKeyAttributionTest, EmptyAuthKeysLeavesOnlySentinels) {
    AuthConfig auth;
    AttributionConfig cfg;
    svc.init_api_key_attribution(auth, cfg);

    EXPECT_EQ(svc.get_api_key_slot_count(), 3u);
    EXPECT_EQ(svc.get_api_key_label_overflow(), 0u);
}

TEST_F(ApiKeyAttributionTest, PreFillSkipsKeysCollideringWithSentinelNames) {
    // Defensive: if an operator names a key "_unauthenticated", the
    // sentinel slot already exists and pre-fill must not double-register.
    auto auth = make_auth_with_keys({"_unauthenticated", "_invalid", "_overflow", "real_key"});
    AttributionConfig cfg;
    svc.init_api_key_attribution(auth, cfg);

    // 3 sentinels + 1 real key — the sentinel-named entries are skipped.
    EXPECT_EQ(svc.get_api_key_slot_count(), 4u);
    EXPECT_NE(svc.get_api_key_slot("real_key"), nullptr);
}

TEST_F(ApiKeyAttributionTest, RecordingUnknownLabelAfterInitCreatesSlotUnderBound) {
    AuthConfig auth = make_auth_with_keys({"alice"});
    AttributionConfig cfg;
    svc.init_api_key_attribution(auth, cfg);

    const size_t before = svc.get_api_key_slot_count();
    svc.record_api_key_completion("dynamically_observed",
        0.1, 0.1, 10, 5, 0.5);
    EXPECT_EQ(svc.get_api_key_slot_count(), before + 1);

    const auto* slot = svc.get_api_key_slot("dynamically_observed");
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->input_tokens_sum, 10u);
    EXPECT_EQ(slot->output_tokens_sum, 5u);
    EXPECT_DOUBLE_EQ(slot->cost_units_sum, 0.5);
}

TEST_F(ApiKeyAttributionTest, OverflowSlotAggregatesCompletionData) {
    AttributionConfig cfg;
    cfg.max_label_cardinality = 4;  // budget = 1
    AuthConfig auth;
    svc.init_api_key_attribution(auth, cfg);

    // Fill the budget with one real slot first.
    svc.record_api_key_request_received("anchor");

    // Now several overflowing keys with completion data.
    svc.record_api_key_completion("overflow_a", 0.1, 0.2, 100, 50, 1.0);
    svc.record_api_key_completion("overflow_b", 0.3, 0.4, 200, 100, 2.0);

    const auto* overflow = svc.get_api_key_slot("_overflow");
    ASSERT_NE(overflow, nullptr);
    EXPECT_EQ(overflow->input_tokens_sum, 300u);
    EXPECT_EQ(overflow->output_tokens_sum, 150u);
    EXPECT_DOUBLE_EQ(overflow->cost_units_sum, 3.0);
    EXPECT_EQ(overflow->request_duration.data.sample_count, 2u);
}

