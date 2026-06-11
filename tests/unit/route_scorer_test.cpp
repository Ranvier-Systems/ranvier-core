// Unit tests for the unified route scorer (pure decision core).
//
// route_scorer.hpp has no Seastar dependencies, so candidates are constructed
// directly and score_and_select() is exercised without any shard state. The
// cases below encode the default-weight parity matrix: each former override
// (load redirect, cost redirect, hardware-price preference) maps to a term,
// and at neutral weights the term must reproduce the override's exact
// keep/divert rule and divert target, including tie-breaks.

#include "route_scorer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

using namespace ranvier;

namespace {

constexpr uint32_t kNoProbe = std::numeric_limits<uint32_t>::max();

// Neutral defaults: pre-scorer behavior.
ScoringWeights default_weights() {
    return ScoringWeights{};
}

ScoreCandidate make_candidate(int32_t id, bool is_anchor = false) {
    ScoreCandidate c;
    c.id = id;
    c.is_anchor = is_anchor;
    return c;
}

}  // namespace

// =============================================================================
// load_hinge edges
// =============================================================================

TEST(LoadHinge, ZeroAtOrBelowAllowance) {
    EXPECT_EQ(load_hinge(0.0, 5.0), 0.0);
    EXPECT_EQ(load_hinge(5.0, 5.0), 0.0);  // at the pivot: no penalty (keep)
}

TEST(LoadHinge, LinearAboveAllowance) {
    EXPECT_EQ(load_hinge(6.0, 5.0), 1.0);
    EXPECT_EQ(load_hinge(12.5, 5.0), 7.5);
}

// =============================================================================
// Placement: anchor and hardware-price term
// =============================================================================

TEST(Placement, AnchorWinsTiesAtNeutralWeights) {
    // ART hit, no price/cost terms: every placement score is 0 (prefix_weight
    // 0 by default) — the anchor must win the tie regardless of position.
    std::vector<ScoreCandidate> c = {
        make_candidate(1),
        make_candidate(2, /*is_anchor=*/true),
        make_candidate(3),
    };
    c[1].affinity = 1.0;

    auto d = score_and_select(c.data(), c.size(), default_weights(), false);
    EXPECT_EQ(d.placement, 1u);
    EXPECT_EQ(d.dispatch, 1u);
    EXPECT_FALSE(d.load_diverted);
    EXPECT_FALSE(d.cost_diverted);
}

TEST(Placement, CheapestEligibleWinsForAnyPositivePriceWeight) {
    // Miss path with two eligible cheaper candidates: the larger advantage
    // (cheapest) must win for any price_weight > 0 — magnitude-independent,
    // like the former strictly-cheaper rule.
    std::vector<ScoreCandidate> c = {
        make_candidate(1, /*is_anchor=*/true),
        make_candidate(2),
        make_candidate(3),
    };
    c[1].price_advantage = 0.25;  // mid-priced
    c[2].price_advantage = 0.75;  // cheapest

    for (double w : {0.001, 1.0, 500.0}) {
        ScoringWeights weights = default_weights();
        weights.price_weight = w;
        auto d = score_and_select(c.data(), c.size(), weights, false);
        EXPECT_EQ(d.placement, 2u) << "price_weight=" << w;
        EXPECT_EQ(d.dispatch, 2u) << "price_weight=" << w;
    }
}

TEST(Placement, EqualPriceAdvantageTieBreaksTowardLowerLoad) {
    // Former rule: equal-price eligible candidates tie toward lower composite
    // load, then lower id.
    std::vector<ScoreCandidate> c = {
        make_candidate(1, /*is_anchor=*/true),
        make_candidate(2),
        make_candidate(3),
    };
    c[1].price_advantage = 0.5;
    c[1].price_tiebreak_load = 4.0;
    c[2].price_advantage = 0.5;
    c[2].price_tiebreak_load = 1.0;

    auto d = score_and_select(c.data(), c.size(), default_weights(), false);
    EXPECT_EQ(d.placement, 2u);

    c[2].price_tiebreak_load = 4.0;  // full tie: lower id wins
    d = score_and_select(c.data(), c.size(), default_weights(), false);
    EXPECT_EQ(c[d.placement].id, 2);
}

TEST(Placement, NoEligibleCandidateKeepsAnchor) {
    // All advantages zero (unpriced fleet / no strictly-cheaper candidate):
    // the deterministic hash spread is preserved.
    std::vector<ScoreCandidate> c = {
        make_candidate(5),
        make_candidate(7, /*is_anchor=*/true),
    };
    auto d = score_and_select(c.data(), c.size(), default_weights(), false);
    EXPECT_EQ(c[d.placement].id, 7);
}

// =============================================================================
// Dispatch: load term
// =============================================================================

TEST(DispatchLoad, CleanAnchorKeepsSeatRegardlessOfOtherLoads) {
    // Anchor at its allowance pays no penalty; an idle alternative must not
    // steal the seat (the former overrides only diverted past the threshold).
    std::vector<ScoreCandidate> c = {
        make_candidate(1, /*is_anchor=*/true),
        make_candidate(2),
    };
    c[0].affinity = 1.0;
    c[0].load = 4.0;
    c[0].load_allowance = 4.0;
    c[1].load = 0.0;
    c[1].load_allowance = 4.0;

    auto d = score_and_select(c.data(), c.size(), default_weights(), true);
    EXPECT_EQ(d.dispatch, 0u);
    EXPECT_FALSE(d.load_diverted);
}

TEST(DispatchLoad, OverAllowanceDivertsToLeastLoaded) {
    // Anchor strictly over the allowance forfeits; among under-allowance
    // candidates the lower load wins, then lower id.
    std::vector<ScoreCandidate> c = {
        make_candidate(1, /*is_anchor=*/true),
        make_candidate(2),
        make_candidate(3),
    };
    for (auto& cand : c) cand.load_allowance = 4.0;
    c[0].affinity = 1.0;
    c[0].load = 5.0;  // hinge 1.0
    c[1].load = 3.0;
    c[2].load = 1.0;

    auto d = score_and_select(c.data(), c.size(), default_weights(), true);
    EXPECT_EQ(c[d.dispatch].id, 3);
    EXPECT_TRUE(d.load_diverted);
    EXPECT_FALSE(d.cost_diverted);
    // Placement is unmoved: load diverts are transient, never learned.
    EXPECT_EQ(c[d.placement].id, 1);
}

TEST(DispatchLoad, ProbeRankBeatsLowerLoadAmongUnderAllowance) {
    // BOUNDED_LOAD spillover: the former re-probe took the FIRST under-cap
    // candidate in probe order, not the least loaded one.
    std::vector<ScoreCandidate> c = {
        make_candidate(1, /*is_anchor=*/true),
        make_candidate(2),
        make_candidate(3),
    };
    for (auto& cand : c) cand.load_allowance = 4.0;
    c[0].affinity = 1.0;
    c[0].load = 9.0;
    c[1].load = 3.0;  // higher load, but first in probe order
    c[1].probe_rank = 0;
    c[2].load = 1.0;
    c[2].probe_rank = 2;

    auto d = score_and_select(c.data(), c.size(), default_weights(), true);
    EXPECT_EQ(c[d.dispatch].id, 2);
    EXPECT_TRUE(d.load_diverted);
}

TEST(DispatchLoad, AllOverAllowanceFallsBackToLeastExcess) {
    // Saturated fleet: every candidate over the allowance — the least loaded
    // wins on raw score (no probe ranks are filled in this case).
    std::vector<ScoreCandidate> c = {
        make_candidate(1, /*is_anchor=*/true),
        make_candidate(2),
        make_candidate(3),
    };
    for (auto& cand : c) cand.load_allowance = 2.0;
    c[0].affinity = 1.0;
    c[0].load = 9.0;
    c[1].load = 5.0;
    c[2].load = 4.0;

    auto d = score_and_select(c.data(), c.size(), default_weights(), true);
    EXPECT_EQ(c[d.dispatch].id, 3);
}

TEST(DispatchLoad, SaturatedTieGoesToLowerIdNotAnchor) {
    // When the anchor forfeited its allowance it also loses tie preference:
    // the former least-loaded scan picked the first (lowest id) among ties.
    std::vector<ScoreCandidate> c = {
        make_candidate(2),
        make_candidate(5, /*is_anchor=*/true),
    };
    for (auto& cand : c) {
        cand.load = 6.0;
        cand.load_allowance = 2.0;
    }
    c[1].affinity = 1.0;

    auto d = score_and_select(c.data(), c.size(), default_weights(), true);
    EXPECT_EQ(c[d.dispatch].id, 2);
}

TEST(DispatchLoad, DisabledLoadTermNeverDiverts) {
    std::vector<ScoreCandidate> c = {
        make_candidate(1, /*is_anchor=*/true),
        make_candidate(2),
    };
    c[0].affinity = 1.0;
    c[0].load = 1000.0;
    c[0].load_allowance = 1.0;
    c[1].load = 0.0;
    c[1].load_allowance = 1.0;

    auto d = score_and_select(c.data(), c.size(), default_weights(),
                              /*load_term_enabled=*/false);
    EXPECT_EQ(d.dispatch, 0u);
    EXPECT_FALSE(d.load_diverted);
}

TEST(DispatchLoad, PrefixWeightAddsStickinessBeyondAllowance) {
    // Weight monotonicity: prefix_weight buys the cache-warm anchor extra
    // over-allowance headroom in load units.
    std::vector<ScoreCandidate> c = {
        make_candidate(1, /*is_anchor=*/true),
        make_candidate(2),
    };
    c[0].affinity = 1.0;
    c[0].load = 7.0;
    c[0].load_allowance = 4.0;  // hinge 3.0
    c[1].load = 0.0;
    c[1].load_allowance = 4.0;

    ScoringWeights w = default_weights();
    auto d = score_and_select(c.data(), c.size(), w, true);
    EXPECT_EQ(c[d.dispatch].id, 2) << "neutral weights divert";

    w.prefix_weight = 4.0;  // bonus 4.0 > hinge 3.0: route survives
    d = score_and_select(c.data(), c.size(), w, true);
    EXPECT_EQ(c[d.dispatch].id, 1);

    w.prefix_weight = 2.5;  // bonus < hinge: divert again
    d = score_and_select(c.data(), c.size(), w, true);
    EXPECT_EQ(c[d.dispatch].id, 2);
}

TEST(DispatchLoad, ZeroLoadWeightDisablesDiverts) {
    std::vector<ScoreCandidate> c = {
        make_candidate(1, /*is_anchor=*/true),
        make_candidate(2),
    };
    c[0].affinity = 1.0;
    c[0].load = 100.0;
    c[0].load_allowance = 1.0;
    c[1].load = 0.0;
    c[1].load_allowance = 1.0;

    ScoringWeights w = default_weights();
    w.load_weight = 0.0;
    auto d = score_and_select(c.data(), c.size(), w, true);
    EXPECT_EQ(c[d.dispatch].id, 1);
}

// =============================================================================
// Dispatch: cost-budget term
// =============================================================================

TEST(DispatchCost, OverBudgetAnchorDivertsToZeroHinge) {
    std::vector<ScoreCandidate> c = {
        make_candidate(1, /*is_anchor=*/true),
        make_candidate(2),
    };
    c[0].affinity = 1.0;
    c[0].cost_hinge = 0.4;
    c[0].cost_pressure = 1.2;
    c[1].cost_hinge = 0.0;
    c[1].cost_pressure = 0.3;

    auto d = score_and_select(c.data(), c.size(), default_weights(), false);
    EXPECT_EQ(c[d.dispatch].id, 2);
    EXPECT_TRUE(d.cost_diverted);
    EXPECT_FALSE(d.load_diverted);
    EXPECT_EQ(c[d.placement].id, 1);  // cost diverts are transient
}

TEST(DispatchCost, ZeroHingeTieBreaksTowardLowerPressure) {
    // Fast-lane target rule: among candidates inside the imbalance band,
    // prefer the least cost pressure.
    std::vector<ScoreCandidate> c = {
        make_candidate(1, /*is_anchor=*/true),
        make_candidate(2),
        make_candidate(3),
    };
    c[0].affinity = 1.0;
    c[0].cost_hinge = 0.5;
    c[0].cost_pressure = 0.9;
    c[1].cost_pressure = 0.4;
    c[2].cost_pressure = 0.1;

    auto d = score_and_select(c.data(), c.size(), default_weights(), false);
    EXPECT_EQ(c[d.dispatch].id, 3);
    EXPECT_TRUE(d.cost_diverted);
}

TEST(DispatchCost, BothHingesSetBothFlags) {
    std::vector<ScoreCandidate> c = {
        make_candidate(1, /*is_anchor=*/true),
        make_candidate(2),
    };
    c[0].affinity = 1.0;
    c[0].load = 6.0;
    c[0].load_allowance = 2.0;
    c[0].cost_hinge = 0.3;
    c[1].load = 0.0;
    c[1].load_allowance = 2.0;

    auto d = score_and_select(c.data(), c.size(), default_weights(), true);
    EXPECT_EQ(c[d.dispatch].id, 2);
    EXPECT_TRUE(d.load_diverted);
    EXPECT_TRUE(d.cost_diverted);
}

TEST(DispatchCost, ZeroCostWeightDisablesDiverts) {
    std::vector<ScoreCandidate> c = {
        make_candidate(1, /*is_anchor=*/true),
        make_candidate(2),
    };
    c[0].affinity = 1.0;
    c[0].cost_hinge = 5.0;
    c[0].cost_pressure = 6.0;

    ScoringWeights w = default_weights();
    w.cost_weight = 0.0;
    auto d = score_and_select(c.data(), c.size(), w, false);
    EXPECT_EQ(c[d.dispatch].id, 1);
    // The hinge is still reported for observability even though the weight
    // silenced it in the score: no divert happened, so no flag either.
    EXPECT_FALSE(d.cost_diverted);
}

// =============================================================================
// Residency discount on the affinity term
// =============================================================================

TEST(Residency, DiscountShrinksTheStickinessBudget) {
    // affinity 0.5 (residency-discounted) halves the prefix bonus: a hinge
    // the full bonus would absorb now diverts.
    std::vector<ScoreCandidate> c = {
        make_candidate(1, /*is_anchor=*/true),
        make_candidate(2),
    };
    c[0].load = 7.0;
    c[0].load_allowance = 4.0;  // hinge 3.0
    c[1].load = 0.0;
    c[1].load_allowance = 4.0;

    ScoringWeights w = default_weights();
    w.prefix_weight = 4.0;

    c[0].affinity = 1.0;  // bonus 4.0 > 3.0: keep
    auto d = score_and_select(c.data(), c.size(), w, true);
    EXPECT_EQ(c[d.dispatch].id, 1);

    c[0].affinity = 0.5;  // bonus 2.0 < 3.0: divert
    d = score_and_select(c.data(), c.size(), w, true);
    EXPECT_EQ(c[d.dispatch].id, 2);
}

// =============================================================================
// Reserved SLO seam + degenerate inputs
// =============================================================================

TEST(Scorer, SloWeightIsInert) {
    std::vector<ScoreCandidate> c = {
        make_candidate(1, /*is_anchor=*/true),
        make_candidate(2),
    };
    c[0].affinity = 1.0;
    c[0].load = 9.0;
    c[0].load_allowance = 2.0;
    c[1].load = 0.0;
    c[1].load_allowance = 2.0;

    ScoringWeights w = default_weights();
    auto base = score_and_select(c.data(), c.size(), w, true);
    w.slo_weight = 123.0;
    auto with_slo = score_and_select(c.data(), c.size(), w, true);
    EXPECT_EQ(base.dispatch, with_slo.dispatch);
    EXPECT_EQ(base.placement, with_slo.placement);
}

TEST(Scorer, EmptyAndSingleCandidate) {
    auto d = score_and_select(nullptr, 0, default_weights(), true);
    EXPECT_EQ(d.placement, 0u);
    EXPECT_EQ(d.dispatch, 0u);

    auto single = make_candidate(9, /*is_anchor=*/true);
    single.load = 100.0;
    single.load_allowance = 1.0;
    single.cost_hinge = 5.0;
    d = score_and_select(&single, 1, default_weights(), true);
    EXPECT_EQ(d.placement, 0u);
    EXPECT_EQ(d.dispatch, 0u);
    // Nowhere to divert to: flags stay clear.
    EXPECT_FALSE(d.load_diverted);
    EXPECT_FALSE(d.cost_diverted);
}

TEST(Scorer, UnfilledProbeRanksAreInert) {
    // probe_rank defaults to UINT32_MAX everywhere; ties must fall through to
    // the load key.
    std::vector<ScoreCandidate> c = {
        make_candidate(1, /*is_anchor=*/true),
        make_candidate(2),
        make_candidate(3),
    };
    for (auto& cand : c) cand.load_allowance = 4.0;
    c[0].affinity = 1.0;
    c[0].load = 9.0;
    c[1].load = 2.0;
    c[2].load = 1.0;
    EXPECT_EQ(c[1].probe_rank, kNoProbe);

    auto d = score_and_select(c.data(), c.size(), default_weights(), true);
    EXPECT_EQ(c[d.dispatch].id, 3);
}
