// Ranvier Core - Unified Route Scorer (pure decision core)
//
// Replaces the sequential override chain (residency downgrade -> load redirect
// -> cost redirect -> hardware-cost preference) with one weighted ranking over
// the live candidates. The hash strategies still compute the *anchor* (the
// deterministic ART/consistent-hash placement); this scorer decides everything
// after the anchor in a single pass.
//
// Two argmaxes over the same candidates, one pass:
//
//   placement score (stable inputs: prefix affinity, hardware price)
//     -> original_selected: what route learning pins. A hardware-price divert
//        IS the miss-path placement — the learned route follows it, so the
//        prefix pins to the backend that actually computes it.
//
//   dispatch score = placement score + transient terms (load, cost budget)
//     -> final_backend: where THIS request goes. Load/cost diverts are
//        transient and never learned.
//
// Default-weight parity contract: with the shipped ScoringWeights defaults,
// every term reduces to the pre-scorer rule, and the existing config keys
// (bounded_load_epsilon, p2c_load_bias, load_imbalance_factor/floor,
// cache_residency_threshold, cost_routing.*, cost_per_hour) keep their
// authority via the allowances/hinges the router computes. The weights shift
// authority from those per-step constants to a blended trade-off only when
// tuned away from defaults.
//
// This header is PURE: no Seastar includes, no shard state — directly
// unit-testable without a reactor (same pattern as rate_limiter_core.hpp and
// proxy_retry_policy.hpp).

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace ranvier {

// Per-signal weights for the unified route score. Exposed via
// RoutingConfig::scoring (YAML routing.scoring.*, env RANVIER_SCORING_*).
struct ScoringWeights {
    // Extra affinity stickiness in load units, added ON TOP of the active hash
    // strategy's own allowance (the hinge pivot below). 0.0 = stickiness is
    // governed entirely by the existing strategy parameters — exact pre-scorer
    // behavior. >0 lets a cache-warm backend carry that much additional
    // over-allowance load before the route is abandoned.
    double prefix_weight = 0.0;

    // Scales the over-allowance load penalty. 1.0 = penalties are in raw load
    // units (one unit of over-allowance load cancels one unit of any other
    // term). 0.0 disables load-driven dispatch diverts entirely (the
    // load_aware_routing toggle also gates the term).
    double load_weight = 1.0;

    // Discounts the affinity bonus by reported cache-cooling:
    //   affinity *= 1 - residency_weight * (1 - residency)
    // 0.0 = residency acts only through the existing hard gate at
    // cache_residency_threshold (exact pre-scorer behavior). Only meaningful
    // together with prefix_weight > 0 until per-backend block residency
    // (BACKLOG P0.1) grades affinity directly.
    double residency_weight = 0.0;

    // Scales the cost-budget hinge (load units per unit of budget-pressure
    // overflow). Inert unless cost_routing.enabled and the request carries an
    // estimated cost.
    double cost_weight = 1.0;

    // Hardware-price preference strength (load units per unit of normalized
    // price advantage). Any value > 0 preserves the cache-miss "prefer the
    // cheapest eligible priced backend" placement; 0.0 disables the price
    // divert entirely (operator escape hatch).
    double price_weight = 1.0;

    // Seam for the future latency/SLO term (BACKLOG §20.1): parsed and carried
    // but not read by any term yet.
    double slo_weight = 0.0;
};

// One routing candidate's signals, gathered shard-locally by the router.
// All fields are plain values so tests can construct candidates directly.
struct ScoreCandidate {
    int32_t id = 0;

    // True for the ART/hash anchor produced by Steps 1-2. Placement prefers
    // the anchor on ties so an un-tuned scorer reproduces the anchor exactly.
    bool is_anchor = false;

    // Prefix-affinity value in [0, 1]. Today: 1.0 for an ART-hit anchor
    // (after the residency gate/discount), 0.0 otherwise — misses have no
    // cache-warm candidate. Seam: P0.1 grades this per backend by block
    // overlap; P0.3 adds pool-role gating.
    double affinity = 0.0;

    // Dispatch load and the strategy allowance it is hinged against. The
    // router fills the strategy-appropriate flavor (capacity-adjusted for
    // BOUNDED_LOAD/P2C, composite for JUMP/MODULAR) and the matching pivot:
    //   BOUNDED_LOAD: cap - 1            (cap = max(1, ceil(avg*(1+eps)));
    //                                     -1 because the pre-scorer trigger
    //                                     diverts at load >= cap, and loads
    //                                     are integral)
    //   P2C:          least_load + bias
    //   JUMP/MODULAR: median*factor + floor
    // Also the tie-break for "divert to least loaded".
    double load = 0.0;
    double load_allowance = 0.0;

    // Cost-budget hinge (>= 0) and raw budget pressure, both precomputed by
    // the router (they need config + fleet aggregates). hinge mirrors the
    // pre-scorer Step 4 triggers; pressure is the "prefer least-cost" tie-break.
    double cost_hinge = 0.0;
    double cost_pressure = 0.0;

    // Hardware-price advantage relative to the anchor, normalized by the
    // anchor's price: (anchor_price - price) / anchor_price. Strictly > 0 ONLY
    // for candidates eligible under the pre-scorer contract (miss path, both
    // priced, strictly cheaper, not overloaded); 0 for the anchor, unpriced
    // and ineligible candidates. Encoding eligibility as "advantage > 0"
    // preserves: unpriced backends are never preferred and never diverted
    // from, and equal price keeps the deterministic hash spread.
    double price_advantage = 0.0;

    // Composite load used to break hardware-price ties (the pre-scorer
    // preference broke equal-price ties toward lower composite load).
    double price_tiebreak_load = 0.0;

    // BOUNDED_LOAD divert spillover order: rank of this candidate's first
    // appearance in the jump-hash probe sequence, filled by the router ONLY
    // for under-allowance candidates when the anchor forfeits under
    // BOUNDED_LOAD. UINT32_MAX = not probed / not applicable. Preserves the
    // strategy's deterministic, cluster-consistent spillover target.
    uint32_t probe_rank = std::numeric_limits<uint32_t>::max();
};

// Outcome of one scoring pass. Indices refer to the caller's candidate array.
struct ScoreDecision {
    size_t placement = 0;       // stable choice: learn target / original_selected
    size_t dispatch = 0;        // transient choice: this request's backend
    bool load_diverted = false; // dispatch != placement and placement was over
                                // its load allowance (Step-3-equivalent divert)
    bool cost_diverted = false; // dispatch != placement and placement carried a
                                // cost-budget hinge (Step-4-equivalent divert)
};

// Over-allowance load penalty. Zero at or below the allowance: a candidate
// inside its strategy's allowance is never penalized, which is what makes the
// anchor sticky at default weights.
inline double load_hinge(double load, double allowance) {
    return load > allowance ? load - allowance : 0.0;
}

// Stable placement sub-score: prefix affinity + hardware-price advantage.
inline double placement_score(const ScoreCandidate& c, const ScoringWeights& w) {
    return w.prefix_weight * c.affinity + w.price_weight * c.price_advantage;
}

// Full dispatch score: placement + transient penalties. load_term_enabled
// reflects whether the pre-scorer pipeline would have run a post-anchor load
// check at all (load_aware_routing on, AND either an ART anchor or a
// load-blind JUMP/MODULAR hash anchor) — when the strategy already
// load-vetted the anchor, the load signal is spent and must not apply twice.
inline double dispatch_score(const ScoreCandidate& c, const ScoringWeights& w,
                             bool load_term_enabled) {
    double s = placement_score(c, w);
    if (load_term_enabled) {
        s -= w.load_weight * load_hinge(c.load, c.load_allowance);
    }
    s -= w.cost_weight * c.cost_hinge;
    return s;
}

// Rank candidates and pick placement + dispatch winners.
//
// Placement tie order:  score desc, anchor first, lower price_tiebreak_load,
//                       lower id.
// Dispatch tie order:   score desc, "clean placement winner" first (the
//                       placement winner keeps its seat whenever it forfeited
//                       nothing — zero load hinge and zero cost hinge), lower
//                       probe_rank, lower cost_pressure, lower load, lower id.
//
// The dispatch tie keys encode the pre-scorer divert targets: probe order for
// BOUNDED_LOAD spillover, least-loaded for P2C/JUMP/MODULAR, least-cost for
// the small-request fast lane. Deterministic by construction (id last).
inline ScoreDecision score_and_select(const ScoreCandidate* candidates, size_t count,
                                      const ScoringWeights& weights,
                                      bool load_term_enabled) {
    ScoreDecision decision{};
    if (count == 0) {
        return decision;
    }

    // Placement argmax.
    size_t best = 0;
    double best_score = placement_score(candidates[0], weights);
    for (size_t i = 1; i < count; ++i) {
        double s = placement_score(candidates[i], weights);
        const ScoreCandidate& c = candidates[i];
        const ScoreCandidate& b = candidates[best];
        bool wins = false;
        if (s > best_score) {
            wins = true;
        } else if (s == best_score) {
            if (c.is_anchor != b.is_anchor) {
                wins = c.is_anchor;
            } else if (c.price_tiebreak_load != b.price_tiebreak_load) {
                wins = c.price_tiebreak_load < b.price_tiebreak_load;
            } else {
                wins = c.id < b.id;
            }
        }
        if (wins) {
            best = i;
            best_score = s;
        }
    }
    decision.placement = best;

    // Dispatch argmax. The placement winner is preferred on ties only while
    // "clean" (no forfeited term) — a placement winner over its allowance or
    // over budget competes on score alone, exactly like the pre-scorer
    // override triggers. Penalties are WEIGHTED: a hinge silenced by a zero
    // weight neither dirties the winner nor counts as a divert cause.
    const ScoreCandidate& p = candidates[decision.placement];
    const double p_load_penalty =
        load_term_enabled ? weights.load_weight * load_hinge(p.load, p.load_allowance)
                          : 0.0;
    const double p_cost_penalty = weights.cost_weight * p.cost_hinge;
    const bool placement_clean = (p_load_penalty == 0.0) && (p_cost_penalty == 0.0);

    best = 0;
    best_score = dispatch_score(candidates[0], weights, load_term_enabled);
    for (size_t i = 1; i < count; ++i) {
        double s = dispatch_score(candidates[i], weights, load_term_enabled);
        const ScoreCandidate& c = candidates[i];
        const ScoreCandidate& b = candidates[best];
        bool wins = false;
        if (s > best_score) {
            wins = true;
        } else if (s == best_score) {
            const bool c_pref = placement_clean && (i == decision.placement);
            const bool b_pref = placement_clean && (best == decision.placement);
            if (c_pref != b_pref) {
                wins = c_pref;
            } else if (c.probe_rank != b.probe_rank) {
                wins = c.probe_rank < b.probe_rank;
            } else if (c.cost_pressure != b.cost_pressure) {
                wins = c.cost_pressure < b.cost_pressure;
            } else if (c.load != b.load) {
                wins = c.load < b.load;
            } else {
                wins = c.id < b.id;
            }
        }
        if (wins) {
            best = i;
            best_score = s;
        }
    }
    decision.dispatch = best;

    if (decision.dispatch != decision.placement) {
        decision.load_diverted = p_load_penalty > 0.0;
        decision.cost_diverted = p_cost_penalty > 0.0;
    }
    return decision;
}

}  // namespace ranvier
