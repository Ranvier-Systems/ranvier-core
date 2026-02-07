// Ranvier Core - Google Mock for PersistenceStore
//
// GMock-based mock for the PersistenceStore interface (src/persistence.hpp).
// Replaces the hand-rolled MockPersistenceStore that was in
// async_persistence_test.cpp with MOCK_METHOD declarations.
//
// Default behaviors (set via ON_CALL in constructor):
//   - open()/close()/is_open() track open/closed state
//   - Write methods (save_*, remove_*, clear_all) return true
//   - Read methods (load_*) return empty vectors
//   - Maintenance methods (checkpoint, verify_integrity) return true
//
// Tests can override any default with EXPECT_CALL as needed.
// Use NiceMock<MockPersistenceStore> to suppress uninteresting-call warnings.
//
// Usage:
//   #include "mocks/mock_persistence_store.hpp"
//   using ::testing::NiceMock;
//   auto store = std::make_unique<NiceMock<MockPersistenceStore>>();
//   store->open("/tmp/test");
//   EXPECT_CALL(*store, save_backend(_, _, _, _, _)).WillOnce(Return(true));

#pragma once

#include "persistence.hpp"
#include <gmock/gmock.h>

namespace ranvier {

class MockPersistenceStore : public PersistenceStore {
public:
    MockPersistenceStore() {
        using ::testing::_;
        using ::testing::Return;
        using ::testing::Invoke;

        // Lifecycle: track open/closed state
        ON_CALL(*this, open(_))
            .WillByDefault(Invoke([this](const std::string&) {
                _open = true;
                return true;
            }));
        ON_CALL(*this, close())
            .WillByDefault(Invoke([this]() { _open = false; }));
        ON_CALL(*this, is_open())
            .WillByDefault(Invoke([this]() { return _open; }));

        // Write operations return success by default
        ON_CALL(*this, save_backend(_, _, _, _, _)).WillByDefault(Return(true));
        ON_CALL(*this, remove_backend(_)).WillByDefault(Return(true));
        ON_CALL(*this, save_route(_, _)).WillByDefault(Return(true));
        ON_CALL(*this, remove_route(_)).WillByDefault(Return(true));
        ON_CALL(*this, remove_routes_for_backend(_)).WillByDefault(Return(true));
        ON_CALL(*this, save_routes_batch(_)).WillByDefault(Return(true));
        ON_CALL(*this, clear_all()).WillByDefault(Return(true));

        // Read operations return empty by default
        ON_CALL(*this, load_backends())
            .WillByDefault(Return(std::vector<BackendRecord>{}));
        ON_CALL(*this, load_routes())
            .WillByDefault(Return(std::vector<RouteRecord>{}));

        // Counts return zero by default
        ON_CALL(*this, route_count()).WillByDefault(Return(size_t{0}));
        ON_CALL(*this, backend_count()).WillByDefault(Return(size_t{0}));
        ON_CALL(*this, last_load_skipped_count()).WillByDefault(Return(size_t{0}));

        // Maintenance returns success by default
        ON_CALL(*this, checkpoint()).WillByDefault(Return(true));
        ON_CALL(*this, verify_integrity()).WillByDefault(Return(true));
    }

    // Lifecycle
    MOCK_METHOD(bool, open, (const std::string& path), (override));
    MOCK_METHOD(void, close, (), (override));
    MOCK_METHOD(bool, is_open, (), (const, override));

    // Backend operations
    MOCK_METHOD(bool, save_backend,
                (BackendId id, const std::string& ip, uint16_t port,
                 uint32_t weight, uint32_t priority),
                (override));
    MOCK_METHOD(bool, remove_backend, (BackendId id), (override));
    MOCK_METHOD(std::vector<BackendRecord>, load_backends, (), (override));

    // Route operations
    MOCK_METHOD(bool, save_route,
                (std::span<const TokenId> tokens, BackendId backend_id),
                (override));
    MOCK_METHOD(bool, remove_route,
                (std::span<const TokenId> tokens),
                (override));
    MOCK_METHOD(bool, remove_routes_for_backend, (BackendId backend_id), (override));
    MOCK_METHOD(std::vector<RouteRecord>, load_routes, (), (override));

    // Bulk operations
    MOCK_METHOD(bool, save_routes_batch,
                (const std::vector<RouteRecord>& routes),
                (override));

    // Maintenance
    MOCK_METHOD(bool, clear_all, (), (override));
    MOCK_METHOD(size_t, route_count, (), (override));
    MOCK_METHOD(size_t, backend_count, (), (override));

    // Crash recovery
    MOCK_METHOD(bool, checkpoint, (), (override));
    MOCK_METHOD(bool, verify_integrity, (), (override));
    MOCK_METHOD(size_t, last_load_skipped_count, (), (const, override));

private:
    bool _open = false;
};

}  // namespace ranvier
