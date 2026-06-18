// Ranvier Core - Telemetry Sink Unit Tests
//
// Reactor-free tests over the telemetry-sink data contract: bucket key
// hash/equality, AggregateRecord merge semantics, wire-stable BackendType /
// HardwareLabel / WorkloadPattern ordinals (the wire-format invariant — see
// src/telemetry_schema.hpp §"FORWARD-COMPATIBILITY CONTRACT"), and the
// NoopSink default. Uses the seastar stubs in tests/stubs so this test
// does NOT need a running reactor.
//
// What this test does NOT cover (out of scope here; needs a reactor):
//   - TelemetryService::record_outcome / snapshot_and_reset shard-local paths
//   - Shard-0 emitter map_reduce → consume drop-guard
//   - Cross-shard foreign_ptr discipline
//
// Those live in the integration build and on the operator's Docker test loop.

#include "route_scorer.hpp"
#include "telemetry_schema.hpp"
#include "telemetry_sink.hpp"
#include "types.hpp"

#include <absl/container/flat_hash_map.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <unordered_set>

using namespace ranvier;

// =============================================================================
// HardwareLabel wire-stable ordinals
// =============================================================================
//
// These ordinals are part of the telemetry-sink wire contract. They MUST NOT
// be renumbered (renaming the C++ symbol is fine). Adding a new label is
// append-only at a strictly-greater integer. See src/types.hpp.

TEST(HardwareLabel, UnspecifiedIsZero) {
    EXPECT_EQ(static_cast<uint8_t>(HardwareLabel::UNSPECIFIED), 0);
}

TEST(HardwareLabel, OrdinalsArePinned) {
    // If this fails, someone re-numbered the enum — that silently breaks
    // comparability of every WindowReport already aggregated by a downstream
    // catalog. Bump the format version and migrate readers FIRST.
    EXPECT_EQ(static_cast<uint8_t>(HardwareLabel::GPU_SMALL), 1);
    EXPECT_EQ(static_cast<uint8_t>(HardwareLabel::GPU_LARGE), 2);
}

TEST(HardwareLabel, StringRoundTrip) {
    for (auto t : {HardwareLabel::UNSPECIFIED,
                   HardwareLabel::GPU_SMALL,
                   HardwareLabel::GPU_LARGE}) {
        auto s = hardware_label_to_string(t);
        auto parsed = parse_hardware_label(s);
        ASSERT_TRUE(parsed.has_value()) << "round-trip lost: " << s;
        EXPECT_EQ(*parsed, t);
    }
}

TEST(HardwareLabel, UnknownStringParsesToNullopt) {
    EXPECT_FALSE(parse_hardware_label("h100").has_value());
    EXPECT_FALSE(parse_hardware_label("").has_value());
    EXPECT_FALSE(parse_hardware_label("GPU_SMALL").has_value());  // case-sensitive
}

// =============================================================================
// BackendType wire-stable ordinals
// =============================================================================
//
// BackendType is a telemetry bucket-key dimension (engine class), so its
// ordinals are part of the wire contract: pinned, append-only, never
// renumbered. The persisted/admin form is the string; these ordinals serve
// only the telemetry wire. No UNSPECIFIED sentinel — every backend has a real
// engine class.

TEST(BackendType, OrdinalsArePinned) {
    // If this fails, someone re-numbered the enum — that silently breaks
    // comparability of every WindowReport already aggregated by engine class.
    EXPECT_EQ(static_cast<uint8_t>(BackendType::VLLM),              0);
    EXPECT_EQ(static_cast<uint8_t>(BackendType::SGLANG),           1);
    EXPECT_EQ(static_cast<uint8_t>(BackendType::TRT_LLM),          2);
    EXPECT_EQ(static_cast<uint8_t>(BackendType::OLLAMA),           3);
    EXPECT_EQ(static_cast<uint8_t>(BackendType::LM_STUDIO),        4);
    EXPECT_EQ(static_cast<uint8_t>(BackendType::CEREBRAS),         5);
    EXPECT_EQ(static_cast<uint8_t>(BackendType::OPENAI_COMPATIBLE), 6);
}

TEST(BackendType, StringRoundTrip) {
    for (auto t : {BackendType::VLLM, BackendType::SGLANG, BackendType::TRT_LLM,
                   BackendType::OLLAMA, BackendType::LM_STUDIO,
                   BackendType::CEREBRAS, BackendType::OPENAI_COMPATIBLE}) {
        auto parsed = parse_backend_type(backend_type_to_string(t));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, t);
    }
}

// =============================================================================
// WorkloadPattern wire-stable ordinals
// =============================================================================

TEST(WorkloadPattern, UnknownIsZero) {
    EXPECT_EQ(static_cast<uint8_t>(WorkloadPattern::UNKNOWN), 0);
}

TEST(WorkloadPattern, OrdinalsArePinned) {
    EXPECT_EQ(static_cast<uint8_t>(WorkloadPattern::AUTOCOMPLETE), 1);
    EXPECT_EQ(static_cast<uint8_t>(WorkloadPattern::CHAT),         2);
    EXPECT_EQ(static_cast<uint8_t>(WorkloadPattern::EDIT),         3);
}

TEST(WorkloadPattern, FromIntentMapsAllValues) {
    EXPECT_EQ(workload_pattern_from_intent(RequestIntent::AUTOCOMPLETE),
              WorkloadPattern::AUTOCOMPLETE);
    EXPECT_EQ(workload_pattern_from_intent(RequestIntent::CHAT),
              WorkloadPattern::CHAT);
    EXPECT_EQ(workload_pattern_from_intent(RequestIntent::EDIT),
              WorkloadPattern::EDIT);
}

// =============================================================================
// TelemetryBucketKey: equality + hashability
// =============================================================================

TEST(TelemetryBucketKey, EqualityIsFieldwise) {
    TelemetryBucketKey a{"llama3", BackendType::VLLM, HardwareLabel::GPU_LARGE, WorkloadPattern::CHAT};
    TelemetryBucketKey b{"llama3", BackendType::VLLM, HardwareLabel::GPU_LARGE, WorkloadPattern::CHAT};
    EXPECT_TRUE(a == b);

    // Each field independently distinguishes:
    TelemetryBucketKey diff_model{"qwen2", BackendType::VLLM,   HardwareLabel::GPU_LARGE, WorkloadPattern::CHAT};
    TelemetryBucketKey diff_bt{"llama3",   BackendType::SGLANG, HardwareLabel::GPU_LARGE, WorkloadPattern::CHAT};
    TelemetryBucketKey diff_hw{"llama3",   BackendType::VLLM,   HardwareLabel::GPU_SMALL, WorkloadPattern::CHAT};
    TelemetryBucketKey diff_wl{"llama3",   BackendType::VLLM,   HardwareLabel::GPU_LARGE, WorkloadPattern::EDIT};
    EXPECT_FALSE(a == diff_model);
    EXPECT_FALSE(a == diff_bt);
    EXPECT_FALSE(a == diff_hw);
    EXPECT_FALSE(a == diff_wl);
}

TEST(TelemetryBucketKey, InsertableIntoFlatHashMap) {
    absl::flat_hash_map<TelemetryBucketKey, int, TelemetryBucketKeyHash> m;
    TelemetryBucketKey k1{"llama3", BackendType::VLLM,   HardwareLabel::GPU_LARGE, WorkloadPattern::CHAT};
    TelemetryBucketKey k2{"qwen2",  BackendType::SGLANG, HardwareLabel::GPU_SMALL, WorkloadPattern::EDIT};
    m[k1] = 7;
    m[k2] = 11;
    ASSERT_EQ(m.size(), 2u);
    EXPECT_EQ(m[k1], 7);
    EXPECT_EQ(m[k2], 11);
    // Second insert of same key replaces:
    TelemetryBucketKey k1_copy = k1;
    m[k1_copy] = 42;
    EXPECT_EQ(m.size(), 2u);
    EXPECT_EQ(m[k1], 42);
}

TEST(TelemetryBucketKey, HashIsDistributionalNotIdentity) {
    // Sanity: distinct keys (overwhelmingly) hash to distinct values. Not a
    // strict invariant — hashes collide — but catches a degenerate hash that
    // returns the same value for every input.
    std::unordered_set<size_t> hashes;
    TelemetryBucketKeyHash h;
    for (auto bt : {BackendType::VLLM, BackendType::SGLANG, BackendType::CEREBRAS}) {
        for (auto hw : {HardwareLabel::UNSPECIFIED,
                        HardwareLabel::GPU_SMALL,
                        HardwareLabel::GPU_LARGE}) {
            for (auto wl : {WorkloadPattern::UNKNOWN,
                            WorkloadPattern::AUTOCOMPLETE,
                            WorkloadPattern::CHAT,
                            WorkloadPattern::EDIT}) {
                for (const auto* family : {"llama3", "qwen2", "mistral", "unspecified"}) {
                    TelemetryBucketKey k{family, bt, hw, wl};
                    hashes.insert(h(k));
                }
            }
        }
    }
    // 3 * 3 * 4 * 4 = 144 inputs; expect at least 130 distinct hashes (loose bound).
    EXPECT_GE(hashes.size(), 130u);
}

// =============================================================================
// AggregateRecord::merge_from
// =============================================================================

TEST(AggregateRecord, MergeFromSumsCounters) {
    AggregateRecord dst;
    AggregateRecord src;

    dst.request_count          = 10;
    dst.cache_hit_count        = 7;
    dst.cache_miss_count       = 3;
    dst.load_redirect_count    = 1;
    dst.cost_redirect_count    = 0;
    dst.fast_lane_count        = 2;
    dst.success_count          = 9;
    dst.failure_count          = 1;
    dst.timeout_count          = 0;
    dst.connection_error_count = 0;

    src.request_count          = 5;
    src.cache_hit_count        = 2;
    src.cache_miss_count       = 3;
    src.load_redirect_count    = 4;
    src.cost_redirect_count    = 2;
    src.fast_lane_count        = 1;
    src.success_count          = 3;
    src.failure_count          = 1;
    src.timeout_count          = 1;
    src.connection_error_count = 0;

    dst.merge_from(src);

    EXPECT_EQ(dst.request_count,          15u);
    EXPECT_EQ(dst.cache_hit_count,         9u);
    EXPECT_EQ(dst.cache_miss_count,        6u);
    EXPECT_EQ(dst.load_redirect_count,     5u);
    EXPECT_EQ(dst.cost_redirect_count,     2u);
    EXPECT_EQ(dst.fast_lane_count,         3u);
    EXPECT_EQ(dst.success_count,          12u);
    EXPECT_EQ(dst.failure_count,           2u);
    EXPECT_EQ(dst.timeout_count,           1u);
    EXPECT_EQ(dst.connection_error_count,  0u);
}

TEST(AggregateRecord, MergeFromAccumulatesHistograms) {
    AggregateRecord a;
    AggregateRecord b;

    // Record some TTFT samples into each
    a.ttft_seconds.record(0.005);  // 5ms
    a.ttft_seconds.record(0.020);  // 20ms
    b.ttft_seconds.record(0.010);  // 10ms

    EXPECT_EQ(a.ttft_seconds.data.sample_count, 2u);
    EXPECT_EQ(b.ttft_seconds.data.sample_count, 1u);

    a.merge_from(b);

    EXPECT_EQ(a.ttft_seconds.data.sample_count, 3u);
    EXPECT_DOUBLE_EQ(a.ttft_seconds.data.sample_sum, 0.005 + 0.020 + 0.010);

    // Source is unchanged — merge is a copy-out into dst, not a move.
    EXPECT_EQ(b.ttft_seconds.data.sample_count, 1u);
}

TEST(AggregateRecord, MergeFromPrefixReuseDepthAccumulates) {
    AggregateRecord a;
    AggregateRecord b;
    a.prefix_reuse_depth.record(0);    // miss
    a.prefix_reuse_depth.record(64);
    b.prefix_reuse_depth.record(128);
    b.prefix_reuse_depth.record(256);
    a.merge_from(b);
    EXPECT_EQ(a.prefix_reuse_depth.data.sample_count, 4u);
    EXPECT_DOUBLE_EQ(a.prefix_reuse_depth.data.sample_sum, 0 + 64 + 128 + 256);
}

// =============================================================================
// WindowReport: format_version is set, forward-compat-safe defaults
// =============================================================================

TEST(WindowReport, DefaultsCarryCurrentFormatVersion) {
    WindowReport report;
    EXPECT_EQ(report.format_version, kTelemetryReportFormatVersion);
    EXPECT_EQ(report.format_version, 3u);  // v3: route-scorer weights added to the strategy snapshot
    EXPECT_TRUE(report.records.empty());
    EXPECT_EQ(report.shard_count, 0u);
    EXPECT_EQ(report.window_eviction_churn, 0u);
    EXPECT_EQ(report.buckets_overflowed, 0u);
}

// The forward-compatibility contract says: a consumer reading a WindowReport
// at a HIGHER format_version must still be able to read the fields it knows.
// We can't simulate a newer producer in C++ directly (no wire form is emitted),
// but we can assert the contract holds for the in-memory struct:
// reading known fields when the version field has been overwritten still works.
TEST(WindowReport, FieldsRemainReadableWhenVersionBumped) {
    WindowReport report;
    report.format_version = 999;  // pretend a future producer
    report.window_start_ms = 1'000'000;
    report.window_end_ms   = 1'060'000;
    report.shard_count = 8;

    AggregateRecord rec;
    rec.key = TelemetryBucketKey{"llama3", BackendType::VLLM, HardwareLabel::GPU_LARGE, WorkloadPattern::CHAT};
    rec.request_count = 42;
    report.records.push_back(std::move(rec));

    // Consumer reads what it knows; ignores the version difference.
    EXPECT_EQ(report.shard_count, 8u);
    ASSERT_EQ(report.records.size(), 1u);
    EXPECT_EQ(report.records[0].request_count, 42u);
    EXPECT_EQ(report.records[0].key.backend_type, BackendType::VLLM);
    EXPECT_EQ(report.records[0].key.hardware_label, HardwareLabel::GPU_LARGE);
}

// =============================================================================
// RoutingStrategyParams: route-scorer weights ride along in the snapshot
// =============================================================================
//
// The per-window strategy snapshot must fully describe the active routing
// config. After routing was consolidated into the unified weighted scorer
// (route_scorer.hpp), the six per-signal weights (config.routing.scoring) are
// part of that config, so they belong in the snapshot. These assert the schema
// carries them and that non-neutral weights survive into WindowReport.strategy
// (the in-process struct the emitter hands to the sink; the snapshot itself is
// built in Application::make_strategy_snapshot, which needs a reactor).

TEST(RoutingStrategyParams, DefaultScoringWeightsMatchScorerParityBaseline) {
    // The snapshot's default weights mirror the shipped ScoringWeights defaults
    // (route_scorer.hpp) — the parity baseline where every term reduces to the
    // pre-scorer rule. If these drift, an operator running stock config would
    // see a "tuned" snapshot that never matches their config.
    RoutingStrategyParams p;
    EXPECT_DOUBLE_EQ(p.scoring.prefix_weight,    0.0);
    EXPECT_DOUBLE_EQ(p.scoring.load_weight,      1.0);
    EXPECT_DOUBLE_EQ(p.scoring.residency_weight, 0.0);
    EXPECT_DOUBLE_EQ(p.scoring.cost_weight,      1.0);
    EXPECT_DOUBLE_EQ(p.scoring.price_weight,     1.0);
    EXPECT_DOUBLE_EQ(p.scoring.slo_weight,       0.0);
}

TEST(RoutingStrategyParams, NonNeutralScoringWeightsRideAlongInWindowReport) {
    // Operator tunes routing.scoring.* away from neutral; make_strategy_snapshot
    // copies RoutingConfig::scoring verbatim (p.scoring = r.scoring). Model that
    // copy here and confirm all six weights survive into WindowReport.strategy.
    ScoringWeights tuned;
    tuned.prefix_weight    = 2.5;
    tuned.load_weight      = 0.25;
    tuned.residency_weight = 0.75;
    tuned.cost_weight      = 3.0;
    tuned.price_weight     = 0.5;
    tuned.slo_weight       = 1.5;

    RoutingStrategyParams snapshot;
    snapshot.scoring = tuned;   // mirrors `p.scoring = r.scoring;`

    WindowReport report;
    report.strategy = snapshot;

    EXPECT_DOUBLE_EQ(report.strategy.scoring.prefix_weight,    2.5);
    EXPECT_DOUBLE_EQ(report.strategy.scoring.load_weight,      0.25);
    EXPECT_DOUBLE_EQ(report.strategy.scoring.residency_weight, 0.75);
    EXPECT_DOUBLE_EQ(report.strategy.scoring.cost_weight,      3.0);
    EXPECT_DOUBLE_EQ(report.strategy.scoring.price_weight,     0.5);
    EXPECT_DOUBLE_EQ(report.strategy.scoring.slo_weight,       1.5);
}

// =============================================================================
// NoopSink: default sink discards every report
// =============================================================================

TEST(NoopSink, IsConstructibleAndPolymorphic) {
    std::unique_ptr<TelemetrySink> sink = std::make_unique<NoopSink>();
    EXPECT_NE(sink.get(), nullptr);
}

TEST(NoopSink, ConsumeReturnsFuture) {
    NoopSink sink;
    WindowReport report;
    // Just verify the call compiles and returns. The stubbed seastar::future
    // is a trivial constructor; this test confirms the interface shape.
    auto fut = sink.consume(report);
    (void)fut;
}

TEST(NoopSink, FactoryReturnsNoopSink) {
    auto sink = make_default_telemetry_sink();
    ASSERT_NE(sink.get(), nullptr);
    // Downcast to confirm it's a NoopSink (the factory contract).
    EXPECT_NE(dynamic_cast<NoopSink*>(sink.get()), nullptr);
}

// =============================================================================
// TelemetrySinkFactory: sink injection seam
// =============================================================================
//
// OSS leaves the factory unset, so get_telemetry_sink_factory() yields the
// NoopSink default; a downstream build calls set_telemetry_sink_factory() at
// start-up to install a real exporter. These pin both ends of that seam.

namespace {
// Test double, distinguishable from NoopSink, to prove the installed factory
// (not the default) produced the sink.
struct FakeExportSink final : TelemetrySink {
    seastar::future<> consume(const WindowReport& /*report*/) override {
        return seastar::make_ready_future<>();
    }
};
}  // namespace

TEST(TelemetrySinkFactory, DefaultsToNoopWhenUnset) {
    auto sink = get_telemetry_sink_factory()();
    ASSERT_NE(sink.get(), nullptr);
    EXPECT_NE(dynamic_cast<NoopSink*>(sink.get()), nullptr);
}

TEST(TelemetrySinkFactory, InstalledFactoryOverridesDefault) {
    set_telemetry_sink_factory([]() -> std::unique_ptr<TelemetrySink> {
        return std::make_unique<FakeExportSink>();
    });

    auto sink = get_telemetry_sink_factory()();
    EXPECT_NE(dynamic_cast<FakeExportSink*>(sink.get()), nullptr);
    EXPECT_EQ(dynamic_cast<NoopSink*>(sink.get()), nullptr);

    // The slot is process-wide; reset so it can't leak into other tests.
    set_telemetry_sink_factory(nullptr);
    EXPECT_NE(dynamic_cast<NoopSink*>(get_telemetry_sink_factory()().get()), nullptr);
}

// =============================================================================
// TelemetryOutcome::Kind: pinned ordinals
// =============================================================================
//
// The Kind enum is part of the recording-site contract; the values surface
// (translated) in the per-bucket success/failure/timeout/connection_error
// counters. They're not directly on the wire, but their ordering matters for
// switch-statement coverage in record_outcome.
TEST(TelemetryOutcome, KindOrdinalsArePinned) {
    EXPECT_EQ(static_cast<uint8_t>(TelemetryOutcome::Kind::SUCCESS),          0);
    EXPECT_EQ(static_cast<uint8_t>(TelemetryOutcome::Kind::FAILURE),          1);
    EXPECT_EQ(static_cast<uint8_t>(TelemetryOutcome::Kind::TIMEOUT),          2);
    EXPECT_EQ(static_cast<uint8_t>(TelemetryOutcome::Kind::CONNECTION_ERROR), 3);
}

// =============================================================================
// merge_hot_prefixes: cluster top-K merge (BACKLOG §21 P1)
// =============================================================================

TEST(MergeHotPrefixes, EmptyInput) {
    auto merged = merge_hot_prefixes({}, 128);
    EXPECT_TRUE(merged.top.empty());
    EXPECT_EQ(merged.total_touches, 0u);
}

TEST(MergeHotPrefixes, SumsCountsPerPrefixAcrossShards) {
    std::vector<HotPrefixWindow> per_shard = {
        {{{10, 5}, {20, 3}}, 8},   // shard 0: fp10=5, fp20=3, total 8
        {{{10, 2}, {30, 9}}, 11},  // shard 1: fp10=2, fp30=9, total 11
    };
    auto merged = merge_hot_prefixes(per_shard, 128);

    EXPECT_EQ(merged.total_touches, 19u);
    ASSERT_EQ(merged.top.size(), 3u);
    // Sorted by summed count desc: fp30=9, fp10=7, fp20=3.
    EXPECT_EQ(merged.top[0].prefix_fp, 30u);
    EXPECT_EQ(merged.top[0].request_count, 9u);
    EXPECT_EQ(merged.top[1].prefix_fp, 10u);
    EXPECT_EQ(merged.top[1].request_count, 7u);  // 5 + 2 summed across shards
    EXPECT_EQ(merged.top[2].prefix_fp, 20u);
    EXPECT_EQ(merged.top[2].request_count, 3u);
}

TEST(MergeHotPrefixes, TruncatesToK) {
    std::vector<HotPrefixWindow> per_shard(1);
    for (uint64_t fp = 0; fp < 50; ++fp) {
        per_shard[0].entries.push_back({fp, fp + 1});  // distinct counts
        per_shard[0].total_touches += fp + 1;
    }
    auto merged = merge_hot_prefixes(per_shard, 10);

    EXPECT_EQ(merged.top.size(), 10u);          // capped at k
    EXPECT_EQ(merged.top[0].prefix_fp, 49u);    // highest count first
    EXPECT_EQ(merged.top[0].request_count, 50u);
    EXPECT_EQ(merged.top[9].request_count, 41u);  // 10th highest
}
