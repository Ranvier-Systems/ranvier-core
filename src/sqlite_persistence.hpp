#pragma once

#include "persistence.hpp"
#include <sqlite3.h>
#include <mutex>

namespace ranvier {

// Forward declaration for friend access
class AsyncPersistenceManager;

/**
 * @brief SQLite-backed persistence store for routes and backends.
 *
 * THREADING MODEL:
 * This class uses std::mutex for thread safety because SQLite is not
 * thread-safe. All public methods MUST be called from seastar::async
 * context, NOT from the reactor thread.
 *
 * DO NOT call these methods directly from HttpController, RouterService,
 * or any other reactor-thread code. Use AsyncPersistenceManager instead,
 * which wraps all calls in seastar::async to run on a thread pool.
 *
 * The mutex is SAFE here because:
 *   1. seastar::async runs on a separate thread pool (not the reactor)
 *   2. AsyncPersistenceManager serializes batch processing via semaphore
 *   3. The reactor thread never blocks on these mutexes
 *
 * CORRECT CALL PATTERN:
 *   HttpController → AsyncPersistenceManager → seastar::async → SqlitePersistence
 *                                                    ↓
 *                                          (thread pool, mutex OK)
 *
 * DANGEROUS PATTERN (must not exist):
 *   HttpController → SqlitePersistence::method()  ← BLOCKS REACTOR!
 *
 * @see AsyncPersistenceManager for the reactor-safe wrapper.
 * @see docs/claude-context.md "No Locks/Async Only" section.
 */
class SqlitePersistence : public PersistenceStore {
    // Only AsyncPersistenceManager (and factory function) may construct instances
    friend class AsyncPersistenceManager;
    friend std::unique_ptr<PersistenceStore> create_persistence_store();

private:
    // Private constructor - only accessible via friend classes
    // This enforces that SqlitePersistence can only be created through
    // AsyncPersistenceManager or the create_persistence_store() factory.
    SqlitePersistence() = default;

public:
    ~SqlitePersistence() override;

    // Non-copyable, non-movable (owns sqlite3* handle)
    SqlitePersistence(const SqlitePersistence&) = delete;
    SqlitePersistence& operator=(const SqlitePersistence&) = delete;
    SqlitePersistence(SqlitePersistence&&) = delete;
    SqlitePersistence& operator=(SqlitePersistence&&) = delete;

    // -------------------------------------------------------------------------
    // Public Interface (called from seastar::async context only)
    // -------------------------------------------------------------------------
    // WARNING: All methods below acquire _mutex and may block.
    // Only call from AsyncPersistenceManager within seastar::async context.

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

    // Per-API-key attribution (memo §7)
    bool log_request(const RequestAttributionRecord& rec) override;
    size_t request_attribution_count() override;
    size_t prune_request_attribution(uint32_t max_rows) override;
    std::vector<RequestAttributionRecord> query_request_attribution(
        int64_t from_ms, int64_t to_ms, const std::string& api_key_id_filter,
        size_t row_limit) override;

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
