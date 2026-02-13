#include <gtest/gtest.h>
#include "sqlite_persistence.hpp"
#include <cstdio>
#include <filesystem>
#include <set>

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

// =============================================================================
// NULL Value Handling Tests (Security Audit Item 7.2.1)
// =============================================================================

// Helper to create a corrupted database with NULL-allowing schema
// This simulates DB corruption, failed migration, or manual edits that bypass constraints
static void create_corrupted_db_with_null_ip(const std::string& path, int null_id) {
    sqlite3* db;
    ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);

    // Create table WITHOUT NOT NULL constraint (simulating corruption/old schema)
    const char* create_sql = R"(
        CREATE TABLE IF NOT EXISTS backends (
            id INTEGER PRIMARY KEY,
            ip TEXT,
            port INTEGER NOT NULL,
            weight INTEGER NOT NULL DEFAULT 100,
            priority INTEGER NOT NULL DEFAULT 0
        )
    )";
    ASSERT_EQ(sqlite3_exec(db, create_sql, nullptr, nullptr, nullptr), SQLITE_OK);

    // Insert a row with NULL ip
    std::string insert_sql = "INSERT INTO backends (id, ip, port, weight, priority) VALUES ("
        + std::to_string(null_id) + ", NULL, 8080, 100, 0)";
    ASSERT_EQ(sqlite3_exec(db, insert_sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);

    sqlite3_close(db);
}

// Test that NULL IP values don't crash and are skipped gracefully
TEST_F(PersistenceTest, BackendWithNullIpIsSkipped) {
    // Create corrupted database first (before opening with our store)
    create_corrupted_db_with_null_ip(test_db_path_, 2);

    // Open with our store - it will try to add columns if needed but won't change existing data
    ASSERT_TRUE(store_->open(test_db_path_));

    // Add a valid backend
    EXPECT_TRUE(store_->save_backend(1, "192.168.1.100", 11434));

    // Close and reopen to ensure fresh load
    store_->close();
    ASSERT_TRUE(store_->open(test_db_path_));

    // Load backends - should only return the valid one, skipping the NULL one
    auto backends = store_->load_backends();
    ASSERT_EQ(backends.size(), 1);
    EXPECT_EQ(backends[0].id, 1);
    EXPECT_EQ(backends[0].ip, "192.168.1.100");

    // Verify that we tracked the skipped record
    EXPECT_GE(store_->last_load_skipped_count(), 1);
}

// Test that empty string IP values are also skipped
TEST_F(PersistenceTest, BackendWithEmptyIpIsSkipped) {
    ASSERT_TRUE(store_->open(test_db_path_));

    // Add a valid backend
    EXPECT_TRUE(store_->save_backend(1, "10.0.0.1", 8080));

    // Close store, then manually insert an empty-string IP backend
    store_->close();

    sqlite3* db;
    ASSERT_EQ(sqlite3_open(test_db_path_.c_str(), &db), SQLITE_OK);
    // Empty string is allowed by NOT NULL constraint
    const char* sql = "INSERT INTO backends (id, ip, port, weight, priority) VALUES (2, '', 8080, 100, 0)";
    ASSERT_EQ(sqlite3_exec(db, sql, nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(db);

    // Reopen and verify
    ASSERT_TRUE(store_->open(test_db_path_));

    // Load backends - should only return the valid one
    auto backends = store_->load_backends();
    ASSERT_EQ(backends.size(), 1);
    EXPECT_EQ(backends[0].id, 1);
    EXPECT_EQ(backends[0].ip, "10.0.0.1");
}

// Helper to create a corrupted database with multiple NULL IPs
static void create_corrupted_db_with_multiple_nulls(const std::string& path) {
    sqlite3* db;
    ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);

    // Create table WITHOUT NOT NULL constraint
    const char* create_sql = R"(
        CREATE TABLE IF NOT EXISTS backends (
            id INTEGER PRIMARY KEY,
            ip TEXT,
            port INTEGER NOT NULL,
            weight INTEGER NOT NULL DEFAULT 100,
            priority INTEGER NOT NULL DEFAULT 0
        )
    )";
    ASSERT_EQ(sqlite3_exec(db, create_sql, nullptr, nullptr, nullptr), SQLITE_OK);

    // Insert rows with NULL/empty ips
    ASSERT_EQ(sqlite3_exec(db, "INSERT INTO backends (id, ip, port, weight, priority) VALUES (2, NULL, 8080, 100, 0)", nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "INSERT INTO backends (id, ip, port, weight, priority) VALUES (3, NULL, 8080, 100, 0)", nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "INSERT INTO backends (id, ip, port, weight, priority) VALUES (4, '', 8080, 100, 0)", nullptr, nullptr, nullptr), SQLITE_OK);

    sqlite3_close(db);
}

// Test multiple NULL values are all skipped correctly
TEST_F(PersistenceTest, MultipleBackendsWithNullIpAreSkipped) {
    // Create corrupted database first
    create_corrupted_db_with_multiple_nulls(test_db_path_);

    // Open with our store
    ASSERT_TRUE(store_->open(test_db_path_));

    // Add valid backends
    EXPECT_TRUE(store_->save_backend(1, "192.168.1.1", 8080));
    EXPECT_TRUE(store_->save_backend(5, "192.168.1.5", 8080));

    // Close and reopen
    store_->close();
    ASSERT_TRUE(store_->open(test_db_path_));

    // Load backends - should only return the 2 valid ones
    auto backends = store_->load_backends();
    ASSERT_EQ(backends.size(), 2);

    // Verify valid backends are present
    bool found_1 = false, found_5 = false;
    for (const auto& b : backends) {
        if (b.id == 1) found_1 = true;
        if (b.id == 5) found_5 = true;
    }
    EXPECT_TRUE(found_1);
    EXPECT_TRUE(found_5);

    // Verify skipped count includes the 3 invalid records
    EXPECT_GE(store_->last_load_skipped_count(), 3);
}

// =============================================================================
// Persistence Returns Raw Data Tests (Rule #7 - No Business Logic in Persistence)
// =============================================================================
// The persistence layer must return raw data without business validation.
// Filtering of backend_id <= 0 is the service/application layer's responsibility.

TEST_F(PersistenceTest, LoadRoutesReturnsZeroBackendId) {
    ASSERT_TRUE(store_->open(test_db_path_));

    std::vector<TokenId> tokens = {100, 200, 300};
    EXPECT_TRUE(store_->save_route(tokens, 0));

    auto routes = store_->load_routes();
    ASSERT_EQ(routes.size(), 1);
    EXPECT_EQ(routes[0].tokens, tokens);
    EXPECT_EQ(routes[0].backend_id, 0);
}

TEST_F(PersistenceTest, LoadRoutesReturnsNegativeBackendId) {
    ASSERT_TRUE(store_->open(test_db_path_));

    // Directly insert a route with negative backend_id via SQL
    // (save_route uses BackendId=int32_t which supports negative values)
    store_->close();

    sqlite3* db;
    ASSERT_EQ(sqlite3_open(test_db_path_.c_str(), &db), SQLITE_OK);

    // Create tables if needed
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS routes (tokens BLOB PRIMARY KEY, backend_id INTEGER NOT NULL)",
        nullptr, nullptr, nullptr), SQLITE_OK);

    // Insert route with negative backend_id
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "INSERT INTO routes (tokens, backend_id) VALUES (?, ?)", -1, &stmt, nullptr), SQLITE_OK);

    std::vector<TokenId> tokens = {400, 500};
    sqlite3_bind_blob(stmt, 1, tokens.data(), static_cast<int>(tokens.size() * sizeof(TokenId)), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, -5);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    // Reopen with our store and verify raw data is returned
    ASSERT_TRUE(store_->open(test_db_path_));
    auto routes = store_->load_routes();
    ASSERT_EQ(routes.size(), 1);
    EXPECT_EQ(routes[0].tokens, tokens);
    EXPECT_EQ(routes[0].backend_id, -5);
}

TEST_F(PersistenceTest, LoadRoutesReturnsMixOfValidAndInvalidBackendIds) {
    ASSERT_TRUE(store_->open(test_db_path_));

    std::vector<TokenId> tokens1 = {10, 20};
    std::vector<TokenId> tokens2 = {30, 40};
    std::vector<TokenId> tokens3 = {50, 60};
    std::vector<TokenId> tokens4 = {70, 80};

    EXPECT_TRUE(store_->save_route(tokens1, 1));    // valid
    EXPECT_TRUE(store_->save_route(tokens2, 0));    // zero (previously filtered)
    EXPECT_TRUE(store_->save_route(tokens3, -1));   // negative (previously filtered)
    EXPECT_TRUE(store_->save_route(tokens4, 42));   // valid

    auto routes = store_->load_routes();
    ASSERT_EQ(routes.size(), 4);

    // All routes should be returned — persistence does not validate backend_id
    std::set<BackendId> backend_ids;
    for (const auto& r : routes) {
        backend_ids.insert(r.backend_id);
    }
    EXPECT_TRUE(backend_ids.count(1));
    EXPECT_TRUE(backend_ids.count(0));
    EXPECT_TRUE(backend_ids.count(-1));
    EXPECT_TRUE(backend_ids.count(42));

    // No records should be counted as skipped — these are valid persistence records
    EXPECT_EQ(store_->last_load_skipped_count(), 0);
}
