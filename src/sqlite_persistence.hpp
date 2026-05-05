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
 * THREADING MODEL (Hard Rule #12):
 * This class wraps blocking sqlite3_* calls. All public methods MUST be
 * called from a dedicated OS worker thread, NEVER from a reactor thread.
 * The mutex protects against accidental concurrent access; the calls
 * themselves still block, so they must run off-reactor.
 *
 * DO NOT call these methods directly from HttpController, RouterService,
 * or any other reactor-thread code. Use AsyncPersistenceManager — it owns
 * a std::thread worker that drains the persistence queue and invokes the
 * methods below directly. seastar::async() is NOT a valid offload mechanism:
 * it runs on the reactor (a stackful coroutine, not a thread-pool dispatch),
 * so wrapping these calls in seastar::async would still stall the shard.
 *
 * The mutex is SAFE here because:
 *   1. The worker thread is a real kernel thread, not a Seastar fiber, so
 *      blocking on the mutex / on SQLite I/O does not block any reactor.
 *   2. AsyncPersistenceManager has a single worker, so contention on the
 *      mutex is effectively only with the recovery-path callers (load_*,
 *      checkpoint, verify_integrity) which run during startup/shutdown.
 *   3. The reactor thread never acquires this mutex.
 *
 * CORRECT CALL PATTERN:
 *   HttpController → AsyncPersistenceManager → MPSC ring → worker std::thread
 *                  (reactor, lock-free)                  ↓
 *                                              SqlitePersistence (blocking)
 *
 * DANGEROUS PATTERN (must not exist):
 *   HttpController → SqlitePersistence::method()  ← BLOCKS REACTOR!
 *
 * @see AsyncPersistenceManager for the reactor-safe wrapper.
 * @see src/tokenizer_thread_pool.cpp for the worker-thread + alien pattern.
 * @see .dev-context/claude-context.md Hard Rule #12.
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
    // Public Interface (called from the persistence worker thread only)
    // -------------------------------------------------------------------------
    // WARNING: All methods below acquire _mutex and may block.
    // Only call from AsyncPersistenceManager's dedicated std::thread worker,
    // or from startup/shutdown recovery paths. NEVER from a reactor thread
    // (including from inside seastar::async — that runs on the reactor too).

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
