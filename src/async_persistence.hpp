#pragma once

#include "persistence.hpp"
#include "mpsc_ring_buffer.hpp"
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sharded.hh>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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
 * THREADING MODEL:
 * The underlying SqlitePersistence uses std::mutex because SQLite is not
 * thread-safe. To avoid blocking the Seastar reactor thread, this class:
 *
 *   1. Provides fire-and-forget queue_*() methods for writes (non-blocking)
 *   2. Processes queued operations in batches via seastar::async()
 *   3. Runs SQLite operations on Seastar's thread pool (not the reactor)
 *
 * LOCK-FREE MPSC QUEUE:
 * The operation queue uses a bounded multi-producer single-consumer ring
 * buffer (Vyukov-style) with per-slot sequence numbers. Multiple reactor
 * shards (producers) CAS-compete on the tail index to enqueue operations
 * lock-free. The persistence worker (sole consumer) drains batches via
 * extract_batch()/drain_queue().
 *
 * ARCHITECTURAL GUARANTEE:
 *   HttpController → AsyncPersistenceManager → seastar::async → SqlitePersistence
 *                 ↑ (lock-free enqueue)                ↓
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

// Per-API-key attribution log row (memo §7.4). Single enqueue per request,
// from the terminal phase of handle_proxy. Plain std::string fields — the
// existing codebase relies on Seastar's do_foreign_free for cross-shard
// destruction of SaveBackendOp.ip / SaveRouteOp.tokens, and this op follows
// the same pattern. (Hard Rule #14 verified — sound.)
struct LogRequestOp {
    std::string request_id;
    std::string api_key_id;
    int64_t     timestamp_ms = 0;
    std::string endpoint;
    std::optional<BackendId> backend_id;
    int32_t     status_code = 0;
    int32_t     latency_ms = 0;
    int64_t     input_tokens = 0;
    int64_t     output_tokens = 0;
    double      cost_units = 0.0;
};

using PersistenceOp = std::variant<
    SaveRouteOp,
    SaveBackendOp,
    RemoveBackendOp,
    RemoveRoutesForBackendOp,
    ClearAllOp,
    LogRequestOp
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

    // Per-API-key attribution (memo §7). When persistence_enabled is false,
    // queue_log_request() silently drops; the request_attribution table is
    // still created but stays empty.
    bool attribution_persistence_enabled = true;
    uint32_t attribution_max_request_rows = 1'000'000;
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
 *   1. Accepting operations on the reactor thread (lock-free enqueue)
 *   2. Processing them in seastar::async() on a separate thread pool
 *   3. Using mutex protection safely (thread pool threads can block)
 *
 * Architecture:
 *   - Lock-free MPSC ring buffer between reactor (producer) and worker (consumer)
 *   - Background timer drains ring buffer and writes to SQLite in batches
 *   - SQLite operations run in seastar::async (off the reactor thread)
 *   - Semaphore ensures batches are processed in order
 *
 * Thread Safety:
 *   - Queue is a lock-free MPSC ring buffer (no mutex on reactor thread)
 *   - SQLite operations serialized via semaphore
 *   - Shutdown flag is atomic for visibility across shards
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
    // These methods queue operations and return immediately (lock-free).
    // Safe to call from any shard (MPSC: multiple producers supported).
    // Operations silently dropped if store not open.

    void queue_save_route(std::span<const TokenId> tokens, BackendId backend_id);
    void queue_save_backend(BackendId id, const std::string& ip, uint16_t port,
                           uint32_t weight = 100, uint32_t priority = 0);
    void queue_remove_backend(BackendId id);
    void queue_remove_routes_for_backend(BackendId backend_id);
    void queue_clear_all();

    // Per-API-key attribution: enqueue one LogRequestOp from handle_proxy's
    // terminal phase. Silently dropped when attribution_persistence_enabled
    // is false. By value + move (Hard Rule #21).
    void queue_log_request(LogRequestOp op);

    // Read-only attribution query for /admin/keys/usage (memo §8). Aggregates
    // per-key stats from a single bounded SELECT and computes percentiles in
    // memory via std::nth_element on the latency vector.
    //
    // Runs the SQLite read on the persistence worker thread via
    // seastar::async, returning to the caller's shard with the result vector
    // copied locally (Hard Rule #14).
    seastar::future<std::vector<AttributionSummaryRow>> query_attribution_summary(
        int64_t from_ms, int64_t to_ms, std::string api_key_id_filter,
        size_t row_limit, bool* truncated);

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
    void execute(LogRequestOp& op, RouteAccumulator& routes);

    // Probe row count and prune oldest rows when over the configured cap.
    // Memo §7.3 Option A. Runs at most once per pruning_probe_threshold log
    // inserts (bounded — Hard Rule #4 / #17).
    void maybe_prune_request_attribution();

    // --- Statistics ---

    void log_stats();

    // --- Member Variables ---

    AsyncPersistenceConfig _config;
    std::unique_ptr<PersistenceStore> _store;  // Owned persistence engine

    // Lock-free MPSC ring buffer: multiple reactor shards produce, single
    // consumer (flush timer -> seastar::async worker) drains batches.
    // Capacity is next power-of-two >= max_queue_depth.
    std::unique_ptr<MpscRingBuffer<PersistenceOp>> _ring;
    std::atomic<size_t> _queue_size{0};  // Maintained for metrics compatibility

    // Generation counter for queue_clear_all(). Producer increments; consumer
    // checks before processing each batch and discards if generation changed.
    std::atomic<uint64_t> _clear_generation{0};
    uint64_t _last_processed_generation{0};  // Consumer-only, no atomic needed

    // Timers
    seastar::timer<> _flush_timer;
    seastar::timer<> _stats_timer;

    // Synchronization
    //
    // TIMER CALLBACK SAFETY (RAII Guard Pattern):
    // Timer callbacks capture `this`, creating a potential use-after-free if the
    // callback executes after destruction begins. The race window is:
    //   1. Timer fires, callback is queued on reactor
    //   2. stop() is called, cancels timer (but callback is already queued)
    //   3. stop() returns, destructor can begin
    //   4. Queued callback executes with dangling `this`
    //
    // Solution: _timer_gate ensures timer callbacks cannot execute during shutdown.
    // - Timer callbacks acquire a gate::holder at entry (fails if gate is closed)
    // - stop() closes the gate FIRST (waits for in-flight callbacks to complete)
    // - Only then are timers cancelled and resources freed
    //
    // This guarantees: No timer callback can access `this` after stop() returns.
    //
    seastar::gate _timer_gate;              // RAII guard for timer callback lifetime
    seastar::gate _flush_gate;              // Tracks in-flight flush operations
    seastar::semaphore _batch_semaphore{1}; // Serializes batch processing

    // State
    std::atomic<bool> _stopping{false};

    // Statistics
    std::atomic<uint64_t> _ops_processed{0};
    std::atomic<uint64_t> _ops_dropped{0};
    std::atomic<uint64_t> _batches_flushed{0};

    // Per-API-key attribution counters (consumer-shard only — no atomics).
    // _log_requests_since_prune triggers a probe-and-prune pass when it
    // crosses pruning_probe_threshold.
    uint64_t _log_requests_inserted = 0;
    uint64_t _log_requests_since_prune = 0;
    static constexpr uint64_t kPruningProbeThreshold = 1000;
};

} // namespace ranvier
