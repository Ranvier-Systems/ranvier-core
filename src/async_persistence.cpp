#include "async_persistence.hpp"
#include "worker_affinity.hpp"
#include <seastar/core/alien.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/util/log.hh>
#include <algorithm>
#include <chrono>

namespace ranvier {

static seastar::logger log_async_persist("async_persist");

// ============================================================================
// RouteAccumulator
// ============================================================================
// Helper class to batch route saves for efficiency.
// Flushes accumulated routes before other operations to maintain ordering.

class AsyncPersistenceManager::RouteAccumulator {
public:
    explicit RouteAccumulator(AsyncPersistenceManager& manager, size_t capacity_hint = 0)
        : _manager(manager) {
        if (capacity_hint > 0) {
            _routes.reserve(capacity_hint);
        }
    }

    ~RouteAccumulator() {
        flush();  // Ensure remaining routes are flushed on destruction
    }

    // Add a route to the batch (will be flushed later)
    // Uses move semantics to avoid copying the token vector
    void add(std::vector<TokenId>&& tokens, BackendId backend_id) {
        RouteRecord record;
        record.tokens = std::move(tokens);
        record.backend_id = backend_id;
        _routes.push_back(std::move(record));
    }

    // Flush accumulated routes to the persistence store
    void flush() {
        if (_routes.empty()) return;

        try {
            if (_manager._store->save_routes_batch(_routes)) {
                _manager._ops_processed += _routes.size();
            } else {
                log_async_persist.warn("Failed to save {} routes to persistence", _routes.size());
            }
        } catch (const std::exception& e) {
            log_async_persist.error("Exception saving routes batch: {}", e.what());
        }
        _routes.clear();
    }

    // Discard accumulated routes (used before clear_all)
    void discard() {
        _routes.clear();
    }

private:
    AsyncPersistenceManager& _manager;
    std::vector<RouteRecord> _routes;
};

// ============================================================================
// Lifecycle
// ============================================================================

AsyncPersistenceManager::AsyncPersistenceManager(AsyncPersistenceConfig config)
    : _config(std::move(config))
    , _store(create_persistence_store())
    , _ring(std::make_unique<MpscRingBuffer<PersistenceOp>>(_config.max_queue_depth)) {}

AsyncPersistenceManager::AsyncPersistenceManager(AsyncPersistenceConfig config,
                                                   std::unique_ptr<PersistenceStore> store)
    : _config(std::move(config))
    , _store(std::move(store))
    , _ring(std::make_unique<MpscRingBuffer<PersistenceOp>>(_config.max_queue_depth)) {}

bool AsyncPersistenceManager::open(const std::string& path) {
    if (!_store) {
        log_async_persist.error("No persistence store available");
        return false;
    }
    return _store->open(path);
}

void AsyncPersistenceManager::close() {
    if (_store && _store->is_open()) {
        _store->close();
    }
}

bool AsyncPersistenceManager::is_open() const {
    return _store && _store->is_open();
}

AsyncPersistenceManager::~AsyncPersistenceManager() {
    // Defensive safety net for the case where the caller forgot to await
    // stop(). Properly shutting down via stop() is REQUIRED for a clean
    // teardown — this branch is BEST-EFFORT only.
    //
    // KNOWN HAZARD: when we reach here without stop(), the worker's
    // post-drain seastar::alien::run_on() will have queued a lambda
    // capturing [this] on the reactor's mailbox. join() returns as soon as
    // the worker thread exits, which races with the lambda actually
    // running on the reactor. If *this is destroyed before the lambda
    // fires, the lambda accesses freed memory (UAF). Mitigating this
    // robustly would require extra synchronisation (atomic suppression +
    // barrier) that has its own races; not worth fixing for a path that
    // is already a programmer error. Always call stop() and await its
    // future before destroying.
    if (_worker_thread && _worker_thread->joinable()) {
        log_async_persist.warn("AsyncPersistenceManager destructed without stop() — "
                               "joining worker thread (best-effort)");
        _shutdown.store(true, std::memory_order_release);
        _worker_thread->join();
    }
}

seastar::future<> AsyncPersistenceManager::start(seastar::alien::instance& alien_instance) {
    if (!_store || !_store->is_open()) {
        log_async_persist.warn("AsyncPersistenceManager started without an open persistence store");
        return seastar::make_ready_future<>();
    }

    if (_started) {
        log_async_persist.warn("AsyncPersistenceManager::start() called twice — ignoring");
        return seastar::make_ready_future<>();
    }

    log_async_persist.info("Starting async persistence manager "
                           "(flush_interval={}ms, max_batch={}, max_queue={})",
                           _config.flush_interval.count(),
                           _config.max_batch_size,
                           _config.max_queue_depth);

    _alien_instance = &alien_instance;
    _owner_shard = seastar::this_shard_id();
    _shutdown.store(false, std::memory_order_release);
    _stopping.store(false, std::memory_order_release);

    // Spawn the dedicated SQLite worker thread.
    // IMPORTANT: alien_instance is captured by reference; its lifetime is
    // owned by app_template and outlives this manager.
    _worker_thread = std::make_unique<std::thread>([this]() {
        worker_loop();
    });

    // Pin the worker off the reactor cores. Best-effort: failure (empty
    // non-reactor cpuset, non-Linux, syscall error) is logged inside the
    // helper and the worker simply continues with inherited affinity.
    // Single-worker caller, so worker_index=0 picks the first non-reactor
    // core deterministically.
    worker_affinity::pin_worker_to_non_reactor_core(
        _worker_thread->native_handle(), /*worker_index=*/0, "persistence");

    if (_config.enable_stats_logging) {
        _stats_timer.set_callback([this] { log_stats(); });
        _stats_timer.arm_periodic(_config.stats_interval);
    }

    _started = true;
    return seastar::make_ready_future<>();
}

seastar::future<> AsyncPersistenceManager::stop() {
    if (!_started) {
        return seastar::make_ready_future<>();
    }
    if (_stopping.exchange(true, std::memory_order_acq_rel)) {
        // stop() already in progress / completed.
        return seastar::make_ready_future<>();
    }

    log_async_persist.info("Stopping async persistence manager...");

    // Order is critical:
    //   1. Close timer gate, cancel stats timer (no more reactor callbacks).
    //   2. Set _shutdown, signalling the worker to drain remaining ops and
    //      exit its loop.
    //   3. Worker, on exit, calls alien::run_on to fulfil _worker_done on
    //      this shard. We await that future (with timeout) — the reactor
    //      is NOT blocked on a thread join.
    //   4. Once the future resolves OR the timeout fires, the thread has
    //      already exited; join is effectively instantaneous.
    //
    // NOTE: stop() is one-way. _started is intentionally left true after
    // stop() resolves; restart is not supported because seastar::gate
    // (used for _timer_gate) cannot be reopened, so a second start() would
    // silently break stats logging. start() detects this via the _started
    // check and ignores re-invocation.
    _worker_done.emplace();
    auto done_future = _worker_done->get_future();

    return _timer_gate.close().then([this] {
        _stats_timer.cancel();
        // Tell the worker to drain remaining queue and exit.
        _shutdown.store(true, std::memory_order_release);
    }).then([this, done_future = std::move(done_future)]() mutable {
        // Bound the wait. If the worker's alien::run_on dispatch fails
        // (reactor torn down, allocation failure, etc.), the promise is
        // never resolved and stop() would otherwise hang forever. The
        // worker thread itself exits regardless of dispatch success, so
        // joining below is safe even on timeout.
        constexpr auto WORKER_EXIT_TIMEOUT = std::chrono::seconds(5);
        return seastar::with_timeout(
                   seastar::lowres_clock::now() + WORKER_EXIT_TIMEOUT,
                   std::move(done_future))
            .handle_exception([](std::exception_ptr ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const seastar::timed_out_error&) {
                    log_async_persist.error(
                        "Persistence worker did not signal completion "
                        "within 5s — alien dispatch may have failed; "
                        "joining anyway");
                } catch (const std::exception& e) {
                    log_async_persist.error(
                        "Persistence worker completion future failed: {}",
                        e.what());
                }
            });
    }).then([this] {
        if (_worker_thread && _worker_thread->joinable()) {
            _worker_thread->join();
        }
        _worker_thread.reset();
        log_async_persist.info("Async persistence manager stopped");
    });
}

// ============================================================================
// Queue API
// ============================================================================

void AsyncPersistenceManager::queue_save_route(std::span<const TokenId> tokens,
                                                BackendId backend_id) {
    if (_stopping.load(std::memory_order_relaxed) || !is_open()) return;

    SaveRouteOp op;
    op.tokens = std::vector<TokenId>(tokens.begin(), tokens.end());
    op.backend_id = backend_id;
    try_enqueue(std::move(op));
}

void AsyncPersistenceManager::queue_save_backend(BackendId id, const std::string& ip,
                                                  uint16_t port, uint32_t weight,
                                                  uint32_t priority,
                                                  const std::string& backend_type,
                                                  const std::string& pool_role) {
    if (_stopping.load(std::memory_order_relaxed) || !is_open()) return;

    SaveBackendOp op{id, ip, port, weight, priority, backend_type, pool_role};
    try_enqueue(std::move(op));
}

void AsyncPersistenceManager::queue_remove_backend(BackendId id) {
    if (_stopping.load(std::memory_order_relaxed) || !is_open()) return;

    RemoveBackendOp op{id};
    try_enqueue(std::move(op));
}

void AsyncPersistenceManager::queue_remove_routes_for_backend(BackendId backend_id) {
    if (_stopping.load(std::memory_order_relaxed) || !is_open()) return;

    RemoveRoutesForBackendOp op{backend_id};
    try_enqueue(std::move(op));
}

void AsyncPersistenceManager::queue_log_request(LogRequestOp op) {
    if (_stopping.load(std::memory_order_relaxed) || !is_open()) return;
    if (!_config.attribution_persistence_enabled) return;
    try_enqueue(std::move(op));
}

void AsyncPersistenceManager::queue_clear_all() {
    if (_stopping.load(std::memory_order_relaxed) || !is_open()) return;

    // Bump generation counter so the consumer (process_batch) will skip all
    // ops enqueued before this point. Can't drain from producer side — only
    // the consumer may read from the MPSC ring buffer.
    _clear_generation.fetch_add(1, std::memory_order_release);

    // Enqueue the ClearAllOp bypassing backpressure — the ring buffer has
    // capacity beyond the configured max_queue_depth (rounded to power of two),
    // so there is always room even when backpressured. Push directly to avoid
    // the _queue_size >= max_queue_depth check in try_enqueue().
    PersistenceOp op{ClearAllOp{}};
    if (_ring->try_push(std::move(op))) {
        _queue_size.fetch_add(1, std::memory_order_relaxed);
    }
}

// ============================================================================
// Queue Management (Private)
// ============================================================================

bool AsyncPersistenceManager::try_enqueue(PersistenceOp op) {
    // Reserve a slot by incrementing _queue_size FIRST, then push. This avoids
    // a TOCTOU race where multiple producers pass the backpressure check
    // simultaneously and overshoot max_queue_depth.
    const auto prev = _queue_size.fetch_add(1, std::memory_order_relaxed);
    if (prev >= _config.max_queue_depth) {
        // Over limit — undo the reservation.
        _queue_size.fetch_sub(1, std::memory_order_relaxed);
        _ops_dropped++;
        return false;
    }
    if (!_ring->try_push(std::move(op))) {
        // Ring buffer full (shouldn't happen if capacity >= max_queue_depth).
        _queue_size.fetch_sub(1, std::memory_order_relaxed);
        _ops_dropped++;
        return false;
    }
    return true;
}

std::vector<PersistenceOp> AsyncPersistenceManager::extract_batch() {
    std::vector<PersistenceOp> batch;
    size_t drained = _ring->drain(batch, _config.max_batch_size);
    if (drained > 0) {
        _queue_size.fetch_sub(drained, std::memory_order_relaxed);
    }
    return batch;
}

std::vector<PersistenceOp> AsyncPersistenceManager::drain_queue() {
    std::vector<PersistenceOp> all_ops;
    size_t drained = _ring->drain_all(all_ops);
    if (drained > 0) {
        _queue_size.fetch_sub(drained, std::memory_order_relaxed);
    }
    return all_ops;
}

size_t AsyncPersistenceManager::queue_depth() const {
    // Lock-free: safe to call from reactor thread without blocking
    return _queue_size.load(std::memory_order_relaxed);
}

// ============================================================================
// Batch Processing
// ============================================================================

void AsyncPersistenceManager::worker_loop() {
    log_async_persist.debug("Persistence worker thread started");

    // Process batches as long as the manager is running. extract_batch() and
    // drain_queue() are both single-consumer operations on the MPSC ring;
    // this thread is the sole consumer.
    while (!_shutdown.load(std::memory_order_acquire)) {
        auto batch = extract_batch();
        if (!batch.empty()) {
            process_batch(std::move(batch));
            continue;
        }
        // Queue empty: sleep for the configured flush interval. This is the
        // batching knob — short interval = lower latency, more wakeups.
        std::this_thread::sleep_for(_config.flush_interval);
    }

    // Shutdown requested. Drain whatever the producers left behind so we
    // don't lose pending writes. Producers are gated by _stopping (set in
    // stop() before _shutdown), so the queue size is bounded from here on.
    while (true) {
        auto batch = extract_batch();
        if (batch.empty()) break;
        process_batch(std::move(batch));
    }

    log_async_persist.debug("Persistence worker thread draining complete");

    // Signal the reactor that we're done. Use alien::run_on to deliver the
    // promise resolution onto the owner shard. The thread itself exits
    // immediately after this dispatch returns; stop() then joins us.
    if (_alien_instance != nullptr) {
        try {
            seastar::alien::run_on(*_alien_instance, _owner_shard, [this]() noexcept {
                if (_worker_done.has_value()) {
                    _worker_done->set_value();
                }
            });
        } catch (const std::exception& e) {
            // If alien dispatch fails (reactor torn down already), there's
            // nothing useful we can do. stop() may hang on the future, but
            // that mirrors the pre-existing failure mode.
            log_async_persist.error("Worker failed to signal completion via alien: {}",
                                    e.what());
        }
    }
}

void AsyncPersistenceManager::process_batch(std::vector<PersistenceOp> batch) {
    if (!is_open() || batch.empty()) return;

    // Snapshot the clear generation before processing. If a queue_clear_all()
    // was called, all pre-existing ops in this batch are stale — skip them
    // and only process ops from the current generation (the ClearAllOp itself
    // and anything enqueued after it).
    const auto gen_before = _clear_generation.load(std::memory_order_acquire);

    // Pre-allocate for common case where batch is mostly routes
    RouteAccumulator routes(*this, batch.size());

    bool seen_clear = false;
    for (auto& op : batch) {
        // If a clear was requested and we haven't seen the ClearAllOp yet,
        // skip stale ops. Once we see ClearAllOp, process it and everything after.
        if (gen_before != _last_processed_generation && !seen_clear) {
            if (std::holds_alternative<ClearAllOp>(op)) {
                seen_clear = true;
                _last_processed_generation = gen_before;
                // Fall through to execute the ClearAllOp
            } else {
                continue;  // Skip stale op
            }
        }

        try {
            std::visit([this, &routes](auto&& arg) {
                execute(arg, routes);
            }, op);
        } catch (const std::exception& e) {
            log_async_persist.error("Exception processing persistence operation: {}", e.what());
        }
    }

    // RouteAccumulator destructor flushes remaining routes
    _batches_flushed++;
}

// ============================================================================
// Operation Handlers
// ============================================================================

void AsyncPersistenceManager::execute(SaveRouteOp& op, RouteAccumulator& routes) {
    // Accumulate for batch insert (more efficient than individual inserts)
    // Move tokens to avoid copying the vector
    routes.add(std::move(op.tokens), op.backend_id);
}

void AsyncPersistenceManager::execute(const SaveBackendOp& op, RouteAccumulator& routes) {
    routes.flush();  // Flush routes first to maintain ordering

    if (_store->save_backend(op.id, op.ip, op.port, op.weight, op.priority, op.backend_type,
                             op.pool_role)) {
        _ops_processed++;
    } else {
        log_async_persist.warn("Failed to save backend {} to persistence", op.id);
    }
}

void AsyncPersistenceManager::execute(const RemoveBackendOp& op, RouteAccumulator& routes) {
    routes.flush();

    if (_store->remove_backend(op.id)) {
        _ops_processed++;
    } else {
        log_async_persist.warn("Failed to remove backend {} from persistence", op.id);
    }
}

void AsyncPersistenceManager::execute(const RemoveRoutesForBackendOp& op, RouteAccumulator& routes) {
    routes.flush();

    if (_store->remove_routes_for_backend(op.backend_id)) {
        _ops_processed++;
    } else {
        log_async_persist.warn("Failed to remove routes for backend {} from persistence",
                               op.backend_id);
    }
}

void AsyncPersistenceManager::execute(const ClearAllOp& /*op*/, RouteAccumulator& routes) {
    routes.discard();  // Don't bother flushing - we're clearing everything

    if (_store->clear_all()) {
        _ops_processed++;
    } else {
        log_async_persist.warn("Failed to clear persistence data");
    }
}

void AsyncPersistenceManager::execute(LogRequestOp& op, RouteAccumulator& routes) {
    // Flush any accumulated routes first to maintain ordering with attribution
    // log inserts (operators expect that a SaveRouteOp enqueued before a
    // LogRequestOp lands in the DB before it).
    routes.flush();

    // Build the attribution record. Hard Rule #7 — this is dumb storage;
    // service layer (handle_proxy) owns all sanitisation/clamping.
    RequestAttributionRecord rec;
    rec.request_id    = std::move(op.request_id);
    rec.api_key_id    = std::move(op.api_key_id);
    rec.timestamp_ms  = op.timestamp_ms;
    rec.endpoint      = std::move(op.endpoint);
    rec.backend_id    = op.backend_id;
    rec.status_code   = op.status_code;
    rec.latency_ms    = op.latency_ms;
    rec.input_tokens  = op.input_tokens;
    rec.output_tokens = op.output_tokens;
    rec.cost_units    = op.cost_units;
    rec.tokens_estimated = op.tokens_estimated;

    if (_store->log_request(rec)) {
        _ops_processed++;
        _log_requests_inserted++;
        _log_requests_since_prune++;
        // Periodic prune probe (memo §7.3 Option A) — bounded by threshold,
        // not per-flush.
        if (_log_requests_since_prune >= kPruningProbeThreshold) {
            _log_requests_since_prune = 0;
            maybe_prune_request_attribution();
        }
    } else {
        log_async_persist.warn("Failed to log request attribution row "
                               "(request_id={})", rec.request_id);
    }
}

void AsyncPersistenceManager::maybe_prune_request_attribution() {
    if (!is_open() || _config.attribution_max_request_rows == 0) return;

    const size_t count = _store->request_attribution_count();
    if (count <= _config.attribution_max_request_rows) {
        return;
    }
    const size_t pruned = _store->prune_request_attribution(_config.attribution_max_request_rows);
    if (pruned > 0) {
        log_async_persist.info(
            "attribution: pruned {} oldest request_attribution rows "
            "(count={} > max_request_rows={})",
            pruned, count, _config.attribution_max_request_rows);
    }
}

// Read-only attribution query (memo §8). Materialises the rows synchronously
// on the calling reactor shard and computes per-key percentiles in C++.
//
// TODO(attribution): refactor to run the SQLite SELECT on the dedicated
// persistence worker thread via the ring-buffer + alien::run_on reply
// pattern, mirroring the write path. The /admin/keys/usage endpoint is
// low-frequency (operator-initiated) but the SQLite read still mutex-blocks
// the reactor here. seastar::async is intentionally NOT used (Rule #12 — it
// runs ON the reactor, not on a worker), so we accept a short synchronous
// stall until the worker-reply plumbing exists.
seastar::future<std::vector<AttributionSummaryRow>>
AsyncPersistenceManager::query_attribution_summary(
    int64_t from_ms, int64_t to_ms, std::string api_key_id_filter,
    size_t row_limit, bool* truncated) {

    if (!is_open()) {
        if (truncated) *truncated = false;
        return seastar::make_ready_future<std::vector<AttributionSummaryRow>>();
    }

    // Synchronous shim: invoke the lambda inline. Hard Rule #16 considerations
    // do not apply (no .then() continuation, no co_await). Hard Rule #12
    // applies — see TODO above.
    auto run = [this,
                from_ms, to_ms,
                filter = std::move(api_key_id_filter),
                row_limit, truncated]() mutable {
        std::vector<RequestAttributionRecord> rows =
            _store->query_request_attribution(from_ms, to_ms, filter, row_limit);

        if (truncated) {
            *truncated = (rows.size() >= row_limit);
        }

        // Aggregate by api_key_id. The SQL ORDER BY (api_key_id, latency_ms)
        // means rows for one key are contiguous and already sorted by
        // latency, so we can compute percentiles by index.
        std::vector<AttributionSummaryRow> out;
        if (rows.empty()) return out;

        auto compute_pct = [](const std::vector<int32_t>& sorted_lat, double q) -> int32_t {
            if (sorted_lat.empty()) return 0;
            // sorted_lat is ascending; pick the q-quantile by nearest-rank.
            size_t idx = static_cast<size_t>(q * (sorted_lat.size() - 1));
            if (idx >= sorted_lat.size()) idx = sorted_lat.size() - 1;
            return sorted_lat[idx];
        };

        size_t i = 0;
        while (i < rows.size()) {
            const std::string& key = rows[i].api_key_id;
            AttributionSummaryRow agg;
            agg.api_key_id = key;
            std::vector<int32_t> latencies;
            latencies.reserve(64);
            size_t j = i;
            while (j < rows.size() && rows[j].api_key_id == key) {
                const auto& r = rows[j];
                agg.request_count++;
                if (r.status_code >= 200 && r.status_code < 400) {
                    agg.success_count++;
                } else {
                    agg.error_count++;
                }
                latencies.push_back(r.latency_ms);
                agg.input_tokens_sum  += r.input_tokens;
                agg.output_tokens_sum += r.output_tokens;
                agg.cost_units_sum    += r.cost_units;
                ++j;
            }
            // latencies are already sorted ascending by SQL ORDER BY clause.
            agg.latency_ms_p50 = compute_pct(latencies, 0.50);
            agg.latency_ms_p95 = compute_pct(latencies, 0.95);
            agg.latency_ms_p99 = compute_pct(latencies, 0.99);
            out.push_back(std::move(agg));
            i = j;
        }
        return out;
    };
    return seastar::make_ready_future<std::vector<AttributionSummaryRow>>(run());
}

// ============================================================================
// Statistics
// ============================================================================

void AsyncPersistenceManager::log_stats() {
    // RAII Timer Safety: Acquire gate holder to prevent execution during shutdown.
    // If the gate is closed (stop() in progress), this throws gate_closed_exception
    // and the callback safely exits without accessing any member state.
    seastar::gate::holder timer_holder;
    try {
        timer_holder = _timer_gate.hold();
    } catch (const seastar::gate_closed_exception&) {
        // Gate closed - stop() is in progress, exit safely
        return;
    }

    if (_stopping.load(std::memory_order_relaxed)) return;

    auto depth = queue_depth();
    auto processed = _ops_processed.load();
    auto dropped = _ops_dropped.load();
    auto batches = _batches_flushed.load();

    log_async_persist.info("Stats: queue_depth={}, ops_processed={}, ops_dropped={}, "
                           "batches_flushed={}",
                           depth, processed, dropped, batches);

    if (dropped > 0) {
        log_async_persist.warn("Persistence queue overflow detected - {} operations dropped. "
                               "Consider increasing max_queue_depth or reducing request rate.",
                               dropped);
    }
}

// ============================================================================
// Read Delegation Methods
// ============================================================================
// These methods delegate to the underlying store for synchronous reads.
// Used during startup (state restoration) and shutdown (summary logging).

std::vector<BackendRecord> AsyncPersistenceManager::load_backends() {
    if (!is_open()) return {};
    return _store->load_backends();
}

std::vector<RouteRecord> AsyncPersistenceManager::load_routes() {
    if (!is_open()) return {};
    return _store->load_routes();
}

bool AsyncPersistenceManager::checkpoint() {
    if (!is_open()) return false;
    return _store->checkpoint();
}

bool AsyncPersistenceManager::verify_integrity() {
    if (!is_open()) return false;
    return _store->verify_integrity();
}

bool AsyncPersistenceManager::clear_all() {
    if (!is_open()) return false;
    return _store->clear_all();
}

size_t AsyncPersistenceManager::last_load_skipped_count() const {
    if (!is_open()) return 0;
    return _store->last_load_skipped_count();
}

size_t AsyncPersistenceManager::backend_count() const {
    if (!is_open()) return 0;
    return _store->backend_count();
}

size_t AsyncPersistenceManager::route_count() const {
    if (!is_open()) return 0;
    return _store->route_count();
}

} // namespace ranvier
