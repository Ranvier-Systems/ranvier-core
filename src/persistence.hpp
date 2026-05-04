#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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

// Per-request attribution row for the request_attribution table.
// Written by AsyncPersistenceManager::queue_log_request() and read by the
// /admin/keys/usage endpoint. See
// docs/architecture/per-api-key-attribution.md §7.1.
struct RequestAttributionRecord {
    std::string request_id;
    std::string api_key_id;             // ApiKey::name; "" for unauthenticated/invalid
    int64_t     timestamp_ms = 0;       // wall-clock ms at request_start
    std::string endpoint;               // e.g. "/v1/chat/completions"
    std::optional<BackendId> backend_id;
    int32_t     status_code = 0;
    int32_t     latency_ms = 0;
    int64_t     input_tokens = 0;
    int64_t     output_tokens = 0;
    double      cost_units = 0.0;
};

// Aggregated row for /admin/keys/usage responses. Computed in C++ from a
// bounded SELECT (memo §8.4) — percentiles are nth_element from the
// in-memory latency vector. See AsyncPersistenceManager::query_attribution_summary.
struct AttributionSummaryRow {
    std::string api_key_id;
    uint64_t request_count = 0;
    uint64_t success_count = 0;
    uint64_t error_count = 0;
    int32_t  latency_ms_p50 = 0;
    int32_t  latency_ms_p95 = 0;
    int32_t  latency_ms_p99 = 0;
    int64_t  input_tokens_sum = 0;
    int64_t  output_tokens_sum = 0;
    double   cost_units_sum = 0.0;
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

    // Per-API-key attribution operations (memo §7).
    // log_request() inserts one row into the request_attribution table.
    // request_attribution_count() returns the current row count.
    // prune_request_attribution() drops oldest rows so at most max_rows remain
    // (no-op when count <= max_rows). Returns the number of rows deleted.
    // query_request_attribution() materialises rows for the /admin/keys/usage
    // endpoint, optionally scoped to a single api_key_id. row_limit caps the
    // returned vector.
    virtual bool log_request(const RequestAttributionRecord& rec) = 0;
    virtual size_t request_attribution_count() = 0;
    virtual size_t prune_request_attribution(uint32_t max_rows) = 0;
    virtual std::vector<RequestAttributionRecord> query_request_attribution(
        int64_t from_ms, int64_t to_ms, const std::string& api_key_id_filter,
        size_t row_limit) = 0;
};

// Factory function to create the default persistence store
std::unique_ptr<PersistenceStore> create_persistence_store();

} // namespace ranvier
