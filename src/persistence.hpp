#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "types.hpp"

namespace ranvier {

// Record types for bulk loading
struct BackendRecord {
    BackendId id;
    std::string ip;
    uint16_t port;
};

struct RouteRecord {
    std::vector<TokenId> tokens;
    BackendId backend_id;
};

// Abstract interface for persistence backends
// This allows swapping SQLite for RocksDB or other stores in the future
class PersistenceStore {
public:
    virtual ~PersistenceStore() = default;

    // Lifecycle
    virtual bool open(const std::string& path) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    // Backend operations
    virtual bool save_backend(BackendId id, const std::string& ip, uint16_t port) = 0;
    virtual bool remove_backend(BackendId id) = 0;
    virtual std::vector<BackendRecord> load_backends() = 0;

    // Route operations
    virtual bool save_route(std::span<const TokenId> tokens, BackendId backend_id) = 0;
    virtual bool remove_route(std::span<const TokenId> tokens) = 0;
    virtual std::vector<RouteRecord> load_routes() = 0;

    // Bulk operations for efficiency
    virtual bool save_routes_batch(const std::vector<RouteRecord>& routes) = 0;

    // Maintenance
    virtual bool clear_all() = 0;
    virtual size_t route_count() = 0;
    virtual size_t backend_count() = 0;
};

// Factory function to create the default persistence store
std::unique_ptr<PersistenceStore> create_persistence_store();

} // namespace ranvier
