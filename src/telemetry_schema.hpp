// Ranvier Core - Telemetry Sink Schema
//
// Data structures for the pluggable telemetry sink: bucket key, aggregate
// per-bucket record, and the periodic window report handed to the sink.
//
// SCOPE: aggregate routing/cache outcomes only. NO prompt or response content,
// NO token IDs, NO per-request identifiers. Every field here is a structural
// statistic — counters, histograms, and a snapshot of the routing-strategy
// parameters in effect for the window. The bucket dimensions are operator-
// labelled (model_family, hardware_tier) and request-derived (workload), all
// content-free.
//
// =============================================================================
// FORWARD-COMPATIBILITY CONTRACT
// =============================================================================
// `WindowReport` and `AggregateRecord` are designed for the same forward-
// compatibility discipline used by `CacheStatePacket` (see gossip_protocol.hpp
// §"FORWARD COMPATIBILITY"). A sink built against format version N MUST
// remain correct when fed a report at format version N+M:
//
//   1. `WindowReport::format_version` is recorded but is NOT a rejection
//      boundary — consumers read fields they know and ignore the rest.
//
//   2. New fields are appended only. Never re-purpose an existing field, never
//      shrink a field's semantic range, never renumber HardwareTier ordinals
//      (see types.hpp).
//
//   3. `AggregateRecord` adds optional new histograms / counters at the end.
//      A consumer reading an older record sees zero-valued defaults for new
//      fields, which is the correct semantic for "this window did not report".
//
//   4. Bump `kFormatVersion` only when readers ALREADY DEPLOYED in the wild
//      may misinterpret the report — i.e. essentially never under the above
//      append-only rule. It exists as an escape hatch for an eventual
//      breaking change, not as a "what's new this release" knob.
//
// This header is the contract; downstream sink implementations are bound by it.

#pragma once

#include "intent_classifier.hpp"   // RequestIntent
#include "metrics_helpers.hpp"     // MetricHistogram, latency bucket helpers
#include "types.hpp"               // BackendId, HardwareTier

#include <absl/container/flat_hash_map.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ranvier {

// =============================================================================
// Format version
// =============================================================================

// Current wire format version of WindowReport. See the FORWARD-COMPATIBILITY
// CONTRACT comment at the top of this file before bumping.
inline constexpr uint16_t kTelemetryReportFormatVersion = 1;

// =============================================================================
// WorkloadPattern (stable wire-mirror of RequestIntent)
// =============================================================================
//
// Mirrors RequestIntent (intent_classifier.hpp) with PINNED ORDINALS. Kept
// distinct from RequestIntent because RequestIntent's ordinals are a
// shard-internal concern (used by hot-path counters and the intent metric
// labels) and the telemetry wire format must not break if someone reshuffles
// the source-of-truth enum. Convert at the recording boundary via
// workload_pattern_from_intent().
enum class WorkloadPattern : uint8_t {
    UNKNOWN      = 0,   // Reserved sentinel — used only if intent classification was disabled.
    AUTOCOMPLETE = 1,
    CHAT         = 2,
    EDIT         = 3,
    // Append only.
};

inline std::string_view workload_pattern_to_string(WorkloadPattern w) {
    switch (w) {
        case WorkloadPattern::UNKNOWN:      return "unknown";
        case WorkloadPattern::AUTOCOMPLETE: return "autocomplete";
        case WorkloadPattern::CHAT:         return "chat";
        case WorkloadPattern::EDIT:         return "edit";
    }
    return "unknown";
}

inline WorkloadPattern workload_pattern_from_intent(RequestIntent intent) {
    switch (intent) {
        case RequestIntent::AUTOCOMPLETE: return WorkloadPattern::AUTOCOMPLETE;
        case RequestIntent::CHAT:         return WorkloadPattern::CHAT;
        case RequestIntent::EDIT:         return WorkloadPattern::EDIT;
    }
    return WorkloadPattern::UNKNOWN;
}

// =============================================================================
// TelemetryBucketKey
// =============================================================================
//
// All three dimensions are content-free:
//   - model_family: operator-set label attached to a backend at registration
//     (NOT parsed from the client `model` field — that's untrusted input).
//     Empty string is normalised to "unspecified".
//   - hardware_tier: closed enum, operator-set per backend.
//   - workload: derived from RequestIntent at the request site.
//
// Cardinality is operator-bounded by construction (both labels come from
// operator config), and the per-shard bucket map enforces a hard cap with an
// _overflow sentinel (see TelemetryService).
struct TelemetryBucketKey {
    std::string    model_family;     // e.g. "llama3", "qwen2", or "unspecified"
    HardwareTier   hardware_tier = HardwareTier::UNSPECIFIED;
    WorkloadPattern workload     = WorkloadPattern::UNKNOWN;

    bool operator==(const TelemetryBucketKey& other) const {
        return hardware_tier == other.hardware_tier
            && workload      == other.workload
            && model_family  == other.model_family;
    }
};

// Hash for absl::flat_hash_map. Combines the three dimensions with a standard
// boost-style mixer. No correctness dependency on the specific mix — purely
// distributional.
struct TelemetryBucketKeyHash {
    size_t operator()(const TelemetryBucketKey& k) const noexcept {
        size_t h = std::hash<std::string>{}(k.model_family);
        h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(k.hardware_tier))
             + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(k.workload))
             + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

// =============================================================================
// Histogram bucket helper for prefix-reuse depth
// =============================================================================
//
// Token-depth buckets for the ART-matched prefix length. Bucket boundaries
// chosen to cover the realistic prefix_token_length range (4–8192) with
// log-ish spacing; matches the typical multi-turn / system-message depth
// distribution observed by operators.
inline std::vector<double> prefix_reuse_depth_buckets() {
    return {0, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
}

// =============================================================================
// AggregateRecord
// =============================================================================
//
// Per-bucket aggregate over one window. ALL fields are delta-since-last-window
// (the per-shard counters are snapshot-and-reset on each emit). Cumulative
// counters live in Prometheus; this is the per-window export view.
//
// FORWARD-COMPAT: append new fields ONLY. New consumers reading old records
// see zero-valued defaults, which is the correct semantic.
struct AggregateRecord {
    // ---- Identity (mirrors TelemetryBucketKey for one-shot serialisation) ----
    TelemetryBucketKey key;

    // ---- Counters ----
    uint64_t request_count    = 0;   // Total terminal-outcome requests in this bucket
    uint64_t cache_hit_count  = 0;   // ART prefix hits (cache_hit on RouteResult)
    uint64_t cache_miss_count = 0;   // ART misses / hash fallback
    // Outcome distributions (these are NOT mutually exclusive — a request can be
    // both a load_redirect AND a cost_redirect; each flag is its own dimension).
    uint64_t load_redirect_count = 0;   // was_load_redirect
    uint64_t cost_redirect_count = 0;   // was_cost_redirect
    uint64_t fast_lane_count     = 0;   // was_fast_lane
    // Terminal-status counts (sum to request_count modulo client_disconnect,
    // which is bucketed as failure per the existing api_key attribution).
    uint64_t success_count          = 0;
    uint64_t failure_count          = 0;
    uint64_t timeout_count          = 0;
    uint64_t connection_error_count = 0;

    // ---- Histograms ----
    MetricHistogram ttft_seconds;          // Time-to-first-byte
    MetricHistogram total_latency_seconds; // End-to-end request latency
    MetricHistogram prefix_reuse_depth;    // Matched ART prefix token-depth (0 on miss)

    AggregateRecord()
        : ttft_seconds(routing_latency_buckets())
        , total_latency_seconds(total_request_latency_buckets())
        , prefix_reuse_depth(prefix_reuse_depth_buckets()) {}

    // Merge another shard's record into this one. INVARIANT: both records
    // share the same key (the caller keys on TelemetryBucketKey first).
    void merge_from(const AggregateRecord& other) {
        request_count          += other.request_count;
        cache_hit_count        += other.cache_hit_count;
        cache_miss_count       += other.cache_miss_count;
        load_redirect_count    += other.load_redirect_count;
        cost_redirect_count    += other.cost_redirect_count;
        fast_lane_count        += other.fast_lane_count;
        success_count          += other.success_count;
        failure_count          += other.failure_count;
        timeout_count          += other.timeout_count;
        connection_error_count += other.connection_error_count;
        merge_histogram(ttft_seconds.data, other.ttft_seconds.data);
        merge_histogram(total_latency_seconds.data, other.total_latency_seconds.data);
        merge_histogram(prefix_reuse_depth.data, other.prefix_reuse_depth.data);
    }

private:
    // Element-wise sum of two histograms with identical bucket boundaries.
    // Bucket boundaries are configured at MetricHistogram construction (always
    // the same helpers for a given field), so this is safe by invariant.
    static void merge_histogram(seastar::metrics::histogram& dst,
                                const seastar::metrics::histogram& src) {
        dst.sample_count += src.sample_count;
        dst.sample_sum   += src.sample_sum;
        const size_t n = std::min(dst.buckets.size(), src.buckets.size());
        for (size_t i = 0; i < n; ++i) {
            dst.buckets[i].count += src.buckets[i].count;
        }
    }
};

// =============================================================================
// RoutingStrategyParams (snapshot of policy in effect for the window)
// =============================================================================
//
// Operators correlating an outcome shift with a config change need to know
// which strategy parameters were live during the window. Snapshotted once per
// window at the emitter, not per bucket.
struct RoutingStrategyParams {
    // RoutingConfig::RoutingMode as a stable string label.
    std::string routing_mode;          // "prefix" | "hash" | "random"
    // RoutingConfig::HashStrategy as a stable string label.
    std::string hash_strategy;         // "jump" | "bounded_load" | "p2c" | "modular"
    double      bounded_load_epsilon  = 0.0;
    uint64_t    p2c_load_bias         = 0;
    bool        load_aware_routing    = false;
    double      load_imbalance_factor = 0.0;
    uint64_t    load_imbalance_floor  = 0;
    bool        cost_routing_enabled  = false;
    double      cache_residency_threshold = 0.0;
};

// =============================================================================
// WindowReport
// =============================================================================
//
// Periodic export payload. One report per window per emitter invocation.
// See the FORWARD-COMPATIBILITY CONTRACT at the top of this file.
struct WindowReport {
    // Forward-compat: recorded, NOT a rejection boundary. See header notes.
    uint16_t format_version = kTelemetryReportFormatVersion;

    // Window bounds (milliseconds since epoch). end_ms - start_ms is the
    // nominal window length; jitter is normal and acceptable.
    int64_t window_start_ms = 0;
    int64_t window_end_ms   = 0;

    // Number of shards that contributed to this report. Operator-visible —
    // a sudden drop suggests a shard fell over (or the cross-shard map_reduce
    // hit a partial failure that was logged but recovered from).
    uint32_t shard_count = 0;

    // Per-bucket aggregates. Order is unspecified.
    std::vector<AggregateRecord> records;

    // Window-level aggregates (NOT bucketed — they are shard-process state).
    uint64_t window_eviction_churn      = 0;   // Sum of ART-eviction deltas across shards
    uint64_t buckets_overflowed         = 0;   // Times a record was attributed to the _overflow sentinel
    uint64_t reports_dropped_backpressure = 0; // Window reports dropped because the prior consume()
                                                // was still in flight (this counter is a running total
                                                // since the emitter started; not a per-window delta)

    // Routing-strategy params in effect for this window. See above.
    RoutingStrategyParams strategy;
};

// =============================================================================
// Outcome record passed from the recording site to TelemetryService
// =============================================================================
//
// Captures everything the http_controller knows at request completion. No
// content, no identifiers — only structural signals derived from RouteResult
// and the measured latencies. Kept as a flat POD so the hot-path call is a
// memcpy + map insert.
struct TelemetryOutcome {
    bool   cache_hit          = false;
    bool   was_load_redirect  = false;
    bool   was_cost_redirect  = false;
    bool   was_fast_lane      = false;
    // Matched ART prefix depth in tokens (0 when no prefix hit). Sourced from
    // RouteResult::matched_prefix_depth — the same depth the ART lookup
    // already computes for the prefix-skip path-compression metric.
    uint32_t matched_prefix_depth = 0;
    double ttft_seconds        = 0.0;  // 0.0 if no first-byte signal was recorded
    double total_latency_seconds = 0.0;

    // Terminal outcome. Exactly one of these must be true.
    enum class Kind : uint8_t {
        SUCCESS = 0,
        FAILURE = 1,
        TIMEOUT = 2,
        CONNECTION_ERROR = 3,
    } kind = Kind::SUCCESS;
};

}  // namespace ranvier
