// Ranvier Core - Telemetry Service
//
// `seastar::sharded<TelemetryService>` owning the per-shard bounded bucket
// map that backs the periodic window report, plus (on shard 0 only) the
// emitter timer that drives `map_reduce → build WindowReport → hand to sink`.
//
// Off by default; the disabled path is a single predictable branch at the
// top of `record_outcome` and zero allocation. When enabled, the recording
// site does a bounded shard-local hash lookup + integer/histogram bumps —
// negligible vs. the routing decision it sits next to.
//
// =============================================================================
// SEASTAR DISCIPLINE (binding)
// =============================================================================
//
//   - Hot path is shard-local only (Rule #1, #0): plain integers, no atomics.
//   - Bucket map is bounded by `max_buckets`; overflow attributes to the
//     `_overflow` sentinel record + bumps a counter (Rule #4).
//   - Emitter timer is gate-guarded (Rule #5); the holder is moved into the
//     async chain so it lives for the full consume() future.
//   - Cross-shard snapshot uses `foreign_ptr` + shard-0-local reallocation in
//     the merge loop (Rule #14, BUG #1/#2).
//   - Sink `consume()` is never awaited on the request path. If the prior
//     consume future has not resolved by the next tick, the new window is
//     dropped and a counter incremented — no queue, no backpressure into
//     routing decisions.
//   - Metrics are deregistered first in stop() (Rule #6).
//   - Merge loop yields every kYieldInterval buckets (Rule #17).

#pragma once

#include "config_infra.hpp"   // TelemetrySinkConfig
#include "telemetry_schema.hpp"
#include "telemetry_sink.hpp"
#include "types.hpp"

#include <absl/container/flat_hash_map.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/timer.hh>

namespace ranvier {

// Per-shard snapshot returned by snapshot_and_reset(). Crossed back to
// shard 0 via `seastar::foreign_ptr<unique_ptr<ShardSnapshot>>` (Rule #14):
// the inner heap allocations stay on the producing shard until shard 0 has
// copied them into shard-0-local containers, then the foreign_ptr destructor
// returns the unique_ptr to the home shard for cleanup.
struct ShardSnapshot {
    std::vector<AggregateRecord> records;
    uint64_t                     eviction_delta  = 0;
    uint64_t                     buckets_overflowed = 0;
};

class TelemetryService {
public:
    TelemetryService() = default;

    // -------------------------------------------------------------------------
    // Per-shard configuration
    // -------------------------------------------------------------------------

    // Apply per-shard configuration. Idempotent. Pre-inserts the `_overflow`
    // sentinel so it does not have to be created lazily during a recording
    // burst that hit the cardinality cap.
    //
    // `eviction_counter_getter` is a shard-local accessor that returns the
    // CUMULATIVE per-shard ART eviction count (typically wired to
    // `RouterService::get_local_routes_evicted()`). The service tracks the
    // last-seen value to compute the per-window delta. Pass a null function
    // to disable eviction-churn reporting (the field will always be 0).
    seastar::future<> start_shard(TelemetrySinkConfig config,
                                  std::function<uint64_t()> eviction_counter_getter);

    // -------------------------------------------------------------------------
    // Shard-0 emitter (called only on shard 0)
    // -------------------------------------------------------------------------

    // Install the sink + container ref + strategy-snapshot source and arm the
    // window timer. MUST be called only on shard 0. Idempotent: a second call
    // replaces the sink and resets the timer. Takes the sink by value (sink
    // is move-only, but std::function/sharded& copy through invoke_on cleanly).
    seastar::future<> start_emitter(
        seastar::sharded<TelemetryService>* container,
        std::unique_ptr<TelemetrySink> sink,
        std::function<RoutingStrategyParams()> strategy_snapshot);

    // -------------------------------------------------------------------------
    // Recording entry (hot path; shard-local)
    // -------------------------------------------------------------------------

    // First line is `if (!_enabled) return;` so the disabled cost is a single
    // predictable branch. When enabled: bounded shard-local hash lookup +
    // integer/histogram bumps. Safe to call from any shard's reactor — touches
    // only this shard's state.
    void record_outcome(const TelemetryBucketKey& key, const TelemetryOutcome& outcome);

    // -------------------------------------------------------------------------
    // Window snapshot (called via invoke_on per shard from shard 0)
    // -------------------------------------------------------------------------

    // Take a snapshot of this shard's per-window state and RESET the local
    // counters. Returns by value; the caller wraps in `foreign_ptr` for safe
    // cross-shard transfer (see emit_async() in telemetry_service.cpp).
    ShardSnapshot snapshot_and_reset();

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    // Sharded lifecycle. On shard 0: closes the emit gate (waits for any
    // in-flight consume), cancels the timer, then deregisters metrics. On
    // every shard: clears the bucket map.
    seastar::future<> stop();

    // -------------------------------------------------------------------------
    // Test / observability accessors
    // -------------------------------------------------------------------------

    bool   is_enabled_for_testing() const { return _enabled; }
    size_t bucket_count_for_testing() const { return _buckets.size(); }
    uint64_t reports_dropped_for_testing() const { return _reports_dropped_backpressure; }

    // Static sentinel that the overflow path attributes to. Exposed so the
    // recording site / tests can match on it without re-typing the literal.
    static TelemetryBucketKey overflow_key() {
        return TelemetryBucketKey{"_overflow", HardwareTier::UNSPECIFIED, WorkloadPattern::UNKNOWN};
    }

private:
    // ---- Per-shard state ----
    bool        _enabled       = false;
    size_t      _max_buckets   = 0;
    std::function<uint64_t()> _eviction_counter_getter;
    uint64_t    _eviction_last_seen = 0;

    absl::flat_hash_map<TelemetryBucketKey, AggregateRecord, TelemetryBucketKeyHash> _buckets;
    uint64_t    _buckets_overflowed = 0;  // overflow attributions in the current window

    // ---- Shard-0 emitter state (all unused on other shards) ----
    seastar::sharded<TelemetryService>* _container = nullptr;
    std::unique_ptr<TelemetrySink>      _sink;
    std::function<RoutingStrategyParams()> _strategy_snapshot;
    seastar::timer<>                    _emit_timer;
    seastar::gate                       _emit_gate;
    std::chrono::seconds                _window{0};
    bool                                _consume_in_flight = false;
    uint64_t                            _reports_dropped_backpressure = 0;
    uint64_t                            _reports_emitted = 0;
    int64_t                             _window_start_ms = 0;
    seastar::metrics::metric_groups     _emitter_metrics;
    bool                                _emitter_started = false;

    // ---- Internal helpers ----

    // Look up the slot for a key. If absent: insert it (capped at _max_buckets)
    // OR attribute to the `_overflow` sentinel and bump _buckets_overflowed.
    AggregateRecord* get_or_overflow_slot(const TelemetryBucketKey& key);

    // Pre-insert the overflow sentinel (called from start_shard).
    void ensure_overflow_sentinel();

    // Timer callback — gate-guarded; fires emit_async() fire-and-forget with
    // the gate holder moved into the chain (Rule #5).
    void on_emit_timer();

    // The actual map_reduce → build → consume sequence. Returns when the
    // sink's consume() future has resolved (success or handled exception).
    seastar::future<> emit_async();
};

}  // namespace ranvier
