// Ranvier Core - VLLMMetrics Unit Tests
//
// Tests for the VLLMMetrics struct, focusing on load_score() behavior
// across idle, loaded, queuing, and saturated scenarios.

#include "vllm_metrics.hpp"
#include <gtest/gtest.h>
#include <cmath>

using namespace ranvier;

// =============================================================================
// Default State
// =============================================================================

class VLLMMetricsTest : public ::testing::Test {};

TEST_F(VLLMMetricsTest, DefaultConstructionIsInvalid) {
    VLLMMetrics m;
    EXPECT_FALSE(m.valid);
    EXPECT_EQ(m.num_requests_running, 0u);
    EXPECT_EQ(m.num_requests_waiting, 0u);
    EXPECT_DOUBLE_EQ(m.gpu_cache_usage_percent, 0.0);
}

TEST_F(VLLMMetricsTest, InvalidMetricsReturnZeroLoadScore) {
    VLLMMetrics m;
    // Even with high values, invalid means 0.0
    m.num_requests_running = 100;
    m.gpu_cache_usage_percent = 1.0;
    EXPECT_DOUBLE_EQ(m.load_score(), 0.0);
}

// =============================================================================
// Idle Backend (valid but no load)
// =============================================================================

TEST_F(VLLMMetricsTest, IdleBackendScoresZero) {
    VLLMMetrics m;
    m.valid = true;
    // All zeros: no requests, no cache pressure
    EXPECT_DOUBLE_EQ(m.load_score(), 0.0);
}

// =============================================================================
// Request Pressure Only (no cache pressure)
// =============================================================================

TEST_F(VLLMMetricsTest, SingleRunningRequestNoQueue) {
    VLLMMetrics m;
    m.valid = true;
    m.num_requests_running = 1;
    // request_pressure = (1+0)/(1+1) = 0.5
    // score = 0.7 * min(0.5/3.0, 1.0) + 0.3 * 0.0
    //       = 0.7 * 0.1667 = ~0.1167
    double score = m.load_score();
    EXPECT_GT(score, 0.0);
    EXPECT_LT(score, 0.2);
}

TEST_F(VLLMMetricsTest, ManyRunningNoQueue) {
    VLLMMetrics m;
    m.valid = true;
    m.num_requests_running = 10;
    // request_pressure = (10+0)/(10+1) = 0.909
    // score = 0.7 * min(0.909/3.0, 1.0) = 0.7 * 0.303 = ~0.212
    double score = m.load_score();
    EXPECT_GT(score, 0.15);
    EXPECT_LT(score, 0.3);
}

TEST_F(VLLMMetricsTest, QueueBuildupIncreasesScore) {
    VLLMMetrics m;
    m.valid = true;
    m.num_requests_running = 5;

    // No queue
    double score_no_queue = m.load_score();

    // Some queue
    m.num_requests_waiting = 5;
    double score_with_queue = m.load_score();

    // Heavy queue
    m.num_requests_waiting = 20;
    double score_heavy_queue = m.load_score();

    EXPECT_LT(score_no_queue, score_with_queue);
    EXPECT_LT(score_with_queue, score_heavy_queue);
}

TEST_F(VLLMMetricsTest, HeavyQueueApproachesRequestCap) {
    VLLMMetrics m;
    m.valid = true;
    m.num_requests_running = 10;
    m.num_requests_waiting = 100;
    // request_pressure = 110/11 = 10.0
    // score = 0.7 * min(10.0/3.0, 1.0) = 0.7 * 1.0 = 0.7
    double score = m.load_score();
    EXPECT_NEAR(score, 0.7, 0.01);
}

// =============================================================================
// Cache Pressure Only (no request load)
// =============================================================================

TEST_F(VLLMMetricsTest, CachePressureOnly) {
    VLLMMetrics m;
    m.valid = true;
    m.gpu_cache_usage_percent = 0.8;
    // request_pressure = 0 (no requests)
    // score = 0.7 * 0.0 + 0.3 * 0.8 = 0.24
    EXPECT_NEAR(m.load_score(), 0.24, 0.001);
}

TEST_F(VLLMMetricsTest, FullCacheNoRequests) {
    VLLMMetrics m;
    m.valid = true;
    m.gpu_cache_usage_percent = 1.0;
    // score = 0.7 * 0.0 + 0.3 * 1.0 = 0.3
    EXPECT_NEAR(m.load_score(), 0.3, 0.001);
}

// =============================================================================
// Combined Pressure
// =============================================================================

TEST_F(VLLMMetricsTest, ModerateLoadWithCache) {
    VLLMMetrics m;
    m.valid = true;
    m.num_requests_running = 8;
    m.num_requests_waiting = 3;
    m.gpu_cache_usage_percent = 0.65;
    // request_pressure = 11/9 = 1.222
    // score = 0.7 * min(1.222/3.0, 1.0) + 0.3 * 0.65
    //       = 0.7 * 0.407 + 0.195 = 0.285 + 0.195 = ~0.48
    double score = m.load_score();
    EXPECT_GT(score, 0.4);
    EXPECT_LT(score, 0.6);
}

TEST_F(VLLMMetricsTest, SaturatedBackend) {
    VLLMMetrics m;
    m.valid = true;
    m.num_requests_running = 10;
    m.num_requests_waiting = 100;
    m.gpu_cache_usage_percent = 1.0;
    // request: 0.7 * 1.0 = 0.7, cache: 0.3 * 1.0 = 0.3 → total = 1.0
    EXPECT_DOUBLE_EQ(m.load_score(), 1.0);
}

// =============================================================================
// Clamping
// =============================================================================

TEST_F(VLLMMetricsTest, ScoreNeverExceedsOne) {
    VLLMMetrics m;
    m.valid = true;
    m.num_requests_running = 1000;
    m.num_requests_waiting = 10000;
    m.gpu_cache_usage_percent = 5.0;  // Invalid value, but shouldn't crash
    EXPECT_LE(m.load_score(), 1.0);
}

// =============================================================================
// Monotonicity — score increases with load
// =============================================================================

TEST_F(VLLMMetricsTest, ScoreIncreasesMonotonicallyWithWaiting) {
    VLLMMetrics m;
    m.valid = true;
    m.num_requests_running = 5;
    m.gpu_cache_usage_percent = 0.3;

    double prev = 0.0;
    for (uint32_t waiting = 0; waiting <= 50; waiting += 5) {
        m.num_requests_waiting = waiting;
        double score = m.load_score();
        EXPECT_GE(score, prev) << "Score decreased at waiting=" << waiting;
        prev = score;
    }
}

TEST_F(VLLMMetricsTest, ScoreIncreasesMonotonicallyWithCache) {
    VLLMMetrics m;
    m.valid = true;
    m.num_requests_running = 5;
    m.num_requests_waiting = 2;

    double prev = 0.0;
    for (int pct = 0; pct <= 100; pct += 10) {
        m.gpu_cache_usage_percent = pct / 100.0;
        double score = m.load_score();
        EXPECT_GE(score, prev) << "Score decreased at cache=" << pct << "%";
        prev = score;
    }
}

// =============================================================================
// Edge: Only Waiting (no running)
// =============================================================================

TEST_F(VLLMMetricsTest, WaitingOnlyNoRunning) {
    VLLMMetrics m;
    m.valid = true;
    m.num_requests_waiting = 10;
    // request_pressure = (0+10)/(0+1) = 10.0
    // score = 0.7 * min(10.0/3.0, 1.0) = 0.7 * 1.0 = 0.7
    EXPECT_NEAR(m.load_score(), 0.7, 0.01);
}
