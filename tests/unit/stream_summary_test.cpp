// Reactor-free unit tests for StreamSummary (Space-Saving bounded top-K).
//
// Pure C++: StreamSummary has no Seastar dependency, so these tests never touch
// a reactor primitive and run standalone under Google Test.

#include "stream_summary.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace ranvier;

namespace {

// Find an entry by key in a snapshot; returns nullptr if absent.
const StreamSummary::Entry* find_key(const std::vector<StreamSummary::Entry>& v,
                                     uint64_t key) {
    for (const auto& e : v) {
        if (e.key == key) return &e;
    }
    return nullptr;
}

}  // namespace

TEST(StreamSummaryTest, EmptyState) {
    StreamSummary s(8);
    EXPECT_EQ(s.size(), 0u);
    EXPECT_EQ(s.total(), 0u);
    EXPECT_EQ(s.capacity(), 8u);
    EXPECT_TRUE(s.snapshot().empty());
}

TEST(StreamSummaryTest, DefaultCapacity) {
    StreamSummary s;
    EXPECT_EQ(s.capacity(), StreamSummary::kDefaultCapacity);
}

TEST(StreamSummaryTest, ZeroCapacityCoercedToOne) {
    StreamSummary s(0);
    EXPECT_EQ(s.capacity(), 1u);
    s.touch(42);
    s.touch(43);  // evicts; capacity-1 summary keeps exactly one slot
    EXPECT_EQ(s.size(), 1u);
}

TEST(StreamSummaryTest, TracksDistinctKeysBelowCapacity) {
    StreamSummary s(4);
    s.touch(1);
    s.touch(2);
    s.touch(3);
    EXPECT_EQ(s.size(), 3u);
    EXPECT_EQ(s.total(), 3u);

    auto snap = s.snapshot();
    for (uint64_t k : {1u, 2u, 3u}) {
        const auto* e = find_key(snap, k);
        ASSERT_NE(e, nullptr);
        EXPECT_EQ(e->count, 1u);
        EXPECT_EQ(e->error, 0u);  // no eviction yet -> exact
    }
}

TEST(StreamSummaryTest, HitsIncrementCount) {
    StreamSummary s(4);
    s.touch(7);
    s.touch(7);
    s.touch(7);
    auto snap = s.snapshot();
    const auto* e = find_key(snap, 7);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->count, 3u);
    EXPECT_EQ(e->error, 0u);
    EXPECT_EQ(s.total(), 3u);
}

TEST(StreamSummaryTest, SnapshotSortedDescendingByCount) {
    StreamSummary s(4);
    s.touch(10); s.touch(10); s.touch(10);  // count 3
    s.touch(20);                            // count 1
    s.touch(30); s.touch(30);               // count 2

    auto snap = s.snapshot();
    ASSERT_EQ(snap.size(), 3u);
    EXPECT_EQ(snap[0].key, 10u);
    EXPECT_EQ(snap[1].key, 30u);
    EXPECT_EQ(snap[2].key, 20u);
}

TEST(StreamSummaryTest, EvictionReplacesMinAndSetsError) {
    StreamSummary s(2);
    s.touch(1); s.touch(1);  // key 1: count 2
    s.touch(2);              // key 2: count 1  (summary now full)
    s.touch(3);              // miss while full -> evict min (key 2, count 1)

    EXPECT_EQ(s.size(), 2u);   // never exceeds capacity (Rule #4)
    EXPECT_EQ(s.total(), 4u);

    auto snap = s.snapshot();
    EXPECT_EQ(find_key(snap, 2), nullptr);  // evicted

    const auto* one = find_key(snap, 1);
    ASSERT_NE(one, nullptr);
    EXPECT_EQ(one->count, 2u);
    EXPECT_EQ(one->error, 0u);

    const auto* three = find_key(snap, 3);
    ASSERT_NE(three, nullptr);
    EXPECT_EQ(three->count, 2u);   // inherited victim count (1) + 1
    EXPECT_EQ(three->error, 1u);   // victim count becomes the error bound
}

TEST(StreamSummaryTest, NeverExceedsCapacityUnderManyDistinctKeys) {
    StreamSummary s(3);
    for (uint64_t k = 0; k < 1000; ++k) {
        s.touch(k);
    }
    EXPECT_EQ(s.size(), 3u);
    EXPECT_EQ(s.snapshot().size(), 3u);
    EXPECT_EQ(s.total(), 1000u);
}

TEST(StreamSummaryTest, HeavyHitterAlwaysRetained) {
    // A key touched far more than total/capacity is guaranteed kept, and since it
    // is never evicted its error stays 0.
    StreamSummary s(3);
    constexpr uint64_t kHot = 999999;
    uint64_t hot_touches = 0;
    for (uint64_t i = 0; i < 200; ++i) {
        s.touch(kHot);
        ++hot_touches;
        s.touch(i);  // 200 distinct cold keys churning through the other slots
    }

    auto snap = s.snapshot();
    ASSERT_FALSE(snap.empty());
    EXPECT_EQ(snap.front().key, kHot);
    EXPECT_EQ(snap.front().count, hot_touches);
    EXPECT_EQ(snap.front().error, 0u);
}

TEST(StreamSummaryTest, CountErrorInvariantHolds) {
    // For every retained entry: count >= error, and the lower bound count-error
    // is a valid (non-negative) frequency estimate.
    StreamSummary s(8);
    for (uint64_t i = 0; i < 500; ++i) {
        s.touch(i % 20);  // 20 distinct keys, cap 8 -> forces evictions
    }
    for (const auto& e : s.snapshot()) {
        EXPECT_GE(e.count, e.error);
        EXPECT_GE(e.count, 1u);
    }
}

TEST(StreamSummaryTest, Clear) {
    StreamSummary s(4);
    s.touch(1); s.touch(2); s.touch(1);
    s.clear();
    EXPECT_EQ(s.size(), 0u);
    EXPECT_EQ(s.total(), 0u);
    EXPECT_TRUE(s.snapshot().empty());

    s.touch(5);  // usable after clear
    EXPECT_EQ(s.size(), 1u);
    EXPECT_EQ(s.total(), 1u);
}
