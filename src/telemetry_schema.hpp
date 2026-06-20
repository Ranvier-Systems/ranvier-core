// Ranvier Core - Telemetry Sink Schema
//
// Data structures for the pluggable telemetry sink: bucket key, aggregate
// per-bucket record, and the periodic window report handed to the sink.
//
// SCOPE: aggregate routing/cache outcomes only. NO prompt or response content,
// NO token IDs, NO per-request identifiers. Every field here is a structural
// statistic — counters, histograms, and a snapshot of the routing-strategy
// parameters in effect for the window. The bucket dimensions are operator- and
// backend-derived plus a request-derived workload — all content-free.
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
//      shrink a field's semantic range, never renumber HardwareLabel or
//      BackendType ordinals (see types.hpp).
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
#include "route_scorer.hpp"        // ScoringWeights
#include "types.hpp"               // BackendId, BackendType, HardwareLabel

#include <absl/container/flat_hash_map.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ranvier {

// =============================================================================
// Format version
// =============================================================================

// Current wire format version of WindowReport. See the FORWARD-COMPATIBILITY
// CONTRACT comment at the top of this file before bumping.
//
//   v4: added the hot-prefix top-K window aggregate (WindowReport::hot_prefixes
//       + hot_prefix_touches; BACKLOG §21 P1). Body widened at the end; a v3
//       reader simply doesn't see the new fields (append-only, format_version
//       recorded not enforced).
//   v3: added the unified route-scorer weights (RoutingStrategyParams::scoring,
//       a ScoringWeights) to the per-window strategy snapshot. The body widened;
//       a v2 reader simply doesn't see the weights — older payloads still parse
//       (format_version is recorded, not a rejection boundary). Mirrors the v2
//       bump made when the bucket key was widened.
//   v2: added `backend_type` (engine class) to the bucket key. A v1 reader
//       collapses engine classes into the other dimensions; the bump signals
//       the wider key.
//   v1: initial telemetry window-report schema.
inline constexpr uint16_t kTelemetryReportFormatVersion = 4;

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
// Every dimension is content-free:
//   - model_family: operator-set label attached to a backend at registration
//     (NOT parsed from the client `model` field — that's untrusted input).
//     Empty string is normalised to "unspecified".
//   - backend_type: engine class (vllm / sglang / …), read from the selected
//     backend — already resolved there, so it costs nothing extra on the hot
//     path. Its own dimension so the catalog can group by engine class.
//   - hardware_label: closed enum, operator-set per backend.
//   - workload: derived from RequestIntent at the request site.
//
// Cardinality is operator-bounded by construction (the backend-derived
// dimensions all come from operator config / the registered backend), and the
// per-shard bucket map enforces a hard cap with an _overflow sentinel (see
// TelemetryService).
struct TelemetryBucketKey {
    std::string     model_family;     // e.g. "llama3", "qwen2", or "unspecified"
    BackendType     backend_type   = BackendType::VLLM;   // engine class
    HardwareLabel   hardware_label = HardwareLabel::UNSPECIFIED;
    WorkloadPattern workload       = WorkloadPattern::UNKNOWN;

    bool operator==(const TelemetryBucketKey& other) const {
        return backend_type   == other.backend_type
            && hardware_label == other.hardware_label
            && workload       == other.workload
            && model_family   == other.model_family;
    }
};

// Hash for absl::flat_hash_map. Combines the key's dimensions with a standard
// boost-style mixer. No correctness dependency on the specific mix — purely
// distributional.
struct TelemetryBucketKeyHash {
    size_t operator()(const TelemetryBucketKey& k) const noexcept {
        size_t h = std::hash<std::string>{}(k.model_family);
        h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(k.backend_type))
             + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= std::hash<uint8_t>{}(static_cast<uint8_t>(k.hardware_label))
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
// Hot-prefix top-K (BACKLOG §21 P1)
// =============================================================================
//
// A content-free fingerprint of a routing prefix and how often it was routed in
// the window. `prefix_fp` is the router's prefix_hash — NOT reversible to tokens.
struct HotPrefixEntry {
    uint64_t prefix_fp     = 0;
    uint64_t request_count = 0;
};

// One shard's per-window top-K snapshot, produced by the router's Space-Saving
// summary (approximate; counts are upper bounds). `total_touches` is the full
// prefix-routed request count this shard saw — the denominator for "what share
// of traffic the hot set accounts for". Crosses shards by value inside
// ShardSnapshot.
struct HotPrefixWindow {
    std::vector<HotPrefixEntry> entries;        // size <= K, unordered
    uint64_t                    total_touches = 0;
};

// Cluster-merged result: top `k` prefixes by summed count + the cluster-wide
// total touch count.
struct MergedHotPrefixes {
    std::vector<HotPrefixEntry> top;            // size <= k, sorted by count desc
    uint64_t                    total_touches = 0;
};

// Merge per-shard hot-prefix windows into a cluster top-K. Sums request_count
// per prefix_fp across shards, returns the top `k` by count (desc). Pure /
// reactor-free so it is unit-testable without a running reactor. The merged map
// is bounded by k * shard_count (each shard contributes <= K entries).
inline MergedHotPrefixes merge_hot_prefixes(const std::vector<HotPrefixWindow>& per_shard,
                                            size_t k) {
    absl::flat_hash_map<uint64_t, uint64_t> summed;
    uint64_t total = 0;
    for (const auto& w : per_shard) {
        total += w.total_touches;
        for (const auto& e : w.entries) {
            summed[e.prefix_fp] += e.request_count;
        }
    }

    MergedHotPrefixes merged;
    merged.total_touches = total;
    merged.top.reserve(summed.size());
    for (const auto& [fp, count] : summed) {
        merged.top.push_back(HotPrefixEntry{fp, count});
    }
    std::sort(merged.top.begin(), merged.top.end(),
              [](const HotPrefixEntry& a, const HotPrefixEntry& b) {
                  return a.request_count > b.request_count;
              });
    if (merged.top.size() > k) {
        merged.top.resize(k);
    }
    return merged;
}

// Concentration of prefix-routed traffic on the hottest prefix(es), 0..1.
// total == 0 (no prefix-routed traffic this window) yields 0.0, not a divide
// by zero. Pure / reactor-free.
inline double hot_prefix_top1_share(const std::vector<HotPrefixEntry>& top, uint64_t total) {
    if (total == 0 || top.empty()) return 0.0;
    return static_cast<double>(top.front().request_count) / static_cast<double>(total);
}
inline double hot_prefix_topk_share(const std::vector<HotPrefixEntry>& top, uint64_t total) {
    if (total == 0) return 0.0;
    uint64_t sum = 0;
    for (const auto& e : top) sum += e.request_count;
    return static_cast<double>(sum) / static_cast<double>(total);
}

// Cache-topology sole-holder signals (BACKLOG §21 P2 slice 3). `holders[i]` is
// the live cluster holder count for top[i] (parallel vectors). A prefix is
// "sole-held" when exactly one node holds it hot — reaping that node is a cache
// cliff for that prefix. Since `top` is THIS node's own top-K (always
// self-applied into the index), holders[i] >= 1 and sole-held == "only we hold
// it". Both pure / reactor-free; tolerant of holders shorter than top (missing
// => treated as 0, e.g. a prefix dropped at the index cap).
//
// `quorum_degraded` is the split-brain freeze (open-decision "DEGRADED-quorum
// behavior", fail-safe): while the cluster view is unreliable a stale-but-
// unevicted peer can inflate holder counts and mask a true sole-holder (the
// fail-dangerous false-negative), so we NEVER report sole_held=false — every hot
// prefix is treated as sole-held until quorum recovers. This freezes the reaping
// signal toward "do not reap".
inline size_t hot_prefix_sole_held_count(const std::vector<size_t>& holders,
                                         bool quorum_degraded = false) {
    if (quorum_degraded) return holders.size();  // freeze: all treated sole-held
    size_t n = 0;
    for (size_t h : holders) {
        if (h == 1) ++n;
    }
    return n;
}
inline double hot_prefix_sole_held_request_share(const std::vector<HotPrefixEntry>& top,
                                                 const std::vector<size_t>& holders,
                                                 uint64_t total,
                                                 bool quorum_degraded = false) {
    if (total == 0) return 0.0;
    uint64_t sum = 0;
    const size_t n = std::min(top.size(), holders.size());
    for (size_t i = 0; i < n; ++i) {
        if (quorum_degraded || holders[i] == 1) sum += top[i].request_count;
    }
    return static_cast<double>(sum) / static_cast<double>(total);
}

// Content-free JSON for GET /v1/cache/topology: prefix FINGERPRINTS (hex) and
// counts only — never token text. When `include_topology` (BACKLOG §21 P2 is
// enabled), each entry carries the live cluster holder count `holders[i]` and a
// `sole_held` flag (exactly one holder), plus a top-level `quorum`. `holders`
// shorter than top => missing counts are 0 (sole_held false). `quorum_degraded`
// surfaces as "quorum":"DEGRADED" and forces every sole_held=true (split-brain
// freeze — see helpers above); the real (untrustworthy) holder count is still
// shown. When `include_topology` is false (P2 disabled) the cluster fields are
// omitted entirely — the endpoint reverts to its P1 shape (prefix_fp +
// request_count), never emitting a misleading sole_held=false. Bounded by
// top.size() (<= K). Pure.
// `verified_holders[i]` (BACKLOG §21 Phase 5d) is the count of holders that report
// top[i] VERIFIED-RESIDENT in their own KV cache (native KV trust) — a subset of
// holders[i]. Each entry gains `verified_holders` and `verified_sole_held` (exactly
// one verified holder => the automated-reaping gate). The DEGRADED freeze applies
// identically: verified_sole_held is forced true (never assert "safe to reap" while
// the cluster view is unreliable). `verified_holders` shorter than top => 0.
inline std::string format_hot_prefix_topology_json(const std::vector<HotPrefixEntry>& top,
                                                   const std::vector<size_t>& holders,
                                                   const std::vector<size_t>& verified_holders,
                                                   uint64_t total_touches,
                                                   int64_t snapshot_age_ms,
                                                   bool quorum_degraded = false,
                                                   bool include_topology = true) {
    std::string out;
    out.reserve(96 + top.size() * 96);
    char head[224];
    if (include_topology) {
        std::snprintf(head, sizeof(head),
            "{\"snapshot_age_ms\":%lld,\"total_touches\":%llu,\"count\":%zu,\"quorum\":\"%s\",\"hot_prefixes\":[",
            static_cast<long long>(snapshot_age_ms),
            static_cast<unsigned long long>(total_touches),
            top.size(),
            (quorum_degraded ? "DEGRADED" : "HEALTHY"));
    } else {
        std::snprintf(head, sizeof(head),
            "{\"snapshot_age_ms\":%lld,\"total_touches\":%llu,\"count\":%zu,\"hot_prefixes\":[",
            static_cast<long long>(snapshot_age_ms),
            static_cast<unsigned long long>(total_touches),
            top.size());
    }
    out += head;
    for (size_t i = 0; i < top.size(); ++i) {
        char ent[224];
        if (include_topology) {
            const size_t h = (i < holders.size()) ? holders[i] : 0;
            const size_t v = (i < verified_holders.size()) ? verified_holders[i] : 0;
            const bool sole_held = quorum_degraded || (h == 1);
            const bool verified_sole_held = quorum_degraded || (v == 1);
            std::snprintf(ent, sizeof(ent),
                "%s{\"prefix_fp\":\"%016llx\",\"request_count\":%llu,\"holders\":%zu,\"sole_held\":%s,"
                "\"verified_holders\":%zu,\"verified_sole_held\":%s}",
                (i == 0 ? "" : ","),
                static_cast<unsigned long long>(top[i].prefix_fp),
                static_cast<unsigned long long>(top[i].request_count),
                h,
                (sole_held ? "true" : "false"),
                v,
                (verified_sole_held ? "true" : "false"));
        } else {
            std::snprintf(ent, sizeof(ent),
                "%s{\"prefix_fp\":\"%016llx\",\"request_count\":%llu}",
                (i == 0 ? "" : ","),
                static_cast<unsigned long long>(top[i].prefix_fp),
                static_cast<unsigned long long>(top[i].request_count));
        }
        out += ent;
    }
    out += "]}";
    return out;
}

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
    ScoringWeights scoring;   // config.routing.scoring — per-signal route-score weights
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

    // Hot-prefix top-K for this window (BACKLOG §21 P1; format v4). Cluster-merged
    // top prefixes by request rate, content-free (prefix fingerprints only), and
    // hot_prefix_touches is the cluster-wide prefix-routed request count — the
    // denominator a consumer uses for top-1 / top-K concentration. Empty when
    // hot-prefix tracking is disabled.
    std::vector<HotPrefixEntry> hot_prefixes;
    uint64_t                    hot_prefix_touches = 0;
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
