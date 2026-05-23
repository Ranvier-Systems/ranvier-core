# Investigation: May 22, 2026 — 13B Affinity-Thrashing Reproduction

**Date:** 2026-05-23
**Investigator:** Claude (static code audit; benchmark data supplied by developer)
**Commit analyzed:** `5079f9f`
**Follow-up to:** [`investigation-289-routing-regression.md`](investigation-289-routing-regression.md)

This is **not** a replacement for investigation #289 — it is a confirmation that
the failure mode #289 predicted is still present on `5079f9f`, plus an audit of
the two routing PRs (`#441 fb5ad97`, `#442 adaec51`) that landed in the
Instance 9 window between #289 and the May 22 reproduction.

## Headline (May 22, 2026, `5079f9f`, 13B, 30m duration)

| Users | Req/GPU | Cache hit | P99 TTFT vs RR | Cache Miss P99 | Incompletes | #289 prediction |
|------:|--------:|----------:|---------------:|---------------:|------------:|-----------------|
| 10    | 1.25    | 58.3%     | **-22%**       | (healthy)      | 0           | Quiet → healthy ✓ |
| 20    | 2.50    | (n/a)     | **-57%** *      | (excluded)     | **639 (6.0%)** | Flapping → diverted req timeouts ✓ |
| 30    | 3.75    | 61.5%     | **+44%**       | **+69%**       | 0           | Load-aware fires every request → cold-cache divert ✓ |

\* The 20u headline excludes the 639 timed-out requests from P99 computation; the
"green" -57% is comparing a 94%-complete prefix-aware run to a 100%-complete RR
run, i.e. comparing different populations. This is the exact metric blindness
the parser updates in Task 2 of this branch surface.

## Audit of `#441` and `#442` against investigation #289

Investigation #289's primary hypothesis (lines 196-227) was that at 2.5 req/GPU
the load-aware logic oscillates around `queue_depth_threshold`, breaking prefix
affinity without reducing tail latency. Both PRs in the Instance 9 window
touched exactly the code the investigation fingered.

### #441 ("honor `load_aware_routing` uniformly across hash strategies") — fb5ad97

Visible in the current tree at:

- `src/router_service.cpp:1194-1201` (BOUNDED_LOAD): explicit "Honor the
  load_aware_routing toggle uniformly across strategies" comment + early return
  to pure jump-hash when the toggle is false.
- `src/router_service.cpp:1279-1283` (P2C): identical pattern.
- `src/router_service.cpp:2428-2498` (post-selection step 3 dispatch): all three
  hash strategies fall through `apply_load_aware_selection` / `bounded_load_select` /
  `p2c_select` under a single `state.config.load_aware_routing` gate.
- `src/router_service.cpp:1349-1371` (startup log): operator-visible
  acknowledgement that the toggle now has uniform semantics.

**Impact relative to investigation #289:** Before #441, JUMP and MODULAR were
the only strategies that consulted `apply_load_aware_selection`. BOUNDED_LOAD
and P2C had their own built-in load awareness that ran *always*, regardless of
the toggle. #441 made the toggle uniform — but **the default
`load_aware_routing=true`** means production is on the same code path as
before. The change widens the operator escape hatch; it does not by itself
explain a new regression vs `08e5a93`. **Unlikely to be the proximate cause.**

The one second-order risk is `apply_load_aware_selection` itself still has no
hysteresis (`router_service.cpp:1036-1105`) — it makes per-request decisions
purely on the current median load, exactly the implementation #289 Fix 2
(lines 282-296) called out as needing a "sticky diversion" mechanism. If
anything #441 made this more discoverable, but it did not introduce the
behavior.

### #442 ("learn the consistent-hash backend, not the diverted one") — adaec51

Visible in the current tree at:

- `src/http_controller.cpp:1898`:
  `learn_target = route_result.was_fast_lane ? 0 : route_result.original_selected;`
- `src/http_controller.cpp:1072-1078` (the learning gate uses `learn_target_backend`,
  populated from `original_selected`, not from the final post-divert backend).
- `src/router_service.hpp` (`PrefixRouteResult::original_selected`) +
  `src/router_service.cpp:2604-2605` (the field is populated with the ART/hash
  choice from step 2, **before** step 3's load-aware override).

**Impact relative to investigation #289:** This is the structural change that
turns #289's predicted flapping into a **persistent** miss-tail regression at
30u rather than a transient at 20u. Mechanism:

1. Cold ART → first request for prefix P consistent-hashes to backend A.
2. Backend A is over `median*factor + floor` at decision time → step 3 diverts
   to backend B. Request is served by B, but step B's KV cache is cold for P.
3. ART learns `P → A` (because `learn_target = original_selected`).
4. Next request for prefix P → ART hit, `selected = A`. But A's load hasn't
   dropped (at 3.75 req/GPU, everyone is near threshold), so step 3 diverts
   to B again.
5. Backend A *never* gets a real request for P → its KV cache for P never
   warms up. Backend B serves P but is permanently classified as "diverted
   target" — the ART still says A, so on every request we pay the
   `apply_load_aware_selection` cost AND the cold-cache cost on B.
6. There is **no escape valve**: the ART's "preferred" backend is the one that
   load-aware keeps redirecting away from. The prefix-affinity intent is
   inverted into a guaranteed cache miss.

This matches the 30u data exactly: cache hit rate stays at 61.5% (the ART is
working — it's reporting hits when ART matches), but Cache Miss P99 is +69%
and Large/XLarge Miss P99 are +123% / +62% because every divert serves a
prefix that the diversion target has never built up cache state for.

Before #442, the ART learned the **served** backend, so under sustained
diversion the ART would converge to "P → B" and the warm path on B would
re-establish prefix affinity. That self-healing is exactly what #442 removed,
on the theory that the consistent-hash assignment is the canonical home and
diversions are transient. At 1.25 req/GPU the assumption holds (no diverts;
ART converges). At 3.75 req/GPU diverts are persistent, the assumption breaks,
and the ART becomes counter-productive.

### Ranking

| PR | Proximate cause of 5079f9f 30u regression? | Confidence |
|----|--------------------------------------------|------------|
| `#442 adaec51` (learn consistent-hash backend) | **Yes — most likely** | Medium-high (mechanism matches signature) |
| `#441 fb5ad97` (uniform load-aware honoring)  | No (default behavior unchanged) | High |
| Investigation #289 root cause (load-aware threshold flapping with no hysteresis) | Yes — necessary precondition | High (re-confirmed by 20u timeouts) |

Both #289's predicted flapping *and* #442's persistent ART-divert misalignment
need to be true for the observed pattern. #441 is a red herring on the failure
mode but a real win on operator clarity.

## Why this is NOT in the "revert #442" bucket yet

The intent behind #442 is correct under low-to-moderate concurrency, and the
10u/30m result is *better* than baseline (P99 -22%, cache hit 58.3%). Reverting
#442 outright would regress that win and re-introduce ART-pollution on
transient diversions.

The fix path that matches the data is one of:

1. **Raise the load-aware threshold so #442's assumption holds.** Today
   `load_imbalance_factor=2.0`, `load_imbalance_floor=2`. At 30u, "median*2+2"
   is just barely above sustained operating load, so diversions are
   continuous. Pushing factor to ~3.0 or floor to ~4 would make diversions
   rare events again, restoring the #442 invariant.
2. **Add hysteresis to `apply_load_aware_selection` (#289 Fix 2).** Once a
   prefix is diverted, stay diverted for N requests so backend B warms up;
   then either commit (learn `P → B` post-warm-up) or revert.
3. **Make ART-learning load-aware.** If the consistent-hash backend is
   sustainedly above threshold, fall back to learning the served backend (the
   pre-#442 behavior) for that prefix only.

Picking among these requires the smoking-gun A/B in
[`next-benchmark-checklist.md`](next-benchmark-checklist.md).

**Per the task brief, this investigation does NOT write a revert patch.**

## Open questions for the next session

1. **Is `load_aware_fallbacks_total` at 30u high relative to total_requests?**
   This is the single counter that closes the case. If `fallbacks/total > 0.5`
   under steady state, the diversion-every-request mechanism is confirmed. The
   parser updates in this branch make this visible; bench.sh now scrapes the
   `/metrics` endpoint at end-of-run.
2. **Per-backend request distribution at 30u — is one backend cold?** If #442
   is the cause, one ART-target backend will show very low active_requests
   while the diversion target absorbs the actual traffic. The parser's new
   per-backend distribution block surfaces this.
3. **Does raising `load_imbalance_factor` to 3.0 close the gap without
   touching code?** This is Experiment B in the next-benchmark checklist.

## Files examined

| File | Lines | Purpose |
|------|-------|---------|
| `src/router_service.cpp` | 1036-1105 | `apply_load_aware_selection`, no-hysteresis confirmation |
| `src/router_service.cpp` | 1194-1250 | `bounded_load_select`, `#441` toggle gate |
| `src/router_service.cpp` | 1267-1317 | `p2c_select`, `#441` toggle gate |
| `src/router_service.cpp` | 2271-2606 | `get_backend_for_prefix`, step 1-4 unified path |
| `src/router_service.cpp` | 2604-2605 | `PrefixRouteResult.original_selected` populated pre-divert |
| `src/http_controller.cpp` | 1072-1110 | learning gate consumes `learn_target_backend` |
| `src/http_controller.cpp` | 1898 | `#442` assignment: `learn_target = original_selected` |
| `src/config_schema.hpp` | 85-124 | thresholds (`load_imbalance_factor`, `load_imbalance_floor`, `bounded_load_epsilon`, `p2c_load_bias`) |
| `src/config_loader.cpp` | 166-173 | env-var bindings for the thresholds above |
| `src/metrics_service.hpp` | 123-127, 999-1002 | Prometheus counters scraped by results_parser.py |

## Note on threshold naming

Investigation #289's recommended fixes used the parameter names
`queue_depth_threshold` and `queue_diff_threshold` (lines 270-276). Those
names **do not exist** in the current `src/config_schema.hpp` or
`src/config_loader.cpp`. The current equivalents are:

| Investigation #289 name | Current equivalent | Default | Env var |
|-------------------------|--------------------|---------|---------|
| `queue_depth_threshold = 4` | `load_imbalance_floor = 2` (additive floor) | 2 | `RANVIER_LOAD_IMBALANCE_FLOOR` |
| `queue_diff_threshold = 2`  | `load_imbalance_factor = 2.0` (multiplicative) | 2.0 | `RANVIER_LOAD_IMBALANCE_FACTOR` |

The current implementation uses `threshold = median * factor + floor`
(router_service.cpp:1080), a relative comparison vs the cluster median. The
investigation's absolute-threshold framing maps onto the floor parameter only;
the factor is conceptually different. The next-benchmark checklist uses the
current parameter names; the spirit of #289 Fix 1 ("raise the threshold")
translates to raising one or both of these.
