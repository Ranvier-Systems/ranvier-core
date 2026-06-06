// Ranvier Core - Telemetry Service
//
// `seastar::sharded<TelemetryService>` owning the per-shard bounded bucket
// map that backs the periodic window report, plus (on shard 0 only) the
// emitter timer that drives `gather → build WindowReport → hand to sink`.
//
// Off by default. When enabled, the recording entry is a bounded shard-local
// hash lookup + integer/histogram bumps — negligible vs. the routing
// decision it sits next to. When disabled, it is a single predictable
// branch.

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
    // sentinel. `eviction_counter_getter` returns the CUMULATIVE per-shard
    // ART eviction count; the service tracks the last-seen value to derive
    // a per-window delta. Pass a null function to disable eviction-churn
    // reporting (the field stays 0).
    seastar::future<> start_shard(TelemetrySinkConfig config,
                                  std::function<uint64_t()> eviction_counter_getter);

    // -------------------------------------------------------------------------
    // Shard-0 emitter (called only on shard 0)
    // -------------------------------------------------------------------------

    // Install the sink + container ref + strategy-snapshot source and arm
    // the window timer. Must be called on shard 0; idempotent.
    seastar::future<> start_emitter(
        seastar::sharded<TelemetryService>* container,
        std::unique_ptr<TelemetrySink> sink,
        std::function<RoutingStrategyParams()> strategy_snapshot);

    // -------------------------------------------------------------------------
    // Recording entry (hot path; shard-local)
    // -------------------------------------------------------------------------

    // Shard-local. Touches only this shard's state, so safe from any shard's
    // reactor. When `_enabled=false` this returns immediately.
    void record_outcome(const TelemetryBucketKey& key, const TelemetryOutcome& outcome);

    // -------------------------------------------------------------------------
    // Window snapshot (called via invoke_on per shard from shard 0)
    // -------------------------------------------------------------------------

    // Snapshot this shard's per-window state and RESET the local counters.
    // Returned by value; the caller wraps in `foreign_ptr` for cross-shard
    // transfer.
    ShardSnapshot snapshot_and_reset();

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    seastar::future<> stop();

    // -------------------------------------------------------------------------
    // Test / observability accessors
    // -------------------------------------------------------------------------

    bool   is_enabled_for_testing() const { return _enabled; }
    size_t bucket_count_for_testing() const { return _buckets.size(); }
    uint64_t reports_dropped_for_testing() const { return _reports_dropped_backpressure; }

    // Sentinel that the overflow path attributes to.
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

    AggregateRecord* get_or_overflow_slot(const TelemetryBucketKey& key);
    void ensure_overflow_sentinel();
    void on_emit_timer();
    seastar::future<> emit_async();
};

}  // namespace ranvier
