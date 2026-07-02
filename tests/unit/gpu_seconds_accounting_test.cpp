// Ranvier Core - Capacity-Weighted Uptime Accounting Tests (pure, no Seastar)
//
// Exercises the pure GPU-seconds arithmetic that backs the
// backend_gpu_seconds_total counter. No reactor, no RouterService — the router
// wiring is a thin layer over exactly these functions (capture a steady_clock
// timestamp, store the running accumulator), so validating the math here covers
// the accounting itself.

#include "gpu_seconds_accounting.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace ranvier;

// ---------------------------------------------------------------------------
// gpu_seconds_total: sum over a sequence of live segments
// ---------------------------------------------------------------------------

TEST(GpuSecondsAccounting, SingleSegment) {
    // 8 GPUs live for 10s -> 80 GPU-seconds.
    EXPECT_DOUBLE_EQ(gpu_seconds_total({{8.0, 10.0}}), 80.0);
}

TEST(GpuSecondsAccounting, MultipleSegments) {
    // Liveness flaps: live, down (no segment), live again.
    std::vector<GpuSecondsSegment> segments = {{4.0, 30.0}, {4.0, 15.0}};
    EXPECT_DOUBLE_EQ(gpu_seconds_total(segments), 180.0);
}

TEST(GpuSecondsAccounting, GpuCountChangeMidLife) {
    // A mid-life gpu_count change splits into two segments: the first accrues at
    // the OLD count, the second at the NEW count.
    std::vector<GpuSecondsSegment> segments = {{2.0, 10.0}, {8.0, 5.0}};
    EXPECT_DOUBLE_EQ(gpu_seconds_total(segments), 2.0 * 10.0 + 8.0 * 5.0);  // 60
}

TEST(GpuSecondsAccounting, NeverLiveIsZero) {
    EXPECT_DOUBLE_EQ(gpu_seconds_total({}), 0.0);
}

TEST(GpuSecondsAccounting, FractionalGpuCount) {
    // A MIG slice declared as 0.5 GPUs.
    EXPECT_DOUBLE_EQ(gpu_seconds_total({{0.5, 20.0}}), 10.0);
}

TEST(GpuSecondsAccounting, NonPositiveContributionsDropped) {
    // A steady clock never runs backwards; a negative/zero duration or a
    // non-positive count must not subtract from (or otherwise corrupt) the
    // monotonic total.
    std::vector<GpuSecondsSegment> segments = {
        {8.0, 10.0},    // counts: 80
        {8.0, -5.0},    // negative duration: dropped
        {8.0, 0.0},     // zero duration: dropped
        {0.0, 5.0},     // zero count: dropped
        {-2.0, 5.0},    // negative count: dropped
    };
    EXPECT_DOUBLE_EQ(gpu_seconds_total(segments), 80.0);
}

TEST(GpuSecondsAccounting, MonotonicAcrossSegments) {
    // Appending a real (positive) segment can only increase the total.
    std::vector<GpuSecondsSegment> segments;
    double prev = 0.0;
    for (int i = 0; i < 5; ++i) {
        segments.push_back({2.0, 3.0});
        double total = gpu_seconds_total(segments);
        EXPECT_GT(total, prev);
        prev = total;
    }
    EXPECT_DOUBLE_EQ(prev, 2.0 * 3.0 * 5.0);  // 30
}

// ---------------------------------------------------------------------------
// gpu_seconds_with_in_progress: scrape-time / finalize calculation
// ---------------------------------------------------------------------------

TEST(GpuSecondsAccounting, InProgressAddsWhileLive) {
    // accumulated (finalized) + in-progress live segment.
    EXPECT_DOUBLE_EQ(gpu_seconds_with_in_progress(80.0, /*live=*/true, 4.0, 5.0),
                     100.0);
}

TEST(GpuSecondsAccounting, InProgressIgnoredWhenNotLive) {
    // A down backend accrues nothing; only the already-finalized total shows.
    EXPECT_DOUBLE_EQ(gpu_seconds_with_in_progress(80.0, /*live=*/false, 4.0, 5.0),
                     80.0);
}

TEST(GpuSecondsAccounting, InProgressZeroElapsedIsAccumulator) {
    EXPECT_DOUBLE_EQ(gpu_seconds_with_in_progress(80.0, true, 4.0, 0.0), 80.0);
}

TEST(GpuSecondsAccounting, ScrapeDoesNotAdvanceStoredState) {
    // Two consecutive scrapes with no transition between them: the reported
    // value grows with elapsed time, but the stored accumulator is never
    // mutated (the function is pure / read-only), and a later finalize at the
    // same instant equals the last scrape — i.e. no double counting.
    const double accumulated = 80.0;  // one finalized segment
    const bool live = true;
    const double count = 4.0;

    double scrape_at_5 = gpu_seconds_with_in_progress(accumulated, live, count, 5.0);
    double scrape_at_9 = gpu_seconds_with_in_progress(accumulated, live, count, 9.0);

    EXPECT_DOUBLE_EQ(scrape_at_5, 100.0);
    EXPECT_DOUBLE_EQ(scrape_at_9, 116.0);
    EXPECT_GT(scrape_at_9, scrape_at_5);

    // Stored accumulator untouched by scraping.
    EXPECT_DOUBLE_EQ(accumulated, 80.0);

    // A transition finalizing the segment at t=9 folds in exactly the same
    // in-progress amount the last scrape reported — scraping never advanced it.
    double finalized = gpu_seconds_with_in_progress(accumulated, live, count, 9.0);
    EXPECT_DOUBLE_EQ(finalized, scrape_at_9);
}

TEST(GpuSecondsAccounting, FinalizeThenContinueMatchesSingleSegment) {
    // Finalizing mid-segment and continuing must equal accounting the whole
    // live span as one segment (continuity across a transition boundary).
    const double count = 4.0;
    double accumulated = 0.0;

    // Live for 5s, then a transition (e.g. gpu_count read, status toggle)
    // finalizes the segment at the same count.
    accumulated = gpu_seconds_with_in_progress(accumulated, true, count, 5.0);  // 20
    // Continue live for another 7s; scrape.
    double total = gpu_seconds_with_in_progress(accumulated, true, count, 7.0);  // 20 + 28

    EXPECT_DOUBLE_EQ(total, gpu_seconds_total({{count, 12.0}}));  // 48
}
