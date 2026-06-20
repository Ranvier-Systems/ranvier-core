// Ranvier Core - Telemetry Service Implementation
//
// See telemetry_service.hpp for the contract and Hard-Rules summary.

#include "telemetry_service.hpp"

#include "stream_summary.hpp"  // StreamSummary::kDefaultCapacity (report top-K cap)
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
    std::function<uint64_t()> eviction_counter_getter,
    std::function<HotPrefixWindow()> hot_prefix_getter) {
    _enabled                 = config.enabled;
    _max_buckets             = config.max_buckets;
    _window                  = config.window;
    _eviction_counter_getter = std::move(eviction_counter_getter);
    _hot_prefix_getter       = std::move(hot_prefix_getter);

    // Baseline so the first window's eviction delta is from start-time,
    // not from process start.
    _eviction_last_seen = _eviction_counter_getter ? _eviction_counter_getter() : 0;

    // Sentinel occupies one of _max_buckets slots, so effective user-visible
    // cardinality is _max_buckets - 1 (negligible at the configured cap).
    ensure_overflow_sentinel();

    return seastar::make_ready_future<>();
}

void TelemetryService::ensure_overflow_sentinel() {
    auto key = overflow_key();
    if (_buckets.find(key) == _buckets.end()) {
        _buckets.emplace(std::move(key), AggregateRecord{});
    }
}

// =============================================================================
// Shard-0 emitter
// =============================================================================

seastar::future<> TelemetryService::start_emitter(
    seastar::sharded<TelemetryService>* container,
    std::unique_ptr<TelemetrySink> sink,
    std::function<RoutingStrategyParams()> strategy_snapshot,
    BackendId self_backend_id,
    std::function<seastar::future<>(std::vector<uint64_t>)> hot_prefix_digest_broadcaster,
    std::function<bool()> quorum_degraded_getter,
    std::function<std::vector<uint64_t>(const std::vector<uint64_t>&)> residency_verified_getter,
    bool cache_topology_enabled) {

    if (seastar::this_shard_id() != 0) {
        return seastar::make_exception_future<>(
            std::runtime_error("TelemetryService::start_emitter must run on shard 0"));
    }

    // Idempotent: clear prior timer + metrics group so a second call doesn't
    // double-register the Prometheus counters.
    if (_emitter_started) {
        _emit_timer.cancel();
    }
    _emitter_metrics.clear();

    _container          = container;
    _sink               = std::move(sink);
    _strategy_snapshot  = std::move(strategy_snapshot);
    _self_backend_id    = self_backend_id;                                  // BACKLOG §21 P2
    _hot_prefix_digest_broadcaster = std::move(hot_prefix_digest_broadcaster);
    _quorum_degraded_getter = std::move(quorum_degraded_getter);            // BACKLOG §21 P2
    _residency_verified_getter = std::move(residency_verified_getter);      // BACKLOG §21 Phase 5a
    _cache_topology_enabled = cache_topology_enabled;                       // BACKLOG §21 P2 dark-launch gate
    _window_start_ms    = now_ms();

    if (!_enabled) {
        log_telemetry().info("telemetry sink disabled; emitter not armed");
        _emitter_started = true;
        return seastar::make_ready_future<>();
    }

    // _window was set by start_shard from the same config; validation
    // rejects window <= 0 when enabled.
    _emit_timer.set_callback([this] { on_emit_timer(); });
    _emit_timer.arm(_window);

    // Rule #6: deregistered first in stop().
    namespace sm = seastar::metrics;
    _emitter_metrics.add_group("ranvier_telemetry_sink", {
        sm::make_counter("reports_emitted_total", _reports_emitted,
            sm::description("Periodic window reports successfully handed to the telemetry sink")),
        sm::make_counter("reports_dropped_backpressure_total", _reports_dropped_backpressure,
            sm::description("Window reports dropped because the prior consume() future had not "
                            "resolved by the next window tick (sink slow / down)")),

        // Hot-prefix concentration (BACKLOG §21 P1). Label-free, shard-0 only —
        // read from the cached merged top-K. A cache-aware autoscaler uses these
        // to size pre-warm / pin headroom for the hot set.
        sm::make_gauge("hot_prefix_top1_request_share",
            sm::description("Fraction of prefix-routed requests hitting the single hottest prefix (0..1)."),
            [this] { return hot_prefix_top1_share(_last_hot_prefixes, _last_hot_prefix_touches); }),
        sm::make_gauge("hot_prefix_topk_request_share",
            sm::description("Fraction of prefix-routed requests hitting the tracked top-K prefixes (0..1)."),
            [this] { return hot_prefix_topk_share(_last_hot_prefixes, _last_hot_prefix_touches); }),
        sm::make_gauge("hot_prefix_distinct_estimate",
            sm::description("Distinct hot prefixes tracked in the last window (<= K)."),
            [this] { return static_cast<double>(_last_hot_prefixes.size()); }),

        // Sole-holder signals (BACKLOG §21 P2 slice 3). Among this node's hot
        // top-K, how many prefixes does exactly one cluster node hold — i.e.
        // reaping this node is a cache cliff for them. Holder counts are queried
        // live (reflect peer digests since the last window), not cached at the
        // window boundary. An autoscaler scrapes every node and unions the set.
        sm::make_gauge("ranvier_sole_held_hot_prefixes",
            sm::description("Count of this node's hot top-K prefixes held by exactly one cluster node "
                            "(sole-held => unsafe to reap that node). Frozen to all hot prefixes while "
                            "quorum is DEGRADED (split-brain freeze)."),
            [this] {
                return static_cast<double>(
                    hot_prefix_sole_held_count(current_holder_counts(), quorum_degraded()));
            }),
        sm::make_gauge("hot_prefix_sole_held_request_share",
            sm::description("Fraction of prefix-routed requests hitting sole-held hot prefixes (0..1). "
                            "Frozen to the full top-K share while quorum is DEGRADED."),
            [this] {
                return hot_prefix_sole_held_request_share(
                    _last_hot_prefixes, current_holder_counts(), _last_hot_prefix_touches,
                    quorum_degraded());
            }),
        sm::make_gauge("topk_snapshot_age_seconds",
            sm::description("Age of the cached hot-prefix snapshot in seconds (0 before the first window)."),
            [this] {
                if (_last_hot_prefix_at.time_since_epoch().count() == 0) return 0.0;
                return std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - _last_hot_prefix_at).count();
            }),
        // BACKLOG §21 Phase 5a: of this node's hot top-K, how many its own backend
        // verifies resident in KV cache right now (native KV-event stream fresh).
        // 0 when native KV-events are disabled or trust is suspended — the local
        // precursor to the cluster verified_holders tier (5c/5d).
        sm::make_gauge("ranvier_hot_prefix_verified_resident",
            sm::description("Count of this node's hot top-K prefixes verified-resident in its own "
                            "backend's KV cache (native KV-event stream fresh). 0 when KV-events "
                            "are disabled or native trust is suspended (membership-only window)."),
            [this] { return static_cast<double>(_last_verified_resident_count); }),
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
    if (!_enabled) {
        return;
    }

    AggregateRecord& slot = *get_or_overflow_slot(key);

    ++slot.request_count;
    if (outcome.cache_hit) {
        ++slot.cache_hit_count;
    } else {
        ++slot.cache_miss_count;
    }
    if (outcome.was_load_redirect) ++slot.load_redirect_count;
    if (outcome.was_cost_redirect) ++slot.cost_redirect_count;
    if (outcome.was_fast_lane)     ++slot.fast_lane_count;

    switch (outcome.kind) {
        case TelemetryOutcome::Kind::SUCCESS:          ++slot.success_count; break;
        case TelemetryOutcome::Kind::FAILURE:          ++slot.failure_count; break;
        case TelemetryOutcome::Kind::TIMEOUT:          ++slot.timeout_count; break;
        case TelemetryOutcome::Kind::CONNECTION_ERROR: ++slot.connection_error_count; break;
    }

    // ttft is 0 when the request failed before any backend byte was seen;
    // recording it would skew the distribution.
    if (outcome.ttft_seconds > 0.0) {
        slot.ttft_seconds.record(outcome.ttft_seconds);
    }
    if (outcome.total_latency_seconds > 0.0) {
        slot.total_latency_seconds.record(outcome.total_latency_seconds);
    }
    // Depth 0 is the "no prefix hit" bucket — meaningful, always record.
    slot.prefix_reuse_depth.record(static_cast<double>(outcome.matched_prefix_depth));
}

AggregateRecord* TelemetryService::get_or_overflow_slot(const TelemetryBucketKey& key) {
    auto it = _buckets.find(key);
    if (it != _buckets.end()) {
        return &it->second;
    }
    if (_buckets.size() < _max_buckets) {
        auto [new_it, _] = _buckets.emplace(key, AggregateRecord{});
        return &new_it->second;
    }
    ++_buckets_overflowed;
    // Sentinel was pre-inserted by start_shard and re-inserted by
    // snapshot_and_reset; reactor-serial access means it is always present
    // here.
    return &_buckets.find(overflow_key())->second;
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

    if (_eviction_counter_getter) {
        uint64_t current = _eviction_counter_getter();
        snap.eviction_delta = current - _eviction_last_seen;
        _eviction_last_seen = current;
    }

    // BACKLOG §21 P1: pull + reset this shard's hot-prefix top-K. The getter
    // resets the router's summary, so window boundaries align with this snapshot.
    if (_hot_prefix_getter) {
        snap.hot_prefixes = _hot_prefix_getter();
    }

    return snap;
}

// =============================================================================
// Emitter timer callback + async chain
// =============================================================================

void TelemetryService::on_emit_timer() {
    // Rule #5: bail out cleanly if stop() has closed the gate.
    seastar::gate::holder holder;
    try {
        holder = _emit_gate.hold();
    } catch (const seastar::gate_closed_exception&) {
        return;
    }

    // Re-arm BEFORE deciding to drop, so window cadence is preserved even
    // when the prior consume() future is still in flight.
    _emit_timer.arm(_window);

    // Sole backpressure throttle: the routing path is never awaited on the
    // sink, so a slow sink drops windows here.
    if (_consume_in_flight) {
        ++_reports_dropped_backpressure;
        return;
    }

    _consume_in_flight = true;

    // Rule #18: discarded future has its own handle_exception. Rule #5:
    // holder moved into .finally() so stop()'s gate.close() blocks on the
    // entire chain including consume().
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

    // Rule #14: each shard wraps its snapshot in a foreign_ptr; reading
    // through fp-> is a safe cross-shard read, and the merge below copies
    // into shard-0-local storage. The foreign_ptrs' destructors at scope
    // end return their unique_ptrs to the home shards for free.
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

    absl::flat_hash_map<TelemetryBucketKey, AggregateRecord, TelemetryBucketKeyHash> merged;
    uint64_t window_eviction      = 0;
    uint64_t window_overflowed    = 0;
    uint32_t contributing_shards  = 0;
    size_t   merged_steps         = 0;
    std::vector<HotPrefixWindow> shard_hot;  // BACKLOG §21 P1; shard-0-local copies
    shard_hot.reserve(per_shard.size());

    for (auto& fp : per_shard) {
        if (!fp) continue;
        ++contributing_shards;
        window_eviction   += fp->eviction_delta;
        window_overflowed += fp->buckets_overflowed;
        // Rule #14: COPY (not move) out of the foreign_ptr — the entries' heap
        // is owned by the home shard. merge below produces shard-0-local storage.
        shard_hot.push_back(fp->hot_prefixes);
        for (const auto& rec : fp->records) {
            auto it = merged.find(rec.key);
            if (it == merged.end()) {
                merged.emplace(rec.key, rec);  // shard-0-local copy
            } else {
                it->second.merge_from(rec);
            }
            // Rule #17: bounded buckets but yielding stays robust if the cap
            // is ever raised.
            if (++merged_steps % kYieldInterval == 0) {
                co_await seastar::coroutine::maybe_yield();
            }
        }
    }

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

    // BACKLOG §21 P1: cluster-merge the per-shard hot-prefix top-K. Bounded to K
    // entries (= per-shard summary capacity); merge input is <= K * shards.
    auto merged_hot = merge_hot_prefixes(shard_hot, StreamSummary::kDefaultCapacity);
    report.hot_prefix_touches = merged_hot.total_touches;
    // Cache (copy) for the concentration gauges + /v1/cache/topology before the
    // move into the report. Synchronous write — no suspension point before a
    // reader can run, so the shard-0 snapshot is never observed torn (Rule #1).
    _last_hot_prefixes       = merged_hot.top;
    _last_hot_prefix_touches = merged_hot.total_touches;
    _last_hot_prefix_at      = std::chrono::steady_clock::now();
    report.hot_prefixes      = std::move(merged_hot.top);

    _window_start_ms = report.window_end_ms;

    // BACKLOG §21 P2: maintain the cluster cache-topology index for this window.
    // Self-apply our own merged top-K (so this node counts as a holder of its own
    // hot prefixes — sole-holder math is wrong otherwise), age out peers gone
    // silent (backstop to peer-death eviction), then gossip our digest to peers.
    // Skipped when P2 is disabled (dark-launch gate) or the node has no cluster
    // identity (_self_backend_id 0).
    if (_cache_topology_enabled && _self_backend_id != 0) {
        std::vector<uint64_t> digest;
        digest.reserve(_last_hot_prefixes.size());
        for (const auto& e : _last_hot_prefixes) digest.push_back(e.prefix_fp);

        // BACKLOG §21 Phase 5a: of our membership hashes, which does our own
        // backend verify resident in KV cache right now? Empty (membership-only)
        // when the getter is null or native trust is absent. The membership
        // `digest` below is self-applied and broadcast UNCHANGED (B-as-superset);
        // only the local ranvier_hot_prefix_verified_resident gauge consumes this
        // today — 5b carries the subset on the wire, 5c/5d build the cluster tier.
        _last_verified_resident_count =
            _residency_verified_getter ? _residency_verified_getter(digest).size() : 0;

        const auto now_tp = _last_hot_prefix_at;  // same steady_clock read as above
        _topology_index.apply_digest(_self_backend_id, digest, now_tp);
        _topology_index.age_out(now_tp - _window * kTopologyStaleWindows);

        if (_hot_prefix_digest_broadcaster) {
            // Rule #18: failures are logged inside the broadcaster; the digest is
            // best-effort and self-corrects next window (latest-value-wins).
            co_await _hot_prefix_digest_broadcaster(std::move(digest));
        }
    }

    // Hand to sink. consume() must not block per the sink contract; the
    // drop-on-backpressure guard upstream protects against a slow sink.
    co_await _sink->consume(report);
    ++_reports_emitted;
}

// =============================================================================
// Lifecycle
// =============================================================================

seastar::future<> TelemetryService::stop() {
    // Rule #6: dereg first.
    _emitter_metrics.clear();

    if (seastar::this_shard_id() == 0 && _emitter_started) {
        _emit_timer.cancel();
        // gate.close() blocks on the in-flight emit_async chain (holder is
        // moved into its .finally()).
        return _emit_gate.close().then([this] {
            _sink.reset();
            _strategy_snapshot = nullptr;
            _hot_prefix_digest_broadcaster = nullptr;  // BACKLOG §21 P2
            _quorum_degraded_getter = nullptr;         // BACKLOG §21 P2
            _container = nullptr;
            _emitter_started = false;
            _buckets.clear();
        });
    }

    _buckets.clear();
    return seastar::make_ready_future<>();
}

// =============================================================================
// Hot-prefix topology JSON (BACKLOG §21 P1)
// =============================================================================

// =============================================================================
// Cluster cache-topology feeds (BACKLOG §21 P2; shard 0 only)
// =============================================================================

void TelemetryService::apply_peer_digest(BackendId node, std::vector<uint64_t> prefix_hashes) {
    if (!_cache_topology_enabled) return;  // dark-launch gate: ignore peer digests
    // Receive-time timestamp drives the age_out backstop in the emit loop.
    _topology_index.apply_digest(node, prefix_hashes, std::chrono::steady_clock::now());
}

void TelemetryService::evict_peer(BackendId node) {
    if (!_cache_topology_enabled) return;
    _topology_index.evict_node(node);
}

std::vector<size_t> TelemetryService::current_holder_counts() const {
    std::vector<size_t> holders;
    holders.reserve(_last_hot_prefixes.size());
    for (const auto& e : _last_hot_prefixes) {
        holders.push_back(_topology_index.holder_count(e.prefix_fp));
    }
    return holders;
}

std::string TelemetryService::hot_prefix_topology_json() const {
    int64_t age_ms = 0;
    if (_last_hot_prefix_at.time_since_epoch().count() != 0) {
        age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - _last_hot_prefix_at).count();
    }
    // P2 disabled => omit the holders/sole_held/quorum surface (revert to P1
    // shape); the empty index would otherwise report a misleading sole_held=false.
    if (!_cache_topology_enabled) {
        return format_hot_prefix_topology_json(_last_hot_prefixes, {}, _last_hot_prefix_touches,
                                               age_ms, /*quorum_degraded=*/false,
                                               /*include_topology=*/false);
    }
    return format_hot_prefix_topology_json(_last_hot_prefixes, current_holder_counts(),
                                           _last_hot_prefix_touches, age_ms, quorum_degraded(),
                                           /*include_topology=*/true);
}

}  // namespace ranvier
