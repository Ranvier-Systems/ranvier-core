#include <gtest/gtest.h>
#include "async_persistence.hpp"
#include "sqlite_persistence.hpp"
#include <filesystem>
#include <thread>
#include <chrono>

using namespace ranvier;

// =============================================================================
// Mock Persistence Store
// =============================================================================
// A simple mock that records operations for verification

class MockPersistenceStore : public PersistenceStore {
public:
    bool open(const std::string& /*path*/) override {
        _is_open = true;
        return true;
    }

    void close() override {
        _is_open = false;
    }

    bool is_open() const override {
        return _is_open;
    }

    bool save_backend(BackendId id, const std::string& ip, uint16_t port,
                      uint32_t weight, uint32_t priority) override {
        std::lock_guard<std::mutex> lock(_mutex);
        _backends_saved.push_back({id, ip, port, weight, priority});
        return true;
    }

    bool remove_backend(BackendId id) override {
        std::lock_guard<std::mutex> lock(_mutex);
        _backends_removed.push_back(id);
        return true;
    }

    std::vector<BackendRecord> load_backends() override {
        return {};
    }

    bool save_route(std::span<const TokenId> tokens, BackendId backend_id) override {
        std::lock_guard<std::mutex> lock(_mutex);
        RouteRecord record;
        record.tokens = std::vector<TokenId>(tokens.begin(), tokens.end());
        record.backend_id = backend_id;
        _routes_saved.push_back(std::move(record));
        return true;
    }

    bool remove_route(std::span<const TokenId> /*tokens*/) override {
        return true;
    }

    bool remove_routes_for_backend(BackendId backend_id) override {
        std::lock_guard<std::mutex> lock(_mutex);
        _routes_removed_for_backend.push_back(backend_id);
        return true;
    }

    std::vector<RouteRecord> load_routes() override {
        return {};
    }

    bool save_routes_batch(const std::vector<RouteRecord>& routes) override {
        std::lock_guard<std::mutex> lock(_mutex);
        _batch_save_count++;
        _total_routes_in_batches += routes.size();
        for (const auto& r : routes) {
            _routes_saved.push_back(r);
        }
        return true;
    }

    bool clear_all() override {
        std::lock_guard<std::mutex> lock(_mutex);
        _clear_all_count++;
        return true;
    }

    size_t route_count() override { return 0; }
    size_t backend_count() override { return 0; }

    bool checkpoint() override { return true; }
    bool verify_integrity() override { return true; }
    size_t last_load_skipped_count() const override { return 0; }

    // Accessors for verification
    size_t get_routes_saved_count() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _routes_saved.size();
    }

    size_t get_backends_saved_count() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _backends_saved.size();
    }

    size_t get_batch_save_count() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _batch_save_count;
    }

    size_t get_total_routes_in_batches() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _total_routes_in_batches;
    }

    size_t get_clear_all_count() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _clear_all_count;
    }

    size_t get_routes_removed_for_backend_count() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _routes_removed_for_backend.size();
    }

    const std::vector<RouteRecord>& get_routes_saved() const {
        return _routes_saved;
    }

private:
    bool _is_open = false;
    mutable std::mutex _mutex;

    std::vector<RouteRecord> _routes_saved;
    std::vector<BackendId> _routes_removed_for_backend;
    std::vector<BackendId> _backends_removed;

    struct BackendSaveRecord {
        BackendId id;
        std::string ip;
        uint16_t port;
        uint32_t weight;
        uint32_t priority;
    };
    std::vector<BackendSaveRecord> _backends_saved;

    size_t _batch_save_count = 0;
    size_t _total_routes_in_batches = 0;
    size_t _clear_all_count = 0;
};

// =============================================================================
// Configuration Tests
// =============================================================================

TEST(AsyncPersistenceConfigTest, DefaultValues) {
    AsyncPersistenceConfig config;

    EXPECT_EQ(config.flush_interval, std::chrono::milliseconds(100));
    EXPECT_EQ(config.max_batch_size, 1000);
    EXPECT_EQ(config.max_queue_depth, 100000);
    EXPECT_TRUE(config.enable_stats_logging);
    EXPECT_EQ(config.stats_interval, std::chrono::seconds(60));
}

TEST(AsyncPersistenceConfigTest, CustomValues) {
    AsyncPersistenceConfig config;
    config.flush_interval = std::chrono::milliseconds(50);
    config.max_batch_size = 500;
    config.max_queue_depth = 50000;
    config.enable_stats_logging = false;
    config.stats_interval = std::chrono::seconds(30);

    EXPECT_EQ(config.flush_interval, std::chrono::milliseconds(50));
    EXPECT_EQ(config.max_batch_size, 500);
    EXPECT_EQ(config.max_queue_depth, 50000);
    EXPECT_FALSE(config.enable_stats_logging);
    EXPECT_EQ(config.stats_interval, std::chrono::seconds(30));
}

// =============================================================================
// Operation Struct Tests
// =============================================================================

TEST(PersistenceOpTest, SaveRouteOp) {
    SaveRouteOp op;
    op.tokens = {100, 200, 300};
    op.backend_id = 42;

    EXPECT_EQ(op.tokens.size(), 3);
    EXPECT_EQ(op.tokens[0], 100);
    EXPECT_EQ(op.backend_id, 42);
}

TEST(PersistenceOpTest, SaveBackendOp) {
    SaveBackendOp op{1, "192.168.1.100", 8080, 200, 1};

    EXPECT_EQ(op.id, 1);
    EXPECT_EQ(op.ip, "192.168.1.100");
    EXPECT_EQ(op.port, 8080);
    EXPECT_EQ(op.weight, 200);
    EXPECT_EQ(op.priority, 1);
}

TEST(PersistenceOpTest, RemoveBackendOp) {
    RemoveBackendOp op{42};
    EXPECT_EQ(op.id, 42);
}

TEST(PersistenceOpTest, RemoveRoutesForBackendOp) {
    RemoveRoutesForBackendOp op{123};
    EXPECT_EQ(op.backend_id, 123);
}

TEST(PersistenceOpTest, VariantHoldsCorrectType) {
    PersistenceOp op1 = SaveRouteOp{{1, 2, 3}, 1};
    PersistenceOp op2 = SaveBackendOp{1, "127.0.0.1", 8080, 100, 0};
    PersistenceOp op3 = RemoveBackendOp{1};
    PersistenceOp op4 = RemoveRoutesForBackendOp{1};
    PersistenceOp op5 = ClearAllOp{};

    EXPECT_TRUE(std::holds_alternative<SaveRouteOp>(op1));
    EXPECT_TRUE(std::holds_alternative<SaveBackendOp>(op2));
    EXPECT_TRUE(std::holds_alternative<RemoveBackendOp>(op3));
    EXPECT_TRUE(std::holds_alternative<RemoveRoutesForBackendOp>(op4));
    EXPECT_TRUE(std::holds_alternative<ClearAllOp>(op5));
}

TEST(PersistenceOpTest, VariantVisit) {
    PersistenceOp op = SaveRouteOp{{100, 200}, 5};

    bool visited = false;
    std::visit([&visited](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, SaveRouteOp>) {
            visited = true;
            EXPECT_EQ(arg.tokens.size(), 2);
            EXPECT_EQ(arg.backend_id, 5);
        }
    }, op);

    EXPECT_TRUE(visited);
}

TEST(PersistenceOpTest, MoveSemantics) {
    std::vector<TokenId> tokens = {1, 2, 3, 4, 5};
    auto* original_data = tokens.data();

    SaveRouteOp op;
    op.tokens = std::move(tokens);

    // After move, op.tokens should have the original data
    EXPECT_EQ(op.tokens.data(), original_data);
    EXPECT_EQ(op.tokens.size(), 5);
}

// =============================================================================
// Queue Behavior Tests (without Seastar runtime)
// =============================================================================
// Note: These tests verify the data structures and logic, but full async
// behavior requires running under Seastar's event loop.

class AsyncPersistenceQueueTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_store_ = std::make_unique<MockPersistenceStore>();
        mock_store_->open("/tmp/test");
    }

    std::unique_ptr<MockPersistenceStore> mock_store_;
};

TEST_F(AsyncPersistenceQueueTest, QueueWithoutStoreDoesNothing) {
    AsyncPersistenceConfig config;
    config.max_queue_depth = 100;
    AsyncPersistenceManager manager(config);
    // Note: set_persistence_store not called

    std::vector<TokenId> tokens = {1, 2, 3};
    manager.queue_save_route(tokens, 1);

    // Should not crash, queue_depth should be 0 (no store set)
    EXPECT_EQ(manager.queue_depth(), 0);
}

TEST_F(AsyncPersistenceQueueTest, QueueAcceptsOperations) {
    AsyncPersistenceConfig config;
    config.max_queue_depth = 100;
    AsyncPersistenceManager manager(config);
    manager.set_persistence_store(mock_store_.get());

    EXPECT_EQ(manager.queue_depth(), 0);

    std::vector<TokenId> tokens = {1, 2, 3};
    manager.queue_save_route(tokens, 1);

    EXPECT_EQ(manager.queue_depth(), 1);

    manager.queue_save_backend(1, "127.0.0.1", 8080, 100, 0);
    EXPECT_EQ(manager.queue_depth(), 2);

    manager.queue_remove_backend(1);
    EXPECT_EQ(manager.queue_depth(), 3);

    manager.queue_remove_routes_for_backend(1);
    EXPECT_EQ(manager.queue_depth(), 4);
}

TEST_F(AsyncPersistenceQueueTest, BackpressureDropsOperations) {
    AsyncPersistenceConfig config;
    config.max_queue_depth = 5;  // Very small queue
    AsyncPersistenceManager manager(config);
    manager.set_persistence_store(mock_store_.get());

    // Fill the queue
    for (int i = 0; i < 5; ++i) {
        std::vector<TokenId> tokens = {static_cast<TokenId>(i)};
        manager.queue_save_route(tokens, i);
    }

    EXPECT_EQ(manager.queue_depth(), 5);
    EXPECT_EQ(manager.operations_dropped(), 0);
    EXPECT_TRUE(manager.is_backpressured());

    // Try to add more - should be dropped
    std::vector<TokenId> tokens = {100};
    manager.queue_save_route(tokens, 100);

    EXPECT_EQ(manager.queue_depth(), 5);  // Still 5
    EXPECT_EQ(manager.operations_dropped(), 1);

    // Add a few more
    manager.queue_save_backend(1, "127.0.0.1", 8080, 100, 0);
    manager.queue_remove_backend(1);

    EXPECT_EQ(manager.operations_dropped(), 3);
}

TEST_F(AsyncPersistenceQueueTest, ClearAllClearsQueue) {
    AsyncPersistenceConfig config;
    config.max_queue_depth = 100;
    AsyncPersistenceManager manager(config);
    manager.set_persistence_store(mock_store_.get());

    // Queue some operations
    for (int i = 0; i < 10; ++i) {
        std::vector<TokenId> tokens = {static_cast<TokenId>(i)};
        manager.queue_save_route(tokens, i);
    }

    EXPECT_EQ(manager.queue_depth(), 10);

    // Clear all should clear the queue and add ClearAllOp
    manager.queue_clear_all();

    EXPECT_EQ(manager.queue_depth(), 1);  // Only ClearAllOp remains
}

TEST_F(AsyncPersistenceQueueTest, ClearAllBypassesBackpressure) {
    AsyncPersistenceConfig config;
    config.max_queue_depth = 5;
    AsyncPersistenceManager manager(config);
    manager.set_persistence_store(mock_store_.get());

    // Fill the queue
    for (int i = 0; i < 5; ++i) {
        std::vector<TokenId> tokens = {static_cast<TokenId>(i)};
        manager.queue_save_route(tokens, i);
    }

    EXPECT_TRUE(manager.is_backpressured());

    // Clear all should still work
    manager.queue_clear_all();

    EXPECT_EQ(manager.queue_depth(), 1);
    EXPECT_FALSE(manager.is_backpressured());
}

TEST_F(AsyncPersistenceQueueTest, UnderlyingStoreAccessible) {
    AsyncPersistenceConfig config;
    AsyncPersistenceManager manager(config);
    manager.set_persistence_store(mock_store_.get());

    EXPECT_EQ(manager.underlying_store(), mock_store_.get());
}

TEST_F(AsyncPersistenceQueueTest, StatisticsInitializedToZero) {
    AsyncPersistenceConfig config;
    AsyncPersistenceManager manager(config);

    EXPECT_EQ(manager.queue_depth(), 0);
    EXPECT_EQ(manager.operations_processed(), 0);
    EXPECT_EQ(manager.operations_dropped(), 0);
    EXPECT_FALSE(manager.is_backpressured());
}

// =============================================================================
// Token Vector Handling Tests
// =============================================================================

TEST_F(AsyncPersistenceQueueTest, LargeTokenVectorQueued) {
    AsyncPersistenceConfig config;
    config.max_queue_depth = 100;
    AsyncPersistenceManager manager(config);
    manager.set_persistence_store(mock_store_.get());

    // Create a large token vector (simulating real LLM context)
    std::vector<TokenId> tokens(10000);
    for (size_t i = 0; i < tokens.size(); ++i) {
        tokens[i] = static_cast<TokenId>(i);
    }

    manager.queue_save_route(tokens, 1);

    EXPECT_EQ(manager.queue_depth(), 1);
}

TEST_F(AsyncPersistenceQueueTest, EmptyTokenVectorQueued) {
    AsyncPersistenceConfig config;
    config.max_queue_depth = 100;
    AsyncPersistenceManager manager(config);
    manager.set_persistence_store(mock_store_.get());

    std::vector<TokenId> empty_tokens;
    manager.queue_save_route(empty_tokens, 1);

    EXPECT_EQ(manager.queue_depth(), 1);
}

TEST_F(AsyncPersistenceQueueTest, SpanToVectorConversion) {
    AsyncPersistenceConfig config;
    config.max_queue_depth = 100;
    AsyncPersistenceManager manager(config);
    manager.set_persistence_store(mock_store_.get());

    // Test with std::span input (simulating the API usage)
    std::vector<TokenId> original = {1, 2, 3, 4, 5};
    std::span<const TokenId> token_span(original);

    manager.queue_save_route(token_span, 1);

    EXPECT_EQ(manager.queue_depth(), 1);

    // Original should be unchanged
    EXPECT_EQ(original.size(), 5);
    EXPECT_EQ(original[0], 1);
}

// =============================================================================
// Concurrent Access Tests
// =============================================================================

TEST_F(AsyncPersistenceQueueTest, ConcurrentQueueOperations) {
    AsyncPersistenceConfig config;
    config.max_queue_depth = 100000;  // Large enough for test
    AsyncPersistenceManager manager(config);
    manager.set_persistence_store(mock_store_.get());

    const int num_threads = 4;
    const int ops_per_thread = 1000;

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&manager, t, ops_per_thread]() {
            for (int i = 0; i < ops_per_thread; ++i) {
                std::vector<TokenId> tokens = {
                    static_cast<TokenId>(t * 10000 + i)
                };
                manager.queue_save_route(tokens, t);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(manager.queue_depth(), num_threads * ops_per_thread);
    EXPECT_EQ(manager.operations_dropped(), 0);
}

TEST_F(AsyncPersistenceQueueTest, ConcurrentQueueWithBackpressure) {
    AsyncPersistenceConfig config;
    config.max_queue_depth = 100;  // Small queue to trigger backpressure
    AsyncPersistenceManager manager(config);
    manager.set_persistence_store(mock_store_.get());

    const int num_threads = 4;
    const int ops_per_thread = 100;

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&manager, t, ops_per_thread]() {
            for (int i = 0; i < ops_per_thread; ++i) {
                std::vector<TokenId> tokens = {
                    static_cast<TokenId>(t * 10000 + i)
                };
                manager.queue_save_route(tokens, t);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Total attempted = 400, max queue = 100
    // So at least 300 should be dropped
    size_t total_ops = manager.queue_depth() + manager.operations_dropped();
    EXPECT_EQ(total_ops, num_threads * ops_per_thread);
    EXPECT_GE(manager.operations_dropped(), 300);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(AsyncPersistenceQueueTest, QueueDepthThreadSafe) {
    AsyncPersistenceConfig config;
    config.max_queue_depth = 10000;
    AsyncPersistenceManager manager(config);
    manager.set_persistence_store(mock_store_.get());

    std::atomic<bool> running{true};
    std::atomic<size_t> max_depth{0};

    // Thread that reads queue_depth
    std::thread reader([&]() {
        while (running) {
            size_t depth = manager.queue_depth();
            size_t current_max = max_depth.load();
            while (depth > current_max && !max_depth.compare_exchange_weak(current_max, depth)) {
                current_max = max_depth.load();
            }
        }
    });

    // Thread that writes
    std::thread writer([&]() {
        for (int i = 0; i < 1000; ++i) {
            std::vector<TokenId> tokens = {static_cast<TokenId>(i)};
            manager.queue_save_route(tokens, 1);
        }
    });

    writer.join();
    running = false;
    reader.join();

    // If there were any race conditions, this would likely crash or give wrong results
    EXPECT_EQ(manager.queue_depth(), 1000);
}

TEST_F(AsyncPersistenceQueueTest, MixedOperationTypes) {
    AsyncPersistenceConfig config;
    config.max_queue_depth = 100;
    AsyncPersistenceManager manager(config);
    manager.set_persistence_store(mock_store_.get());

    // Queue different operation types
    std::vector<TokenId> tokens1 = {1, 2, 3};
    std::vector<TokenId> tokens2 = {4, 5, 6};
    std::vector<TokenId> tokens3 = {7, 8, 9};
    std::vector<TokenId> tokens4 = {10, 11, 12};

    manager.queue_save_route(tokens1, 1);
    manager.queue_save_backend(1, "127.0.0.1", 8080, 100, 0);
    manager.queue_save_route(tokens2, 2);
    manager.queue_remove_backend(1);
    manager.queue_save_route(tokens3, 3);
    manager.queue_remove_routes_for_backend(2);
    manager.queue_save_route(tokens4, 4);

    EXPECT_EQ(manager.queue_depth(), 7);
}

// =============================================================================
// Integration with Real SQLite Store (without Seastar)
// =============================================================================
// These tests verify the queue works with the real SQLite store,
// but without actually running the async flush (which requires Seastar).

class AsyncPersistenceIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_db_path_ = "/tmp/async_persist_test_" +
            std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".db";
        store_ = create_persistence_store();
        store_->open(test_db_path_);
    }

    void TearDown() override {
        store_->close();
        std::remove(test_db_path_.c_str());
        std::remove((test_db_path_ + "-wal").c_str());
        std::remove((test_db_path_ + "-shm").c_str());
    }

    std::string test_db_path_;
    std::unique_ptr<PersistenceStore> store_;
};

TEST_F(AsyncPersistenceIntegrationTest, QueueWithRealStore) {
    AsyncPersistenceConfig config;
    config.max_queue_depth = 100;
    AsyncPersistenceManager manager(config);
    manager.set_persistence_store(store_.get());

    // Queue operations
    std::vector<TokenId> tokens = {1, 2, 3};
    manager.queue_save_route(tokens, 1);
    manager.queue_save_backend(1, "127.0.0.1", 8080, 100, 0);

    EXPECT_EQ(manager.queue_depth(), 2);

    // underlying_store should be the real store
    EXPECT_EQ(manager.underlying_store(), store_.get());
    EXPECT_TRUE(manager.underlying_store()->is_open());
}

TEST_F(AsyncPersistenceIntegrationTest, DirectStoreAccessStillWorks) {
    AsyncPersistenceConfig config;
    AsyncPersistenceManager manager(config);
    manager.set_persistence_store(store_.get());

    // Can still use underlying store directly for reads
    auto* underlying = manager.underlying_store();
    ASSERT_NE(underlying, nullptr);

    // Write directly (simulating startup load)
    underlying->save_backend(1, "192.168.1.1", 11434, 100, 0);
    underlying->save_route({100, 200, 300}, 1);

    // Verify writes persisted
    auto backends = underlying->load_backends();
    EXPECT_EQ(backends.size(), 1);

    auto routes = underlying->load_routes();
    EXPECT_EQ(routes.size(), 1);
}
