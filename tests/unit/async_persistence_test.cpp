#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "async_persistence.hpp"
#include "sqlite_persistence.hpp"
#include "mocks/mock_persistence_store.hpp"
#include <filesystem>
#include <thread>
#include <chrono>

using namespace ranvier;
using ::testing::NiceMock;

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
    // Tests create their own stores via the new constructor
};

TEST_F(AsyncPersistenceQueueTest, QueueWithoutOpenStoreDoesNothing) {
    AsyncPersistenceConfig config;
    config.max_queue_depth = 100;
    // Create manager with mock store but don't open it
    auto store = std::make_unique<NiceMock<MockPersistenceStore>>();
    AsyncPersistenceManager manager(config, std::move(store));
    // Note: store is not opened

    std::vector<TokenId> tokens = {1, 2, 3};
    manager.queue_save_route(tokens, 1);

    // Should not crash, queue_depth should be 0 (store not open)
    EXPECT_EQ(manager.queue_depth(), 0);
}

TEST_F(AsyncPersistenceQueueTest, QueueAcceptsOperations) {
    AsyncPersistenceConfig config;
    config.max_queue_depth = 100;
    auto store = std::make_unique<NiceMock<MockPersistenceStore>>();
    store->open("/tmp/test");
    AsyncPersistenceManager manager(config, std::move(store));

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
    auto store = std::make_unique<NiceMock<MockPersistenceStore>>();
    store->open("/tmp/test");
    AsyncPersistenceManager manager(config, std::move(store));

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

TEST_F(AsyncPersistenceQueueTest, ClearAllEnqueuesClearOp) {
    // queue_clear_all() bumps a generation counter and enqueues a ClearAllOp.
    // The consumer (process_batch) will skip stale ops before the ClearAllOp.
    // The queue_depth() includes both stale and new ops until consumed.
    AsyncPersistenceConfig config;
    config.max_queue_depth = 100;
    auto store = std::make_unique<NiceMock<MockPersistenceStore>>();
    store->open("/tmp/test");
    AsyncPersistenceManager manager(config, std::move(store));

    // Queue some operations
    for (int i = 0; i < 10; ++i) {
        std::vector<TokenId> tokens = {static_cast<TokenId>(i)};
        manager.queue_save_route(tokens, i);
    }

    EXPECT_EQ(manager.queue_depth(), 10);

    // Clear all enqueues ClearAllOp (stale ops remain until consumed)
    manager.queue_clear_all();

    EXPECT_EQ(manager.queue_depth(), 11);  // 10 stale + 1 ClearAllOp
}

TEST_F(AsyncPersistenceQueueTest, ClearAllBypassesBackpressure) {
    // queue_clear_all() must succeed even when the queue is at the
    // backpressure limit. It bypasses the max_queue_depth check and
    // pushes directly to the ring buffer (which has extra capacity
    // from power-of-two rounding).
    AsyncPersistenceConfig config;
    config.max_queue_depth = 5;
    auto store = std::make_unique<NiceMock<MockPersistenceStore>>();
    store->open("/tmp/test");
    AsyncPersistenceManager manager(config, std::move(store));

    // Fill the queue to backpressure limit
    for (int i = 0; i < 5; ++i) {
        std::vector<TokenId> tokens = {static_cast<TokenId>(i)};
        manager.queue_save_route(tokens, i);
    }

    EXPECT_TRUE(manager.is_backpressured());

    // Clear all should still enqueue the ClearAllOp
    manager.queue_clear_all();

    EXPECT_EQ(manager.queue_depth(), 6);  // 5 stale + 1 ClearAllOp
}

TEST_F(AsyncPersistenceQueueTest, IsOpenReflectsStoreState) {
    AsyncPersistenceConfig config;
    auto store = std::make_unique<NiceMock<MockPersistenceStore>>();
    auto* store_ptr = store.get();
    AsyncPersistenceManager manager(config, std::move(store));

    EXPECT_FALSE(manager.is_open());

    store_ptr->open("/tmp/test");
    EXPECT_TRUE(manager.is_open());
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
    auto store = std::make_unique<NiceMock<MockPersistenceStore>>();
    store->open("/tmp/test");
    AsyncPersistenceManager manager(config, std::move(store));

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
    auto store = std::make_unique<NiceMock<MockPersistenceStore>>();
    store->open("/tmp/test");
    AsyncPersistenceManager manager(config, std::move(store));

    std::vector<TokenId> empty_tokens;
    manager.queue_save_route(empty_tokens, 1);

    EXPECT_EQ(manager.queue_depth(), 1);
}

TEST_F(AsyncPersistenceQueueTest, SpanToVectorConversion) {
    AsyncPersistenceConfig config;
    config.max_queue_depth = 100;
    auto store = std::make_unique<NiceMock<MockPersistenceStore>>();
    store->open("/tmp/test");
    AsyncPersistenceManager manager(config, std::move(store));

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
    // Multiple producer threads enqueue concurrently (MPSC queue).
    // Validates that all ops are accepted when queue has sufficient capacity.
    AsyncPersistenceConfig config;
    config.max_queue_depth = 100000;  // Large enough for test
    auto store = std::make_unique<NiceMock<MockPersistenceStore>>();
    store->open("/tmp/test");
    AsyncPersistenceManager manager(config, std::move(store));

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
    // Multiple producer threads exceed max_queue_depth.
    // Validates backpressure drops excess operations.
    AsyncPersistenceConfig config;
    config.max_queue_depth = 100;  // Small queue to trigger backpressure
    auto store = std::make_unique<NiceMock<MockPersistenceStore>>();
    store->open("/tmp/test");
    AsyncPersistenceManager manager(config, std::move(store));

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
    auto store = std::make_unique<NiceMock<MockPersistenceStore>>();
    store->open("/tmp/test");
    AsyncPersistenceManager manager(config, std::move(store));

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
    auto store = std::make_unique<NiceMock<MockPersistenceStore>>();
    store->open("/tmp/test");
    AsyncPersistenceManager manager(config, std::move(store));

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
        if (store_) {
            store_->close();
        }
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
    AsyncPersistenceManager manager(config, std::move(store_));

    // Queue operations
    std::vector<TokenId> tokens = {1, 2, 3};
    manager.queue_save_route(tokens, 1);
    manager.queue_save_backend(1, "127.0.0.1", 8080, 100, 0);

    EXPECT_EQ(manager.queue_depth(), 2);
    EXPECT_TRUE(manager.is_open());
}

TEST_F(AsyncPersistenceIntegrationTest, DelegatedReadMethodsWork) {
    AsyncPersistenceConfig config;
    AsyncPersistenceManager manager(config, std::move(store_));

    // The manager should delegate reads to the underlying store
    EXPECT_TRUE(manager.is_open());

    // Initially empty
    auto backends = manager.load_backends();
    EXPECT_EQ(backends.size(), 0);

    auto routes = manager.load_routes();
    EXPECT_EQ(routes.size(), 0);

    // Use the new API to verify integrity and counts
    EXPECT_TRUE(manager.verify_integrity());
    EXPECT_EQ(manager.backend_count(), 0);
    EXPECT_EQ(manager.route_count(), 0);
}

// =============================================================================
// Ownership and Lifecycle Tests
// =============================================================================

TEST(AsyncPersistenceOwnershipTest, ManagerOwnsStoreViaFactory) {
    // Test production constructor uses factory and owns the store
    AsyncPersistenceConfig config;
    AsyncPersistenceManager manager(config);

    // Store is created but not opened yet
    EXPECT_FALSE(manager.is_open());

    // Operations on closed store should be no-ops
    std::vector<TokenId> tokens = {1, 2, 3};
    manager.queue_save_route(tokens, 1);
    EXPECT_EQ(manager.queue_depth(), 0);  // Not queued because store not open
}

TEST(AsyncPersistenceOwnershipTest, ManagerOwnsInjectedStore) {
    // Test that manager takes ownership of injected store
    AsyncPersistenceConfig config;
    auto store = std::make_unique<NiceMock<MockPersistenceStore>>();
    auto* store_ptr = store.get();

    // Manager takes ownership
    AsyncPersistenceManager manager(config, std::move(store));

    // Original unique_ptr should be null
    EXPECT_EQ(store.get(), nullptr);

    // Store should be accessible via manager
    store_ptr->open("/tmp/test");
    EXPECT_TRUE(manager.is_open());
}

TEST(AsyncPersistenceOwnershipTest, OpenAndCloseLifecycle) {
    std::string test_db = "/tmp/async_persist_lifecycle_test.db";

    // Cleanup any previous test artifacts
    std::remove(test_db.c_str());
    std::remove((test_db + "-wal").c_str());
    std::remove((test_db + "-shm").c_str());

    AsyncPersistenceConfig config;
    AsyncPersistenceManager manager(config);

    // Initially not open
    EXPECT_FALSE(manager.is_open());

    // Open should succeed
    EXPECT_TRUE(manager.open(test_db));
    EXPECT_TRUE(manager.is_open());

    // Close
    manager.close();
    EXPECT_FALSE(manager.is_open());

    // Close should be idempotent
    manager.close();  // Should not crash
    EXPECT_FALSE(manager.is_open());

    // Cleanup
    std::remove(test_db.c_str());
    std::remove((test_db + "-wal").c_str());
    std::remove((test_db + "-shm").c_str());
}

TEST(AsyncPersistenceOwnershipTest, CloseIsIdempotent) {
    AsyncPersistenceConfig config;
    auto store = std::make_unique<NiceMock<MockPersistenceStore>>();
    store->open("/tmp/test");
    AsyncPersistenceManager manager(config, std::move(store));

    EXPECT_TRUE(manager.is_open());

    // First close
    manager.close();
    EXPECT_FALSE(manager.is_open());

    // Second close should not crash
    manager.close();
    EXPECT_FALSE(manager.is_open());

    // Third close should not crash
    manager.close();
    EXPECT_FALSE(manager.is_open());
}

TEST(AsyncPersistenceOwnershipTest, DelegatedMaintenanceMethods) {
    AsyncPersistenceConfig config;
    auto store = std::make_unique<NiceMock<MockPersistenceStore>>();
    store->open("/tmp/test");
    AsyncPersistenceManager manager(config, std::move(store));

    // Test delegated maintenance methods
    EXPECT_TRUE(manager.checkpoint());
    EXPECT_TRUE(manager.verify_integrity());
    EXPECT_TRUE(manager.clear_all());
    EXPECT_EQ(manager.last_load_skipped_count(), 0);
}

TEST(AsyncPersistenceOwnershipTest, DelegatedMethodsReturnDefaultsWhenClosed) {
    AsyncPersistenceConfig config;
    AsyncPersistenceManager manager(config);
    // Note: not opened

    // Read operations should return empty/defaults
    auto backends = manager.load_backends();
    EXPECT_TRUE(backends.empty());

    auto routes = manager.load_routes();
    EXPECT_TRUE(routes.empty());

    EXPECT_EQ(manager.backend_count(), 0);
    EXPECT_EQ(manager.route_count(), 0);
    EXPECT_EQ(manager.last_load_skipped_count(), 0);

    // Maintenance operations should return false
    EXPECT_FALSE(manager.checkpoint());
    EXPECT_FALSE(manager.verify_integrity());
    EXPECT_FALSE(manager.clear_all());
}

// =============================================================================
// max_queue_depth() accessor tests
// =============================================================================

TEST(AsyncPersistence, MaxQueueDepthReturnsConfiguredValue) {
    AsyncPersistenceConfig config;
    config.max_queue_depth = 50000;  // Custom value

    AsyncPersistenceManager manager(config, std::make_unique<NiceMock<MockPersistenceStore>>());
    EXPECT_EQ(manager.max_queue_depth(), 50000);
}

TEST(AsyncPersistence, MaxQueueDepthReturnsDefaultValue) {
    AsyncPersistenceConfig config;  // Default max_queue_depth = 100000

    AsyncPersistenceManager manager(config, std::make_unique<NiceMock<MockPersistenceStore>>());
    EXPECT_EQ(manager.max_queue_depth(), 100000);
}
