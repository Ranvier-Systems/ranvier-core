// Ranvier Core - Routing-Score Weights Provider Unit Tests
//
// Reactor-free tests over the routing-weights provider seam:
//   - RoutingWeightsKey equality / hashability
//   - NoopRoutingWeightsProvider default + the process-wide factory slot
//   - build_routing_weights_snapshot (the off-request-path consult)
//   - resolve_scoring_weights (the hot-path pointer/read) + the two required
//     parity checks against the unified route scorer:
//       (1) no provider installed  -> identical routes/weights to the baseline
//       (2) a provider overriding one key -> only that key's resolved weights
//           change; every other key stays on the configured baseline
//
// The provider seam is pure C++ (no Seastar on the query path), so this test
// needs no reactor and no stubs beyond what route_scorer.hpp / types.hpp pull
// in. Mirrors the style of telemetry_sink_test.cpp.

#include "route_scorer.hpp"
#include "routing_weights_provider.hpp"
#include "types.hpp"

#include <absl/container/flat_hash_map.h>
#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

using namespace ranvier;

// =============================================================================
// RoutingWeightsKey: equality + hashability
// =============================================================================

TEST(RoutingWeightsKey, EqualityIsFieldwise) {
    RoutingWeightsKey a{"llama3", BackendType::VLLM, HardwareLabel::GPU_LARGE};
    RoutingWeightsKey b{"llama3", BackendType::VLLM, HardwareLabel::GPU_LARGE};
    EXPECT_TRUE(a == b);

    // Each dimension independently distinguishes:
    RoutingWeightsKey diff_model{"qwen2", BackendType::VLLM,   HardwareLabel::GPU_LARGE};
    RoutingWeightsKey diff_type {"llama3", BackendType::SGLANG, HardwareLabel::GPU_LARGE};
    RoutingWeightsKey diff_hw   {"llama3", BackendType::VLLM,   HardwareLabel::GPU_SMALL};
    EXPECT_FALSE(a == diff_model);
    EXPECT_FALSE(a == diff_type);
    EXPECT_FALSE(a == diff_hw);
}

TEST(RoutingWeightsKey, EmptyModelFamilyIsAValidDistinctKey) {
    // An unlabeled fleet resolves to an empty model_family; it must be a stable,
    // usable key (not conflated with any labeled family).
    RoutingWeightsKey unlabeled{"", BackendType::VLLM, HardwareLabel::UNSPECIFIED};
    RoutingWeightsKey labeled{"llama3", BackendType::VLLM, HardwareLabel::UNSPECIFIED};
    EXPECT_FALSE(unlabeled == labeled);

    absl::flat_hash_map<RoutingWeightsKey, int, RoutingWeightsKeyHash> m;
    m[unlabeled] = 1;
    m[labeled] = 2;
    EXPECT_EQ(m.size(), 2u);
}

TEST(RoutingWeightsKey, InsertableIntoFlatHashMap) {
    absl::flat_hash_map<RoutingWeightsKey, int, RoutingWeightsKeyHash> m;
    RoutingWeightsKey k1{"llama3", BackendType::VLLM,   HardwareLabel::GPU_LARGE};
    RoutingWeightsKey k2{"qwen2",  BackendType::SGLANG, HardwareLabel::GPU_SMALL};
    m[k1] = 7;
    m[k2] = 11;
    ASSERT_EQ(m.size(), 2u);
    EXPECT_EQ(m[k1], 7);
    EXPECT_EQ(m[k2], 11);
    RoutingWeightsKey k1_copy = k1;
    m[k1_copy] = 42;  // same key replaces
    EXPECT_EQ(m.size(), 2u);
    EXPECT_EQ(m[k1], 42);
}

TEST(RoutingWeightsKey, HashIsDistributionalNotIdentity) {
    // Sanity: distinct keys (overwhelmingly) hash to distinct values — catches a
    // degenerate hash that collapses every input to one bucket.
    std::unordered_set<size_t> hashes;
    RoutingWeightsKeyHash h;
    for (auto bt : {BackendType::VLLM, BackendType::SGLANG, BackendType::CEREBRAS}) {
        for (auto hw : {HardwareLabel::UNSPECIFIED,
                        HardwareLabel::GPU_SMALL,
                        HardwareLabel::GPU_LARGE}) {
            for (const auto* family : {"llama3", "qwen2", "mistral", "unspecified"}) {
                hashes.insert(h(RoutingWeightsKey{family, bt, hw}));
            }
        }
    }
    // 3 * 3 * 4 = 36 inputs; expect at least 32 distinct hashes (loose bound).
    EXPECT_GE(hashes.size(), 32u);
}

// =============================================================================
// NoopRoutingWeightsProvider: the default provider always abstains
// =============================================================================

TEST(NoopRoutingWeightsProvider, AlwaysReturnsNullopt) {
    NoopRoutingWeightsProvider provider;
    EXPECT_FALSE(provider.weights_for(
        RoutingWeightsKey{"llama3", BackendType::VLLM, HardwareLabel::GPU_LARGE}).has_value());
    EXPECT_FALSE(provider.weights_for(RoutingWeightsKey{}).has_value());
}

TEST(NoopRoutingWeightsProvider, FactoryReturnsNoop) {
    auto provider = make_default_routing_weights_provider();
    ASSERT_NE(provider.get(), nullptr);
    EXPECT_NE(dynamic_cast<NoopRoutingWeightsProvider*>(provider.get()), nullptr);
}

// =============================================================================
// RoutingWeightsProviderFactory: process-wide injection seam
// =============================================================================
//
// The stock build leaves the slot unset, so get_routing_weights_provider_factory()
// yields the no-op default; a downstream build calls
// set_routing_weights_provider_factory() at start-up to install a real provider.
// These pin both ends of that seam (mirrors the telemetry-sink factory test).

namespace {
// Test double, distinguishable from the no-op, proving the installed factory
// (not the default) produced the provider.
struct FakeWeightsProvider final : RoutingWeightsProvider {
    RoutingWeightsKey override_key;
    ScoringWeights    override_weights;

    std::optional<ScoringWeights> weights_for(const RoutingWeightsKey& key) override {
        if (key == override_key) return override_weights;
        return std::nullopt;  // no opinion on any other key
    }
};
}  // namespace

TEST(RoutingWeightsProviderFactory, DefaultsToNoopWhenUnset) {
    auto provider = get_routing_weights_provider_factory()();
    ASSERT_NE(provider.get(), nullptr);
    EXPECT_NE(dynamic_cast<NoopRoutingWeightsProvider*>(provider.get()), nullptr);
}

TEST(RoutingWeightsProviderFactory, InstalledFactoryOverridesDefault) {
    set_routing_weights_provider_factory([]() -> std::unique_ptr<RoutingWeightsProvider> {
        return std::make_unique<FakeWeightsProvider>();
    });

    auto provider = get_routing_weights_provider_factory()();
    EXPECT_NE(dynamic_cast<FakeWeightsProvider*>(provider.get()), nullptr);
    EXPECT_EQ(dynamic_cast<NoopRoutingWeightsProvider*>(provider.get()), nullptr);

    // The slot is process-wide; reset so it can't leak into other tests.
    set_routing_weights_provider_factory(nullptr);
    EXPECT_NE(dynamic_cast<NoopRoutingWeightsProvider*>(
                  get_routing_weights_provider_factory()().get()),
              nullptr);
}

// =============================================================================
// build_routing_weights_snapshot: the off-request-path consult
// =============================================================================

TEST(BuildRoutingWeightsSnapshot, NoopProviderYieldsEmptySnapshot) {
    NoopRoutingWeightsProvider provider;
    std::vector<RoutingWeightsKey> keys = {
        {"llama3", BackendType::VLLM,   HardwareLabel::GPU_LARGE},
        {"qwen2",  BackendType::SGLANG, HardwareLabel::GPU_SMALL},
    };
    auto snapshot = build_routing_weights_snapshot(provider, keys);
    EXPECT_TRUE(snapshot.empty());
}

TEST(BuildRoutingWeightsSnapshot, OnlyOverriddenKeysAppear) {
    RoutingWeightsKey key_a{"llama3", BackendType::VLLM,   HardwareLabel::GPU_LARGE};
    RoutingWeightsKey key_b{"qwen2",  BackendType::SGLANG, HardwareLabel::GPU_SMALL};

    FakeWeightsProvider provider;
    provider.override_key = key_a;
    provider.override_weights.prefix_weight = 3.0;  // any non-baseline marker

    auto snapshot = build_routing_weights_snapshot(provider, {key_a, key_b});
    ASSERT_EQ(snapshot.size(), 1u);
    ASSERT_TRUE(snapshot.contains(key_a));
    EXPECT_FALSE(snapshot.contains(key_b));  // "no opinion" key omitted
    EXPECT_DOUBLE_EQ(snapshot.at(key_a).prefix_weight, 3.0);
}

// =============================================================================
// resolve_scoring_weights: the hot-path pointer/read
// =============================================================================

TEST(ResolveScoringWeights, EmptySnapshotReturnsBaselineByReference) {
    RoutingWeightsSnapshot empty;
    ScoringWeights baseline;  // configured routing.scoring.* defaults
    RoutingWeightsKey key{"llama3", BackendType::VLLM, HardwareLabel::GPU_LARGE};

    const ScoringWeights& resolved = resolve_scoring_weights(empty, key, baseline);
    // Pointer/read, not a copy: the safe-default path returns the SAME object.
    EXPECT_EQ(&resolved, &baseline);
}

TEST(ResolveScoringWeights, MissingKeyFallsThroughToBaseline) {
    RoutingWeightsKey present{"llama3", BackendType::VLLM,   HardwareLabel::GPU_LARGE};
    RoutingWeightsKey absent {"qwen2",  BackendType::SGLANG, HardwareLabel::GPU_SMALL};
    ScoringWeights baseline;

    RoutingWeightsSnapshot snapshot;
    ScoringWeights override_w;
    override_w.load_weight = 0.25;
    snapshot.insert_or_assign(present, override_w);

    const ScoringWeights& resolved = resolve_scoring_weights(snapshot, absent, baseline);
    EXPECT_EQ(&resolved, &baseline);  // key not in map -> configured baseline
}

TEST(ResolveScoringWeights, PresentKeyReturnsOverride) {
    RoutingWeightsKey key{"llama3", BackendType::VLLM, HardwareLabel::GPU_LARGE};
    ScoringWeights baseline;

    RoutingWeightsSnapshot snapshot;
    ScoringWeights override_w;
    override_w.load_weight = 0.25;
    snapshot.insert_or_assign(key, override_w);

    const ScoringWeights& resolved = resolve_scoring_weights(snapshot, key, baseline);
    EXPECT_NE(&resolved, &baseline);
    EXPECT_DOUBLE_EQ(resolved.load_weight, 0.25);
}

// =============================================================================
// Scorer parity: the two required end-to-end checks
// =============================================================================
//
// Scenario (a cache-miss hardware-price divert): an anchor and a strictly-
// cheaper priced candidate. At baseline weights (price_weight=1.0) placement
// diverts to the cheaper backend. A provider override that zeroes price_weight
// for one key keeps placement on the anchor. This lets us prove both required
// properties through the real score_and_select().

namespace {
// Two candidates: index 0 is the anchor, index 1 is a strictly-cheaper priced
// alternative (price_advantage > 0). No load/cost terms in play.
std::vector<ScoreCandidate> make_price_divert_candidates() {
    ScoreCandidate anchor;
    anchor.id = 1;
    anchor.is_anchor = true;
    anchor.price_advantage = 0.0;  // the anchor is never "cheaper than itself"

    ScoreCandidate cheaper;
    cheaper.id = 2;
    cheaper.is_anchor = false;
    cheaper.price_advantage = 0.5;  // 50% cheaper than the anchor

    return {anchor, cheaper};
}
}  // namespace

// (1) No provider installed -> the snapshot is empty -> the resolved weights ARE
//     the configured baseline, so the scorer's decision is identical.
TEST(ScorerParity, NoProviderReproducesConfiguredBaseline) {
    const ScoringWeights baseline;  // stock routing.scoring.* defaults
    auto candidates = make_price_divert_candidates();

    // Reference decision: score directly with the configured baseline.
    const ScoreDecision baseline_decision = score_and_select(
        candidates.data(), candidates.size(), baseline, /*load_term_enabled=*/false);
    EXPECT_EQ(baseline_decision.placement, 1u);  // diverts to the cheaper backend

    // No provider => empty snapshot => resolve returns the baseline unchanged.
    RoutingWeightsSnapshot empty;
    RoutingWeightsKey any_key{"llama3", BackendType::VLLM, HardwareLabel::GPU_LARGE};
    const ScoringWeights& resolved = resolve_scoring_weights(empty, any_key, baseline);

    const ScoreDecision resolved_decision = score_and_select(
        candidates.data(), candidates.size(), resolved, /*load_term_enabled=*/false);

    EXPECT_EQ(resolved_decision.placement, baseline_decision.placement);
    EXPECT_EQ(resolved_decision.dispatch,  baseline_decision.dispatch);
}

// (2) A provider overriding ONE key changes only that key's resolved weights and
//     leaves every other key on the configured baseline.
TEST(ScorerParity, OverrideChangesOnlyTheTargetedKey) {
    const ScoringWeights baseline;
    auto candidates = make_price_divert_candidates();

    const ScoreDecision baseline_decision = score_and_select(
        candidates.data(), candidates.size(), baseline, /*load_term_enabled=*/false);
    ASSERT_EQ(baseline_decision.placement, 1u);  // cheaper backend at baseline

    RoutingWeightsKey key_a{"llama3", BackendType::VLLM,   HardwareLabel::GPU_LARGE};
    RoutingWeightsKey key_b{"qwen2",  BackendType::SGLANG, HardwareLabel::GPU_SMALL};

    // Provider disables the hardware-price term for key_a only.
    FakeWeightsProvider provider;
    provider.override_key = key_a;
    provider.override_weights = baseline;
    provider.override_weights.price_weight = 0.0;

    auto snapshot = build_routing_weights_snapshot(provider, {key_a, key_b});
    ASSERT_EQ(snapshot.size(), 1u);

    // key_a: the override is applied -> price divert is off -> anchor keeps the seat.
    const ScoringWeights& weights_a = resolve_scoring_weights(snapshot, key_a, baseline);
    const ScoreDecision decision_a = score_and_select(
        candidates.data(), candidates.size(), weights_a, /*load_term_enabled=*/false);
    EXPECT_EQ(decision_a.placement, 0u);  // anchor, NOT the cheaper backend

    // key_b: no opinion -> configured baseline -> identical to the reference.
    const ScoringWeights& weights_b = resolve_scoring_weights(snapshot, key_b, baseline);
    const ScoreDecision decision_b = score_and_select(
        candidates.data(), candidates.size(), weights_b, /*load_term_enabled=*/false);
    EXPECT_EQ(decision_b.placement, baseline_decision.placement);
    EXPECT_EQ(decision_b.dispatch,  baseline_decision.dispatch);
    EXPECT_EQ(&weights_b, &baseline);  // untouched key resolves to the baseline object
}
