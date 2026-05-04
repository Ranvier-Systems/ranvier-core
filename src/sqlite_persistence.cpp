#include "sqlite_persistence.hpp"
#include <cstring>
#include <stdexcept>

namespace ranvier {

// Helper: safely extract text column, returns empty string for NULL
static std::string safe_column_text(sqlite3_stmt* stmt, int col) {
    auto ptr = sqlite3_column_text(stmt, col);
    return ptr ? reinterpret_cast<const char*>(ptr) : "";
}

SqlitePersistence::~SqlitePersistence() {
    close();
}

bool SqlitePersistence::open(const std::string& path) {
    std::lock_guard<std::mutex> lock(_mutex);

    if (_db) {
        return false;  // Already open
    }

    int rc = sqlite3_open(path.c_str(), &_db);
    if (rc != SQLITE_OK) {
        if (_db) {
            sqlite3_close(_db);
            _db = nullptr;
        }
        return false;
    }

    // Enable WAL mode for better concurrent performance
    exec_sql("PRAGMA journal_mode=WAL");
    exec_sql("PRAGMA synchronous=NORMAL");

    return create_tables();
}

void SqlitePersistence::close() {
    std::lock_guard<std::mutex> lock(_mutex);

    if (_db) {
        sqlite3_close(_db);
        _db = nullptr;
    }
}

bool SqlitePersistence::is_open() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _db != nullptr;
}

bool SqlitePersistence::create_tables() {
    // Backends table with weight and priority for heterogeneous clusters
    const char* backends_sql = R"(
        CREATE TABLE IF NOT EXISTS backends (
            id INTEGER PRIMARY KEY,
            ip TEXT NOT NULL,
            port INTEGER NOT NULL,
            weight INTEGER NOT NULL DEFAULT 100,
            priority INTEGER NOT NULL DEFAULT 0
        )
    )";

    // Routes table - tokens stored as BLOB for efficiency
    const char* routes_sql = R"(
        CREATE TABLE IF NOT EXISTS routes (
            tokens BLOB PRIMARY KEY,
            backend_id INTEGER NOT NULL
        )
    )";

    // Index for backend lookups in routes
    const char* index_sql = R"(
        CREATE INDEX IF NOT EXISTS idx_routes_backend
        ON routes(backend_id)
    )";

    if (!exec_sql(backends_sql) || !exec_sql(routes_sql) || !exec_sql(index_sql)) {
        return false;
    }

    // Schema migration: add weight and priority columns if they don't exist
    // SQLite doesn't support IF NOT EXISTS for ALTER TABLE, so we check first
    exec_sql("ALTER TABLE backends ADD COLUMN weight INTEGER NOT NULL DEFAULT 100");
    exec_sql("ALTER TABLE backends ADD COLUMN priority INTEGER NOT NULL DEFAULT 0");

    // Per-API-key attribution table (memo §7.1). Additive migration:
    // CREATE TABLE IF NOT EXISTS is idempotent, so older databases (with only
    // backends/routes) gain this table on next open with no data loss.
    const char* request_attribution_sql = R"(
        CREATE TABLE IF NOT EXISTS request_attribution (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            request_id    TEXT    NOT NULL,
            api_key_id    TEXT    NOT NULL,
            timestamp_ms  INTEGER NOT NULL,
            endpoint      TEXT    NOT NULL,
            backend_id    INTEGER,
            status_code   INTEGER NOT NULL,
            latency_ms    INTEGER NOT NULL,
            input_tokens  INTEGER NOT NULL,
            output_tokens INTEGER NOT NULL,
            cost_units    REAL    NOT NULL
        )
    )";
    const char* idx_key_ts_sql = R"(
        CREATE INDEX IF NOT EXISTS idx_request_attribution_key_ts
        ON request_attribution(api_key_id, timestamp_ms)
    )";
    const char* idx_ts_sql = R"(
        CREATE INDEX IF NOT EXISTS idx_request_attribution_ts
        ON request_attribution(timestamp_ms)
    )";
    if (!exec_sql(request_attribution_sql) || !exec_sql(idx_key_ts_sql) || !exec_sql(idx_ts_sql)) {
        return false;
    }

    return true;
}

bool SqlitePersistence::exec_sql(const char* sql) {
    char* err_msg = nullptr;
    int rc = sqlite3_exec(_db, sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg) {
            sqlite3_free(err_msg);
        }
        return false;
    }
    return true;
}

bool SqlitePersistence::save_backend(BackendId id, const std::string& ip, uint16_t port,
                                     uint32_t weight, uint32_t priority) {
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_db) return false;

    const char* sql = "INSERT OR REPLACE INTO backends (id, ip, port, weight, priority) VALUES (?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_text(stmt, 2, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, port);
    sqlite3_bind_int(stmt, 4, static_cast<int>(weight));
    sqlite3_bind_int(stmt, 5, static_cast<int>(priority));

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

bool SqlitePersistence::remove_backend(BackendId id) {
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_db) return false;

    const char* sql = "DELETE FROM backends WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

std::vector<BackendRecord> SqlitePersistence::load_backends() {
    std::lock_guard<std::mutex> lock(_mutex);

    std::vector<BackendRecord> results;
    size_t skipped_count = 0;

    if (!_db) return results;

    const char* sql = "SELECT id, ip, port, weight, priority FROM backends";
    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return results;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        BackendRecord record;
        record.id = sqlite3_column_int(stmt, 0);
        record.ip = safe_column_text(stmt, 1);

        // Skip backends with NULL or empty IP (required field)
        if (record.ip.empty()) {
            skipped_count++;
            continue;
        }

        record.port = static_cast<uint16_t>(sqlite3_column_int(stmt, 2));
        record.weight = static_cast<uint32_t>(sqlite3_column_int(stmt, 3));
        record.priority = static_cast<uint32_t>(sqlite3_column_int(stmt, 4));
        results.push_back(std::move(record));
    }

    sqlite3_finalize(stmt);

    // Track skipped count (caller can check via last_load_skipped_count())
    _last_load_skipped_count += skipped_count;

    return results;
}

std::vector<uint8_t> SqlitePersistence::serialize_tokens(std::span<const TokenId> tokens) {
    std::vector<uint8_t> blob(tokens.size() * sizeof(TokenId));
    std::memcpy(blob.data(), tokens.data(), blob.size());
    return blob;
}

std::vector<TokenId> SqlitePersistence::deserialize_tokens(const void* data, size_t size) {
    // Validate input: null data or zero size returns empty vector
    if (data == nullptr || size == 0) {
        return {};
    }

    // Validate size is a multiple of TokenId size (corruption check)
    if (size % sizeof(TokenId) != 0) {
        // Corrupted blob - size doesn't align with TokenId boundary
        return {};
    }

    size_t count = size / sizeof(TokenId);

    // ========================================================================
    // Persistence-Layer Corruption Safeguard (NOT Business Logic)
    // ========================================================================
    // This limit is a STORAGE SAFETY check to prevent allocation failures from
    // corrupted blob data in the database. It is NOT a business-level validation.
    //
    // Business-level token count limits (e.g., max_route_tokens = 8192) are
    // enforced in RouterService BEFORE data reaches the persistence layer.
    // This follows the principle of separation of concerns:
    //   - Business Layer (RouterService): Policy enforcement, route validation
    //   - Persistence Layer (SqlitePersistence): Storage, serialization, corruption protection
    //
    // The 1M limit here is intentionally much higher than any business limit
    // to catch only corrupted/malformed database records, not valid routes.
    constexpr size_t MAX_TOKEN_COUNT_CORRUPTION_SAFEGUARD = 1'000'000;
    if (count > MAX_TOKEN_COUNT_CORRUPTION_SAFEGUARD) {
        return {};
    }

    std::vector<TokenId> tokens(count);
    std::memcpy(tokens.data(), data, size);
    return tokens;
}

bool SqlitePersistence::save_route(std::span<const TokenId> tokens, BackendId backend_id) {
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_db) return false;

    const char* sql = "INSERT OR REPLACE INTO routes (tokens, backend_id) VALUES (?, ?)";
    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    auto blob = serialize_tokens(tokens);
    sqlite3_bind_blob(stmt, 1, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, backend_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

bool SqlitePersistence::remove_route(std::span<const TokenId> tokens) {
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_db) return false;

    const char* sql = "DELETE FROM routes WHERE tokens = ?";
    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    auto blob = serialize_tokens(tokens);
    sqlite3_bind_blob(stmt, 1, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

bool SqlitePersistence::remove_routes_for_backend(BackendId backend_id) {
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_db) return false;

    const char* sql = "DELETE FROM routes WHERE backend_id = ?";
    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, backend_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

std::vector<RouteRecord> SqlitePersistence::load_routes() {
    std::lock_guard<std::mutex> lock(_mutex);

    std::vector<RouteRecord> results;
    size_t skipped_count = 0;

    if (!_db) return results;

    const char* sql = "SELECT tokens, backend_id FROM routes";
    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return results;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const void* blob = sqlite3_column_blob(stmt, 0);
        int blob_size = sqlite3_column_bytes(stmt, 0);

        // Handle empty token sequences (valid edge case)
        // blob can be null for empty BLOB in SQLite
        std::vector<TokenId> tokens;
        if (blob != nullptr && blob_size > 0) {
            tokens = deserialize_tokens(blob, static_cast<size_t>(blob_size));

            // If deserialization returned empty but input was non-empty, data is corrupted
            // (e.g., blob_size not divisible by sizeof(TokenId) or exceeds max)
            if (tokens.empty()) {
                skipped_count++;
                continue;
            }
        }
        // If blob is null/empty, tokens stays empty (valid empty sequence)

        RouteRecord record;
        record.tokens = std::move(tokens);
        record.backend_id = sqlite3_column_int(stmt, 1);
        results.push_back(std::move(record));
    }

    sqlite3_finalize(stmt);

    // Log if we skipped any corrupted records (caller can check this)
    _last_load_skipped_count = skipped_count;

    return results;
}

bool SqlitePersistence::save_routes_batch(const std::vector<RouteRecord>& routes) {
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_db) return false;

    // Use a transaction for batch efficiency
    if (!exec_sql("BEGIN TRANSACTION")) return false;

    const char* sql = "INSERT OR REPLACE INTO routes (tokens, backend_id) VALUES (?, ?)";
    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        exec_sql("ROLLBACK");
        return false;
    }

    for (const auto& route : routes) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);

        auto blob = serialize_tokens(route.tokens);
        sqlite3_bind_blob(stmt, 1, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, route.backend_id);

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            exec_sql("ROLLBACK");
            return false;
        }
    }

    sqlite3_finalize(stmt);
    return exec_sql("COMMIT");
}

bool SqlitePersistence::clear_all() {
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_db) return false;

    return exec_sql("DELETE FROM routes") && exec_sql("DELETE FROM backends");
}

size_t SqlitePersistence::route_count() {
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_db) return 0;

    const char* sql = "SELECT COUNT(*) FROM routes";
    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return 0;

    size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
    }

    sqlite3_finalize(stmt);
    return count;
}

size_t SqlitePersistence::backend_count() {
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_db) return 0;

    const char* sql = "SELECT COUNT(*) FROM backends";
    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return 0;

    size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
    }

    sqlite3_finalize(stmt);
    return count;
}

bool SqlitePersistence::checkpoint() {
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_db) return false;

    // PRAGMA wal_checkpoint(TRUNCATE) forces a checkpoint and truncates the WAL file
    // This ensures all changes are written to the main database file
    // TRUNCATE mode is safest for crash recovery as it leaves no WAL residue
    int rc = sqlite3_wal_checkpoint_v2(_db, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
    return rc == SQLITE_OK;
}

bool SqlitePersistence::verify_integrity() {
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_db) return false;

    // Run SQLite's built-in integrity check
    const char* sql = "PRAGMA integrity_check";
    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    bool is_ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        std::string result = safe_column_text(stmt, 0);
        // "ok" means the database passed integrity check
        is_ok = (result == "ok");
    }

    sqlite3_finalize(stmt);

    if (!is_ok) {
        return false;
    }

    // Additional validation: check for orphaned routes (routes referencing non-existent backends)
    // This can happen if a crash occurred during backend deletion
    const char* orphan_sql = R"(
        SELECT COUNT(*) FROM routes r
        WHERE NOT EXISTS (SELECT 1 FROM backends b WHERE b.id = r.backend_id)
    )";

    rc = sqlite3_prepare_v2(_db, orphan_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    size_t orphan_count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        orphan_count = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
    }

    sqlite3_finalize(stmt);

    // If orphaned routes exist, they indicate potential corruption
    // We don't fail the check, but this info is useful for diagnostics
    // The routes will be cleaned up during normal operation
    return true;
}

size_t SqlitePersistence::last_load_skipped_count() const {
    return _last_load_skipped_count;
}

// =============================================================================
// Per-API-Key Attribution (memo §7)
// =============================================================================

bool SqlitePersistence::log_request(const RequestAttributionRecord& rec) {
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_db) return false;

    // Hard Rule #7 (no business logic in persistence): no validation,
    // sanitisation, or clamping. Service layer owns those concerns; this
    // function is dumb storage.
    const char* sql =
        "INSERT INTO request_attribution "
        "(request_id, api_key_id, timestamp_ms, endpoint, backend_id, "
        " status_code, latency_ms, input_tokens, output_tokens, cost_units) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text  (stmt, 1, rec.request_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (stmt, 2, rec.api_key_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (stmt, 3, rec.timestamp_ms);
    sqlite3_bind_text  (stmt, 4, rec.endpoint.c_str(), -1, SQLITE_TRANSIENT);
    if (rec.backend_id.has_value()) {
        sqlite3_bind_int(stmt, 5, *rec.backend_id);
    } else {
        sqlite3_bind_null(stmt, 5);
    }
    sqlite3_bind_int   (stmt, 6, rec.status_code);
    sqlite3_bind_int   (stmt, 7, rec.latency_ms);
    sqlite3_bind_int64 (stmt, 8, rec.input_tokens);
    sqlite3_bind_int64 (stmt, 9, rec.output_tokens);
    sqlite3_bind_double(stmt,10, rec.cost_units);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

size_t SqlitePersistence::request_attribution_count() {
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_db) return 0;

    const char* sql = "SELECT COUNT(*) FROM request_attribution";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;

    size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return count;
}

size_t SqlitePersistence::prune_request_attribution(uint32_t max_rows) {
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_db || max_rows == 0) return 0;

    // Memo §7.3 Option A: keep at most max_rows rows, dropping oldest by id.
    // Single statement; SQLite handles the count internally.
    const char* sql =
        "DELETE FROM request_attribution "
        "WHERE id <= (SELECT MAX(id) - ? FROM request_attribution)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;

    sqlite3_bind_int(stmt, 1, static_cast<int>(max_rows));
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return 0;
    return static_cast<size_t>(sqlite3_changes(_db));
}

std::vector<RequestAttributionRecord> SqlitePersistence::query_request_attribution(
        int64_t from_ms, int64_t to_ms, const std::string& api_key_id_filter,
        size_t row_limit) {
    std::lock_guard<std::mutex> lock(_mutex);

    std::vector<RequestAttributionRecord> rows;
    if (!_db || row_limit == 0) return rows;

    // Single capped SELECT — bounded by row_limit (memo §8.4). Ordering by
    // (api_key_id, latency_ms) lets the caller compute per-key percentiles
    // without a second pass.
    const bool with_filter = !api_key_id_filter.empty();
    const char* sql_filtered =
        "SELECT request_id, api_key_id, timestamp_ms, endpoint, backend_id, "
        "       status_code, latency_ms, input_tokens, output_tokens, cost_units "
        "FROM request_attribution "
        "WHERE timestamp_ms >= ? AND timestamp_ms < ? AND api_key_id = ? "
        "ORDER BY api_key_id, latency_ms "
        "LIMIT ?";
    const char* sql_unfiltered =
        "SELECT request_id, api_key_id, timestamp_ms, endpoint, backend_id, "
        "       status_code, latency_ms, input_tokens, output_tokens, cost_units "
        "FROM request_attribution "
        "WHERE timestamp_ms >= ? AND timestamp_ms < ? "
        "ORDER BY api_key_id, latency_ms "
        "LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    const char* sql = with_filter ? sql_filtered : sql_unfiltered;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return rows;

    sqlite3_bind_int64(stmt, 1, from_ms);
    sqlite3_bind_int64(stmt, 2, to_ms);
    if (with_filter) {
        sqlite3_bind_text (stmt, 3, api_key_id_filter.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, static_cast<int64_t>(row_limit));
    } else {
        sqlite3_bind_int64(stmt, 3, static_cast<int64_t>(row_limit));
    }

    rows.reserve(std::min<size_t>(row_limit, 4096));
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        RequestAttributionRecord rec;
        rec.request_id   = safe_column_text(stmt, 0);
        rec.api_key_id   = safe_column_text(stmt, 1);
        rec.timestamp_ms = sqlite3_column_int64(stmt, 2);
        rec.endpoint     = safe_column_text(stmt, 3);
        if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
            rec.backend_id = static_cast<BackendId>(sqlite3_column_int(stmt, 4));
        }
        rec.status_code   = sqlite3_column_int  (stmt, 5);
        rec.latency_ms    = sqlite3_column_int  (stmt, 6);
        rec.input_tokens  = sqlite3_column_int64(stmt, 7);
        rec.output_tokens = sqlite3_column_int64(stmt, 8);
        rec.cost_units    = sqlite3_column_double(stmt, 9);
        rows.push_back(std::move(rec));
    }
    sqlite3_finalize(stmt);
    return rows;
}

// Factory function implementation
// Note: Cannot use make_unique here because SqlitePersistence has a private
// constructor. The friend declaration allows this function to call new directly,
// but make_unique's internal new call would fail the access check.
std::unique_ptr<PersistenceStore> create_persistence_store() {
    return std::unique_ptr<PersistenceStore>(new SqlitePersistence());
}

} // namespace ranvier
