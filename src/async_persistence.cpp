#include "async_persistence.hpp"
#include <seastar/core/thread.hh>
#include <seastar/util/log.hh>

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
    : _config(std::move(config)) {}

void AsyncPersistenceManager::set_persistence_store(PersistenceStore* store) {
    _store = store;
}

seastar::future<> AsyncPersistenceManager::start() {
    if (!_store) {
        log_async_persist.warn("AsyncPersistenceManager started without a persistence store");
        return seastar::make_ready_future<>();
    }

    log_async_persist.info("Starting async persistence manager "
                           "(flush_interval={}ms, max_batch={}, max_queue={})",
                           _config.flush_interval.count(),
                           _config.max_batch_size,
                           _config.max_queue_depth);

    _flush_timer.set_callback([this] { on_flush_timer(); });
    _flush_timer.arm_periodic(_config.flush_interval);

    if (_config.enable_stats_logging) {
        _stats_timer.set_callback([this] { log_stats(); });
        _stats_timer.arm_periodic(_config.stats_interval);
    }

    return seastar::make_ready_future<>();
}

seastar::future<> AsyncPersistenceManager::stop() {
    log_async_persist.info("Stopping async persistence manager...");
    _stopping.store(true, std::memory_order_relaxed);

    _flush_timer.cancel();
    _stats_timer.cancel();

    // Wait for in-flight batches, then flush remaining queue
    return _flush_gate.close().then([this] {
        auto final_batch = drain_queue();

        if (!final_batch.empty() && _store) {
            log_async_persist.info("Flushing {} pending operations before shutdown",
                                   final_batch.size());
            return seastar::async([this, batch = std::move(final_batch)]() mutable {
                process_batch(std::move(batch));
            });
        }
        return seastar::make_ready_future<>();
    });
}

// ============================================================================
// Queue API
// ============================================================================

void AsyncPersistenceManager::queue_save_route(std::span<const TokenId> tokens,
                                                BackendId backend_id) {
    if (_stopping.load(std::memory_order_relaxed) || !_store) return;

    SaveRouteOp op;
    op.tokens = std::vector<TokenId>(tokens.begin(), tokens.end());
    op.backend_id = backend_id;
    try_enqueue(std::move(op));
}

void AsyncPersistenceManager::queue_save_backend(BackendId id, const std::string& ip,
                                                  uint16_t port, uint32_t weight,
                                                  uint32_t priority) {
    if (_stopping.load(std::memory_order_relaxed) || !_store) return;

    SaveBackendOp op{id, ip, port, weight, priority};
    try_enqueue(std::move(op));
}

void AsyncPersistenceManager::queue_remove_backend(BackendId id) {
    if (_stopping.load(std::memory_order_relaxed) || !_store) return;

    RemoveBackendOp op{id};
    try_enqueue(std::move(op));
}

void AsyncPersistenceManager::queue_remove_routes_for_backend(BackendId backend_id) {
    if (_stopping.load(std::memory_order_relaxed) || !_store) return;

    RemoveRoutesForBackendOp op{backend_id};
    try_enqueue(std::move(op));
}

void AsyncPersistenceManager::queue_clear_all() {
    if (_stopping.load(std::memory_order_relaxed) || !_store) return;

    std::lock_guard<std::mutex> lock(_queue_mutex);
    _queue.clear();  // Discard pending ops - they'll be cleared anyway
    _queue.push_back(ClearAllOp{});
}

// ============================================================================
// Queue Management (Private)
// ============================================================================

bool AsyncPersistenceManager::try_enqueue(PersistenceOp op) {
    std::lock_guard<std::mutex> lock(_queue_mutex);

    if (_queue.size() >= _config.max_queue_depth) {
        _ops_dropped++;
        return false;
    }

    _queue.push_back(std::move(op));
    return true;
}

std::vector<PersistenceOp> AsyncPersistenceManager::extract_batch() {
    std::lock_guard<std::mutex> lock(_queue_mutex);

    size_t batch_size = std::min(_queue.size(), _config.max_batch_size);
    if (batch_size == 0) return {};

    std::vector<PersistenceOp> batch;
    batch.reserve(batch_size);

    for (size_t i = 0; i < batch_size; ++i) {
        batch.push_back(std::move(_queue.front()));
        _queue.pop_front();
    }

    return batch;
}

std::vector<PersistenceOp> AsyncPersistenceManager::drain_queue() {
    std::lock_guard<std::mutex> lock(_queue_mutex);

    std::vector<PersistenceOp> all_ops;
    all_ops.reserve(_queue.size());

    while (!_queue.empty()) {
        all_ops.push_back(std::move(_queue.front()));
        _queue.pop_front();
    }

    return all_ops;
}

size_t AsyncPersistenceManager::queue_depth() const {
    std::lock_guard<std::mutex> lock(_queue_mutex);
    return _queue.size();
}

// ============================================================================
// Batch Processing
// ============================================================================

void AsyncPersistenceManager::on_flush_timer() {
    if (_stopping.load(std::memory_order_relaxed) || !_store) return;

    auto batch = extract_batch();
    if (batch.empty()) return;

    // Process batch off the reactor thread, serialized by semaphore
    (void)seastar::try_with_gate(_flush_gate, [this, batch = std::move(batch)]() mutable {
        return seastar::get_units(_batch_semaphore, 1).then(
            [this, batch = std::move(batch)](seastar::semaphore_units<> units) mutable {
                return seastar::async(
                    [this, batch = std::move(batch), units = std::move(units)]() mutable {
                        process_batch(std::move(batch));
                    });
            });
    }).handle_exception([](std::exception_ptr ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const seastar::gate_closed_exception&) {
            // Expected during shutdown
        } catch (const std::exception& e) {
            log_async_persist.error("Error in flush operation: {}", e.what());
        }
    });
}

void AsyncPersistenceManager::process_batch(std::vector<PersistenceOp> batch) {
    if (!_store || batch.empty()) return;

    // Pre-allocate for common case where batch is mostly routes
    RouteAccumulator routes(*this, batch.size());

    for (auto& op : batch) {
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

    if (_store->save_backend(op.id, op.ip, op.port, op.weight, op.priority)) {
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

// ============================================================================
// Statistics
// ============================================================================

void AsyncPersistenceManager::log_stats() {
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

} // namespace ranvier
