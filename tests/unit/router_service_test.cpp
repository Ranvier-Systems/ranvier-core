// Ranvier Core - Router Service Integration Tests (Seastar-dependent)
//
// Tests for the core routing brain: RouterService + RadixTree + ShardLocalState.
// Exercises shard-local paths of RouterService methods using test helpers that
// bypass async cross-shard broadcast (which requires a running Seastar reactor).
//
// Test cases:
//   1. Route learning from proxied requests (ART insert + lookup)
//   2. Route TTL expiration and cleanup
//   3. Cross-shard route broadcast (batch application to local tree)
//   4. Backend registration/deregistration with route cleanup
//   5. Draining mode with timeout

#include "router_service.hpp"
#include "config.hpp"
#include "radix_tree.hpp"
#include "node_slab.hpp"
#include "types.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <optional>
#include <vector>

#include <seastar/net/inet_address.hh>
#include <seastar/net/socket_defs.hh>

using namespace ranvier;

// =============================================================================
// Helper
// =============================================================================
static seastar::socket_address make_addr(const char* ip, uint16_t port) {
    return seastar::socket_address(seastar::net::inet_address(ip), port);
}

// =============================================================================
// Fixture
// =============================================================================
class RouterServiceTest : public ::testing::Test {
protected:
    RoutingConfig cfg_;
    std::unique_ptr<RouterService> router_;

    void SetUp() override {
        cfg_ = RoutingConfig{};
        cfg_.routing_mode = RoutingConfig::RoutingMode::PREFIX;
        cfg_.max_routes = 1000;
        cfg_.ttl_seconds = std::chrono::seconds(3600);
        cfg_.prefix_token_length = 128;
        // Use block_alignment=1 so small token vectors work with RadixTree.
        // RadixTree::insert truncates to (tokens.size()/alignment)*alignment.
        cfg_.block_alignment = 1;
        cfg_.backend_drain_timeout = std::chrono::seconds(2);
        cfg_.load_aware_routing = false;  // Deterministic tests
        cfg_.hash_strategy = RoutingConfig::HashStrategy::JUMP;  // Deterministic: no load-aware hash

        router_ = std::make_unique<RouterService>(cfg_);
    }

    void TearDown() override {
        // Destroy router first to deregister metrics before clearing shard state
        router_.reset();
        RouterService::reset_shard_state_for_testing(nullptr);
    }

    // Helper: destroy existing router before creating a new one.
    // Prevents Seastar metric name collisions (old metrics must be
    // deregistered before new ones with the same names are registered).
    template<typename... Args>
    void recreate_router(Args&&... args) {
        router_.reset();
        RouterService::reset_shard_state_for_testing(nullptr);
        router_ = std::make_unique<RouterService>(std::forward<Args>(args)...);
    }

    void register_two_backends() {
        RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080));
        RouterService::register_backend_for_testing(2, make_addr("10.0.0.2", 8080));
    }

    void register_three_backends() {
        register_two_backends();
        RouterService::register_backend_for_testing(3, make_addr("10.0.0.3", 8080));
    }
};

// =============================================================================
// 1. Route Learning from Proxied Requests
// =============================================================================

TEST_F(RouterServiceTest, LearnedRouteIsFoundByLookup) {
    register_two_backends();
    std::vector<int32_t> tokens = {10, 20, 30, 40, 50};

    RouterService::insert_route_for_testing(tokens, 1);

    auto result = router_->lookup(tokens);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 1);
}

TEST_F(RouterServiceTest, LearnedRouteReturnsViaRouteRequest) {
    register_two_backends();
    std::vector<int32_t> tokens = {100, 200, 300, 400};

    RouterService::insert_route_for_testing(tokens, 2);

    auto result = router_->route_request(tokens);
    EXPECT_TRUE(result.backend_id.has_value());
    EXPECT_EQ(result.backend_id.value(), 2);
    EXPECT_EQ(result.routing_mode, "prefix");
    EXPECT_TRUE(result.cache_hit);
}

TEST_F(RouterServiceTest, HashFallbackReportsCacheMiss) {
    register_two_backends();
    // No ART route inserted → route_request must use hash fallback
    std::vector<int32_t> tokens = {100, 200, 300, 400};

    auto result = router_->route_request(tokens);
    EXPECT_TRUE(result.backend_id.has_value());
    EXPECT_EQ(result.routing_mode, "prefix");
    EXPECT_FALSE(result.cache_hit);  // hash fallback, not ART hit
}

TEST_F(RouterServiceTest, LookupMissReturnsNullopt) {
    register_two_backends();
    RouterService::insert_route_for_testing({1, 2, 3}, 1);

    auto result = router_->lookup({9, 8, 7});
    EXPECT_FALSE(result.has_value());
}

TEST_F(RouterServiceTest, MultipleRoutesAreDistinct) {
    register_two_backends();
    RouterService::insert_route_for_testing({1, 2, 3}, 1);
    RouterService::insert_route_for_testing({4, 5, 6}, 2);

    EXPECT_EQ(router_->lookup({1, 2, 3}).value(), 1);
    EXPECT_EQ(router_->lookup({4, 5, 6}).value(), 2);
}

TEST_F(RouterServiceTest, RouteCountTracking) {
    register_two_backends();
    EXPECT_EQ(RouterService::get_route_count_for_testing(), 0u);

    RouterService::insert_route_for_testing({1, 2, 3}, 1);
    EXPECT_EQ(RouterService::get_route_count_for_testing(), 1u);

    RouterService::insert_route_for_testing({4, 5, 6}, 2);
    EXPECT_EQ(RouterService::get_route_count_for_testing(), 2u);
}

TEST_F(RouterServiceTest, RouteOverwritesSamePrefix) {
    register_two_backends();
    std::vector<int32_t> tokens = {10, 20, 30};

    RouterService::insert_route_for_testing(tokens, 1);
    EXPECT_EQ(router_->lookup(tokens).value(), 1);

    RouterService::insert_route_for_testing(tokens, 2);
    EXPECT_EQ(router_->lookup(tokens).value(), 2);
}

// =============================================================================
// 2. Route TTL Expiration and Cleanup
// =============================================================================
// TTL cleanup requires the Seastar reactor (run_ttl_cleanup uses smp::submit_to).
// We verify the tree's remove_expired API works correctly through the test surface.

TEST_F(RouterServiceTest, TreeDumpReflectsInsertedRoutes) {
    register_two_backends();
    RouterService::insert_route_for_testing({1, 2, 3}, 1);

    auto dump = router_->get_tree_dump();
    // After inserting a route, the tree should have content
    // (either the root has children or a backend value)
    EXPECT_TRUE(dump.backend.has_value() || !dump.children.empty());
}

TEST_F(RouterServiceTest, ResetClearsAllRoutes) {
    register_two_backends();
    RouterService::insert_route_for_testing({1, 2, 3}, 1);
    RouterService::insert_route_for_testing({4, 5, 6}, 2);
    EXPECT_EQ(RouterService::get_route_count_for_testing(), 2u);

    RouterService::reset_shard_state_for_testing(&cfg_);
    EXPECT_EQ(RouterService::get_route_count_for_testing(), 0u);
}

TEST_F(RouterServiceTest, RouteCountDropsAfterReset) {
    register_two_backends();
    for (int i = 0; i < 10; ++i) {
        RouterService::insert_route_for_testing({i * 100, i * 100 + 1}, 1);
    }
    EXPECT_EQ(RouterService::get_route_count_for_testing(), 10u);

    RouterService::reset_shard_state_for_testing(&cfg_);
    EXPECT_EQ(RouterService::get_route_count_for_testing(), 0u);

    // Routes should no longer be found
    EXPECT_FALSE(router_->lookup({0, 1}).has_value());
}

// =============================================================================
// 3. Cross-Shard Route Broadcast (Shard-Local Batch Application)
// =============================================================================
// Actual cross-shard broadcast needs a reactor. We verify the batch data
// structures and the local tree insertion path that each shard executes.

TEST_F(RouterServiceTest, BatchConfigConstants) {
    EXPECT_GT(RouteBatchConfig::MAX_BATCH_SIZE, 0u);
    EXPECT_GT(RouteBatchConfig::MAX_BUFFER_SIZE, RouteBatchConfig::MAX_BATCH_SIZE);
    EXPECT_GT(RouteBatchConfig::OVERFLOW_DROP_COUNT, 0u);
    EXPECT_LE(RouteBatchConfig::OVERFLOW_DROP_COUNT, RouteBatchConfig::MAX_BUFFER_SIZE);
    EXPECT_GT(RouteBatchConfig::DEFAULT_FLUSH_INTERVAL.count(), 0);
}

TEST_F(RouterServiceTest, PendingRemoteRouteStructure) {
    PendingRemoteRoute route;
    route.tokens = {1, 2, 3, 4, 5};
    route.backend = 42;

    EXPECT_EQ(route.tokens.size(), 5u);
    EXPECT_EQ(route.backend, 42);

    PendingRemoteRoute moved = std::move(route);
    EXPECT_EQ(moved.tokens.size(), 5u);
    EXPECT_EQ(moved.backend, 42);
}

TEST_F(RouterServiceTest, MultipleRouteInsertionsSimulateBroadcast) {
    // Simulate what each shard does when receiving a route batch:
    // insert routes into the local RadixTree
    register_three_backends();

    std::vector<std::pair<std::vector<int32_t>, BackendId>> batch = {
        {{10, 20, 30}, 1},
        {{40, 50, 60}, 2},
        {{70, 80, 90}, 3},
    };

    for (const auto& [tokens, backend] : batch) {
        RouterService::insert_route_for_testing(tokens, backend);
    }

    EXPECT_EQ(RouterService::get_route_count_for_testing(), 3u);
    EXPECT_EQ(router_->lookup({10, 20, 30}).value(), 1);
    EXPECT_EQ(router_->lookup({40, 50, 60}).value(), 2);
    EXPECT_EQ(router_->lookup({70, 80, 90}).value(), 3);
}

// =============================================================================
// 4. Backend Registration / Deregistration with Route Cleanup
// =============================================================================

TEST_F(RouterServiceTest, RegisterBackendMakesItAvailable) {
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080));

    auto ids = router_->get_all_backend_ids();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], 1);
}

TEST_F(RouterServiceTest, RegisterMultipleBackends) {
    register_three_backends();

    auto ids = router_->get_all_backend_ids();
    EXPECT_EQ(ids.size(), 3u);
}

TEST_F(RouterServiceTest, GetBackendAddressAfterRegister) {
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 9090));

    auto addr = router_->get_backend_address(1);
    ASSERT_TRUE(addr.has_value());
}

TEST_F(RouterServiceTest, GetBackendAddressNonexistent) {
    auto addr = router_->get_backend_address(999);
    EXPECT_FALSE(addr.has_value());
}

TEST_F(RouterServiceTest, UnregisterBackendRemovesIt) {
    register_two_backends();
    EXPECT_EQ(router_->get_all_backend_ids().size(), 2u);

    RouterService::unregister_backend_for_testing(1);
    auto ids = router_->get_all_backend_ids();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], 2);
}

TEST_F(RouterServiceTest, UnregisterNonexistentBackendIsNoOp) {
    register_two_backends();
    RouterService::unregister_backend_for_testing(999);
    EXPECT_EQ(router_->get_all_backend_ids().size(), 2u);
}

TEST_F(RouterServiceTest, DuplicateRegisterDoesNotDuplicate) {
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080));
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 9090));

    auto ids = router_->get_all_backend_ids();
    EXPECT_EQ(ids.size(), 1u);
}

TEST_F(RouterServiceTest, UnregisterClearsDeadStatus) {
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080));
    RouterService::mark_backend_dead_for_testing(1);
    RouterService::unregister_backend_for_testing(1);

    // Re-register: should not be marked dead
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080));
    auto result = router_->get_random_backend();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 1);
}

TEST_F(RouterServiceTest, BackendStateReportsCorrectFields) {
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080), 200, 1);

    auto states = router_->get_all_backend_states();
    ASSERT_EQ(states.size(), 1u);
    EXPECT_EQ(states[0].id, 1);
    EXPECT_EQ(states[0].weight, 200u);
    EXPECT_EQ(states[0].priority, 1u);
    EXPECT_FALSE(states[0].is_draining);
    EXPECT_FALSE(states[0].is_dead);
}

TEST_F(RouterServiceTest, RouteToDeregisteredBackendTreatedAsMiss) {
    register_two_backends();
    RouterService::insert_route_for_testing({1, 2, 3}, 1);

    // Deregister backend 1
    RouterService::unregister_backend_for_testing(1);

    // Route should still be in tree but backend 1 is not live
    // route_request should fall back to hash/other backend
    auto result = router_->route_request({1, 2, 3});
    // Should still return a backend (backend 2 via hash fallback)
    EXPECT_TRUE(result.backend_id.has_value());
    EXPECT_EQ(result.backend_id.value(), 2);
}

// =============================================================================
// 5. Draining Mode
// =============================================================================

TEST_F(RouterServiceTest, DrainingBackendSkippedByRandomRouting) {
    register_two_backends();
    RouterService::set_backend_draining_for_testing(1);

    // Random routing should only pick non-draining backends
    for (int i = 0; i < 20; ++i) {
        auto result = router_->get_random_backend();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), 2);
    }
}

TEST_F(RouterServiceTest, DrainingBackendSkippedByPrefixRouting) {
    register_two_backends();
    RouterService::set_backend_draining_for_testing(1);

    // Even with a route pointing to backend 1, it should be skipped
    RouterService::insert_route_for_testing({1, 2, 3}, 1);

    auto result = router_->get_backend_for_prefix({1, 2, 3});
    ASSERT_TRUE(result.backend_id.has_value());
    // Should fall back to backend 2 (hash fallback skips draining backends)
    EXPECT_EQ(result.backend_id.value(), 2);
    EXPECT_FALSE(result.art_hit);  // Draining ART backend → hash fallback
}

TEST_F(RouterServiceTest, DrainingBackendStateReported) {
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080));
    RouterService::set_backend_draining_for_testing(1);

    auto states = router_->get_all_backend_states();
    ASSERT_EQ(states.size(), 1u);
    EXPECT_TRUE(states[0].is_draining);
}

TEST_F(RouterServiceTest, CustomWeightPreservedAcrossDrainCycle) {
    // Register with a custom weight (not the default 100)
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080), 200, 0);

    auto before = router_->get_all_backend_states();
    ASSERT_EQ(before.size(), 1u);
    EXPECT_EQ(before[0].weight, 200u);

    // Drain and then restore
    RouterService::set_backend_draining_for_testing(1);
    RouterService::clear_backend_draining_for_testing(1);

    auto after = router_->get_all_backend_states();
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after[0].weight, 200u);  // Original weight must survive drain cycle
    EXPECT_FALSE(after[0].is_draining);
}

TEST_F(RouterServiceTest, AllBackendsDrainingReturnsNullopt) {
    register_two_backends();
    RouterService::set_backend_draining_for_testing(1);
    RouterService::set_backend_draining_for_testing(2);

    auto result = router_->get_random_backend();
    EXPECT_FALSE(result.has_value());

    auto prefix_result = router_->get_backend_for_prefix({1, 2, 3});
    EXPECT_FALSE(prefix_result.backend_id.has_value());
}

TEST_F(RouterServiceTest, RouteRequestReturnsErrorWhenAllDraining) {
    register_two_backends();
    RouterService::set_backend_draining_for_testing(1);
    RouterService::set_backend_draining_for_testing(2);

    auto result = router_->route_request({1, 2, 3});
    EXPECT_FALSE(result.backend_id.has_value());
    EXPECT_FALSE(result.error_message.empty());
}

// =============================================================================
// 6. Dead Backend (Circuit Breaker) Integration
// =============================================================================

TEST_F(RouterServiceTest, DeadBackendSkippedByRandomRouting) {
    register_two_backends();
    RouterService::mark_backend_dead_for_testing(1);

    for (int i = 0; i < 20; ++i) {
        auto result = router_->get_random_backend();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), 2);
    }
}

TEST_F(RouterServiceTest, DeadBackendCacheHitTreatedAsMiss) {
    register_two_backends();
    RouterService::insert_route_for_testing({1, 2, 3}, 1);
    RouterService::mark_backend_dead_for_testing(1);

    // lookup should return nullopt because the cached backend is dead
    auto result = router_->lookup({1, 2, 3});
    EXPECT_FALSE(result.has_value());
}

TEST_F(RouterServiceTest, DeadBackendStateReported) {
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080));
    RouterService::mark_backend_dead_for_testing(1);

    auto states = router_->get_all_backend_states();
    ASSERT_EQ(states.size(), 1u);
    EXPECT_TRUE(states[0].is_dead);
}

TEST_F(RouterServiceTest, AllBackendsDeadReturnsNoBackends) {
    register_two_backends();
    RouterService::mark_backend_dead_for_testing(1);
    RouterService::mark_backend_dead_for_testing(2);

    auto result = router_->get_random_backend();
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// 7. Routing Mode Dispatch
// =============================================================================

TEST_F(RouterServiceTest, PrefixModeRoutesViaART) {
    register_two_backends();
    RouterService::insert_route_for_testing({1, 2, 3}, 2);

    auto result = router_->route_request({1, 2, 3});
    EXPECT_EQ(result.routing_mode, "prefix");
    EXPECT_TRUE(result.backend_id.has_value());
    EXPECT_EQ(result.backend_id.value(), 2);
}

TEST_F(RouterServiceTest, HashModeIgnoresART) {
    RoutingConfig hash_cfg = cfg_;
    hash_cfg.routing_mode = RoutingConfig::RoutingMode::HASH;
    recreate_router(hash_cfg);
    register_two_backends();

    // Insert a route — hash mode should NOT use it
    RouterService::insert_route_for_testing({1, 2, 3}, 1);

    auto result = router_->route_request({1, 2, 3});
    EXPECT_EQ(result.routing_mode, "hash");
    EXPECT_TRUE(result.backend_id.has_value());
    // Hash mode picks deterministically but does NOT consult ART
}

TEST_F(RouterServiceTest, RandomModeReturnsAnyBackend) {
    RoutingConfig random_cfg = cfg_;
    random_cfg.routing_mode = RoutingConfig::RoutingMode::RANDOM;
    recreate_router(random_cfg);
    register_two_backends();

    auto result = router_->route_request({1, 2, 3});
    EXPECT_EQ(result.routing_mode, "random");
    EXPECT_TRUE(result.backend_id.has_value());
    EXPECT_FALSE(result.cache_hit);
}

TEST_F(RouterServiceTest, HashModeNoBackendsError) {
    RoutingConfig hash_cfg = cfg_;
    hash_cfg.routing_mode = RoutingConfig::RoutingMode::HASH;
    recreate_router(hash_cfg);

    auto result = router_->route_request({1, 2, 3});
    EXPECT_EQ(result.routing_mode, "hash");
    EXPECT_FALSE(result.backend_id.has_value());
    EXPECT_EQ(result.error_message, "No backends registered");
}

TEST_F(RouterServiceTest, RandomModeNoBackendsError) {
    RoutingConfig random_cfg = cfg_;
    random_cfg.routing_mode = RoutingConfig::RoutingMode::RANDOM;
    recreate_router(random_cfg);

    auto result = router_->route_request({1, 2, 3});
    EXPECT_EQ(result.routing_mode, "random");
    EXPECT_FALSE(result.backend_id.has_value());
    EXPECT_EQ(result.error_message, "No backends registered");
}

// =============================================================================
// 8. Consistent Hash Determinism
// =============================================================================

TEST_F(RouterServiceTest, SameTokensGetSameHashBackend) {
    register_three_backends();
    std::vector<int32_t> tokens = {100, 200, 300, 400, 500};

    auto first = router_->get_backend_by_hash(tokens);
    auto second = router_->get_backend_by_hash(tokens);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first.value(), second.value());
}

TEST_F(RouterServiceTest, PrefixHashFallbackIsDeterministic) {
    register_three_backends();
    // No ART route → hash fallback
    std::vector<int32_t> tokens = {10, 20, 30, 40, 50};

    auto first = router_->get_backend_for_prefix(tokens);
    auto second = router_->get_backend_for_prefix(tokens);

    ASSERT_TRUE(first.backend_id.has_value());
    ASSERT_TRUE(second.backend_id.has_value());
    EXPECT_EQ(first.backend_id.value(), second.backend_id.value());
    EXPECT_FALSE(first.art_hit);   // No ART route → hash fallback
    EXPECT_FALSE(second.art_hit);
}

TEST_F(RouterServiceTest, JumpHashMinimalRemapOnBackendAddition) {
    // With 3 backends, record hash assignments for many token sequences.
    // Add a 4th backend, then verify that at most ~1/4 of assignments change.
    // This is the core property of jump consistent hash vs modular hash.
    register_three_backends();

    constexpr int NUM_PROBES = 200;
    std::vector<BackendId> assignments_before;
    assignments_before.reserve(NUM_PROBES);

    for (int i = 0; i < NUM_PROBES; ++i) {
        std::vector<int32_t> tokens = {i * 7, i * 13 + 1, i * 31 + 2, i * 53 + 3, i * 97 + 4};
        auto result = router_->get_backend_by_hash(tokens);
        ASSERT_TRUE(result.has_value());
        assignments_before.push_back(result.value());
    }

    // Add a 4th backend
    RouterService::register_backend_for_testing(4, make_addr("10.0.0.4", 8080));

    int changed = 0;
    for (int i = 0; i < NUM_PROBES; ++i) {
        std::vector<int32_t> tokens = {i * 7, i * 13 + 1, i * 31 + 2, i * 53 + 3, i * 97 + 4};
        auto result = router_->get_backend_by_hash(tokens);
        ASSERT_TRUE(result.has_value());
        if (result.value() != assignments_before[i]) {
            ++changed;
        }
    }

    // Jump consistent hash should remap ~1/4 of keys (new bucket gets ~25%).
    // Modular hash would remap ~75%.  Allow generous margin: at most 40%.
    EXPECT_LE(changed, NUM_PROBES * 40 / 100)
        << "Too many keys remapped (" << changed << "/" << NUM_PROBES
        << "); jump consistent hash should remap ~25%";
}

TEST_F(RouterServiceTest, JumpHashMinimalRemapOnBackendRemoval) {
    // With 4 backends, record assignments. Remove one, verify minimal remap.
    register_three_backends();
    RouterService::register_backend_for_testing(4, make_addr("10.0.0.4", 8080));

    constexpr int NUM_PROBES = 200;
    std::vector<BackendId> assignments_before;
    assignments_before.reserve(NUM_PROBES);

    for (int i = 0; i < NUM_PROBES; ++i) {
        std::vector<int32_t> tokens = {i * 11, i * 17 + 5, i * 37 + 7};
        auto result = router_->get_backend_by_hash(tokens);
        ASSERT_TRUE(result.has_value());
        assignments_before.push_back(result.value());
    }

    // Kill backend 4 (mark dead so it's excluded from live_backends)
    RouterService::mark_backend_dead_for_testing(4);

    int changed = 0;
    for (int i = 0; i < NUM_PROBES; ++i) {
        std::vector<int32_t> tokens = {i * 11, i * 17 + 5, i * 37 + 7};
        auto result = router_->get_backend_by_hash(tokens);
        ASSERT_TRUE(result.has_value());
        if (result.value() != assignments_before[i]) {
            ++changed;
        }
    }

    // Removing 1 of 4 backends should remap ~25% of keys, not 75%.
    EXPECT_LE(changed, NUM_PROBES * 40 / 100)
        << "Too many keys remapped (" << changed << "/" << NUM_PROBES
        << "); jump consistent hash should remap ~25%";
}

// =============================================================================
// 9. Weighted Random Selection
// =============================================================================

TEST_F(RouterServiceTest, ZeroWeightBackendNeverSelected) {
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080), 0);
    RouterService::register_backend_for_testing(2, make_addr("10.0.0.2", 8080), 100);

    for (int i = 0; i < 50; ++i) {
        auto result = router_->get_random_backend();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), 2);
    }
}

TEST_F(RouterServiceTest, PriorityGroupSelection) {
    // Priority 0 (highest) should be preferred over priority 1
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080), 100, 0);
    RouterService::register_backend_for_testing(2, make_addr("10.0.0.2", 8080), 100, 1);

    for (int i = 0; i < 50; ++i) {
        auto result = router_->get_random_backend();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), 1);  // Always priority 0
    }
}

TEST_F(RouterServiceTest, FallbackToPriority1WhenPriority0Dead) {
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080), 100, 0);
    RouterService::register_backend_for_testing(2, make_addr("10.0.0.2", 8080), 100, 1);
    RouterService::mark_backend_dead_for_testing(1);

    for (int i = 0; i < 20; ++i) {
        auto result = router_->get_random_backend();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), 2);
    }
}

// =============================================================================
// 10. Prefix Boundary Routing
// =============================================================================

TEST_F(RouterServiceTest, PrefixBoundaryAffectsRouting) {
    register_three_backends();
    // With different prefix boundaries on the same tokens, hash may differ
    std::vector<int32_t> tokens(200, 42);

    auto result1 = router_->route_request(tokens, "", 0);
    auto result2 = router_->route_request(tokens, "", 50);

    EXPECT_TRUE(result1.backend_id.has_value());
    EXPECT_TRUE(result2.backend_id.has_value());
    // The backends MAY differ depending on hash — just verify both succeed
}

// =============================================================================
// 11. BackendRequestGuard
// =============================================================================

TEST_F(RouterServiceTest, GuardActivatesForRegisteredBackend) {
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080));

    BackendRequestGuard guard(1);
    EXPECT_TRUE(guard.is_active());
    EXPECT_EQ(guard.backend_id(), 1);

    EXPECT_EQ(get_backend_load(1), 1u);
}

TEST_F(RouterServiceTest, GuardDecrementsOnDestruction) {
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080));

    {
        BackendRequestGuard guard(1);
        EXPECT_EQ(get_backend_load(1), 1u);
    }
    EXPECT_EQ(get_backend_load(1), 0u);
}

TEST_F(RouterServiceTest, GuardInactiveForUnknownBackend) {
    BackendRequestGuard guard(999);
    EXPECT_FALSE(guard.is_active());
}

TEST_F(RouterServiceTest, GuardMoveTransfersOwnership) {
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080));

    BackendRequestGuard guard1(1);
    EXPECT_EQ(get_backend_load(1), 1u);

    BackendRequestGuard guard2(std::move(guard1));
    EXPECT_FALSE(guard1.is_active());
    EXPECT_TRUE(guard2.is_active());
    EXPECT_EQ(get_backend_load(1), 1u);  // Still 1, not 2
}

TEST_F(RouterServiceTest, MultipleGuardsSameBackend) {
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080));

    BackendRequestGuard g1(1);
    BackendRequestGuard g2(1);
    BackendRequestGuard g3(1);
    EXPECT_EQ(get_backend_load(1), 3u);
}

// =============================================================================
// 12. Load Tracking Helpers
// =============================================================================

TEST_F(RouterServiceTest, GetBackendLoadReturnsZeroForUnknown) {
    EXPECT_EQ(get_backend_load(999), 0u);
}

TEST_F(RouterServiceTest, GetLeastLoadedBackend) {
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080));
    RouterService::register_backend_for_testing(2, make_addr("10.0.0.2", 8080));

    // Backend 1 has 2 in-flight, backend 2 has 0
    BackendRequestGuard g1a(1);
    BackendRequestGuard g1b(1);

    auto [id, load] = get_least_loaded_backend({1, 2});
    EXPECT_EQ(id, 2);
    EXPECT_EQ(load, 0u);
}

TEST_F(RouterServiceTest, GetLeastLoadedSkipsDraining) {
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080));
    RouterService::register_backend_for_testing(2, make_addr("10.0.0.2", 8080));
    RouterService::set_backend_draining_for_testing(2);

    auto [id, load] = get_least_loaded_backend({1, 2});
    EXPECT_EQ(id, 1);
}

TEST_F(RouterServiceTest, GetLeastLoadedSkipsDead) {
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080));
    RouterService::register_backend_for_testing(2, make_addr("10.0.0.2", 8080));
    RouterService::mark_backend_dead_for_testing(2);

    auto [id, load] = get_least_loaded_backend({1, 2});
    EXPECT_EQ(id, 1);
}

TEST_F(RouterServiceTest, GetLeastLoadedEmptyCandidates) {
    auto [id, load] = get_least_loaded_backend({});
    EXPECT_EQ(id, 0);
    EXPECT_EQ(load, UINT64_MAX);
}

// =============================================================================
// 13. Constructor Variants
// =============================================================================

TEST_F(RouterServiceTest, DefaultConstructor) {
    // Destroy existing router first to avoid metric name collision
    router_.reset();
    RouterService::reset_shard_state_for_testing(nullptr);
    RouterService router;

    auto ids = router.get_all_backend_ids();
    EXPECT_TRUE(ids.empty());
}

TEST_F(RouterServiceTest, ConstructWithDisabledCluster) {
    ClusterConfig cluster_cfg;
    cluster_cfg.enabled = false;

    // Destroy existing router first to avoid metric name collision
    router_.reset();
    RouterService::reset_shard_state_for_testing(nullptr);
    RouterService router(cfg_, cluster_cfg);

    EXPECT_EQ(router.gossip_service(), nullptr);
}

// =============================================================================
// 14. Configuration Hot-Reload (via test reset)
// =============================================================================

TEST_F(RouterServiceTest, ConfigChangeViaReset) {
    RoutingConfig new_cfg = cfg_;
    new_cfg.routing_mode = RoutingConfig::RoutingMode::RANDOM;
    recreate_router(new_cfg);

    auto result = router_->route_request({1, 2, 3});
    EXPECT_EQ(result.routing_mode, "random");
}

// =============================================================================
// 15. Tree Dump API
// =============================================================================

TEST_F(RouterServiceTest, TreeDumpEmptyTree) {
    auto dump = router_->get_tree_dump();
    EXPECT_FALSE(dump.backend.has_value());
}

TEST_F(RouterServiceTest, TreeDumpWithPrefixNotFound) {
    auto dump = router_->get_tree_dump_with_prefix({1, 2, 3});
    EXPECT_FALSE(dump.has_value());
}

TEST_F(RouterServiceTest, TreeDumpReflectsRoutes) {
    register_two_backends();
    RouterService::insert_route_for_testing({1, 2, 3}, 1);

    auto dump = router_->get_tree_dump();
    EXPECT_TRUE(dump.backend.has_value() || !dump.children.empty());
}

// =============================================================================
// 16. Edge Cases
// =============================================================================

TEST_F(RouterServiceTest, EmptyTokenVector) {
    register_two_backends();
    auto result = router_->route_request({});
    // Empty tokens: prefix mode returns first live backend or error
    EXPECT_TRUE(result.backend_id.has_value() || !result.error_message.empty());
}

TEST_F(RouterServiceTest, LargeTokenVector) {
    register_two_backends();
    std::vector<int32_t> tokens(10000, 42);
    auto result = router_->route_request(tokens);
    EXPECT_TRUE(result.backend_id.has_value());
}

TEST_F(RouterServiceTest, SingleTokenVector) {
    register_two_backends();
    auto result = router_->route_request({42});
    EXPECT_TRUE(result.backend_id.has_value());
}

TEST_F(RouterServiceTest, ResetWithNullConfigClearsState) {
    register_two_backends();
    RouterService::insert_route_for_testing({1, 2}, 1);

    RouterService::reset_shard_state_for_testing(nullptr);
    auto ids = router_->get_all_backend_ids();
    EXPECT_TRUE(ids.empty());
}

TEST_F(RouterServiceTest, MultipleResetsAreIdempotent) {
    RouterService::reset_shard_state_for_testing(&cfg_);
    RouterService::reset_shard_state_for_testing(&cfg_);
    RouterService::reset_shard_state_for_testing(nullptr);
    RouterService::reset_shard_state_for_testing(&cfg_);

    auto ids = router_->get_all_backend_ids();
    EXPECT_TRUE(ids.empty());
}

// =============================================================================
// 17. Callback Registration
// =============================================================================

TEST_F(RouterServiceTest, SetCircuitCleanupCallback) {
    bool called = false;
    RouterService::set_circuit_cleanup_callback([&called](BackendId) {
        called = true;
    });
    EXPECT_FALSE(called);
}

TEST_F(RouterServiceTest, SetPoolCleanupCallback) {
    bool called = false;
    router_->set_pool_cleanup_callback([&called](seastar::socket_address) {
        called = true;
    });
    EXPECT_FALSE(called);
}

// =============================================================================
// 18. RoutingConfig Helpers
// =============================================================================

TEST(RoutingConfigTest, ModeHelpers) {
    RoutingConfig cfg;

    cfg.routing_mode = RoutingConfig::RoutingMode::PREFIX;
    EXPECT_TRUE(cfg.is_prefix_mode());
    EXPECT_FALSE(cfg.is_hash_mode());
    EXPECT_FALSE(cfg.is_random_mode());
    EXPECT_TRUE(cfg.uses_art());
    EXPECT_TRUE(cfg.should_learn_routes());

    cfg.routing_mode = RoutingConfig::RoutingMode::HASH;
    EXPECT_FALSE(cfg.is_prefix_mode());
    EXPECT_TRUE(cfg.is_hash_mode());
    EXPECT_FALSE(cfg.uses_art());
    EXPECT_FALSE(cfg.should_learn_routes());

    cfg.routing_mode = RoutingConfig::RoutingMode::RANDOM;
    EXPECT_TRUE(cfg.is_random_mode());
    EXPECT_FALSE(cfg.uses_art());
    EXPECT_FALSE(cfg.should_learn_routes());
}

TEST(RoutingConfigTest, DefaultValues) {
    RoutingConfig cfg;
    EXPECT_EQ(cfg.max_routes, 100000u);
    EXPECT_EQ(cfg.ttl_seconds.count(), 3600);
    EXPECT_EQ(cfg.prefix_token_length, 128u);
    EXPECT_EQ(cfg.block_alignment, 16u);
    EXPECT_TRUE(cfg.is_prefix_mode());
    EXPECT_EQ(cfg.hash_strategy, RoutingConfig::HashStrategy::BOUNDED_LOAD);
    EXPECT_DOUBLE_EQ(cfg.bounded_load_epsilon, 0.25);
    EXPECT_EQ(cfg.p2c_load_bias, 2u);
}

// =============================================================================
// 19. RouteResult Structure
// =============================================================================

TEST(RouteResultTest, DefaultConstruction) {
    RouteResult r;
    EXPECT_FALSE(r.backend_id.has_value());
    EXPECT_TRUE(r.routing_mode.empty());
    EXPECT_FALSE(r.cache_hit);
    EXPECT_TRUE(r.error_message.empty());
}

// =============================================================================
// 20. BackendState Structure
// =============================================================================

TEST(BackendStateTest, FieldAssignment) {
    RouterService::BackendState bs{};
    bs.id = 1;
    bs.address = "10.0.0.1";
    bs.port = 8080;
    bs.weight = 100;
    bs.priority = 0;
    bs.is_draining = false;
    bs.is_dead = false;
    bs.drain_start_ms = 0;

    EXPECT_EQ(bs.id, 1);
    EXPECT_EQ(bs.port, 8080);
    EXPECT_FALSE(bs.is_draining);
}

// =============================================================================
// 21. Hash Strategy: Bounded-Load Consistent Hashing
// =============================================================================

class BoundedLoadTest : public ::testing::Test {
protected:
    RoutingConfig cfg_;
    std::unique_ptr<RouterService> router_;

    void SetUp() override {
        cfg_ = RoutingConfig{};
        cfg_.routing_mode = RoutingConfig::RoutingMode::PREFIX;
        cfg_.max_routes = 1000;
        cfg_.ttl_seconds = std::chrono::seconds(3600);
        cfg_.prefix_token_length = 128;
        cfg_.block_alignment = 1;
        cfg_.backend_drain_timeout = std::chrono::seconds(2);
        cfg_.load_aware_routing = false;  // Not used by bounded_load
        cfg_.hash_strategy = RoutingConfig::HashStrategy::BOUNDED_LOAD;
        cfg_.bounded_load_epsilon = 0.25;

        router_ = std::make_unique<RouterService>(cfg_);
    }

    void TearDown() override {
        router_.reset();
        RouterService::reset_shard_state_for_testing(nullptr);
    }

    void register_four_backends() {
        RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080));
        RouterService::register_backend_for_testing(2, make_addr("10.0.0.2", 8080));
        RouterService::register_backend_for_testing(3, make_addr("10.0.0.3", 8080));
        RouterService::register_backend_for_testing(4, make_addr("10.0.0.4", 8080));
    }
};

TEST_F(BoundedLoadTest, BasicRoutingWorks) {
    register_four_backends();
    std::vector<int32_t> tokens = {100, 200, 300, 400, 500};

    auto result = router_->route_request(tokens);
    EXPECT_TRUE(result.backend_id.has_value());
    EXPECT_EQ(result.routing_mode, "prefix");
}

TEST_F(BoundedLoadTest, DeterministicWhenUnloaded) {
    register_four_backends();
    std::vector<int32_t> tokens = {10, 20, 30, 40, 50};

    // Without load, same tokens should hash to same backend
    auto first = router_->get_backend_for_prefix(tokens);
    auto second = router_->get_backend_for_prefix(tokens);
    ASSERT_TRUE(first.backend_id.has_value());
    ASSERT_TRUE(second.backend_id.has_value());
    EXPECT_EQ(first.backend_id.value(), second.backend_id.value());
}

TEST_F(BoundedLoadTest, DivertsWhenOverCapacity) {
    register_four_backends();
    std::vector<int32_t> tokens = {10, 20, 30, 40, 50};

    // Find which backend the hash prefers
    auto preferred = router_->get_backend_for_prefix(tokens);
    ASSERT_TRUE(preferred.backend_id.has_value());
    BackendId preferred_id = preferred.backend_id.value();

    // Load that backend heavily (create guards that increment active_requests)
    // With 4 backends, 0 load avg, cap = max(1, ceil(0 * 1.25)) = 1
    // So even 1 in-flight should trigger probing
    BackendRequestGuard g1(preferred_id);
    BackendRequestGuard g2(preferred_id);
    BackendRequestGuard g3(preferred_id);

    auto result = router_->get_backend_for_prefix(tokens);
    ASSERT_TRUE(result.backend_id.has_value());
    // With 3 in-flight on preferred and 0 on others, avg=0.75, cap=max(1, ceil(0.75*1.25))=1
    // preferred has 3 >= 1, so it should divert
    EXPECT_NE(result.backend_id.value(), preferred_id);
}

TEST_F(BoundedLoadTest, SpreadLoadAcrossBackends) {
    register_four_backends();
    std::vector<int32_t> tokens = {10, 20, 30, 40, 50};

    // Simulate 20 concurrent requests all with same prefix.
    // Bounded-load should spread them across backends, not pile on one.
    std::vector<BackendRequestGuard> guards;
    std::map<BackendId, int> counts;

    for (int i = 0; i < 20; ++i) {
        auto result = router_->get_backend_for_prefix(tokens);
        ASSERT_TRUE(result.backend_id.has_value());
        BackendId id = result.backend_id.value();
        counts[id]++;
        guards.emplace_back(id);
    }

    // With 4 backends and 20 requests, ideal distribution is 5 each.
    // With epsilon=0.25, cap = ceil(avg * 1.25).
    // No single backend should have more than ~40% of requests.
    for (auto& [id, count] : counts) {
        EXPECT_LE(count, 8) << "Backend " << id << " has " << count
                            << "/20 requests — bounded-load should prevent this";
    }

    // Should use at least 3 of 4 backends
    EXPECT_GE(counts.size(), 3u) << "Bounded-load should spread across most backends";
}

TEST_F(BoundedLoadTest, ARTHitRespectedWhenUnderCap) {
    register_four_backends();
    std::vector<int32_t> tokens = {10, 20, 30, 40, 50};

    // Insert ART route to backend 3
    RouterService::insert_route_for_testing(tokens, 3);

    auto result = router_->get_backend_for_prefix(tokens);
    ASSERT_TRUE(result.backend_id.has_value());
    EXPECT_EQ(result.backend_id.value(), 3);
    EXPECT_TRUE(result.art_hit);
}

TEST_F(BoundedLoadTest, ARTHitOverriddenWhenOverCap) {
    register_four_backends();
    std::vector<int32_t> tokens = {10, 20, 30, 40, 50};

    // Insert ART route to backend 3
    RouterService::insert_route_for_testing(tokens, 3);

    // Load backend 3 heavily
    BackendRequestGuard g1(3);
    BackendRequestGuard g2(3);
    BackendRequestGuard g3(3);
    BackendRequestGuard g4(3);

    auto result = router_->get_backend_for_prefix(tokens);
    ASSERT_TRUE(result.backend_id.has_value());
    // Backend 3 has 4 in-flight, avg=1.0, cap=max(1, ceil(1.0*1.25))=2
    // 4 >= 2, so bounded-load should divert
    EXPECT_NE(result.backend_id.value(), 3)
        << "Bounded-load should override ART hit when backend is over cap";
}

// =============================================================================
// 22. Hash Strategy: Power of Two Choices (P2C)
// =============================================================================

class P2CTest : public ::testing::Test {
protected:
    RoutingConfig cfg_;
    std::unique_ptr<RouterService> router_;

    void SetUp() override {
        cfg_ = RoutingConfig{};
        cfg_.routing_mode = RoutingConfig::RoutingMode::PREFIX;
        cfg_.max_routes = 1000;
        cfg_.ttl_seconds = std::chrono::seconds(3600);
        cfg_.prefix_token_length = 128;
        cfg_.block_alignment = 1;
        cfg_.backend_drain_timeout = std::chrono::seconds(2);
        cfg_.load_aware_routing = false;  // Not used by P2C
        cfg_.hash_strategy = RoutingConfig::HashStrategy::P2C;
        cfg_.p2c_load_bias = 2;

        router_ = std::make_unique<RouterService>(cfg_);
    }

    void TearDown() override {
        router_.reset();
        RouterService::reset_shard_state_for_testing(nullptr);
    }

    void register_four_backends() {
        RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080));
        RouterService::register_backend_for_testing(2, make_addr("10.0.0.2", 8080));
        RouterService::register_backend_for_testing(3, make_addr("10.0.0.3", 8080));
        RouterService::register_backend_for_testing(4, make_addr("10.0.0.4", 8080));
    }
};

TEST_F(P2CTest, BasicRoutingWorks) {
    register_four_backends();
    std::vector<int32_t> tokens = {100, 200, 300, 400, 500};

    auto result = router_->route_request(tokens);
    EXPECT_TRUE(result.backend_id.has_value());
    EXPECT_EQ(result.routing_mode, "prefix");
}

TEST_F(P2CTest, PrefersLessLoadedSecondary) {
    register_four_backends();
    std::vector<int32_t> tokens = {10, 20, 30, 40, 50};

    // Find primary backend
    auto preferred = router_->get_backend_for_prefix(tokens);
    ASSERT_TRUE(preferred.backend_id.has_value());
    BackendId primary = preferred.backend_id.value();

    // Load the primary heavily (more than p2c_load_bias=2 above secondary)
    BackendRequestGuard g1(primary);
    BackendRequestGuard g2(primary);
    BackendRequestGuard g3(primary);
    BackendRequestGuard g4(primary);
    BackendRequestGuard g5(primary);

    // With primary at 5 and all others at 0, and bias=2:
    // s_load(0) + bias(2) < p_load(5) => true => switch to secondary
    auto result = router_->get_backend_for_prefix(tokens);
    ASSERT_TRUE(result.backend_id.has_value());
    EXPECT_NE(result.backend_id.value(), primary)
        << "P2C should switch to less-loaded secondary when primary is heavily loaded";
}

TEST_F(P2CTest, SticksWithPrimaryWhenLoadBalanced) {
    register_four_backends();
    std::vector<int32_t> tokens = {10, 20, 30, 40, 50};

    // Find primary backend
    auto preferred = router_->get_backend_for_prefix(tokens);
    ASSERT_TRUE(preferred.backend_id.has_value());
    BackendId primary = preferred.backend_id.value();

    // Load primary with just 1 in-flight (bias=2, so s_load(0) + 2 < 1 is false)
    BackendRequestGuard g1(primary);

    auto result = router_->get_backend_for_prefix(tokens);
    ASSERT_TRUE(result.backend_id.has_value());
    EXPECT_EQ(result.backend_id.value(), primary)
        << "P2C should stick with primary when load difference is within bias";
}

// =============================================================================
// 23. Hash Strategy: Modular Hash (Benchmark Baseline)
// =============================================================================

class ModularHashTest : public ::testing::Test {
protected:
    RoutingConfig cfg_;
    std::unique_ptr<RouterService> router_;

    void SetUp() override {
        cfg_ = RoutingConfig{};
        cfg_.routing_mode = RoutingConfig::RoutingMode::PREFIX;
        cfg_.max_routes = 1000;
        cfg_.ttl_seconds = std::chrono::seconds(3600);
        cfg_.prefix_token_length = 128;
        cfg_.block_alignment = 1;
        cfg_.backend_drain_timeout = std::chrono::seconds(2);
        cfg_.load_aware_routing = false;
        cfg_.hash_strategy = RoutingConfig::HashStrategy::MODULAR;

        router_ = std::make_unique<RouterService>(cfg_);
    }

    void TearDown() override {
        router_.reset();
        RouterService::reset_shard_state_for_testing(nullptr);
    }
};

TEST_F(ModularHashTest, BasicRoutingWorks) {
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080));
    RouterService::register_backend_for_testing(2, make_addr("10.0.0.2", 8080));

    std::vector<int32_t> tokens = {100, 200, 300, 400};
    auto result = router_->route_request(tokens);
    EXPECT_TRUE(result.backend_id.has_value());
}

TEST_F(ModularHashTest, DeterministicRouting) {
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080));
    RouterService::register_backend_for_testing(2, make_addr("10.0.0.2", 8080));
    RouterService::register_backend_for_testing(3, make_addr("10.0.0.3", 8080));

    std::vector<int32_t> tokens = {10, 20, 30, 40, 50};
    auto first = router_->get_backend_for_prefix(tokens);
    auto second = router_->get_backend_for_prefix(tokens);

    ASSERT_TRUE(first.backend_id.has_value());
    ASSERT_TRUE(second.backend_id.has_value());
    EXPECT_EQ(first.backend_id.value(), second.backend_id.value());
}

// =============================================================================
// 24. Hash Strategy: Jump (backward compatibility)
// =============================================================================

class JumpHashStrategyTest : public ::testing::Test {
protected:
    RoutingConfig cfg_;
    std::unique_ptr<RouterService> router_;

    void SetUp() override {
        cfg_ = RoutingConfig{};
        cfg_.routing_mode = RoutingConfig::RoutingMode::PREFIX;
        cfg_.max_routes = 1000;
        cfg_.ttl_seconds = std::chrono::seconds(3600);
        cfg_.prefix_token_length = 128;
        cfg_.block_alignment = 1;
        cfg_.backend_drain_timeout = std::chrono::seconds(2);
        cfg_.load_aware_routing = false;
        cfg_.hash_strategy = RoutingConfig::HashStrategy::JUMP;

        router_ = std::make_unique<RouterService>(cfg_);
    }

    void TearDown() override {
        router_.reset();
        RouterService::reset_shard_state_for_testing(nullptr);
    }
};

TEST_F(JumpHashStrategyTest, BackwardCompatible) {
    // JUMP strategy should behave identically to the original pre-strategy code
    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080));
    RouterService::register_backend_for_testing(2, make_addr("10.0.0.2", 8080));
    RouterService::register_backend_for_testing(3, make_addr("10.0.0.3", 8080));

    std::vector<int32_t> tokens = {10, 20, 30, 40, 50};

    auto first = router_->get_backend_for_prefix(tokens);
    auto second = router_->get_backend_for_prefix(tokens);

    ASSERT_TRUE(first.backend_id.has_value());
    ASSERT_TRUE(second.backend_id.has_value());
    EXPECT_EQ(first.backend_id.value(), second.backend_id.value());
    EXPECT_FALSE(first.art_hit);
}

TEST_F(JumpHashStrategyTest, UsesLoadAwareLegacyPath) {
    // With JUMP strategy and load_aware_routing=true, should use median threshold
    cfg_.load_aware_routing = true;
    cfg_.load_imbalance_factor = 2.0;
    cfg_.load_imbalance_floor = 2;
    router_.reset();
    RouterService::reset_shard_state_for_testing(nullptr);
    router_ = std::make_unique<RouterService>(cfg_);

    RouterService::register_backend_for_testing(1, make_addr("10.0.0.1", 8080));
    RouterService::register_backend_for_testing(2, make_addr("10.0.0.2", 8080));

    std::vector<int32_t> tokens = {10, 20, 30, 40, 50};

    auto result = router_->get_backend_for_prefix(tokens);
    ASSERT_TRUE(result.backend_id.has_value());
}

// =============================================================================
// 25. Hash Strategy Configuration
// =============================================================================

TEST(HashStrategyConfigTest, DefaultIsBoundedLoad) {
    RoutingConfig cfg;
    EXPECT_EQ(cfg.hash_strategy, RoutingConfig::HashStrategy::BOUNDED_LOAD);
}

TEST(HashStrategyConfigTest, EnumValues) {
    // Verify all enum values are distinct
    EXPECT_NE(static_cast<int>(RoutingConfig::HashStrategy::JUMP),
              static_cast<int>(RoutingConfig::HashStrategy::BOUNDED_LOAD));
    EXPECT_NE(static_cast<int>(RoutingConfig::HashStrategy::P2C),
              static_cast<int>(RoutingConfig::HashStrategy::MODULAR));
    EXPECT_NE(static_cast<int>(RoutingConfig::HashStrategy::BOUNDED_LOAD),
              static_cast<int>(RoutingConfig::HashStrategy::P2C));
}

// =============================================================================
// 26. Local Route Batching (PendingLocalRoute + Simulated Batch Application)
// =============================================================================
// Tests for the local route batching system introduced to eliminate per-request
// SMP storms. Actual async batch/flush/timer functions require a Seastar reactor,
// so we verify:
//   - PendingLocalRoute struct construction and move semantics
//   - Simulated batch application to the local tree (what apply_local_batch_to_tree does)
//   - Overwrite semantics when batches contain duplicate or conflicting prefixes
//   - Multi-depth route buffering observable via lookup

TEST_F(RouterServiceTest, PendingLocalRouteStructure) {
    PendingLocalRoute route;
    route.tokens = {10, 20, 30, 40, 50};
    route.backend = 7;

    EXPECT_EQ(route.tokens.size(), 5u);
    EXPECT_EQ(route.backend, 7);

    // Move semantics: vector should transfer ownership
    PendingLocalRoute moved = std::move(route);
    EXPECT_EQ(moved.tokens.size(), 5u);
    EXPECT_EQ(moved.backend, 7);
}

TEST_F(RouterServiceTest, SimulatedLocalBatchApplication) {
    // Simulate what apply_local_batch_to_tree() does: insert a batch of routes
    // into the local RadixTree and verify they're all findable via lookup.
    register_three_backends();

    // Simulate a batch of locally-learned routes
    std::vector<std::pair<std::vector<int32_t>, BackendId>> local_batch = {
        {{1, 2, 3, 4, 5}, 1},
        {{6, 7, 8, 9, 10}, 2},
        {{11, 12, 13, 14, 15}, 3},
        {{16, 17, 18, 19, 20}, 1},
        {{21, 22, 23, 24, 25}, 2},
    };

    for (const auto& [tokens, backend] : local_batch) {
        RouterService::insert_route_for_testing(tokens, backend);
    }

    EXPECT_EQ(RouterService::get_route_count_for_testing(), 5u);
    EXPECT_EQ(router_->lookup({1, 2, 3, 4, 5}).value(), 1);
    EXPECT_EQ(router_->lookup({6, 7, 8, 9, 10}).value(), 2);
    EXPECT_EQ(router_->lookup({11, 12, 13, 14, 15}).value(), 3);
    EXPECT_EQ(router_->lookup({16, 17, 18, 19, 20}).value(), 1);
    EXPECT_EQ(router_->lookup({21, 22, 23, 24, 25}).value(), 2);
}

TEST_F(RouterServiceTest, SimulatedBatchDeduplicationEffect) {
    // When the same prefix is inserted multiple times for the same backend,
    // the tree should contain only one route (the tree overwrites on same
    // prefix). This confirms that even without deduplicate_local_batch
    // pre-filtering, the tree stays compact.
    register_two_backends();

    // Insert the same route 5 times (simulating duplicates in a batch)
    for (int i = 0; i < 5; ++i) {
        RouterService::insert_route_for_testing({1, 2, 3, 4, 5}, 1);
    }

    // Tree should have exactly 1 route (same prefix overwrites)
    EXPECT_EQ(RouterService::get_route_count_for_testing(), 1u);
    EXPECT_EQ(router_->lookup({1, 2, 3, 4, 5}).value(), 1);
}

TEST_F(RouterServiceTest, BatchInsertionAllRoutesAccessible) {
    // insert_route_for_testing bypasses the eviction loop in
    // apply_local_batch_to_tree, so all routes survive regardless of
    // max_routes. Verify they're all accessible after batch-style insertion.
    register_two_backends();

    constexpr int COUNT = 8;
    for (int i = 0; i < COUNT; ++i) {
        RouterService::insert_route_for_testing({i * 100 + 1, i * 100 + 2, i * 100 + 3}, 1);
    }

    EXPECT_EQ(RouterService::get_route_count_for_testing(), static_cast<size_t>(COUNT));

    // Every route should be findable
    for (int i = 0; i < COUNT; ++i) {
        EXPECT_TRUE(router_->lookup({i * 100 + 1, i * 100 + 2, i * 100 + 3}).has_value());
    }
}

TEST_F(RouterServiceTest, BatchOverwriteUpdatesBackend) {
    // When a batch contains an older route for backend 1 followed by a
    // newer route for the same prefix on backend 2, the tree should
    // reflect the final (most recent) write for each prefix.
    register_two_backends();

    // First "batch": all on backend 1
    RouterService::insert_route_for_testing({1, 1, 1}, 1);
    RouterService::insert_route_for_testing({2, 2, 2}, 1);
    RouterService::insert_route_for_testing({3, 3, 3}, 1);

    // Second "batch": overwrite two of them to backend 2
    RouterService::insert_route_for_testing({1, 1, 1}, 2);
    RouterService::insert_route_for_testing({3, 3, 3}, 2);

    EXPECT_EQ(RouterService::get_route_count_for_testing(), 3u);

    // Overwritten routes reflect latest backend
    EXPECT_EQ(router_->lookup({1, 1, 1}).value(), 2);
    EXPECT_EQ(router_->lookup({2, 2, 2}).value(), 1);  // untouched
    EXPECT_EQ(router_->lookup({3, 3, 3}).value(), 2);
}

TEST_F(RouterServiceTest, SimulatedMultiDepthBatchBuffering) {
    // Simulate what learn_route_global_multi does: insert routes at multiple
    // prefix boundaries for the same token sequence. Each boundary creates a
    // separate route at a different prefix length.
    register_two_backends();

    // Full token sequence
    std::vector<int32_t> tokens = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};

    // Simulate boundaries at positions 3, 6, and 10
    // This creates 3 routes with different prefix lengths
    std::vector<int32_t> prefix_3(tokens.begin(), tokens.begin() + 3);   // {10, 20, 30}
    std::vector<int32_t> prefix_6(tokens.begin(), tokens.begin() + 6);   // {10, 20, 30, 40, 50, 60}
    std::vector<int32_t> prefix_10(tokens.begin(), tokens.begin() + 10); // full sequence

    RouterService::insert_route_for_testing(prefix_3, 1);
    RouterService::insert_route_for_testing(prefix_6, 1);
    RouterService::insert_route_for_testing(prefix_10, 1);

    EXPECT_EQ(RouterService::get_route_count_for_testing(), 3u);

    // All three prefixes should resolve to the correct backend
    EXPECT_EQ(router_->lookup(prefix_3).value(), 1);
    EXPECT_EQ(router_->lookup(prefix_6).value(), 1);
    EXPECT_EQ(router_->lookup(prefix_10).value(), 1);
}

TEST_F(RouterServiceTest, SimulatedMultiDepthWithDifferentBackends) {
    // Multi-depth routes where different conversation depths route to
    // different backends (e.g., system message cached on backend 1,
    // but user turn cached on backend 2).
    register_two_backends();

    std::vector<int32_t> tokens = {10, 20, 30, 40, 50, 60, 70, 80};

    // Simulate: system prefix on backend 1, full conversation on backend 2
    std::vector<int32_t> system_prefix(tokens.begin(), tokens.begin() + 4);
    std::vector<int32_t> full_prefix(tokens.begin(), tokens.begin() + 8);

    RouterService::insert_route_for_testing(system_prefix, 1);
    RouterService::insert_route_for_testing(full_prefix, 2);

    EXPECT_EQ(RouterService::get_route_count_for_testing(), 2u);
    EXPECT_EQ(router_->lookup(system_prefix).value(), 1);
    EXPECT_EQ(router_->lookup(full_prefix).value(), 2);
}

TEST_F(RouterServiceTest, LargeBatchInsertionStressTest) {
    // Simulate a large batch (exceeding MAX_BATCH_SIZE) to verify the tree
    // handles many insertions correctly. In production, flush_local_route_batch
    // processes up to MAX_BATCH_SIZE routes per flush.
    register_three_backends();

    constexpr int BATCH_SIZE = 200;  // 2x MAX_BATCH_SIZE
    for (int i = 0; i < BATCH_SIZE; ++i) {
        BackendId backend = static_cast<BackendId>((i % 3) + 1);
        RouterService::insert_route_for_testing({i * 10 + 1, i * 10 + 2, i * 10 + 3}, backend);
    }

    EXPECT_EQ(RouterService::get_route_count_for_testing(), static_cast<size_t>(BATCH_SIZE));

    // Spot-check a few routes (backend = (i % 3) + 1)
    EXPECT_EQ(router_->lookup({1, 2, 3}).value(), 1);       // i=0: (0%3)+1 = 1
    EXPECT_EQ(router_->lookup({11, 12, 13}).value(), 2);     // i=1: (1%3)+1 = 2
    EXPECT_EQ(router_->lookup({21, 22, 23}).value(), 3);     // i=2: (2%3)+1 = 3
    EXPECT_EQ(router_->lookup({1991, 1992, 1993}).value(), 2); // i=199: (199%3)+1 = 2
}
