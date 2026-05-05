#pragma once

#include "persistence.hpp"
#include "mpsc_ring_buffer.hpp"
#include <seastar/core/alien.hh>
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/sharded.hh>
#include <atomic>
#include <memory>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

namespace ranvier {

/**
 * @file async_persistence.hpp
 * @brief Reactor-safe async wrapper for SQLite persistence.
 *
 * IMPORTANT: This is the ONLY valid entry point for persistence operations
 * from reactor-thread code (HttpController, RouterService, etc.).
 *
 * THREADING MODEL (Hard Rule #12):
 * SQLite is not thread-safe and its calls block. To keep them off the
 * reactor, this class owns a single dedicated OS worker thread (std::thread)
 * that drains the operation queue and invokes SqlitePersistence directly.
 * Completion / shutdown signalling back to the reactor uses
 * seastar::alien::run_on().
 *
 *   1. queue_*() methods are fire-and-forget, lock-free, callable from any
 *      shard. They push into the MPSC ring buffer.
 *   2. The worker thread is the sole consumer: it drains batches and runs
 *      blocking sqlite3_* calls on its own kernel thread.
 *   3. seastar::async() is NOT used. seastar::async runs on the reactor
 *      (it's a stackful coroutine, not a thread-pool dispatch). Wrapping
 *      blocking SQLite in seastar::async would stall the shard for the
 *      duration of every flush — which was the previous (broken) design.
 *
 * LOCK-FREE MPSC QUEUE:
 * The operation queue uses a bounded multi-producer single-consumer ring
 * buffer (Vyukov-style) with per-slot sequence numbers. Multiple reactor
 * shards (producers) CAS-compete on the tail index to enqueue operations
 * lock-free. The dedicated worker thread (sole consumer) drains batches
 * via extract_batch()/drain_queue().
 *
 * ARCHITECTURAL GUARANTEE:
 *   HttpController → AsyncPersistenceManager → MPSC ring → worker std::thread
 *                  (any reactor shard, lock-free)        ↓
 *                                            (blocking SQLite, mutex OK)
 *
 * @see SqlitePersistence for the underlying mutex-protected implementation.
 * @see src/tokenizer_thread_pool.cpp for the canonical alien::run_on pattern.
 * @see .dev-context/claude-context.md Hard Rule #12.
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
 * The "No Locks/Async Only" rule in Seastar means reactor threads must
 * never block. SQLite, however, is a blocking C library. This class
 * bridges the gap by running the blocking calls on a dedicated OS thread
 * (NOT a Seastar thread / seastar::async — those run on the reactor).
 *
 * Architecture:
 *   - Lock-free MPSC ring buffer: any reactor shard can enqueue.
 *   - One std::thread worker drains the ring buffer in batches and calls
 *     SqlitePersistence directly. Batching is driven by the worker's own
 *     idle-sleep (flush_interval) when the queue is empty.
 *   - On shutdown, the worker drains remaining ops, then signals the
 *     reactor via seastar::alien::run_on() so stop() can resolve its
 *     future without joining a thread on the reactor.
 *
 * Thread Safety:
 *   - Queue is a lock-free MPSC ring buffer (no mutex on reactor thread)
 *   - Single consumer (the worker thread) — no batch serialisation needed
 *   - Stopping/shutdown flags are atomic for visibility across threads
 *   - Clear-all uses an atomic generation counter checked by the consumer
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

    ~AsyncPersistenceManager();

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

    // Start the async manager: spawns the dedicated SQLite worker thread
    // and arms the stats timer. Must be called after open() succeeds.
    //
    // alien_instance: Seastar's alien instance (lives on app_template).
    //   Used by the worker thread to signal completion back to this shard.
    //   Must outlive this AsyncPersistenceManager.
    seastar::future<> start(seastar::alien::instance& alien_instance);

    // Stop the async manager: rejects new enqueues, lets the worker drain
    // the remaining queue, then resolves when the worker has exited.
    // Must be called before close(). Safe to call multiple times.
    seastar::future<> stop();

    // Close the persistence store (flushes WAL and closes database).
    // Must be called after stop() completes. Safe to call multiple times.
    void close();

    // -------------------------------------------------------------------------
    // Write Operations (Fire-and-Forget Queue API)
    // -------------------------------------------------------------------------
    // These methods queue operations and return immediately (lock-free).
    // Safe to call from any shard (MPSC: multiple producers supported).
    // Operations silently dropped if store not open.

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

    // Worker thread main loop. Drains the queue in batches and processes
    // them synchronously (blocking SQLite calls run here, off the reactor).
    // On shutdown, drains remaining ops, then signals the reactor via
    // seastar::alien::run_on() so stop() can resolve its future.
    void worker_loop();

    // Process a batch of operations. Runs ON THE WORKER THREAD ONLY.
    // Never call this from a reactor context — it blocks on SQLite.
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

    // Lock-free MPSC ring buffer: multiple reactor shards produce, single
    // consumer (the worker thread) drains batches.
    // Capacity is next power-of-two >= max_queue_depth.
    std::unique_ptr<MpscRingBuffer<PersistenceOp>> _ring;
    std::atomic<size_t> _queue_size{0};  // Maintained for metrics compatibility

    // Generation counter for queue_clear_all(). Producer increments; consumer
    // checks before processing each batch and discards if generation changed.
    std::atomic<uint64_t> _clear_generation{0};
    uint64_t _last_processed_generation{0};  // Worker-only, no atomic needed

    // Stats timer (logs queue depth periodically). Runs on the reactor.
    seastar::timer<> _stats_timer;

    // RAII guard for the stats timer callback: closed in stop() before the
    // timer is cancelled, so no in-flight callback can access `this` after
    // stop() returns.
    seastar::gate _timer_gate;

    // ----- Worker thread plumbing -----

    // Dedicated OS worker thread that drains _ring and calls SQLite directly.
    // Spawned in start(), joined in stop().
    std::unique_ptr<std::thread> _worker_thread;

    // Alien instance for worker → reactor signalling. Lifetime managed by
    // app_template; non-owning pointer here. Set in start().
    seastar::alien::instance* _alien_instance = nullptr;

    // Reactor shard that owns this manager (the one that called start()).
    // Worker uses this as the target shard for seastar::alien::run_on().
    unsigned _owner_shard = 0;

    // Resolved by the worker (via alien::run_on) once it has fully drained
    // and is about to exit. stop() awaits this before joining the thread,
    // so the reactor never blocks on std::thread::join().
    std::optional<seastar::promise<>> _worker_done;

    // Worker-thread shutdown signal: set in stop(), read by worker_loop().
    std::atomic<bool> _shutdown{false};

    // Producer-side stop signal: set in stop(), checked by queue_*() to
    // reject new enqueues immediately.
    std::atomic<bool> _stopping{false};

    // Set true once start() has spawned the worker; stop() is a no-op
    // otherwise. Idempotent shutdown.
    bool _started = false;

    // Statistics
    std::atomic<uint64_t> _ops_processed{0};
    std::atomic<uint64_t> _ops_dropped{0};
    std::atomic<uint64_t> _batches_flushed{0};
};

} // namespace ranvier
