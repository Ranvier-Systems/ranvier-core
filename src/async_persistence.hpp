#pragma once

#include "persistence.hpp"
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sharded.hh>
#include <deque>
#include <mutex>
#include <variant>

namespace ranvier {

// Operation types for the persistence queue
struct SaveRouteOp {
    std::vector<TokenId> tokens;
    BackendId backend_id;
};

struct SaveBackendOp {
    BackendId id;
    std::string ip;
    uint16_t port;
    uint32_t weight;
    uint32_t priority;
};

struct RemoveBackendOp {
    BackendId id;
};

struct RemoveRoutesForBackendOp {
    BackendId backend_id;
};

struct ClearAllOp {};

// Variant type for all persistence operations
using PersistenceOp = std::variant<
    SaveRouteOp,
    SaveBackendOp,
    RemoveBackendOp,
    RemoveRoutesForBackendOp,
    ClearAllOp
>;

// Configuration for async persistence
struct AsyncPersistenceConfig {
    // How often to flush the queue to disk
    std::chrono::milliseconds flush_interval{100};

    // Maximum number of operations to batch in a single flush
    size_t max_batch_size = 1000;

    // Maximum queue depth before applying backpressure
    size_t max_queue_depth = 100000;

    // Whether to log queue statistics periodically
    bool enable_stats_logging = true;

    // How often to log stats (if enabled)
    std::chrono::seconds stats_interval{60};
};

// Async persistence manager that decouples persistence from the hot path
//
// Design:
// - Runs only on Shard 0 (persistence is typically shard-local anyway)
// - Uses an in-memory queue for pending operations
// - Background timer drains the queue and writes to SQLite in batches
// - SQLite operations run in seastar::thread to avoid blocking the reactor
// - Fire-and-forget API for the hot path (queue_save_route, etc.)
//
// Thread safety:
// - Queue operations are protected by a spinlock for cross-shard access
// - SQLite operations are serialized in the background thread
class AsyncPersistenceManager {
public:
    explicit AsyncPersistenceManager(AsyncPersistenceConfig config = {});
    ~AsyncPersistenceManager() = default;

    // Non-copyable, non-movable (owns Seastar primitives)
    AsyncPersistenceManager(const AsyncPersistenceManager&) = delete;
    AsyncPersistenceManager& operator=(const AsyncPersistenceManager&) = delete;
    AsyncPersistenceManager(AsyncPersistenceManager&&) = delete;
    AsyncPersistenceManager& operator=(AsyncPersistenceManager&&) = delete;

    // Initialize with underlying persistence store
    // Must be called before start() and only on Shard 0
    void set_persistence_store(PersistenceStore* store);

    // Start the background flush loop
    // Returns a future that completes when the manager is ready
    seastar::future<> start();

    // Stop the manager gracefully, flushing any pending operations
    // Returns a future that completes when all operations are flushed
    seastar::future<> stop();

    // === Fire-and-forget API for hot path ===
    // These methods queue operations and return immediately
    // They are safe to call from any shard

    // Queue a route save operation (fire-and-forget)
    void queue_save_route(std::span<const TokenId> tokens, BackendId backend_id);

    // Queue a backend save operation (fire-and-forget)
    void queue_save_backend(BackendId id, const std::string& ip, uint16_t port,
                           uint32_t weight = 100, uint32_t priority = 0);

    // Queue a backend removal operation (fire-and-forget)
    void queue_remove_backend(BackendId id);

    // Queue a routes-for-backend removal operation (fire-and-forget)
    void queue_remove_routes_for_backend(BackendId backend_id);

    // Queue a clear-all operation (fire-and-forget)
    void queue_clear_all();

    // === Synchronous passthrough API for startup/shutdown ===
    // These methods call the underlying store directly (blocking!)
    // Only use during initialization or after stop()

    PersistenceStore* underlying_store() const { return _store; }

    // === Stats and monitoring ===

    // Get current queue depth
    size_t queue_depth() const;

    // Get total operations processed
    uint64_t operations_processed() const { return _ops_processed; }

    // Get total operations dropped (due to queue overflow)
    uint64_t operations_dropped() const { return _ops_dropped; }

    // Check if backpressure is being applied
    bool is_backpressured() const { return queue_depth() >= _config.max_queue_depth; }

private:
    // Process a batch of operations (runs in seastar::thread)
    void process_batch(std::vector<PersistenceOp> batch);

    // Timer callback for periodic flushing
    void on_flush_timer();

    // Log statistics
    void log_stats();

    AsyncPersistenceConfig _config;
    PersistenceStore* _store = nullptr;

    // Queue for pending operations (protected by spinlock)
    mutable std::mutex _queue_mutex;
    std::deque<PersistenceOp> _queue;

    // Background flush timer
    seastar::timer<> _flush_timer;

    // Stats logging timer
    seastar::timer<> _stats_timer;

    // Gate for tracking in-flight flush operations
    seastar::gate _flush_gate;

    // Flag to indicate shutdown
    bool _stopping = false;

    // Statistics
    std::atomic<uint64_t> _ops_processed{0};
    std::atomic<uint64_t> _ops_dropped{0};
    std::atomic<uint64_t> _batches_flushed{0};
};

} // namespace ranvier
