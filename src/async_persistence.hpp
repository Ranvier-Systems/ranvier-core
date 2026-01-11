#pragma once

#include "persistence.hpp"
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sharded.hh>
#include <deque>
#include <memory>
#include <mutex>
#include <variant>

namespace ranvier {

/**
 * @file async_persistence.hpp
 * @brief Reactor-safe async wrapper for SQLite persistence.
 *
 * IMPORTANT: This is the ONLY valid entry point for persistence operations
 * from reactor-thread code (HttpController, RouterService, etc.).
 *
 * THREADING MODEL:
 * The underlying SqlitePersistence uses std::mutex because SQLite is not
 * thread-safe. To avoid blocking the Seastar reactor thread, this class:
 *
 *   1. Provides fire-and-forget queue_*() methods for writes (non-blocking)
 *   2. Processes queued operations in batches via seastar::async()
 *   3. Runs SQLite operations on Seastar's thread pool (not the reactor)
 *
 * The std::mutex in the queue (_queue_mutex) is acceptable because:
 *   - Lock contention is minimal (queue operations are O(1))
 *   - The critical section is extremely short (push/pop from deque)
 *   - Cross-shard access requires some synchronization
 *
 * ARCHITECTURAL GUARANTEE:
 *   HttpController → AsyncPersistenceManager → seastar::async → SqlitePersistence
 *                 ↑                                    ↓
 *           (reactor thread)                  (thread pool, mutex OK)
 *
 * @see SqlitePersistence for the underlying mutex-protected implementation.
 * @see docs/claude-context.md "No Locks/Async Only" section.
 */

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
/**
 * @brief Reactor-safe async wrapper for SQLite persistence operations.
 *
 * This class owns the underlying PersistenceStore and provides a complete
 * abstraction over the persistence layer. Consumers should NEVER access
 * the underlying SqlitePersistence implementation directly.
 *
 * WHY THIS CLASS EXISTS:
 * The "No Locks/Async Only" rule in Seastar means reactor threads must never
 * block. However, SQLite requires mutex protection because it's not thread-safe.
 * This class bridges the gap by:
 *   1. Accepting operations on the reactor thread (non-blocking queue)
 *   2. Processing them in seastar::async() on a separate thread pool
 *   3. Using mutex protection safely (thread pool threads can block)
 *
 * Architecture:
 *   - Fire-and-forget queue API for the hot request path
 *   - Background timer drains queue and writes to SQLite in batches
 *   - SQLite operations run in seastar::async (off the reactor thread)
 *   - Semaphore ensures batches are processed in order
 *
 * Thread Safety:
 *   - Queue protected by std::mutex (safe for cross-shard access)
 *   - SQLite operations serialized via semaphore
 *   - Shutdown flag is atomic for visibility across shards
 *
 * MUTEX JUSTIFICATION (per docs/claude-context.md):
 * The _queue_mutex is acceptable because:
 *   1. Critical sections are extremely short (deque push/pop)
 *   2. No blocking I/O occurs while holding the lock
 *   3. Cross-shard access to the shared queue requires synchronization
 *   4. The alternative (per-shard queues) would complicate shutdown ordering
 */

class AsyncPersistenceManager {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    // Production constructor: creates the underlying SQLite store via factory.
    explicit AsyncPersistenceManager(AsyncPersistenceConfig config = {});

    // Test constructor: allows injecting a custom PersistenceStore for mocking.
    // The injected store must already be open, or open() must be called separately.
    AsyncPersistenceManager(AsyncPersistenceConfig config, std::unique_ptr<PersistenceStore> store);

    ~AsyncPersistenceManager() = default;

    // Non-copyable, non-movable (owns unique resources)
    AsyncPersistenceManager(const AsyncPersistenceManager&) = delete;
    AsyncPersistenceManager& operator=(const AsyncPersistenceManager&) = delete;
    AsyncPersistenceManager(AsyncPersistenceManager&&) = delete;
    AsyncPersistenceManager& operator=(AsyncPersistenceManager&&) = delete;

    // -------------------------------------------------------------------------
    // Lifecycle (call in order: open -> start -> [use] -> stop -> close)
    // -------------------------------------------------------------------------

    // Open the persistence store at the given path.
    // Creates the underlying SQLite database if it doesn't exist.
    // Returns true on success, false on failure.
    bool open(const std::string& path);

    // Check if the persistence store is open and ready.
    bool is_open() const;

    // Start the async manager (arms flush timer).
    // Must be called after open() succeeds.
    seastar::future<> start();

    // Stop the async manager (flushes remaining queue).
    // Must be called before close(). Safe to call multiple times.
    seastar::future<> stop();

    // Close the persistence store (flushes WAL and closes database).
    // Must be called after stop() completes. Safe to call multiple times.
    void close();

    // -------------------------------------------------------------------------
    // Write Operations (Fire-and-Forget Queue API)
    // -------------------------------------------------------------------------
    // These methods queue operations and return immediately (non-blocking).
    // Safe to call from any shard. Operations silently dropped if store not open.

    void queue_save_route(std::span<const TokenId> tokens, BackendId backend_id);
    void queue_save_backend(BackendId id, const std::string& ip, uint16_t port,
                           uint32_t weight = 100, uint32_t priority = 0);
    void queue_remove_backend(BackendId id);
    void queue_remove_routes_for_backend(BackendId backend_id);
    void queue_clear_all();

    // -------------------------------------------------------------------------
    // Read Operations (Synchronous, for Startup/Recovery)
    // -------------------------------------------------------------------------
    // These operations read directly from the underlying store (blocking).
    // Call during startup before serving requests, or during shutdown.
    // Returns empty/zero values if store is not open.

    std::vector<BackendRecord> load_backends();
    std::vector<RouteRecord> load_routes();
    size_t backend_count() const;
    size_t route_count() const;
    size_t last_load_skipped_count() const;

    // -------------------------------------------------------------------------
    // Maintenance Operations (Synchronous)
    // -------------------------------------------------------------------------
    // Call during startup or shutdown only - not safe during normal operation.

    // Flush WAL to main database file - call after critical writes
    bool checkpoint();

    // Run integrity check and validate data structures
    bool verify_integrity();

    // Clear all persisted data (synchronous).
    // WARNING: For startup recovery only. Do not call while serving requests.
    bool clear_all();

    // -------------------------------------------------------------------------
    // Monitoring (Thread-safe, can call anytime)
    // -------------------------------------------------------------------------

    size_t queue_depth() const;
    size_t max_queue_depth() const { return _config.max_queue_depth; }
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
    std::unique_ptr<PersistenceStore> _store;  // Owned persistence engine

    // Queue (protected by mutex for cross-shard access)
    mutable std::mutex _queue_mutex;
    std::deque<PersistenceOp> _queue;
    std::atomic<size_t> _queue_size{0};  // Lock-free queue depth for metrics

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
