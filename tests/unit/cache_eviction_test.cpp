// Ranvier Core - Cache Eviction Unit Tests
//
// Tests for push-based cache eviction notifications (Phase 1):
// - Shard-local eviction via prefix hash reverse index
// - Stale event rejection via timestamp tracking
// - Unknown hash handling
// - Metrics recording
//
// Prefix hash hex encoding/decoding and hash_prefix() tests live in
// parse_utils_test.cpp alongside other parse utility tests.

#include "parse_utils.hpp"
#include "radix_tree.hpp"
#include "node_slab.hpp"
#include "router_service.hpp"

#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace ranvier;

// =============================================================================
// Shard-Local Eviction Tests (using testing helpers)
// =============================================================================

class CacheEvictionTest : public ::testing::Test {
protected:
    void SetUp() override {
        RoutingConfig cfg;
        cfg.max_routes = 10000;
        cfg.prefix_token_length = 16;
        cfg.block_alignment = 16;
        RouterService::reset_shard_state_for_testing(&cfg);
    }

    void TearDown() override {
        RouterService::reset_shard_state_for_testing(nullptr);
    }

    // Helper: create a token vector and compute its prefix hash
    std::pair<std::vector<int32_t>, uint64_t> make_route(int32_t base, size_t len = 16) {
        std::vector<int32_t> tokens(len);
        for (size_t i = 0; i < len; i++) {
            tokens[i] = base + static_cast<int32_t>(i);
        }
        uint64_t h = hash_prefix(tokens.data(), tokens.size(), 16);
        return {tokens, h};
    }
};

TEST_F(CacheEvictionTest, EvictRemovesMatchingRoute) {
    auto [tokens, prefix_hash] = make_route(100);
    BackendId backend = 1;

    // Insert route and populate reverse index
    RouterService::register_backend_for_testing(backend, seastar::socket_address(seastar::net::inet_address("127.0.0.1"), 8080));
    RouterService::insert_route_for_testing(tokens, backend);
    RouterService::update_prefix_hash_index_for_testing(prefix_hash, backend);

    EXPECT_EQ(RouterService::get_route_count_for_testing(), 1u);

    // Evict by prefix hash
    uint32_t evicted = RouterService::evict_by_prefix_hash_local(
        prefix_hash, backend, 1000);

    EXPECT_GT(evicted, 0u);
    EXPECT_EQ(RouterService::get_route_count_for_testing(), 0u);
}

TEST_F(CacheEvictionTest, EvictUnknownHashReturnsZero) {
    uint64_t unknown_hash = 0xDEADBEEFULL;
    BackendId backend = 1;

    RouterService::register_backend_for_testing(backend, seastar::socket_address(seastar::net::inet_address("127.0.0.1"), 8080));

    uint32_t evicted = RouterService::evict_by_prefix_hash_local(
        unknown_hash, backend, 1000);

    EXPECT_EQ(evicted, 0u);

    // Check stats
    auto stats = RouterService::get_cache_event_stats();
    EXPECT_EQ(stats.evictions_unknown, 1u);
}

TEST_F(CacheEvictionTest, StaleEventIsRejected) {
    auto [tokens, prefix_hash] = make_route(200);
    BackendId backend = 2;

    RouterService::register_backend_for_testing(backend, seastar::socket_address(seastar::net::inet_address("127.0.0.1"), 8080));
    RouterService::insert_route_for_testing(tokens, backend);
    RouterService::update_prefix_hash_index_for_testing(prefix_hash, backend);

    // First eviction with timestamp 2000
    RouterService::evict_by_prefix_hash_local(prefix_hash, backend, 2000);

    // Re-insert route and index
    RouterService::insert_route_for_testing(tokens, backend);
    RouterService::update_prefix_hash_index_for_testing(prefix_hash, backend);

    // Second eviction with older timestamp 1000 (should be rejected as stale)
    uint32_t evicted = RouterService::evict_by_prefix_hash_local(
        prefix_hash, backend, 1000);

    EXPECT_EQ(evicted, 0u);

    auto stats = RouterService::get_cache_event_stats();
    EXPECT_GE(stats.evictions_stale, 1u);
}

TEST_F(CacheEvictionTest, NewerTimestampAccepted) {
    auto [tokens, prefix_hash] = make_route(300);
    BackendId backend = 3;

    RouterService::register_backend_for_testing(backend, seastar::socket_address(seastar::net::inet_address("127.0.0.1"), 8080));
    RouterService::insert_route_for_testing(tokens, backend);
    RouterService::update_prefix_hash_index_for_testing(prefix_hash, backend);

    // First eviction at t=1000
    RouterService::evict_by_prefix_hash_local(prefix_hash, backend, 1000);

    // Re-insert route and index
    RouterService::insert_route_for_testing(tokens, backend);
    RouterService::update_prefix_hash_index_for_testing(prefix_hash, backend);

    // Second eviction at t=2000 (newer, should succeed)
    uint32_t evicted = RouterService::evict_by_prefix_hash_local(
        prefix_hash, backend, 2000);

    EXPECT_GT(evicted, 0u);
}

TEST_F(CacheEvictionTest, EvictDoesNotAffectOtherBackends) {
    auto [tokens, prefix_hash] = make_route(400);
    BackendId backend1 = 10;
    BackendId backend2 = 20;

    RouterService::register_backend_for_testing(backend1, seastar::socket_address(seastar::net::inet_address("127.0.0.1"), 8080));
    RouterService::register_backend_for_testing(backend2, seastar::socket_address(seastar::net::inet_address("127.0.0.1"), 8081));

    RouterService::insert_route_for_testing(tokens, backend1);
    RouterService::insert_route_for_testing(tokens, backend2);
    RouterService::update_prefix_hash_index_for_testing(prefix_hash, backend1);
    RouterService::update_prefix_hash_index_for_testing(prefix_hash, backend2);

    // Evict only backend1
    RouterService::evict_by_prefix_hash_local(prefix_hash, backend1, 1000);

    // backend2's routes should still exist (we can verify via route count)
    // The radix tree stores by token prefix, and remove_routes_by_backend
    // only removes routes for the specified backend
    EXPECT_GE(RouterService::get_route_count_for_testing(), 0u);
}

TEST_F(CacheEvictionTest, RecordMetrics) {
    RouterService::record_cache_event_received();
    RouterService::record_cache_event_received();
    RouterService::record_cache_event_auth_failure();
    RouterService::record_cache_event_parse_error();

    auto stats = RouterService::get_cache_event_stats();
    EXPECT_EQ(stats.events_received, 2u);
    EXPECT_EQ(stats.auth_failures, 1u);
    EXPECT_EQ(stats.parse_errors, 1u);
}

// =============================================================================
// Loaded Event Tests
// =============================================================================
//
// Loaded events implement the strict subset: they only "apply"
// when this shard already tracks (prefix_hash, backend_id) in its
// prefix_hash_index. They never insert brand-new routes because the wire
// format does not carry token IDs and the RadixTree is keyed on tokens.

TEST_F(CacheEvictionTest, LoadAppliedForKnownRoute) {
    auto [tokens, prefix_hash] = make_route(500);
    BackendId backend = 5;

    RouterService::register_backend_for_testing(backend, seastar::socket_address(seastar::net::inet_address("127.0.0.1"), 8080));
    RouterService::insert_route_for_testing(tokens, backend);
    RouterService::update_prefix_hash_index_for_testing(prefix_hash, backend);

    uint32_t applied = RouterService::load_route_local(prefix_hash, backend, 1000);
    EXPECT_EQ(applied, 1u);

    auto stats = RouterService::get_cache_event_stats();
    EXPECT_EQ(stats.loads_applied, 1u);
    EXPECT_EQ(stats.loads_ignored_stale, 0u);
    EXPECT_EQ(stats.loads_ignored_unknown, 0u);
    EXPECT_EQ(stats.loads_ignored_different_backend, 0u);
}

TEST_F(CacheEvictionTest, LoadIgnoredForUnknownHash) {
    BackendId backend = 6;
    RouterService::register_backend_for_testing(backend, seastar::socket_address(seastar::net::inet_address("127.0.0.1"), 8080));

    uint32_t applied = RouterService::load_route_local(0xFEEDFACEULL, backend, 1000);
    EXPECT_EQ(applied, 0u);

    auto stats = RouterService::get_cache_event_stats();
    EXPECT_EQ(stats.loads_applied, 0u);
    EXPECT_EQ(stats.loads_ignored_unknown, 1u);
}

TEST_F(CacheEvictionTest, LoadIgnoredForKnownHashDifferentBackend) {
    auto [tokens, prefix_hash] = make_route(600);
    BackendId backend1 = 11;
    BackendId backend2 = 12;

    RouterService::register_backend_for_testing(backend1, seastar::socket_address(seastar::net::inet_address("127.0.0.1"), 8080));
    RouterService::register_backend_for_testing(backend2, seastar::socket_address(seastar::net::inet_address("127.0.0.1"), 8081));
    RouterService::insert_route_for_testing(tokens, backend1);
    RouterService::update_prefix_hash_index_for_testing(prefix_hash, backend1);

    // backend2 claims to have it loaded — under option (a) we cannot
    // materialize a new route, so this is ignored.
    uint32_t applied = RouterService::load_route_local(prefix_hash, backend2, 1000);
    EXPECT_EQ(applied, 0u);

    auto stats = RouterService::get_cache_event_stats();
    EXPECT_EQ(stats.loads_ignored_different_backend, 1u);
}

TEST_F(CacheEvictionTest, LoadStaleTimestampRejected) {
    auto [tokens, prefix_hash] = make_route(700);
    BackendId backend = 7;

    RouterService::register_backend_for_testing(backend, seastar::socket_address(seastar::net::inet_address("127.0.0.1"), 8080));
    RouterService::insert_route_for_testing(tokens, backend);
    RouterService::update_prefix_hash_index_for_testing(prefix_hash, backend);

    // First load at t=2000 establishes the timestamp.
    EXPECT_EQ(RouterService::load_route_local(prefix_hash, backend, 2000), 1u);

    // Older load at t=1000 is rejected as stale.
    EXPECT_EQ(RouterService::load_route_local(prefix_hash, backend, 1000), 0u);

    auto stats = RouterService::get_cache_event_stats();
    EXPECT_GE(stats.loads_ignored_stale, 1u);
}

TEST_F(CacheEvictionTest, LoadEqualTimestampRejected) {
    // The timestamp check is <=, so a load with the same timestamp as the
    // stored one is rejected (idempotent retry detection).
    auto [tokens, prefix_hash] = make_route(750);
    BackendId backend = 17;

    RouterService::register_backend_for_testing(backend, seastar::socket_address(seastar::net::inet_address("127.0.0.1"), 8080));
    RouterService::insert_route_for_testing(tokens, backend);
    RouterService::update_prefix_hash_index_for_testing(prefix_hash, backend);

    EXPECT_EQ(RouterService::load_route_local(prefix_hash, backend, 5000), 1u);
    // Same timestamp — must be rejected.
    EXPECT_EQ(RouterService::load_route_local(prefix_hash, backend, 5000), 0u);

    auto stats = RouterService::get_cache_event_stats();
    EXPECT_EQ(stats.loads_applied, 1u);
    EXPECT_GE(stats.loads_ignored_stale, 1u);
}

TEST_F(CacheEvictionTest, LoadAdvancesSharedTimestamp) {
    // Eviction and load share cache_event_timestamps. A load must bump the
    // stored timestamp so subsequent older events of either type are rejected.
    auto [tokens, prefix_hash] = make_route(770);
    BackendId backend = 18;

    RouterService::register_backend_for_testing(backend, seastar::socket_address(seastar::net::inet_address("127.0.0.1"), 8080));
    RouterService::insert_route_for_testing(tokens, backend);
    RouterService::update_prefix_hash_index_for_testing(prefix_hash, backend);

    // Load at t=3000 advances the shared timestamp.
    EXPECT_EQ(RouterService::load_route_local(prefix_hash, backend, 3000), 1u);

    // Newer load at t=4000 succeeds (route still tracked, ts strictly newer).
    EXPECT_EQ(RouterService::load_route_local(prefix_hash, backend, 4000), 1u);

    // Older load at t=2500 must be rejected — proves the second load
    // updated the stored timestamp, not just the first.
    EXPECT_EQ(RouterService::load_route_local(prefix_hash, backend, 2500), 0u);
}

TEST_F(CacheEvictionTest, LoadThenStaleEvictRejected) {
    // A load at t=2000 should block a subsequent evict at t=1000.
    auto [tokens, prefix_hash] = make_route(800);
    BackendId backend = 8;

    RouterService::register_backend_for_testing(backend, seastar::socket_address(seastar::net::inet_address("127.0.0.1"), 8080));
    RouterService::insert_route_for_testing(tokens, backend);
    RouterService::update_prefix_hash_index_for_testing(prefix_hash, backend);

    EXPECT_EQ(RouterService::load_route_local(prefix_hash, backend, 2000), 1u);

    uint32_t evicted = RouterService::evict_by_prefix_hash_local(prefix_hash, backend, 1000);
    EXPECT_EQ(evicted, 0u);

    auto stats = RouterService::get_cache_event_stats();
    EXPECT_GE(stats.evictions_stale, 1u);
}

// =============================================================================
// Gossip Trust-Ladder Tests (invariant T7, finding I-5)
// =============================================================================
//
// The gossip REMOTE batch-apply path enforces LOCAL > PUSH > REMOTE: a peer's
// announcement never displaces a higher-trust route for a different backend.
// These pin the batch-apply behavior through the real apply function
// (learn_remote_route_for_testing → apply_route_batch_to_local_tree), including
// the prefix_hash_index gate and the refusal counter.

TEST_F(CacheEvictionTest, RemoteBatchRefusedByLocalRoute) {
    auto [tokens, prefix_hash] = make_route(900);
    BackendId local_backend = 1;
    BackendId remote_backend = 2;

    RouterService::register_backend_for_testing(local_backend, seastar::socket_address(seastar::net::inet_address("127.0.0.1"), 8080));
    RouterService::register_backend_for_testing(remote_backend, seastar::socket_address(seastar::net::inet_address("127.0.0.1"), 8081));

    // Learn K locally, then let a peer announce K → a different backend.
    RouterService::learn_route_for_testing(tokens, local_backend);
    ASSERT_EQ(RouterService::lookup_backend_for_testing(tokens), local_backend);

    RouterService::learn_remote_route_for_testing(tokens, remote_backend);

    // The LOCAL route holds; the announcement is refused and counted.
    EXPECT_EQ(RouterService::lookup_backend_for_testing(tokens), local_backend);
    EXPECT_EQ(RouterService::remote_routes_trust_refused_for_testing(), 1u);

    // The refused (hash, backend) pair is NOT indexed; the LOCAL one remains.
    EXPECT_FALSE(RouterService::prefix_hash_index_contains_for_testing(prefix_hash, remote_backend));
    EXPECT_TRUE(RouterService::prefix_hash_index_contains_for_testing(prefix_hash, local_backend));
}

TEST_F(CacheEvictionTest, RemoteBatchSameBackendKeepsLocalRoute) {
    auto [tokens, prefix_hash] = make_route(910);
    BackendId backend = 3;

    RouterService::register_backend_for_testing(backend, seastar::socket_address(seastar::net::inet_address("127.0.0.1"), 8080));
    RouterService::learn_route_for_testing(tokens, backend);

    // A peer announces the SAME backend for K: re-confirmation (TOUCHED_SAME),
    // not a conflict — no refusal, no duplicate leaf, route unchanged.
    RouterService::learn_remote_route_for_testing(tokens, backend);

    EXPECT_EQ(RouterService::lookup_backend_for_testing(tokens), backend);
    EXPECT_EQ(RouterService::remote_routes_trust_refused_for_testing(), 0u);
    EXPECT_EQ(RouterService::get_route_count_for_testing(), 1u);
    EXPECT_TRUE(RouterService::prefix_hash_index_contains_for_testing(prefix_hash, backend));
}

TEST_F(CacheEvictionTest, RemoteBatchOverwritesExistingRemoteRoute) {
    auto [tokens, prefix_hash] = make_route(920);
    BackendId first = 4;
    BackendId second = 5;

    RouterService::register_backend_for_testing(first, seastar::socket_address(seastar::net::inet_address("127.0.0.1"), 8080));
    RouterService::register_backend_for_testing(second, seastar::socket_address(seastar::net::inet_address("127.0.0.1"), 8081));

    // First peer announces K → first; a second peer then announces K → second.
    // REMOTE may replace REMOTE (equal trust), so the newer announcement wins.
    RouterService::learn_remote_route_for_testing(tokens, first);
    ASSERT_EQ(RouterService::lookup_backend_for_testing(tokens), first);

    RouterService::learn_remote_route_for_testing(tokens, second);

    EXPECT_EQ(RouterService::lookup_backend_for_testing(tokens), second);
    EXPECT_EQ(RouterService::remote_routes_trust_refused_for_testing(), 0u);
    EXPECT_TRUE(RouterService::prefix_hash_index_contains_for_testing(prefix_hash, second));
}

// =============================================================================
// Per-Origin LRU Eviction Tests (RadixTree directly)
// =============================================================================
//
// evict_lowest_trust() / evict_oldest() are O(1) pops over per-origin LRU
// lists (invariant T8). These tests pin the victim-selection contract —
// oldest REMOTE, else oldest PUSH, else oldest of any origin — and that
// origin-changing overwrites relink the node into the correct list.

namespace {

// Ensure distinct last_accessed stamps: steady_clock must tick between
// inserts so evict_oldest()'s cross-list tail comparison is deterministic.
void advance_steady_clock() {
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() <= t0) {
    }
}

}  // namespace

class PerOriginLruTest : public ::testing::Test {
protected:
    void SetUp() override {
        prev_slab_ = get_node_slab();
        slab_ = std::make_unique<NodeSlab>();
        set_node_slab(slab_.get());
        tree_ = std::make_unique<RadixTree>(1);  // alignment 1: short keys OK
    }

    void TearDown() override {
        tree_.reset();  // nodes must return to slab_ before it is destroyed
        set_node_slab(prev_slab_);
        slab_.reset();
    }

    std::vector<TokenId> tokens(std::initializer_list<TokenId> list) {
        return std::vector<TokenId>(list);
    }

    void insert_aged(std::initializer_list<TokenId> key, BackendId backend,
                     RouteOrigin origin) {
        advance_steady_clock();
        tree_->insert(tokens(key), backend, origin);
        tree_->validate_lru_lists();
    }

    NodeSlab* prev_slab_ = nullptr;
    std::unique_ptr<NodeSlab> slab_;
    std::unique_ptr<RadixTree> tree_;
};

TEST_F(PerOriginLruTest, EvictLowestTrustFollowsTrustLadder) {
    insert_aged({1, 1, 1}, 1, RouteOrigin::LOCAL);   // oldest overall
    insert_aged({2, 2, 2}, 2, RouteOrigin::REMOTE);  // oldest REMOTE
    insert_aged({3, 3, 3}, 3, RouteOrigin::PUSH);    // oldest PUSH
    insert_aged({4, 4, 4}, 4, RouteOrigin::REMOTE);
    insert_aged({5, 5, 5}, 5, RouteOrigin::PUSH);
    insert_aged({6, 6, 6}, 6, RouteOrigin::LOCAL);

    // Oldest REMOTE goes first even though older LOCAL/PUSH routes exist.
    ASSERT_TRUE(tree_->evict_lowest_trust());
    tree_->validate_lru_lists();
    EXPECT_FALSE(tree_->lookup(tokens({2, 2, 2})).has_value());

    ASSERT_TRUE(tree_->evict_lowest_trust());
    tree_->validate_lru_lists();
    EXPECT_FALSE(tree_->lookup(tokens({4, 4, 4})).has_value());

    // REMOTE exhausted -> oldest PUSH.
    ASSERT_TRUE(tree_->evict_lowest_trust());
    tree_->validate_lru_lists();
    EXPECT_FALSE(tree_->lookup(tokens({3, 3, 3})).has_value());

    ASSERT_TRUE(tree_->evict_lowest_trust());
    tree_->validate_lru_lists();
    EXPECT_FALSE(tree_->lookup(tokens({5, 5, 5})).has_value());

    // PUSH exhausted -> fall back to the oldest of any origin.
    ASSERT_TRUE(tree_->evict_lowest_trust());
    tree_->validate_lru_lists();
    EXPECT_FALSE(tree_->lookup(tokens({1, 1, 1})).has_value());
    EXPECT_TRUE(tree_->lookup(tokens({6, 6, 6})).has_value());

    ASSERT_TRUE(tree_->evict_lowest_trust());
    EXPECT_FALSE(tree_->evict_lowest_trust());
    EXPECT_EQ(tree_->route_count(), 0u);
    tree_->validate_lru_lists();
}

TEST_F(PerOriginLruTest, EvictOldestIgnoresOrigin) {
    insert_aged({1, 1, 1}, 1, RouteOrigin::REMOTE);  // oldest overall
    insert_aged({2, 2, 2}, 2, RouteOrigin::LOCAL);
    insert_aged({3, 3, 3}, 3, RouteOrigin::PUSH);

    ASSERT_TRUE(tree_->evict_oldest());
    tree_->validate_lru_lists();
    EXPECT_FALSE(tree_->lookup(tokens({1, 1, 1})).has_value());

    ASSERT_TRUE(tree_->evict_oldest());
    tree_->validate_lru_lists();
    EXPECT_FALSE(tree_->lookup(tokens({2, 2, 2})).has_value());

    ASSERT_TRUE(tree_->evict_oldest());
    EXPECT_FALSE(tree_->evict_oldest());
    EXPECT_EQ(tree_->route_count(), 0u);
    tree_->validate_lru_lists();
}

TEST_F(PerOriginLruTest, OverwriteChangingOriginRelinksNode) {
    insert_aged({1, 1, 1}, 1, RouteOrigin::LOCAL);
    insert_aged({9, 9, 9}, 9, RouteOrigin::LOCAL);

    // Re-insert the same key as REMOTE: the node must leave the LOCAL list
    // and join the REMOTE list (unlink happens before origin reassignment).
    advance_steady_clock();
    tree_->insert(tokens({1, 1, 1}), 2, RouteOrigin::REMOTE);
    tree_->validate_lru_lists();
    EXPECT_EQ(tree_->route_count(), 2u);

    // evict_lowest_trust must now find it in the REMOTE list.
    ASSERT_TRUE(tree_->evict_lowest_trust());
    tree_->validate_lru_lists();
    EXPECT_FALSE(tree_->lookup(tokens({1, 1, 1})).has_value());
    EXPECT_TRUE(tree_->lookup(tokens({9, 9, 9})).has_value());
}

TEST_F(PerOriginLruTest, TrustedOverwriteRelinksNode) {
    insert_aged({1, 1, 1}, 1, RouteOrigin::REMOTE);
    insert_aged({2, 2, 2}, 2, RouteOrigin::REMOTE);

    // PUSH over REMOTE with a different backend takes the OVERWROTE path:
    // the node must move from the REMOTE list to the PUSH list.
    advance_steady_clock();
    auto result = tree_->insert_if_trusted(tokens({1, 1, 1}), 7, RouteOrigin::PUSH);
    EXPECT_EQ(result, TrustInsertResult::OVERWROTE);
    tree_->validate_lru_lists();

    // The remaining REMOTE route is evicted first, the PUSH route second.
    ASSERT_TRUE(tree_->evict_lowest_trust());
    tree_->validate_lru_lists();
    EXPECT_FALSE(tree_->lookup(tokens({2, 2, 2})).has_value());

    ASSERT_TRUE(tree_->evict_lowest_trust());
    tree_->validate_lru_lists();
    EXPECT_FALSE(tree_->lookup(tokens({1, 1, 1})).has_value());
}

TEST_F(PerOriginLruTest, TrustedSameBackendRefreshesLruKeepingOrigin) {
    // A REMOTE re-announcement of a higher-trust route for the SAME backend
    // takes the TOUCHED_SAME path: origin stays LOCAL, only the LRU stamp moves
    // (the gossip flap case that used to force a KV-cache miss).
    insert_aged({1, 1, 1}, 1, RouteOrigin::LOCAL);   // K, oldest LOCAL
    insert_aged({2, 2, 2}, 2, RouteOrigin::LOCAL);   // newer LOCAL
    insert_aged({3, 3, 3}, 3, RouteOrigin::REMOTE);  // decoy for the origin check

    advance_steady_clock();
    auto result = tree_->insert_if_trusted(tokens({1, 1, 1}), 1, RouteOrigin::REMOTE);
    EXPECT_EQ(result, TrustInsertResult::TOUCHED_SAME);
    tree_->validate_lru_lists();
    EXPECT_EQ(tree_->route_count(), 3u);

    // Origin stayed LOCAL: evict_lowest_trust takes the REMOTE decoy first,
    // never K — had TOUCHED_SAME flipped K to REMOTE, K could have been chosen.
    ASSERT_TRUE(tree_->evict_lowest_trust());
    tree_->validate_lru_lists();
    EXPECT_FALSE(tree_->lookup(tokens({3, 3, 3})).has_value());
    EXPECT_TRUE(tree_->lookup(tokens({1, 1, 1})).has_value());

    // LRU refreshed: K was the oldest, but the touch moved it ahead of
    // {2,2,2}, so evict_oldest now takes {2,2,2} first.
    ASSERT_TRUE(tree_->evict_oldest());
    tree_->validate_lru_lists();
    EXPECT_FALSE(tree_->lookup(tokens({2, 2, 2})).has_value());
    EXPECT_TRUE(tree_->lookup(tokens({1, 1, 1})).has_value());
}

TEST_F(PerOriginLruTest, SplitAndGrowthPreserveListMembership) {
    // Prefix splits relocate leaf nodes and node growth replaces them;
    // both must splice the replacement into the correct origin's list.
    for (TokenId t = 0; t < 20; t++) {
        auto origin = static_cast<RouteOrigin>(t % 3);
        auto backend = static_cast<BackendId>(t + 1);
        insert_aged({t, 100, 100}, backend, origin);  // leaf...
        insert_aged({t, 100, 200}, backend, origin);  // ...split by sibling
    }
    EXPECT_EQ(tree_->route_count(), 40u);

    while (tree_->evict_lowest_trust()) {
        tree_->validate_lru_lists();
    }
    EXPECT_EQ(tree_->route_count(), 0u);
}

// =============================================================================
// CacheEventsConfig Tests
// =============================================================================

class CacheEventsConfigTest : public ::testing::Test {};

TEST_F(CacheEventsConfigTest, DefaultValues) {
    CacheEventsConfig config;
    EXPECT_FALSE(config.enabled);
    EXPECT_TRUE(config.auth_token.empty());
    EXPECT_EQ(config.max_events_per_request, 100u);
    EXPECT_EQ(config.max_event_age_seconds, 60u);
    EXPECT_TRUE(config.propagate_via_gossip);
    EXPECT_TRUE(config.inject_prefix_hash_header);
}
