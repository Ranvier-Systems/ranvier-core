#include <gtest/gtest.h>
#include "sqlite_persistence.hpp"
#include <filesystem>
#include <cstdio>

using namespace ranvier;

class PersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Use a unique temp file for each test
        test_db_path_ = "/tmp/ranvier_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".db";
        store_ = create_persistence_store();
    }

    void TearDown() override {
        store_->close();
        // Clean up test database files
        std::remove(test_db_path_.c_str());
        std::remove((test_db_path_ + "-wal").c_str());
        std::remove((test_db_path_ + "-shm").c_str());
    }

    std::string test_db_path_;
    std::unique_ptr<PersistenceStore> store_;
};

// =============================================================================
// Basic Lifecycle Tests
// =============================================================================

TEST_F(PersistenceTest, OpenAndClose) {
    EXPECT_FALSE(store_->is_open());
    EXPECT_TRUE(store_->open(test_db_path_));
    EXPECT_TRUE(store_->is_open());
    store_->close();
    EXPECT_FALSE(store_->is_open());
}

TEST_F(PersistenceTest, DoubleOpenFails) {
    EXPECT_TRUE(store_->open(test_db_path_));
    EXPECT_FALSE(store_->open(test_db_path_));  // Should fail - already open
}

TEST_F(PersistenceTest, OperationsFailWhenClosed) {
    EXPECT_FALSE(store_->save_backend(1, "127.0.0.1", 8080));
    EXPECT_TRUE(store_->load_backends().empty());
}

// =============================================================================
// Backend Tests
// =============================================================================

TEST_F(PersistenceTest, SaveAndLoadBackend) {
    ASSERT_TRUE(store_->open(test_db_path_));

    EXPECT_TRUE(store_->save_backend(1, "192.168.1.100", 11434));
    EXPECT_TRUE(store_->save_backend(2, "192.168.1.101", 11435));

    auto backends = store_->load_backends();
    ASSERT_EQ(backends.size(), 2);

    // Find backend 1
    auto it = std::find_if(backends.begin(), backends.end(),
        [](const BackendRecord& r) { return r.id == 1; });
    ASSERT_NE(it, backends.end());
    EXPECT_EQ(it->ip, "192.168.1.100");
    EXPECT_EQ(it->port, 11434);

    // Find backend 2
    it = std::find_if(backends.begin(), backends.end(),
        [](const BackendRecord& r) { return r.id == 2; });
    ASSERT_NE(it, backends.end());
    EXPECT_EQ(it->ip, "192.168.1.101");
    EXPECT_EQ(it->port, 11435);
}

TEST_F(PersistenceTest, UpdateBackend) {
    ASSERT_TRUE(store_->open(test_db_path_));

    EXPECT_TRUE(store_->save_backend(1, "192.168.1.100", 11434));
    EXPECT_TRUE(store_->save_backend(1, "10.0.0.50", 8080));  // Update

    auto backends = store_->load_backends();
    ASSERT_EQ(backends.size(), 1);
    EXPECT_EQ(backends[0].id, 1);
    EXPECT_EQ(backends[0].ip, "10.0.0.50");
    EXPECT_EQ(backends[0].port, 8080);
}

TEST_F(PersistenceTest, RemoveBackend) {
    ASSERT_TRUE(store_->open(test_db_path_));

    store_->save_backend(1, "192.168.1.100", 11434);
    store_->save_backend(2, "192.168.1.101", 11435);

    EXPECT_EQ(store_->backend_count(), 2);
    EXPECT_TRUE(store_->remove_backend(1));
    EXPECT_EQ(store_->backend_count(), 1);

    auto backends = store_->load_backends();
    ASSERT_EQ(backends.size(), 1);
    EXPECT_EQ(backends[0].id, 2);
}

// =============================================================================
// Route Tests
// =============================================================================

TEST_F(PersistenceTest, SaveAndLoadRoute) {
    ASSERT_TRUE(store_->open(test_db_path_));

    std::vector<TokenId> tokens = {100, 200, 300, 400};
    EXPECT_TRUE(store_->save_route(tokens, 1));

    auto routes = store_->load_routes();
    ASSERT_EQ(routes.size(), 1);
    EXPECT_EQ(routes[0].tokens, tokens);
    EXPECT_EQ(routes[0].backend_id, 1);
}

TEST_F(PersistenceTest, SaveMultipleRoutes) {
    ASSERT_TRUE(store_->open(test_db_path_));

    std::vector<TokenId> tokens1 = {100, 200, 300};
    std::vector<TokenId> tokens2 = {400, 500, 600};
    std::vector<TokenId> tokens3 = {100, 200, 999};  // Shares prefix with tokens1

    store_->save_route(tokens1, 1);
    store_->save_route(tokens2, 2);
    store_->save_route(tokens3, 3);

    EXPECT_EQ(store_->route_count(), 3);
}

TEST_F(PersistenceTest, UpdateRoute) {
    ASSERT_TRUE(store_->open(test_db_path_));

    std::vector<TokenId> tokens = {100, 200, 300};
    store_->save_route(tokens, 1);
    store_->save_route(tokens, 2);  // Update to different backend

    auto routes = store_->load_routes();
    ASSERT_EQ(routes.size(), 1);
    EXPECT_EQ(routes[0].backend_id, 2);
}

TEST_F(PersistenceTest, RemoveRoute) {
    ASSERT_TRUE(store_->open(test_db_path_));

    std::vector<TokenId> tokens1 = {100, 200, 300};
    std::vector<TokenId> tokens2 = {400, 500, 600};

    store_->save_route(tokens1, 1);
    store_->save_route(tokens2, 2);

    EXPECT_EQ(store_->route_count(), 2);
    EXPECT_TRUE(store_->remove_route(tokens1));
    EXPECT_EQ(store_->route_count(), 1);

    auto routes = store_->load_routes();
    ASSERT_EQ(routes.size(), 1);
    EXPECT_EQ(routes[0].tokens, tokens2);
}

TEST_F(PersistenceTest, RemoveRoutesForBackend) {
    ASSERT_TRUE(store_->open(test_db_path_));

    // Create routes for multiple backends
    std::vector<TokenId> tokens1 = {100, 200, 300};
    std::vector<TokenId> tokens2 = {400, 500, 600};
    std::vector<TokenId> tokens3 = {700, 800, 900};
    std::vector<TokenId> tokens4 = {111, 222, 333};

    store_->save_route(tokens1, 1);  // Backend 1
    store_->save_route(tokens2, 1);  // Backend 1
    store_->save_route(tokens3, 2);  // Backend 2
    store_->save_route(tokens4, 3);  // Backend 3

    EXPECT_EQ(store_->route_count(), 4);

    // Remove all routes for backend 1
    EXPECT_TRUE(store_->remove_routes_for_backend(1));
    EXPECT_EQ(store_->route_count(), 2);

    // Verify only backends 2 and 3 routes remain
    auto routes = store_->load_routes();
    ASSERT_EQ(routes.size(), 2);
    for (const auto& route : routes) {
        EXPECT_NE(route.backend_id, 1);
    }

    // Remove non-existent backend should still succeed
    EXPECT_TRUE(store_->remove_routes_for_backend(999));
    EXPECT_EQ(store_->route_count(), 2);
}

TEST_F(PersistenceTest, LongTokenSequence) {
    ASSERT_TRUE(store_->open(test_db_path_));

    // Test with a longer sequence (simulating real LLM context)
    std::vector<TokenId> tokens(1000);
    for (int i = 0; i < 1000; ++i) {
        tokens[i] = i * 7;  // Some pattern
    }

    EXPECT_TRUE(store_->save_route(tokens, 42));

    auto routes = store_->load_routes();
    ASSERT_EQ(routes.size(), 1);
    EXPECT_EQ(routes[0].tokens, tokens);
    EXPECT_EQ(routes[0].backend_id, 42);
}

// =============================================================================
// Batch Operations
// =============================================================================

TEST_F(PersistenceTest, BatchSaveRoutes) {
    ASSERT_TRUE(store_->open(test_db_path_));

    std::vector<RouteRecord> batch;
    for (int i = 0; i < 100; ++i) {
        RouteRecord record;
        record.tokens = {static_cast<TokenId>(i * 100), static_cast<TokenId>(i * 100 + 1)};
        record.backend_id = i % 5;
        batch.push_back(std::move(record));
    }

    EXPECT_TRUE(store_->save_routes_batch(batch));
    EXPECT_EQ(store_->route_count(), 100);
}

TEST_F(PersistenceTest, BatchSaveEmpty) {
    ASSERT_TRUE(store_->open(test_db_path_));

    std::vector<RouteRecord> empty_batch;
    EXPECT_TRUE(store_->save_routes_batch(empty_batch));
    EXPECT_EQ(store_->route_count(), 0);
}

// =============================================================================
// Persistence Across Sessions
// =============================================================================

TEST_F(PersistenceTest, DataSurvivesReopen) {
    // First session - write data
    {
        ASSERT_TRUE(store_->open(test_db_path_));
        store_->save_backend(1, "192.168.1.100", 11434);
        std::vector<TokenId> tokens = {100, 200, 300};
        store_->save_route(tokens, 1);
        store_->close();
    }

    // Second session - verify data persisted
    {
        auto store2 = create_persistence_store();
        ASSERT_TRUE(store2->open(test_db_path_));

        auto backends = store2->load_backends();
        ASSERT_EQ(backends.size(), 1);
        EXPECT_EQ(backends[0].ip, "192.168.1.100");

        auto routes = store2->load_routes();
        ASSERT_EQ(routes.size(), 1);
        EXPECT_EQ(routes[0].backend_id, 1);
    }
}

// =============================================================================
// Clear All
// =============================================================================

TEST_F(PersistenceTest, ClearAll) {
    ASSERT_TRUE(store_->open(test_db_path_));

    store_->save_backend(1, "192.168.1.100", 11434);
    store_->save_backend(2, "192.168.1.101", 11435);
    std::vector<TokenId> tokens1 = {100, 200};
    std::vector<TokenId> tokens2 = {300, 400};
    store_->save_route(tokens1, 1);
    store_->save_route(tokens2, 2);

    EXPECT_EQ(store_->backend_count(), 2);
    EXPECT_EQ(store_->route_count(), 2);

    EXPECT_TRUE(store_->clear_all());

    EXPECT_EQ(store_->backend_count(), 0);
    EXPECT_EQ(store_->route_count(), 0);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(PersistenceTest, EmptyTokenSequence) {
    ASSERT_TRUE(store_->open(test_db_path_));

    std::vector<TokenId> empty_tokens;
    EXPECT_TRUE(store_->save_route(empty_tokens, 1));

    auto routes = store_->load_routes();
    ASSERT_EQ(routes.size(), 1);
    EXPECT_TRUE(routes[0].tokens.empty());
}

TEST_F(PersistenceTest, SingleTokenSequence) {
    ASSERT_TRUE(store_->open(test_db_path_));

    std::vector<TokenId> single = {42};
    EXPECT_TRUE(store_->save_route(single, 1));

    auto routes = store_->load_routes();
    ASSERT_EQ(routes.size(), 1);
    EXPECT_EQ(routes[0].tokens, single);
}

TEST_F(PersistenceTest, NegativeTokenIds) {
    ASSERT_TRUE(store_->open(test_db_path_));

    std::vector<TokenId> tokens = {-1, -100, 0, 100, -50};
    EXPECT_TRUE(store_->save_route(tokens, 1));

    auto routes = store_->load_routes();
    ASSERT_EQ(routes.size(), 1);
    EXPECT_EQ(routes[0].tokens, tokens);
}

// =============================================================================
// Backend Weight and Priority Tests
// =============================================================================

TEST_F(PersistenceTest, BackendWeightAndPriority) {
    ASSERT_TRUE(store_->open(test_db_path_));

    // Save backends with different weights and priorities
    EXPECT_TRUE(store_->save_backend(1, "192.168.1.100", 11434, 200, 0));  // High capacity, primary
    EXPECT_TRUE(store_->save_backend(2, "192.168.1.101", 11434, 100, 0));  // Normal capacity, primary
    EXPECT_TRUE(store_->save_backend(3, "192.168.1.102", 11434, 50, 1));   // Low capacity, secondary

    auto backends = store_->load_backends();
    ASSERT_EQ(backends.size(), 3);

    // Find and verify backend 1
    auto it = std::find_if(backends.begin(), backends.end(),
        [](const BackendRecord& r) { return r.id == 1; });
    ASSERT_NE(it, backends.end());
    EXPECT_EQ(it->weight, 200);
    EXPECT_EQ(it->priority, 0);

    // Find and verify backend 2
    it = std::find_if(backends.begin(), backends.end(),
        [](const BackendRecord& r) { return r.id == 2; });
    ASSERT_NE(it, backends.end());
    EXPECT_EQ(it->weight, 100);
    EXPECT_EQ(it->priority, 0);

    // Find and verify backend 3
    it = std::find_if(backends.begin(), backends.end(),
        [](const BackendRecord& r) { return r.id == 3; });
    ASSERT_NE(it, backends.end());
    EXPECT_EQ(it->weight, 50);
    EXPECT_EQ(it->priority, 1);
}

TEST_F(PersistenceTest, BackendDefaultWeightAndPriority) {
    ASSERT_TRUE(store_->open(test_db_path_));

    // Save backend with default weight and priority
    EXPECT_TRUE(store_->save_backend(1, "192.168.1.100", 11434));

    auto backends = store_->load_backends();
    ASSERT_EQ(backends.size(), 1);
    EXPECT_EQ(backends[0].weight, 100);    // Default weight
    EXPECT_EQ(backends[0].priority, 0);    // Default priority
}

TEST_F(PersistenceTest, UpdateBackendWeightAndPriority) {
    ASSERT_TRUE(store_->open(test_db_path_));

    // Initial registration
    EXPECT_TRUE(store_->save_backend(1, "192.168.1.100", 11434, 100, 0));

    // Update with new weight and priority
    EXPECT_TRUE(store_->save_backend(1, "192.168.1.100", 11434, 300, 2));

    auto backends = store_->load_backends();
    ASSERT_EQ(backends.size(), 1);
    EXPECT_EQ(backends[0].weight, 300);
    EXPECT_EQ(backends[0].priority, 2);
}

TEST_F(PersistenceTest, BackendWeightAndPrioritySurviveReopen) {
    // First session - write data with weights/priorities
    {
        ASSERT_TRUE(store_->open(test_db_path_));
        store_->save_backend(1, "192.168.1.100", 11434, 250, 1);
        store_->close();
    }

    // Second session - verify weights/priorities persisted
    {
        auto store2 = create_persistence_store();
        ASSERT_TRUE(store2->open(test_db_path_));

        auto backends = store2->load_backends();
        ASSERT_EQ(backends.size(), 1);
        EXPECT_EQ(backends[0].weight, 250);
        EXPECT_EQ(backends[0].priority, 1);
    }
}
