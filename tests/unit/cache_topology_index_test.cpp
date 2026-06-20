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

// Slice 2b sole-holder lifecycle: encodes the exact sequence the telemetry
// emitter + gossip callbacks drive against the index, so the contract slice 3
// consumes is pinned without a reactor.
//   - emit self-apply  -> apply_digest(self, our top-K)
//   - peer digest recv -> apply_digest(peer, ...)   (gossip callback)
//   - peer death       -> evict_node(peer)          (prune hook)
//   - peer goes silent -> age_out(now - window*N)   (emit backstop)
TEST(CacheTopologyIndexTest, P2SoleHolderLifecycle) {
    CacheTopologyIndex idx;
    constexpr BackendId kSelf = 7, kPeerA = 8, kPeerB = 9;
    constexpr uint64_t kOurs = 0xABC;    // a hot prefix only we hold
    constexpr uint64_t kShared = 0xDEF;  // a hot prefix we and a peer hold
    const auto base = t0();
    using std::chrono::seconds;

    // Emit window: self-apply our merged top-K. The correctness point — a prefix
    // only we hold must read as 1 holder (sole-held), NOT 0. Without self-apply
    // the autoscaler would think nobody holds it and reap it.
    idx.apply_digest(kSelf, {kOurs, kShared}, base);
    EXPECT_EQ(idx.holder_count(kOurs), 1u);
    EXPECT_EQ(idx.holder_count(kShared), 1u);

    // Peer A's digest arrives (gossip apply_peer_digest): kShared becomes shared.
    idx.apply_digest(kPeerA, {kShared}, base + seconds(1));
    EXPECT_EQ(idx.holder_count(kShared), 2u);  // shared -> safe to reap on A
    EXPECT_EQ(idx.holder_count(kOurs), 1u);     // ours is still sole-held

    // Peer A dies (prune hook -> evict_node): kShared reverts to sole-held by us.
    idx.evict_node(kPeerA);
    EXPECT_EQ(idx.holder_count(kShared), 1u);

    // Peer B reports kShared, then goes silent without being marked dead.
    idx.apply_digest(kPeerB, {kShared}, base + seconds(2));
    EXPECT_EQ(idx.holder_count(kShared), 2u);

    // Next emit window: the emitter self-applies FIRST (refreshing our last_seen
    // to now), THEN ages out anyone older than now - window*N — the exact order
    // in emit_async. So our just-refreshed entry always survives; only the silent
    // peer B (last_seen base+2s) is older than the base+5s cutoff and drops.
    const auto t_emit = base + seconds(10);
    idx.apply_digest(kSelf, {kOurs, kShared}, t_emit);   // self refresh
    EXPECT_EQ(idx.age_out(t_emit - seconds(5)), 1u);     // only silent peer B
    EXPECT_EQ(idx.holder_count(kShared), 1u);            // sole-held by us again
    EXPECT_EQ(idx.holder_count(kOurs), 1u);              // and ours never lapsed
    EXPECT_EQ(idx.node_count(), 1u);                     // only self remains, fresh
}

// =============================================================================
// BACKLOG §21 Phase 5c: verified-resident holder tier
// =============================================================================
// The membership tier (holder_count) is unchanged; verified_holder_count is the
// strict subset of holders whose digest marked a hash verified-resident in their
// own KV cache. verified ⊆ membership is enforced inside apply_digest.

TEST(CacheTopologyIndexTest, VerifiedSubsetRecorded) {
    CacheTopologyIndex idx;
    // Node 1 holds {10,20,30}; verifies only {10,30} resident.
    idx.apply_digest(1, {10, 20, 30}, {10, 30}, t0());

    EXPECT_EQ(idx.holder_count(10), 1u);
    EXPECT_EQ(idx.holder_count(20), 1u);
    EXPECT_EQ(idx.verified_holder_count(10), 1u);
    EXPECT_EQ(idx.verified_holder_count(20), 0u);  // member but not verified
    EXPECT_EQ(idx.verified_holder_count(30), 1u);
    ASSERT_NE(idx.verified_holders_of(10), nullptr);
    EXPECT_TRUE(idx.verified_holders_of(10)->contains(1));
    EXPECT_EQ(idx.verified_holders_of(20), nullptr);
}

TEST(CacheTopologyIndexTest, MembershipOnlyDigestHasNoVerified) {
    CacheTopologyIndex idx;
    // 3-arg overload (or empty verified) => membership-only, e.g. an old/non-native
    // peer: holders count, but nothing is verified-resident.
    idx.apply_digest(1, {10, 20}, t0());
    EXPECT_EQ(idx.holder_count(10), 1u);
    EXPECT_EQ(idx.verified_holder_count(10), 0u);
    EXPECT_EQ(idx.verified_holders_of(10), nullptr);
}

TEST(CacheTopologyIndexTest, VerifiedOutsideMembershipIsDropped) {
    CacheTopologyIndex idx;
    // A malformed peer claims 99 verified but doesn't hold it: verified ⊆ membership
    // must drop it so it can't inflate the verified count.
    idx.apply_digest(1, {10, 20}, {10, 99}, t0());
    EXPECT_EQ(idx.verified_holder_count(10), 1u);
    EXPECT_EQ(idx.verified_holder_count(99), 0u);
    EXPECT_EQ(idx.holder_count(99), 0u);  // not a member either
}

TEST(CacheTopologyIndexTest, SetReplaceUpdatesVerifiedTier) {
    CacheTopologyIndex idx;
    auto now = t0();
    idx.apply_digest(1, {10, 20}, {10, 20}, now);
    EXPECT_EQ(idx.verified_holder_count(10), 1u);
    EXPECT_EQ(idx.verified_holder_count(20), 1u);

    // Next window: 20 evicted from KV (still a member), 10 stays verified, 30 new
    // and verified.
    idx.apply_digest(1, {10, 20, 30}, {10, 30}, now);
    EXPECT_EQ(idx.holder_count(20), 1u);            // still a member
    EXPECT_EQ(idx.verified_holder_count(20), 0u);   // no longer verified-resident
    EXPECT_EQ(idx.verified_holder_count(10), 1u);
    EXPECT_EQ(idx.verified_holder_count(30), 1u);
}

TEST(CacheTopologyIndexTest, EvictNodeClearsVerified) {
    CacheTopologyIndex idx;
    auto now = t0();
    idx.apply_digest(1, {10}, {10}, now);
    idx.apply_digest(2, {10}, {10}, now);
    EXPECT_EQ(idx.verified_holder_count(10), 2u);

    idx.evict_node(2);
    EXPECT_EQ(idx.verified_holder_count(10), 1u);   // self-cleaned with the node
    EXPECT_TRUE(idx.verified_holders_of(10)->contains(1));
}

TEST(CacheTopologyIndexTest, AgeOutClearsVerified) {
    CacheTopologyIndex idx;
    auto base = t0();
    idx.apply_digest(1, {10}, {10}, base);
    idx.apply_digest(2, {10}, {10}, base + std::chrono::seconds(10));

    EXPECT_EQ(idx.age_out(base + std::chrono::seconds(5)), 1u);  // node 1 stale
    EXPECT_EQ(idx.verified_holder_count(10), 1u);                // only node 2 left
}

// The core distinction 5d gates reaping on: a prefix can be membership-shared
// (holder_count==2 => looks safe) yet verified-sole-held (verified_holder_count==1
// => actually only one node truly has it resident, unsafe to reap that one).
TEST(CacheTopologyIndexTest, VerifiedSoleHeldDespiteSharedMembership) {
    CacheTopologyIndex idx;
    auto now = t0();
    // Both nodes route the prefix (membership), but only node 1 verifies it resident
    // (node 2 routes it but evicted it from KV, or is non-native => empty verified).
    idx.apply_digest(1, {0xAA}, {0xAA}, now);
    idx.apply_digest(2, {0xAA}, {}, now);

    EXPECT_EQ(idx.holder_count(0xAA), 2u);           // membership: shared
    EXPECT_EQ(idx.verified_holder_count(0xAA), 1u);  // residency: sole-held by node 1
    EXPECT_TRUE(idx.verified_holders_of(0xAA)->contains(1));
    EXPECT_FALSE(idx.verified_holders_of(0xAA)->contains(2));
}
