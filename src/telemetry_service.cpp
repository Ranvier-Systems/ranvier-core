// Ranvier Core - Telemetry Service Implementation
//
// See telemetry_service.hpp for the contract and Hard-Rules summary.

#include "telemetry_service.hpp"

#include "types.hpp"

#include <boost/range/irange.hpp>

#include <chrono>
#include <utility>

#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/core/loop.hh>           // parallel_for_each
#include <seastar/core/sharded.hh>        // also provides foreign_ptr / make_foreign
#include <seastar/core/smp.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/util/log.hh>

namespace ranvier {

namespace {

seastar::logger& log_telemetry() {
    static seastar::logger l("telemetry_sink");
    return l;
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace

// =============================================================================
// Per-shard configuration
// =============================================================================

seastar::future<> TelemetryService::start_shard(
    TelemetrySinkConfig config,
    std::function<uint64_t()> eviction_counter_getter) {
    _enabled                 = config.enabled;
    _max_buckets             = config.max_buckets;
    _window                  = config.window;  // emitter (shard 0) uses this; harmless on other shards
    _eviction_counter_getter = std::move(eviction_counter_getter);

    // Snapshot the eviction baseline now so the first window's delta is
    // measured from start-time, not from process start.
    _eviction_last_seen = _eviction_counter_getter ? _eviction_counter_getter() : 0;

    // Pre-insert the overflow sentinel so the cap-reached fast path doesn't
    // need to allocate. The sentinel does NOT count toward _max_buckets
    // (it's always present); see get_or_overflow_slot().
    ensure_overflow_sentinel();

    return seastar::make_ready_future<>();
}

void TelemetryService::ensure_overflow_sentinel() {
    auto key = overflow_key();
    if (_buckets.find(key) == _buckets.end()) {
        _buckets.emplace(std::move(key), AggregateRecord{});
    }
    // The sentinel's `key` field is left default-constructed inside
    // AggregateRecord; snapshot_and_reset() rewrites it before export.
}

// =============================================================================
// Shard-0 emitter
// =============================================================================

seastar::future<> TelemetryService::start_emitter(
    seastar::sharded<TelemetryService>* container,
    std::unique_ptr<TelemetrySink> sink,
    std::function<RoutingStrategyParams()> strategy_snapshot) {

    // Defensive: this method must be called on shard 0 only. Callers in
    // application.cpp use invoke_on(0, ...) — guard against misuse.
    if (seastar::this_shard_id() != 0) {
        return seastar::make_exception_future<>(
            std::runtime_error("TelemetryService::start_emitter must run on shard 0"));
    }

    // If start_emitter has already run, replace sink and re-arm timer.
    if (_emitter_started) {
        _emit_timer.cancel();
    }

    _container          = container;
    _sink               = std::move(sink);
    _strategy_snapshot  = std::move(strategy_snapshot);
    _window_start_ms    = now_ms();

    // Only run the emitter when telemetry is actually enabled. With
    // _enabled=false the timer never arms, recording is a single branch on
    // every shard, and the sink object is held but never invoked.
    if (!_enabled) {
        log_telemetry().info("telemetry sink disabled; emitter not armed");
        _emitter_started = true;
        return seastar::make_ready_future<>();
    }

    // _window was set by start_shard() from the same config. Config
    // validation in config_loader.cpp rejects window <= 0 when enabled,
    // so this is safe.
    _emit_timer.set_callback([this] { on_emit_timer(); });
    _emit_timer.arm(_window);

    // Register shard-0-only observability counters for the emitter itself.
    // Hard Rule #6: these are deregistered first in stop().
    namespace sm = seastar::metrics;
    _emitter_metrics.add_group("ranvier_telemetry_sink", {
        sm::make_counter("reports_emitted_total", _reports_emitted,
            sm::description("Periodic window reports successfully handed to the telemetry sink")),
        sm::make_counter("reports_dropped_backpressure_total", _reports_dropped_backpressure,
            sm::description("Window reports dropped because the prior consume() future had not "
                            "resolved by the next window tick (sink slow / down)")),
    });

    _emitter_started = true;
    log_telemetry().info("telemetry sink emitter armed (window={}s, max_buckets={})",
                         _window.count(), _max_buckets);
    return seastar::make_ready_future<>();
}

// =============================================================================
// Recording entry (hot path)
// =============================================================================

void TelemetryService::record_outcome(const TelemetryBucketKey& key,
                                      const TelemetryOutcome& outcome) {
    // Hard Rule #4 / negligible-disabled-cost: single predictable branch.
    if (!_enabled) {
        return;
    }

    AggregateRecord* slot = get_or_overflow_slot(key);
    if (!slot) {
        return;  // Shouldn't happen — sentinel is pre-inserted — but be defensive.
    }

    ++slot->request_count;
    if (outcome.cache_hit) {
        ++slot->cache_hit_count;
    } else {
        ++slot->cache_miss_count;
    }
    if (outcome.was_load_redirect) ++slot->load_redirect_count;
    if (outcome.was_cost_redirect) ++slot->cost_redirect_count;
    if (outcome.was_fast_lane)     ++slot->fast_lane_count;

    switch (outcome.kind) {
        case TelemetryOutcome::Kind::SUCCESS:          ++slot->success_count; break;
        case TelemetryOutcome::Kind::FAILURE:          ++slot->failure_count; break;
        case TelemetryOutcome::Kind::TIMEOUT:          ++slot->timeout_count; break;
        case TelemetryOutcome::Kind::CONNECTION_ERROR: ++slot->connection_error_count; break;
    }

    // Histograms — record only when we have a sensible signal. ttft can be
    // zero when the request failed before any backend byte was seen; emitting
    // 0 would skew the TTFT distribution.
    if (outcome.ttft_seconds > 0.0) {
        slot->ttft_seconds.record(outcome.ttft_seconds);
    }
    if (outcome.total_latency_seconds > 0.0) {
        slot->total_latency_seconds.record(outcome.total_latency_seconds);
    }
    // prefix_reuse_depth records the matched depth (0 is meaningful — it's
    // the "no prefix hit" bucket — so always record).
    slot->prefix_reuse_depth.record(static_cast<double>(outcome.matched_prefix_depth));
}

AggregateRecord* TelemetryService::get_or_overflow_slot(const TelemetryBucketKey& key) {
    auto it = _buckets.find(key);
    if (it != _buckets.end()) {
        return &it->second;
    }
    // New key. Either insert (under cap) or attribute to the overflow
    // sentinel and bump _buckets_overflowed.
    if (_buckets.size() < _max_buckets) {
        auto [inserted, _] = _buckets.emplace(key, AggregateRecord{});
        return &inserted->second;
    }
    ++_buckets_overflowed;
    auto sentinel_it = _buckets.find(overflow_key());
    if (sentinel_it == _buckets.end()) {
        // Should have been pre-inserted by start_shard; defensive re-create.
        auto [inserted, _] = _buckets.emplace(overflow_key(), AggregateRecord{});
        return &inserted->second;
    }
    return &sentinel_it->second;
}

// =============================================================================
// Window snapshot
// =============================================================================

ShardSnapshot TelemetryService::snapshot_and_reset() {
    ShardSnapshot snap;
    snap.records.reserve(_buckets.size());

    for (auto& [key, rec] : _buckets) {
        // Skip empty records (typical for the pre-inserted overflow sentinel
        // when no overflow occurred). Reduces report noise.
        if (rec.request_count == 0
            && rec.cache_hit_count == 0
            && rec.cache_miss_count == 0) {
            continue;
        }
        rec.key = key;
        snap.records.push_back(std::move(rec));
    }

    // Reset per-window state. Keep the pre-inserted overflow sentinel so the
    // next window starts ready.
    _buckets.clear();
    ensure_overflow_sentinel();

    snap.buckets_overflowed = _buckets_overflowed;
    _buckets_overflowed = 0;

    // Eviction delta: current cumulative minus last-seen. The getter is null
    // when telemetry was started without an eviction source; that's fine —
    // the field stays 0.
    if (_eviction_counter_getter) {
        uint64_t current = _eviction_counter_getter();
        // Defence against the counter going backwards (shouldn't happen for
        // a monotonic counter, but be safe): treat as zero delta.
        snap.eviction_delta = (current >= _eviction_last_seen)
            ? (current - _eviction_last_seen) : 0;
        _eviction_last_seen = current;
    }

    return snap;
}

// =============================================================================
// Emitter timer callback + async chain
// =============================================================================

void TelemetryService::on_emit_timer() {
    // Hard Rule #5: try to acquire gate holder. If gate closed (shutdown in
    // progress), return without rearming or accessing further state.
    seastar::gate::holder holder;
    try {
        holder = _emit_gate.hold();
    } catch (const seastar::gate_closed_exception&) {
        return;
    }

    // Re-arm immediately so window cadence is preserved even when the prior
    // consume future is still in flight (we'll drop that window — see below).
    _emit_timer.arm(_window);

    // Drop-on-backpressure: if the prior consume() future has not resolved,
    // increment the dropped counter and return without starting a new emit.
    // The routing path is NEVER awaited on the sink, so this is the sole
    // backpressure throttle.
    if (_consume_in_flight) {
        ++_reports_dropped_backpressure;
        return;
    }

    _consume_in_flight = true;

    // Fire-and-forget emit. The gate holder is moved into the .finally()
    // chain so that stop() (which closes the gate) waits for the entire
    // async chain — including the sink's consume() future — to resolve.
    // Hard Rule #18: a discarded future must be gate-guarded and have its
    // own handle_exception.
    (void)emit_async()
        .handle_exception([](std::exception_ptr ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                log_telemetry().warn("emit_async failed: {}", e.what());
            } catch (...) {
                log_telemetry().warn("emit_async failed with unknown exception");
            }
        })
        .finally([this, holder = std::move(holder)] {
            _consume_in_flight = false;
        });
}

seastar::future<> TelemetryService::emit_async() {
    if (!_container || !_sink) {
        co_return;
    }

    // ------------------------------------------------------------------------
    // Stage 1: gather per-shard snapshots via foreign_ptr (Hard Rule #14).
    //
    // Each per-shard lambda allocates a ShardSnapshot on its OWN shard, wraps
    // in foreign_ptr. The returned future lands on shard 0 carrying the
    // foreign_ptr; reading via fp-> is a safe cross-shard read. We copy the
    // contents into shard-0-local containers in stage 2; the foreign_ptr's
    // destructor at scope end returns the unique_ptr to its home shard for
    // cleanup.
    // ------------------------------------------------------------------------
    using ForeignSnap = seastar::foreign_ptr<std::unique_ptr<ShardSnapshot>>;
    std::vector<ForeignSnap> per_shard(seastar::smp::count);

    co_await seastar::parallel_for_each(
        boost::irange(0u, seastar::smp::count),
        [this, &per_shard](unsigned shard_id) {
            return _container->invoke_on(shard_id, [](TelemetryService& s) {
                return seastar::make_foreign(
                    std::make_unique<ShardSnapshot>(s.snapshot_and_reset()));
            }).then([&per_shard, shard_id](ForeignSnap fp) {
                per_shard[shard_id] = std::move(fp);
            });
        });

    // ------------------------------------------------------------------------
    // Stage 2: merge into shard-0-local report.
    //
    // The merged map is allocated on shard 0; we copy each foreign record in
    // via operator[]/emplace, which allocates fresh shard-0 storage. The
    // foreign source records stay on their home shards until per_shard goes
    // out of scope (their foreign_ptr destructors handle cross-shard return).
    // ------------------------------------------------------------------------
    absl::flat_hash_map<TelemetryBucketKey, AggregateRecord, TelemetryBucketKeyHash> merged;
    uint64_t window_eviction      = 0;
    uint64_t window_overflowed    = 0;
    uint32_t contributing_shards  = 0;
    size_t   merged_steps         = 0;

    for (auto& fp : per_shard) {
        if (!fp) continue;
        ++contributing_shards;
        window_eviction   += fp->eviction_delta;
        window_overflowed += fp->buckets_overflowed;
        for (const auto& rec : fp->records) {
            auto it = merged.find(rec.key);
            if (it == merged.end()) {
                // Copy into a shard-0-local AggregateRecord.
                merged.emplace(rec.key, rec);
            } else {
                it->second.merge_from(rec);
            }
            // Rule #17: yield periodically when many buckets are present so
            // a large merge doesn't stall the reactor on shard 0.
            if (++merged_steps % kYieldInterval == 0) {
                co_await seastar::coroutine::maybe_yield();
            }
        }
    }

    // ------------------------------------------------------------------------
    // Stage 3: build the report and hand to the sink.
    // ------------------------------------------------------------------------
    WindowReport report;
    report.format_version              = kTelemetryReportFormatVersion;
    report.window_start_ms             = _window_start_ms;
    report.window_end_ms               = now_ms();
    report.shard_count                 = contributing_shards;
    report.window_eviction_churn       = window_eviction;
    report.buckets_overflowed          = window_overflowed;
    report.reports_dropped_backpressure = _reports_dropped_backpressure;
    if (_strategy_snapshot) {
        report.strategy = _strategy_snapshot();
    }

    report.records.reserve(merged.size());
    for (auto& [k, v] : merged) {
        v.key = k;
        report.records.push_back(std::move(v));
    }

    _window_start_ms = report.window_end_ms;

    // Hand to sink. consume() must not block per the sink contract; the
    // drop-on-backpressure guard upstream protects against a slow sink.
    co_await _sink->consume(report);
    ++_reports_emitted;
}

// =============================================================================
// Lifecycle
// =============================================================================

seastar::future<> TelemetryService::stop() {
    // Hard Rule #6: deregister metrics first so Prometheus scrape lambdas
    // don't fire mid-shutdown. Only shard 0 holds the emitter metrics group.
    _emitter_metrics.clear();

    if (seastar::this_shard_id() == 0 && _emitter_started) {
        _emit_timer.cancel();
        // Wait for any in-flight emit_async chain to resolve. The chain's
        // gate holder was moved into .finally(), so close() blocks on it.
        return _emit_gate.close().then([this] {
            _sink.reset();
            _strategy_snapshot = nullptr;
            _container = nullptr;
            _emitter_started = false;
            _buckets.clear();
        });
    }

    _buckets.clear();
    return seastar::make_ready_future<>();
}

}  // namespace ranvier
