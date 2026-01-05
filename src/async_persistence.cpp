#include "async_persistence.hpp"
#include <seastar/core/thread.hh>
#include <seastar/core/sleep.hh>
#include <seastar/util/log.hh>

namespace ranvier {

static seastar::logger log_async_persist("async_persist");

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

    log_async_persist.info("Starting async persistence manager (flush_interval={}ms, max_batch={}, max_queue={})",
                           _config.flush_interval.count(), _config.max_batch_size, _config.max_queue_depth);

    // Start the flush timer
    _flush_timer.set_callback([this] { on_flush_timer(); });
    _flush_timer.arm_periodic(_config.flush_interval);

    // Start stats logging timer if enabled
    if (_config.enable_stats_logging) {
        _stats_timer.set_callback([this] { log_stats(); });
        _stats_timer.arm_periodic(_config.stats_interval);
    }

    return seastar::make_ready_future<>();
}

seastar::future<> AsyncPersistenceManager::stop() {
    log_async_persist.info("Stopping async persistence manager...");
    _stopping.store(true, std::memory_order_relaxed);

    // Cancel timers to prevent new batches from starting
    _flush_timer.cancel();
    _stats_timer.cancel();

    // Wait for any in-flight batches to complete before final flush
    // This ensures proper ordering of operations
    return _flush_gate.close().then([this] {
        // Now drain remaining queue with a final flush
        std::vector<PersistenceOp> final_batch;
        {
            std::lock_guard<std::mutex> lock(_queue_mutex);
            final_batch.reserve(_queue.size());
            while (!_queue.empty()) {
                final_batch.push_back(std::move(_queue.front()));
                _queue.pop_front();
            }
        }

        if (!final_batch.empty() && _store) {
            log_async_persist.info("Flushing {} pending operations before shutdown", final_batch.size());

            return seastar::async([this, batch = std::move(final_batch)]() mutable {
                process_batch(std::move(batch));
            });
        }

        return seastar::make_ready_future<>();
    });
}

void AsyncPersistenceManager::queue_save_route(std::span<const TokenId> tokens, BackendId backend_id) {
    if (_stopping.load(std::memory_order_relaxed) || !_store) return;

    SaveRouteOp op;
    op.tokens = std::vector<TokenId>(tokens.begin(), tokens.end());
    op.backend_id = backend_id;

    // Check backpressure inside the lock to avoid TOCTOU race
    std::lock_guard<std::mutex> lock(_queue_mutex);
    if (_queue.size() >= _config.max_queue_depth) {
        _ops_dropped++;
        return;
    }
    _queue.push_back(std::move(op));
}

void AsyncPersistenceManager::queue_save_backend(BackendId id, const std::string& ip, uint16_t port,
                                                  uint32_t weight, uint32_t priority) {
    if (_stopping.load(std::memory_order_relaxed) || !_store) return;

    SaveBackendOp op;
    op.id = id;
    op.ip = ip;
    op.port = port;
    op.weight = weight;
    op.priority = priority;

    // Check backpressure inside the lock to avoid TOCTOU race
    std::lock_guard<std::mutex> lock(_queue_mutex);
    if (_queue.size() >= _config.max_queue_depth) {
        _ops_dropped++;
        return;
    }
    _queue.push_back(std::move(op));
}

void AsyncPersistenceManager::queue_remove_backend(BackendId id) {
    if (_stopping.load(std::memory_order_relaxed) || !_store) return;

    RemoveBackendOp op;
    op.id = id;

    // Check backpressure inside the lock to avoid TOCTOU race
    std::lock_guard<std::mutex> lock(_queue_mutex);
    if (_queue.size() >= _config.max_queue_depth) {
        _ops_dropped++;
        return;
    }
    _queue.push_back(std::move(op));
}

void AsyncPersistenceManager::queue_remove_routes_for_backend(BackendId backend_id) {
    if (_stopping.load(std::memory_order_relaxed) || !_store) return;

    RemoveRoutesForBackendOp op;
    op.backend_id = backend_id;

    // Check backpressure inside the lock to avoid TOCTOU race
    std::lock_guard<std::mutex> lock(_queue_mutex);
    if (_queue.size() >= _config.max_queue_depth) {
        _ops_dropped++;
        return;
    }
    _queue.push_back(std::move(op));
}

void AsyncPersistenceManager::queue_clear_all() {
    if (_stopping.load(std::memory_order_relaxed) || !_store) return;

    ClearAllOp op;

    std::lock_guard<std::mutex> lock(_queue_mutex);
    // Clear all pending ops since we're clearing everything anyway
    // No backpressure check needed - clearing frees space
    _queue.clear();
    _queue.push_back(std::move(op));
}

size_t AsyncPersistenceManager::queue_depth() const {
    std::lock_guard<std::mutex> lock(_queue_mutex);
    return _queue.size();
}

void AsyncPersistenceManager::on_flush_timer() {
    if (_stopping.load(std::memory_order_relaxed) || !_store) return;

    // Extract a batch from the queue
    std::vector<PersistenceOp> batch;
    {
        std::lock_guard<std::mutex> lock(_queue_mutex);
        size_t batch_size = std::min(_queue.size(), _config.max_batch_size);
        if (batch_size == 0) return;

        batch.reserve(batch_size);
        for (size_t i = 0; i < batch_size; ++i) {
            batch.push_back(std::move(_queue.front()));
            _queue.pop_front();
        }
    }

    // Process the batch in a seastar::thread to avoid blocking the reactor
    // The gate ensures we wait for this during shutdown
    // The semaphore serializes batch processing to maintain operation order
    (void)seastar::try_with_gate(_flush_gate, [this, batch = std::move(batch)]() mutable {
        return seastar::get_units(_batch_semaphore, 1).then(
            [this, batch = std::move(batch)](seastar::semaphore_units<> units) mutable {
                return seastar::async([this, batch = std::move(batch), units = std::move(units)]() mutable {
                    process_batch(std::move(batch));
                    // units released automatically when destroyed
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
    // This runs in seastar::async (a separate system thread), safe for blocking I/O
    if (!_store || batch.empty()) return;

    // Collect routes for batch insert
    std::vector<RouteRecord> route_batch;

    // Helper to flush accumulated routes with error handling
    auto flush_routes = [this, &route_batch]() {
        if (route_batch.empty()) return;
        try {
            if (_store->save_routes_batch(route_batch)) {
                _ops_processed += route_batch.size();
            } else {
                log_async_persist.warn("Failed to save {} routes to persistence", route_batch.size());
            }
        } catch (const std::exception& e) {
            log_async_persist.error("Exception saving routes batch: {}", e.what());
        }
        route_batch.clear();
    };

    for (auto& op : batch) {
        try {
            std::visit([this, &route_batch, &flush_routes](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;

                if constexpr (std::is_same_v<T, SaveRouteOp>) {
                    // Accumulate routes for batch processing
                    RouteRecord record;
                    record.tokens = std::move(arg.tokens);
                    record.backend_id = arg.backend_id;
                    route_batch.push_back(std::move(record));
                }
                else if constexpr (std::is_same_v<T, SaveBackendOp>) {
                    // Flush any accumulated routes first
                    flush_routes();
                    // Process backend save immediately
                    if (_store->save_backend(arg.id, arg.ip, arg.port, arg.weight, arg.priority)) {
                        _ops_processed++;
                    } else {
                        log_async_persist.warn("Failed to save backend {} to persistence", arg.id);
                    }
                }
                else if constexpr (std::is_same_v<T, RemoveBackendOp>) {
                    // Flush any accumulated routes first
                    flush_routes();
                    // Process backend removal
                    if (_store->remove_backend(arg.id)) {
                        _ops_processed++;
                    } else {
                        log_async_persist.warn("Failed to remove backend {} from persistence", arg.id);
                    }
                }
                else if constexpr (std::is_same_v<T, RemoveRoutesForBackendOp>) {
                    // Flush any accumulated routes first
                    flush_routes();
                    // Process routes removal
                    if (_store->remove_routes_for_backend(arg.backend_id)) {
                        _ops_processed++;
                    } else {
                        log_async_persist.warn("Failed to remove routes for backend {} from persistence", arg.backend_id);
                    }
                }
                else if constexpr (std::is_same_v<T, ClearAllOp>) {
                    // Clear all - discard any pending routes (they'll be cleared anyway)
                    route_batch.clear();
                    if (_store->clear_all()) {
                        _ops_processed++;
                    } else {
                        log_async_persist.warn("Failed to clear persistence data");
                    }
                }
            }, op);
        } catch (const std::exception& e) {
            // Log and continue with remaining operations
            log_async_persist.error("Exception processing persistence operation: {}", e.what());
        }
    }

    // Flush any remaining accumulated routes
    flush_routes();

    _batches_flushed++;
}

void AsyncPersistenceManager::log_stats() {
    if (_stopping.load(std::memory_order_relaxed)) return;

    auto depth = queue_depth();
    log_async_persist.info("Stats: queue_depth={}, ops_processed={}, ops_dropped={}, batches_flushed={}",
                           depth, _ops_processed.load(), _ops_dropped.load(), _batches_flushed.load());

    if (_ops_dropped > 0) {
        log_async_persist.warn("Persistence queue overflow detected - {} operations dropped. "
                               "Consider increasing max_queue_depth or reducing request rate.",
                               _ops_dropped.load());
    }
}

} // namespace ranvier
