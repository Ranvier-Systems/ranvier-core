#pragma once

#include "persistence.hpp"
#include <sqlite3.h>
#include <mutex>

namespace ranvier {

class SqlitePersistence : public PersistenceStore {
public:
    SqlitePersistence() = default;
    ~SqlitePersistence() override;

    // Non-copyable, non-movable (owns sqlite3* handle)
    SqlitePersistence(const SqlitePersistence&) = delete;
    SqlitePersistence& operator=(const SqlitePersistence&) = delete;
    SqlitePersistence(SqlitePersistence&&) = delete;
    SqlitePersistence& operator=(SqlitePersistence&&) = delete;

    // Lifecycle
    bool open(const std::string& path) override;
    void close() override;
    bool is_open() const override;

    // Backend operations
    bool save_backend(BackendId id, const std::string& ip, uint16_t port,
                      uint32_t weight = 100, uint32_t priority = 0) override;
    bool remove_backend(BackendId id) override;
    std::vector<BackendRecord> load_backends() override;

    // Route operations
    bool save_route(std::span<const TokenId> tokens, BackendId backend_id) override;
    bool remove_route(std::span<const TokenId> tokens) override;
    bool remove_routes_for_backend(BackendId backend_id) override;
    std::vector<RouteRecord> load_routes() override;

    // Bulk operations
    bool save_routes_batch(const std::vector<RouteRecord>& routes) override;

    // Maintenance
    bool clear_all() override;
    size_t route_count() override;
    size_t backend_count() override;

    // Crash recovery support
    bool checkpoint() override;
    bool verify_integrity() override;
    size_t last_load_skipped_count() const override;

private:
    bool create_tables();
    bool exec_sql(const char* sql);

    // Serialize/deserialize token vectors to/from BLOB
    static std::vector<uint8_t> serialize_tokens(std::span<const TokenId> tokens);
    static std::vector<TokenId> deserialize_tokens(const void* data, size_t size);

    sqlite3* _db = nullptr;
    mutable std::mutex _mutex;  // Thread safety for SQLite operations
    size_t _last_load_skipped_count = 0;  // Count of corrupted records skipped during load
};

} // namespace ranvier
