// Ranvier Core - prefix_hash_index Lifecycle Tests
//
// Pins invariants R1 and R2 (.dev-context/invariants.md):
// - R1: index membership ⊆ live routing state. The TTL-cycle rebuild
//   (ttl_cleanup_on_shard phase 3) drops entries whose backing route was
//   removed, while entries owned by a fresh native KV stream survive and
//   the per-backend native entry counters are recomputed.
// - R2: the learn paths insert index entries hashed at the route's effective
//   boundary (tokens.size()), the same depth routing and push evictions
//   hash at — not re-capped at prefix_token_length.
//
// Reactor-free: follows the cache_eviction_test fixture pattern, driving
// RouterService's shard-local state through the *_for_testing seams.

#include "kv_event_ledger.hpp"
#include "parse_utils.hpp"
#include "router_service.hpp"

#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <vector>

using namespace ranvier;

class PrefixHashIndexLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        RoutingConfig cfg;
        cfg.max_routes = 10000;
        cfg.prefix_token_length = 128;
        cfg.block_alignment = 16;
        RouterService::reset_shard_state_for_testing(&cfg);
    }

    void TearDown() override {
        RouterService::reset_shard_state_for_testing(nullptr);
    }

    static std::vector<int32_t> make_tokens(int32_t base, size_t len) {
        std::vector<int32_t> tokens(len);
        for (size_t i = 0; i < len; i++) {
            tokens[i] = base + static_cast<int32_t>(i);
        }
        return tokens;
    }

    static seastar::socket_address make_addr(uint16_t port) {
        return seastar::socket_address(seastar::net::inet_address("127.0.0.1"), port);
    }

    static std::chrono::steady_clock::time_point expire_all_cutoff() {
        return std::chrono::steady_clock::now() + std::chrono::seconds(60);
    }

    static std::chrono::steady_clock::time_point expire_none_cutoff() {
        return std::chrono::steady_clock::now() - std::chrono::seconds(60);
    }
};

// [R1] A learned route's index entry does not outlive the route: once TTL
// expiry removes the route, the rebuild drops the (hash, backend) entry.
TEST_F(PrefixHashIndexLifecycleTest, TtlExpiryPrunesLearnedIndexEntry) {
    auto tokens = make_tokens(100, 16);
    uint64_t prefix_hash = hash_prefix(tokens.data(), tokens.size(), 16);
    BackendId backend = 1;

    RouterService::register_backend_for_testing(backend, make_addr(8080));
    RouterService::learn_route_for_testing(tokens, backend);

    EXPECT_EQ(RouterService::get_route_count_for_testing(), 1u);
    EXPECT_TRUE(RouterService::prefix_hash_index_contains_for_testing(prefix_hash, backend));

    size_t removed = RouterService::ttl_cleanup_phases_for_testing(expire_all_cutoff());

    EXPECT_EQ(removed, 1u);
    EXPECT_EQ(RouterService::get_route_count_for_testing(), 0u);
    EXPECT_FALSE(RouterService::prefix_hash_index_contains_for_testing(prefix_hash, backend));
    EXPECT_EQ(RouterService::prefix_hash_index_size_for_testing(), 0u);
}

// [R1] The rebuild keeps entries whose routes are still live.
TEST_F(PrefixHashIndexLifecycleTest, RebuildKeepsLiveRouteEntries) {
    auto tokens_a = make_tokens(200, 16);
    auto tokens_b = make_tokens(300, 32);
    uint64_t hash_a = hash_prefix(tokens_a.data(), tokens_a.size(), 16);
    uint64_t hash_b = hash_prefix(tokens_b.data(), tokens_b.size(), 16);

    RouterService::register_backend_for_testing(1, make_addr(8080));
    RouterService::register_backend_for_testing(2, make_addr(8081));
    RouterService::learn_route_for_testing(tokens_a, 1);
    RouterService::learn_route_for_testing(tokens_b, 2);

    size_t removed = RouterService::ttl_cleanup_phases_for_testing(expire_none_cutoff());

    EXPECT_EQ(removed, 0u);
    EXPECT_EQ(RouterService::get_route_count_for_testing(), 2u);
    EXPECT_TRUE(RouterService::prefix_hash_index_contains_for_testing(hash_a, 1));
    EXPECT_TRUE(RouterService::prefix_hash_index_contains_for_testing(hash_b, 2));
    EXPECT_EQ(RouterService::prefix_hash_index_size_for_testing(), 2u);
}

// [R2] A route learned at a prefix_boundary beyond prefix_token_length must
// be indexed at the boundary depth, so a push eviction hashed at that depth
// applies instead of counting evictions_unknown. (Learn paths receive tokens
// already truncated to the effective boundary by learn_route_global.)
TEST_F(PrefixHashIndexLifecycleTest, BoundaryDepthLocalLearnEntryMatchesEviction) {
    auto tokens = make_tokens(500, 192);  // prefix_boundary=192 > prefix_token_length=128
    uint64_t boundary_hash = hash_prefix(tokens.data(), 192, 16);
    BackendId backend = 2;

    RouterService::register_backend_for_testing(backend, make_addr(8080));
    RouterService::learn_route_for_testing(tokens, backend);

    uint32_t evicted = RouterService::evict_by_prefix_hash_local(boundary_hash, backend, 1000);

    EXPECT_GT(evicted, 0u);
    auto stats = RouterService::get_cache_event_stats();
    EXPECT_EQ(stats.evictions_applied, 1u);
    EXPECT_EQ(stats.evictions_unknown, 0u);
    EXPECT_FALSE(RouterService::prefix_hash_index_contains_for_testing(boundary_hash, backend));
}

// [R2] Same contract on the gossip (remote) apply path.
TEST_F(PrefixHashIndexLifecycleTest, BoundaryDepthRemoteLearnEntryMatchesEviction) {
    auto tokens = make_tokens(700, 192);
    uint64_t boundary_hash = hash_prefix(tokens.data(), 192, 16);
    BackendId backend = 3;

    RouterService::register_backend_for_testing(backend, make_addr(8080));
    RouterService::learn_remote_route_for_testing(tokens, backend);

    uint32_t evicted = RouterService::evict_by_prefix_hash_local(boundary_hash, backend, 1000);

    EXPECT_GT(evicted, 0u);
    auto stats = RouterService::get_cache_event_stats();
    EXPECT_EQ(stats.evictions_applied, 1u);
    EXPECT_EQ(stats.evictions_unknown, 0u);
}

// [R1] Entries owned by a fresh native KV stream survive the rebuild (their
// lifecycle belongs to UPSERT/REMOVE/CLEAR/RESET), and the native entry
// counter reflects the preserved entries. Orphan entries of backends without
// native freshness are dropped.
TEST_F(PrefixHashIndexLifecycleTest, NativeFreshEntriesSurviveRebuild) {
    BackendId fresh_backend = 4;
    BackendId stale_backend = 5;
    RouterService::register_backend_for_testing(fresh_backend, make_addr(8080));
    RouterService::register_backend_for_testing(stale_backend, make_addr(8081));

    // Real native op path: creates the index entry, counts it, marks fresh.
    NativeKvOp op;
    op.kind = NativeKvOp::Kind::UPSERT;
    op.backend = fresh_backend;
    op.prefix_hash = 0xAAULL;
    op.ts_ms = 1000;
    RouterService::apply_native_kv_ops_local(&op, 1);
    EXPECT_EQ(RouterService::native_index_entries_for_testing(fresh_backend), 1u);

    // Orphan entry: no backing route, no native freshness.
    RouterService::update_prefix_hash_index_for_testing(0xBBULL, stale_backend);

    RouterService::ttl_cleanup_phases_for_testing(expire_all_cutoff());

    EXPECT_TRUE(RouterService::prefix_hash_index_contains_for_testing(0xAAULL, fresh_backend));
    EXPECT_FALSE(RouterService::prefix_hash_index_contains_for_testing(0xBBULL, stale_backend));
    EXPECT_EQ(RouterService::native_index_entries_for_testing(fresh_backend), 1u);
    EXPECT_EQ(RouterService::native_index_entries_for_testing(stale_backend), 0u);
}

// [R1] With verified residency disabled (freshness TTL 0), nothing counts as
// fresh: native entries are dropped by the rebuild and the per-backend
// counter is erased, so a resuming stream starts from a clean slate instead
// of tripping MAX_ENTRIES_PER_BACKEND early.
TEST_F(PrefixHashIndexLifecycleTest, FreshnessDisabledDropsNativeEntriesAndCounter) {
    RoutingConfig cfg;
    cfg.max_routes = 10000;
    cfg.prefix_token_length = 128;
    cfg.block_alignment = 16;
    cfg.kv_residency_freshness_ttl = std::chrono::seconds(0);
    RouterService::reset_shard_state_for_testing(&cfg);

    BackendId backend = 6;
    RouterService::register_backend_for_testing(backend, make_addr(8080));

    NativeKvOp op;
    op.kind = NativeKvOp::Kind::UPSERT;
    op.backend = backend;
    op.prefix_hash = 0xCCULL;
    op.ts_ms = 1000;
    RouterService::apply_native_kv_ops_local(&op, 1);
    EXPECT_EQ(RouterService::native_index_entries_for_testing(backend), 1u);

    RouterService::ttl_cleanup_phases_for_testing(expire_all_cutoff());

    EXPECT_FALSE(RouterService::prefix_hash_index_contains_for_testing(0xCCULL, backend));
    EXPECT_EQ(RouterService::native_index_entries_for_testing(backend), 0u);
}

// [R1] Learn/expire churn keeps the index flat — the observable behind the
// router_prefix_hash_index_size gauge acceptance criterion.
TEST_F(PrefixHashIndexLifecycleTest, LearnExpireChurnKeepsIndexFlat) {
    BackendId backend = 7;
    RouterService::register_backend_for_testing(backend, make_addr(8080));

    for (int i = 0; i < 100; i++) {
        RouterService::learn_route_for_testing(make_tokens(1000 + i * 16, 16), backend);
        EXPECT_EQ(RouterService::prefix_hash_index_size_for_testing(), 1u);
        RouterService::ttl_cleanup_phases_for_testing(expire_all_cutoff());
        EXPECT_EQ(RouterService::prefix_hash_index_size_for_testing(), 0u);
    }
    EXPECT_EQ(RouterService::get_route_count_for_testing(), 0u);
}
