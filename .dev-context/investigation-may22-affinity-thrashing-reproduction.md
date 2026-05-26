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

## Addendum (2026-05-25): `#527 f1c70ee` cache-residency routing changes the picture

After this investigation was written, commit `f1c70ee` (#527, "Route by where a
prefix still resides, not just where it was last served") merged to main. It is
directly relevant and **must be accounted for in any benchmark run on `f1c70ee`+,
including the May 25 Exp A attempt** (which ran on `f1c70ee` with residency
routing active at its default).

What it does (`src/router_service.cpp` ~2455-2520, `docs/internals/cache-residency-routing.md`):
on an ART hit, if the owning backend's gossiped KV-cache residency is below
`cache_residency_threshold` (default `0.2` → cache >80% full), the hit is
**downgraded to a miss** and re-routed through the hash strategy with the
cache-cold backend excluded. New env var `RANVIER_CACHE_RESIDENCY_THRESHOLD`
(0.0 disables). New counter `ranvier_router_residency_route_downgrades_total`.

Two consequences for this investigation:

1. **It is a second, independent diversion mechanism.** The #442 analysis above
   assumed load-aware (Step 3) was the only thing breaking ART affinity. On
   `f1c70ee`+, residency downgrades (Step 1→2) break it too, *even with
   `--no-load-aware`*. At 30u/13B cache pressure is high, so residency
   downgrades may fire heavily. The miss-tail regression on `f1c70ee` could be
   driven by residency routing, load-aware, or both — they must be separated
   (see Experiment A vs A2 in the checklist).

2. **It partially mitigates the #442 "no escape valve" problem — but only on the
   residency path.** A residency downgrade sets `art_hit=false` and re-runs the
   hash strategy, so `original_selected` becomes the *new* backend and the ART
   re-learns `P → new` (self-healing). The load-aware divert (Step 3) still does
   NOT do this — `art_hit` stays true and `original_selected` is the pre-divert
   ART target, so #442's persistent misalignment is unchanged on the load-aware
   path. So #527 is not a fix for the load-aware pathology; it's a parallel
   mechanism with better learning hygiene.

**Benchmark comparability caveat:** the May 22 `5079f9f` data predates #527, so
comparing a `f1c70ee`+ prefix-aware run against the May 22 numbers mixes two
routing algorithms. To reproduce the `5079f9f` behavior on `f1c70ee`, set
`RANVIER_CACHE_RESIDENCY_THRESHOLD=0.0`.

The parser now scrapes `residency_route_downgrades_total` alongside
`load_aware_fallbacks_total`, so both diversion sources are visible in the
comparison output.

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
4. **(New, post-#527) Which diversion mechanism dominates at 30u — load-aware
   or residency downgrades?** Experiment A disables both; A2 isolates residency.
   Compare `load_aware_fallbacks_total` vs `residency_route_downgrades_total` in
   the parser's ROUTING COUNTERS block.

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

## Empirical results — Experiments A & B (2026-05-25, `f2066e6`, 13B 30u 30m)

Ran the smoking-gun A/B on a fresh 8×A100 instance. Both prefix-aware legs
paired with a same-instance round-robin baseline. Prometheus counters captured
(scrape fixed: Seastar exposes metrics as `seastar_ranvier_*`). Workload:
stress dist, prefix-ratio 0.9, **5 large prefixes across 8 backends**.

| Run | Diversion config | P99 TTFT vs RR | Cache Miss P99 | Cache hit | Incompletes | load_aware_fallbacks | residency_downgrades | backend Gini |
|-----|------------------|---------------:|---------------:|----------:|------------:|---------------------:|---------------------:|-------------:|
| **A** | load-aware OFF + residency OFF (0.0) | **-76%** | **-76%** | 89.3% | **9.4%** (1528) | 0 | 0 | **0.534** |
| **B** | load-aware ON (f=3.0, fl=4) + residency default (0.2) | **+167%** | **+219%** | 59.5% | 0 | **1535 (10.7%)** | **0** | 0.248 |

### Finding 1 — Residency routing (#527) is INERT in this environment

`residency_route_downgrades_total = 0` in **both** runs, including B where the
threshold was at its default 0.2. The pre-run worry that #527 was a confound is
**refuted**. Likely cause: the gossiped residency signal never dropped below
0.2 (cache pressure never crossed ~80% as measured) or the CACHE_STATE signal
isn't populated in this benchmark topology, so `get_cached_residency()` returns
the no-signal sentinel and the downgrade never fires. Either way, residency
routing did not influence these results and can be set aside for the 30u
miss-tail analysis.

### Finding 2 — Pure affinity over-concentrates (the 9.4% timeouts are real)

Exp A's per-backend distribution (cumulative `backend_latency_seconds_count`):
`[b1=1697, b2=303, b3=298, b4=11, b5=290, b6=6, b7=472, b8=1123]`, **Gini
0.534**. Two backends (b1, b8) absorb the load; two (b4, b6) are essentially
idle. With 5 hot prefixes pinned by consistent hash across 8 backends, pure
affinity *cannot* use more than 5 backends (pigeonhole) — the 2 hottest
overload at 30u and shed 9.4% of requests past the 300 s stream timeout. The
completed requests are fast cache hits (P99 -76%, hit rate 89.3%), so the
aggregate P99 looks like a win while ~1 in 11 requests silently fails. This is
the metric-blindness the RUN STATUS banner now catches.

### Finding 3 — Load-aware diversions are the miss-tail driver, and they are cold (confirms #442)

Exp B fired load-aware on **10.7%** of requests (1535 fallbacks). That
diversion *worked* as load balancing — Gini fell to 0.248, no idle backends,
**0 timeouts**. But it was catastrophic for latency: **P99 +167%, Cache Miss
P99 +219%, XLarge Miss P99 +213%**, and cache hit fell from A's 89.3% to 59.5%.

This is the #442 mechanism, now observed rather than predicted: each diverted
request is served by a backend that is cold for that prefix, and because the
ART keeps learning the *original* (overloaded) consistent-hash target, the
diversion target never warms up — every divert is a fresh cold miss. A single
hot prefix gets sprayed across the rotating set of least-loaded backends, none
of which retains it, which is why a 10.7% diversion rate drags the cache hit
rate down ~30 pp and blows out the miss tail far out of proportion to the
diversion count.

### Conclusion

At 30u with this workload, **both threshold extremes lose to round-robin**:
- No diversion → 9.4% timeouts (over-concentration).
- Diversion (even modest, 10.7%) → +167% P99 (cold-miss tail, no escape valve).

Threshold tuning cannot square this circle: the diverted requests are
*structurally* cold because of #442's learn-the-original behavior. The data now
**empirically supports the #442 proximate-cause ranking** from the static audit
above. Residency routing and #441 are both ruled out as contributors.

**The fix is structural, not a config change — flag for human decision per the
task brief.** The candidate that matches the data is #289 Fix 2 / may22 fix
options 2–3: when load-aware diverts a prefix to backend B persistently, learn
`P → B` (hysteresis / sticky diversion) so B warms up and subsequent requests
are real hits. This requires touching the learn-target logic
(`http_controller.cpp:1898`) and/or `apply_load_aware_selection`
(`router_service.cpp`), so it is out of scope for this benchmark session.

### Caveat / open question

The 5-prefix workload forces extreme concentration (only 5 of 8 backends usable
under pure affinity). A higher prefix count (e.g. `NUM_LARGE_PREFIXES=50`) would
relax the pigeonhole and may let pure affinity avoid timeouts without diversion
— worth one run to confirm the timeouts are workload-concentration-driven and
not a separate routing defect. Experiment C (20u repeats) is also still open:
at 20u the lower load may let pure affinity complete without timeouts, which
would localize the timeout failure to the 30u over-concentration regime.
