// Reactor-free unit tests for CacheTopologyIndex (BACKLOG §21 P2 slice 2a).
//
// Pure structure — no Seastar. Covers per-node set-replace, peer-death eviction,
// TTL age-out, self-cleaning, sole-holder counting, and the Rule #4 bounds.

#include "cache_topology_index.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

using namespace ranvier;

namespace {
CacheTopologyIndex::TimePoint t0() { return CacheTopologyIndex::Clock::now(); }
}  // namespace

TEST(CacheTopologyIndexTest, EmptyState) {
    CacheTopologyIndex idx;
    EXPECT_EQ(idx.prefix_count(), 0u);
    EXPECT_EQ(idx.node_count(), 0u);
    EXPECT_EQ(idx.holder_count(123), 0u);
    EXPECT_EQ(idx.holders_of(123), nullptr);
}

TEST(CacheTopologyIndexTest, ApplyAddsHolders) {
    CacheTopologyIndex idx;
    idx.apply_digest(/*node=*/1, {10, 20}, t0());

    EXPECT_EQ(idx.node_count(), 1u);
    EXPECT_EQ(idx.prefix_count(), 2u);
    EXPECT_EQ(idx.holder_count(10), 1u);
    ASSERT_NE(idx.holders_of(10), nullptr);
    EXPECT_TRUE(idx.holders_of(10)->contains(1));
}

TEST(CacheTopologyIndexTest, SetReplaceSupersedesPriorDigest) {
    CacheTopologyIndex idx;
    auto now = t0();
    idx.apply_digest(1, {10, 20}, now);
    idx.apply_digest(1, {20, 30}, now);  // 10 dropped, 30 added, 20 kept

    EXPECT_EQ(idx.node_count(), 1u);
    EXPECT_EQ(idx.holder_count(10), 0u);  // node 1 no longer holds 10 -> self-cleaned
    EXPECT_EQ(idx.holder_count(20), 1u);
    EXPECT_EQ(idx.holder_count(30), 1u);
    EXPECT_EQ(idx.prefix_count(), 2u);
}

TEST(CacheTopologyIndexTest, MultipleHoldersAndSoleDetection) {
    CacheTopologyIndex idx;
    auto now = t0();
    idx.apply_digest(1, {10}, now);
    idx.apply_digest(2, {10}, now);
    EXPECT_EQ(idx.holder_count(10), 2u);  // shared -> safe to reap either

    idx.evict_node(2);
    EXPECT_EQ(idx.holder_count(10), 1u);  // now sole-held by node 1 -> unsafe to reap
    ASSERT_NE(idx.holders_of(10), nullptr);
    EXPECT_TRUE(idx.holders_of(10)->contains(1));
}

TEST(CacheTopologyIndexTest, EvictNodeRemovesAllItsHolders) {
    CacheTopologyIndex idx;
    auto now = t0();
    idx.apply_digest(1, {10, 20}, now);
    idx.apply_digest(2, {20}, now);

    idx.evict_node(1);
    EXPECT_EQ(idx.node_count(), 1u);
    EXPECT_EQ(idx.holder_count(10), 0u);   // only node 1 held it
    EXPECT_EQ(idx.holder_count(20), 1u);   // node 2 still holds it
    EXPECT_EQ(idx.prefix_count(), 1u);
}

TEST(CacheTopologyIndexTest, EmptyDigestSelfCleansButKeepsNode) {
    CacheTopologyIndex idx;
    auto now = t0();
    idx.apply_digest(1, {10}, now);
    idx.apply_digest(1, {}, now);  // node reports no hot prefixes this window

    EXPECT_EQ(idx.prefix_count(), 0u);  // 10 self-cleaned
    EXPECT_EQ(idx.node_count(), 1u);    // node still tracked (last_seen refreshed)
    EXPECT_EQ(idx.holder_count(10), 0u);
}

TEST(CacheTopologyIndexTest, AgeOutEvictsStaleNodes) {
    CacheTopologyIndex idx;
    auto base = t0();
    idx.apply_digest(1, {10}, base);
    idx.apply_digest(2, {20}, base + std::chrono::seconds(10));

    size_t evicted = idx.age_out(base + std::chrono::seconds(5));
    EXPECT_EQ(evicted, 1u);              // node 1 (last_seen=base) is stale
    EXPECT_EQ(idx.node_count(), 1u);
    EXPECT_EQ(idx.holder_count(10), 0u); // node 1's prefix gone
    EXPECT_EQ(idx.holder_count(20), 1u); // node 2 fresh
}

TEST(CacheTopologyIndexTest, RefreshUpdatesLastSeen) {
    CacheTopologyIndex idx;
    auto base = t0();
    idx.apply_digest(1, {10}, base);
    idx.apply_digest(1, {10}, base + std::chrono::seconds(10));  // refresh

    // A cutoff older than the refresh must NOT evict it.
    EXPECT_EQ(idx.age_out(base + std::chrono::seconds(5)), 0u);
    EXPECT_EQ(idx.node_count(), 1u);
}

TEST(CacheTopologyIndexTest, BoundedByMaxNodes) {
    CacheTopologyIndex idx;
    auto now = t0();
    for (BackendId n = 0; n < static_cast<BackendId>(CacheTopologyIndex::MAX_NODES) + 5; ++n) {
        idx.apply_digest(n, {static_cast<uint64_t>(n)}, now);
    }
    EXPECT_EQ(idx.node_count(), CacheTopologyIndex::MAX_NODES);
    EXPECT_GT(idx.node_overflow(), 0u);
}

TEST(CacheTopologyIndexTest, BoundedByMaxPrefixes) {
    CacheTopologyIndex idx;
    auto now = t0();
    // Spread distinct prefixes across nodes (each node's digest is small) until
    // the prefix cap trips.
    uint64_t h = 0;
    for (BackendId n = 0; n < 400; ++n) {
        std::vector<uint64_t> hashes;
        for (int i = 0; i < 100; ++i) hashes.push_back(h++);
        idx.apply_digest(n, hashes, now);
    }
    EXPECT_EQ(idx.prefix_count(), CacheTopologyIndex::MAX_HOT_PREFIXES);
    EXPECT_GT(idx.prefix_overflow(), 0u);
}
