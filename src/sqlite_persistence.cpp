#include "sqlite_persistence.hpp"
#include <cstring>
#include <stdexcept>

namespace ranvier {

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
    // Backends table
    const char* backends_sql = R"(
        CREATE TABLE IF NOT EXISTS backends (
            id INTEGER PRIMARY KEY,
            ip TEXT NOT NULL,
            port INTEGER NOT NULL
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

    return exec_sql(backends_sql) && exec_sql(routes_sql) && exec_sql(index_sql);
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

bool SqlitePersistence::save_backend(BackendId id, const std::string& ip, uint16_t port) {
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_db) return false;

    const char* sql = "INSERT OR REPLACE INTO backends (id, ip, port) VALUES (?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_text(stmt, 2, ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, port);

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
    if (!_db) return results;

    const char* sql = "SELECT id, ip, port FROM backends";
    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return results;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        BackendRecord record;
        record.id = sqlite3_column_int(stmt, 0);
        record.ip = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        record.port = static_cast<uint16_t>(sqlite3_column_int(stmt, 2));
        results.push_back(std::move(record));
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<uint8_t> SqlitePersistence::serialize_tokens(std::span<const TokenId> tokens) {
    std::vector<uint8_t> blob(tokens.size() * sizeof(TokenId));
    std::memcpy(blob.data(), tokens.data(), blob.size());
    return blob;
}

std::vector<TokenId> SqlitePersistence::deserialize_tokens(const void* data, size_t size) {
    size_t count = size / sizeof(TokenId);
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

std::vector<RouteRecord> SqlitePersistence::load_routes() {
    std::lock_guard<std::mutex> lock(_mutex);

    std::vector<RouteRecord> results;
    if (!_db) return results;

    const char* sql = "SELECT tokens, backend_id FROM routes";
    sqlite3_stmt* stmt = nullptr;

    int rc = sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return results;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        RouteRecord record;

        const void* blob = sqlite3_column_blob(stmt, 0);
        int blob_size = sqlite3_column_bytes(stmt, 0);
        record.tokens = deserialize_tokens(blob, static_cast<size_t>(blob_size));
        record.backend_id = sqlite3_column_int(stmt, 1);

        results.push_back(std::move(record));
    }

    sqlite3_finalize(stmt);
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

// Factory function implementation
std::unique_ptr<PersistenceStore> create_persistence_store() {
    return std::make_unique<SqlitePersistence>();
}

} // namespace ranvier
