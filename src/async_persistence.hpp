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

// ============================================================================
// Operation Types
// ============================================================================
// Each operation type represents a single persistence action that can be queued.

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

using PersistenceOp = std::variant<
    SaveRouteOp,
    SaveBackendOp,
    RemoveBackendOp,
    RemoveRoutesForBackendOp,
    ClearAllOp
>;

// ============================================================================
// Configuration
// ============================================================================

struct AsyncPersistenceConfig {
    std::chrono::milliseconds flush_interval{100};  // How often to flush to disk
    size_t max_batch_size = 1000;                   // Max operations per batch
    size_t max_queue_depth = 100000;                // Backpressure threshold
    bool enable_stats_logging = true;               // Log queue statistics
    std::chrono::seconds stats_interval{60};        // Stats logging interval
};

// ============================================================================
// AsyncPersistenceManager
// ============================================================================
// Decouples SQLite persistence from the Seastar reactor thread.
//
// Architecture:
//   - Fire-and-forget queue API for the hot request path
//   - Background timer drains queue and writes to SQLite in batches
//   - SQLite operations run in seastar::async (off the reactor thread)
//   - Semaphore ensures batches are processed in order
//
// Thread Safety:
//   - Queue protected by std::mutex (safe for cross-shard access)
//   - SQLite operations serialized via semaphore
//   - Shutdown flag is atomic for visibility across shards

class AsyncPersistenceManager {
public:
    explicit AsyncPersistenceManager(AsyncPersistenceConfig config = {});
    ~AsyncPersistenceManager() = default;

    // Non-copyable, non-movable
    AsyncPersistenceManager(const AsyncPersistenceManager&) = delete;
    AsyncPersistenceManager& operator=(const AsyncPersistenceManager&) = delete;
    AsyncPersistenceManager(AsyncPersistenceManager&&) = delete;
    AsyncPersistenceManager& operator=(AsyncPersistenceManager&&) = delete;

    // --- Lifecycle ---

    void set_persistence_store(PersistenceStore* store);
    seastar::future<> start();
    seastar::future<> stop();

    // --- Fire-and-Forget Queue API (Hot Path) ---
    // These methods queue operations and return immediately.
    // Safe to call from any shard.

    void queue_save_route(std::span<const TokenId> tokens, BackendId backend_id);
    void queue_save_backend(BackendId id, const std::string& ip, uint16_t port,
                           uint32_t weight = 100, uint32_t priority = 0);
    void queue_remove_backend(BackendId id);
    void queue_remove_routes_for_backend(BackendId backend_id);
    void queue_clear_all();

    // --- Direct Access (Startup/Shutdown Only) ---

    PersistenceStore* underlying_store() const { return _store; }

    // --- Monitoring ---

    size_t queue_depth() const;
    uint64_t operations_processed() const { return _ops_processed.load(); }
    uint64_t operations_dropped() const { return _ops_dropped.load(); }
    bool is_backpressured() const { return queue_depth() >= _config.max_queue_depth; }

private:
    // --- Queue Management ---

    // Try to enqueue an operation, respecting backpressure.
    // Returns true if enqueued, false if dropped due to backpressure.
    bool try_enqueue(PersistenceOp op);

    // Extract up to max_batch_size operations from the queue.
    // Returns empty vector if queue is empty.
    std::vector<PersistenceOp> extract_batch();

    // Drain the entire queue (used during shutdown).
    std::vector<PersistenceOp> drain_queue();

    // --- Batch Processing ---

    // Timer callback that triggers batch processing.
    void on_flush_timer();

    // Process a batch of operations (runs in seastar::async).
    void process_batch(std::vector<PersistenceOp> batch);

    // Helper class to accumulate routes for batch insertion.
    class RouteAccumulator;

    // Execute individual operations (called by process_batch).
    // Note: SaveRouteOp is passed by non-const ref to allow move semantics.
    void execute(SaveRouteOp& op, RouteAccumulator& routes);
    void execute(const SaveBackendOp& op, RouteAccumulator& routes);
    void execute(const RemoveBackendOp& op, RouteAccumulator& routes);
    void execute(const RemoveRoutesForBackendOp& op, RouteAccumulator& routes);
    void execute(const ClearAllOp& op, RouteAccumulator& routes);

    // --- Statistics ---

    void log_stats();

    // --- Member Variables ---

    AsyncPersistenceConfig _config;
    PersistenceStore* _store = nullptr;

    // Queue (protected by mutex for cross-shard access)
    mutable std::mutex _queue_mutex;
    std::deque<PersistenceOp> _queue;

    // Timers
    seastar::timer<> _flush_timer;
    seastar::timer<> _stats_timer;

    // Synchronization
    seastar::gate _flush_gate;              // Tracks in-flight flush operations
    seastar::semaphore _batch_semaphore{1}; // Serializes batch processing

    // State
    std::atomic<bool> _stopping{false};

    // Statistics
    std::atomic<uint64_t> _ops_processed{0};
    std::atomic<uint64_t> _ops_dropped{0};
    std::atomic<uint64_t> _batches_flushed{0};
};

} // namespace ranvier
