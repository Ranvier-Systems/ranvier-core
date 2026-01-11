#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "types.hpp"

namespace ranvier {

/**
 * @file persistence.hpp
 * @brief Abstract interface for persistence backends.
 *
 * WARNING: Do not use PersistenceStore implementations directly from
 * reactor-thread code. All persistence operations must go through
 * AsyncPersistenceManager to avoid blocking the Seastar reactor.
 *
 * @see AsyncPersistenceManager for reactor-safe access.
 * @see SqlitePersistence for the default SQLite implementation.
 */

// Record types for bulk loading
struct BackendRecord {
    BackendId id;
    std::string ip;
    uint16_t port;
    uint32_t weight = 100;    // Relative weight for load balancing (default 100)
    uint32_t priority = 0;    // Priority group (0 = highest, higher = lower priority)
};

struct RouteRecord {
    std::vector<TokenId> tokens;
    BackendId backend_id;
};

/**
 * @brief Abstract interface for persistence backends.
 *
 * This allows swapping SQLite for RocksDB or other stores in the future.
 *
 * IMPORTANT - THREADING CONTRACT:
 * Implementations may use blocking operations (mutexes, I/O). Callers MUST
 * invoke these methods from seastar::async context, NOT from the reactor thread.
 * Use AsyncPersistenceManager as the reactor-safe wrapper.
 *
 * @see AsyncPersistenceManager for reactor-safe access patterns.
 */
class PersistenceStore {
public:
    virtual ~PersistenceStore() = default;

    // Lifecycle
    virtual bool open(const std::string& path) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    // Backend operations
    virtual bool save_backend(BackendId id, const std::string& ip, uint16_t port,
                              uint32_t weight = 100, uint32_t priority = 0) = 0;
    virtual bool remove_backend(BackendId id) = 0;
    virtual std::vector<BackendRecord> load_backends() = 0;

    // Route operations
    virtual bool save_route(std::span<const TokenId> tokens, BackendId backend_id) = 0;
    virtual bool remove_route(std::span<const TokenId> tokens) = 0;
    virtual bool remove_routes_for_backend(BackendId backend_id) = 0;
    virtual std::vector<RouteRecord> load_routes() = 0;

    // Bulk operations for efficiency
    virtual bool save_routes_batch(const std::vector<RouteRecord>& routes) = 0;

    // Maintenance
    virtual bool clear_all() = 0;
    virtual size_t route_count() = 0;
    virtual size_t backend_count() = 0;

    // Crash recovery support
    // checkpoint() flushes WAL to main database file - call after critical writes
    virtual bool checkpoint() = 0;
    // verify_integrity() runs SQLite integrity check and validates data structures
    virtual bool verify_integrity() = 0;
    // Returns count of records skipped during last load due to corruption
    virtual size_t last_load_skipped_count() const = 0;
};

// Factory function to create the default persistence store
std::unique_ptr<PersistenceStore> create_persistence_store();

} // namespace ranvier
