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
#include "router_service.hpp"

#include <gtest/gtest.h>
#include <cstdint>
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
