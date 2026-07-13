# Design Proposal: Should Ranvier Gate Prefix-Aware Routing on Load?

**Status:** Investigation + design (no implementation). Static-analysis-only repo.
**Motivating data:** 2026-07 50-prefix re-baseline, commit `817a1b5`, 8×A100, median-of-3
A/B (prefix vs round-robin). See `docs/benchmarks/benchmark-results-current.md`.
**Author context:** requested via BACKLOG §25 follow-up (the 30–47% fallback finding).

---

## TL;DR — recommendation

**Gate only if cheap — and almost certainly you won't need a gate.** The +29% low-load P99
regression is real, but (1) it is *not* caused by Ranvier's per-request overhead, (2) it is
*not* caused by sustained backend concentration (Gini rules that out), and (3) it is best
explained by **transient cross-node hot-spotting** that the codebase already has a purpose-built,
*disabled* mitigation for (`cross_shard_load_sync`) plus a queueing-theory sign-flip that is
inherent to prefix-vs-random below a utilization crossover.

Before designing a new "load-gated routing mode" mechanism (which is expensive and risks the
proven high-load win), do the two cheap things first:

1. **Fix the threshold leg before running it.** As written it will measure ~nothing — the shipped
   strategy is `BOUNDED_LOAD`, and `load_imbalance_factor/floor` are **inert** under `BOUNDED_LOAD`
   (proof in §3.1). The equivalent knob is `bounded_load_epsilon`. Run an *epsilon* sweep (or run
   the factor/floor sweep under `hash_strategy=jump`), not the current run file.
2. **A/B `cross_shard_load_sync=true` at 13B/10u.** If transient concentration is the driver, this
   recovers the low-load regression *without* touching affinity and *without* risking the high-load
   win. The config already exists and is off by default in the benchmark.

Build a real load-gate mechanism only if both of those fail to recover the regression **and**
the low-load timeout increase is judged a product problem. My honest read (§4): at 16 req/s on
8 idle A100s the absolute P99 (~3–4 s) is not the SLO that matters; the high-load tail is. The
one thing that *does* deserve attention is the **worse incompletes** (timeouts) at 13B/10u —
that is a reliability signal, not just a latency number.

---

## 1. Root-cause confirmation

The task asks us to distinguish, using the code:

- **(a)** backend concentration adding queueing at low load,
- **(b)** rising per-request routing/tokenization/boundary overhead at low load,
- **(c)** something else.

Conclusion up front: **the dominant contributor is (a), but specifically the *transient*
(sub-second, micro-burst) flavor of concentration, not sustained concentration.** (b) is real but
arithmetically negligible for the tail. There is no code *bug* — (c) is the inherent
queueing-theory sign-flip of prefix-vs-random below a utilization crossover, which (a) is the
mechanism of.

### 1.1 What the current data already implies

| Signal (from `benchmark-results-current.md`) | What it rules in / out |
|---|---|
| Effect is **monotonic in throughput** (−13% at 47 r/s → +29% at 16 r/s), *not* in user count or model size | The driver scales with **queue pressure / spare capacity**, i.e. a load-dependent trade-off — consistent with (a)/(c), not with a fixed per-request cost. |
| **Gini stayed low (0.06–0.13)** on every prefix run | **Rules out *sustained* concentration.** Over the whole run, requests are spread evenly across backends. Whatever hurts P99 is *not* visible in the whole-run per-backend distribution. |
| **Load-aware fallbacks 30–47%** on every prefix run | A third to a half of requests are already being diverted *off* affinity. So affinity is not pinning traffic; the aggregate is well-balanced. This *reinforces* "not sustained concentration." |
| **Cache-hit +3× everywhere (12→43–52%) but decoupled from P99** | The prefill-skip saving a cache hit buys is not what moves P99 at low load — because prefill is cheap when the GPU is uncontended. Confirms the *value* side of the trade-off shrinks as load falls. |
| **Overhead ~1.7 ms (30u) → ~16 ms (10u)** | Real and load-inverse, but see §1.3: ~14 ms of delta against a ~3–4 s P99 is ~0.4%. Cannot produce +29%. |
| **13B/10u incompletes "prefix worse"** | Timeouts, not just latency. This is the one low-load signal with genuine product weight (§4). |

The combination **low Gini + large P99 regression** is the crux. A whole-run Gini cannot see a
sub-second burst where several correlated requests pick the same cache-warm backend, spike its
queue, then rebalance — the aggregate counts stay even (low Gini) while the *tail* (P99) absorbs
the transient queueing. So the data is not just *compatible* with transient concentration; it
actively points there and away from the sustained kind.

### 1.2 Why (a)-transient is the mechanism — grounded in the code

The routing load signal is `active_requests` per backend (`router_service.cpp:84`,
`get_composite_backend_load()` at `:1117`), optionally blended with a **GPU-load score from the
vLLM `/metrics` scrape** (lagging, cached, `gpu_load_cache_ttl` staleness at `:1142`/`:1163`).
Under the shipped `BOUNDED_LOAD` strategy the divert allowance at low load collapses toward zero
(`compute_load_allowance` at `:1324`: `cap = max(1, ceil(avg·(1+ε)))`, `allowance = cap − 1`; with
`avg ≈ 0` at low load, `cap = 1`, `allowance = 0`), so affinity is brittle — but that only affects
whether a *single-shard* decision diverts.

The transient hot-spot is a **cross-decision visibility** problem, and the codebase says so in its
own words. `http_controller.cpp:1993–1996`:

> *"Speculative load increment: create BackendRequestGuard immediately after routing so that
> concurrent routing decisions … see updated load counters. Without this, requests completing
> tokenization at the same time all see load=0 and pile onto the same backend (transient
> hot-spot)."*

The speculative increment (`BackendRequestGuard` at `router_service.cpp:870`, `++active_requests`)
closes this **within a single shard**. It does **not** close it:

- **Across shards** — `active_requests` is shard-local; cross-shard visibility requires
  `cross_shard_load_sync` (config `config_schema.hpp:168`, populated into `get_backend_load()` at
  `router_service.cpp:1067`). **The benchmark runs it OFF** (`docker-compose.benchmark-real.yml`:
  `RANVIER_CROSS_SHARD_LOAD_SYNC=false`).
- **Across nodes** — the matrix is a **3-node cluster**. Node-to-node in-flight load is *never*
  shared via `active_requests`; the only cross-node load signal is the lagging vLLM GPU-load
  scrape. Two nodes can route the same hot prefix to the same cache-warm backend inside one scrape
  interval, each seeing its GPU-load as stale-low.

And the benchmark **removes the natural stagger** that would otherwise decorrelate arrivals:
`RANVIER_ACCEPT_CLIENT_TOKENS=1` + `RANVIER_ENABLE_TOKEN_FORWARDING=1` mean clients ship
`prompt_token_ids`, so Ranvier **skips the 5–13 ms tokenization FFI**. The `cross_shard_load_sync`
doc comment (`config_schema.hpp:150–167`) predicts precisely this:

> *"With server-side tokenization (~10-12ms), natural stagger mitigates this — but client-tokenize
> needs explicit sync."*

So the benchmark configuration — **client-tokenize + `cross_shard_load_sync=false` + 3 nodes +
`BOUNDED_LOAD`'s ~0 low-load allowance** — is the exact recipe the code was written to warn about.
At low load this is maximally damaging because round-robin's independent uniform spread gives every
request its *own* idle backend "for free," while prefix routing converts naturally-parallel
independent requests into either (i) a queue behind the burst on the cache-warm backend or (ii) a
load-diverted cache **miss** (full prefill) — and the cache-hit prefill saving that would offset it
is small precisely because prefill is cheap on an uncontended GPU. That is the sign-flip.

### 1.3 Why (b) — per-request overhead — is a red herring for the tail

The overhead rising as load falls is real and has a plausible mechanism (tokenizer thread-pool /
reactor **cold-start**: at low load the dedicated workers block on the job queue and pay
futex-wake + scheduler + cold-cache + CPU-frequency-ramp latency per request; at high load the
pipeline is warm — see `tokenizer_thread_pool`, `worker_affinity`). But the magnitude disqualifies
it as the cause of the *regression*:

- 13B/10u P99 ≈ 3–4 s. The overhead **delta** is ~14 ms (16 − 1.7). That is ~0.4% of P99 — it
  cannot produce +29% (~+1 s).
- It is a genuine A/B differential (random mode skips tokenization *and* boundary detection
  entirely — `route_request` RANDOM branch at `router_service.cpp:2599`), so it counts fully
  against prefix. Still ~0.4%.
- Where 14 ms *would* be a meaningful fraction is the sub-second regime (8B, P99 < 1 s). But 8B is
  a **−13% win**, so overhead is not causing harm there either.

Net: (b) contributes to the *gradient* (it makes prefix look slightly worse everywhere, more so at
low load) but is not the *regression*. It is worth a footnote, not a fix. **Caveat honestly
recorded:** if the overhead number is actually dominated by in-Ranvier *queue-wait* (scheduler /
rate-limiter) rather than tokenizer cold-start, that would be a different story — but queue-wait
falls at low load, so an overhead that *rises* at low load points to cold-start, not queueing,
which is the non-scaling kind.

### 1.4 Evidence that would confirm/refute each — from the report dirs

The raw per-node captures (`benchmark-reports/rebaseline/matrix/*`) are **not committed** to the
repo (only the summary table is). When the campaign is re-run, these are the discriminating
signals (all are first-class fields per BACKLOG §25 item 9's 3-node scrape + Gini machinery):

- **(a)-transient vs (a)-sustained:** whole-run Gini is already low → sustained is out. To *see*
  transient bursts you need **sub-run time-series of per-backend `active_requests`** (or vLLM
  `num_requests_running`/`waiting` per backend over time from `prometheus_metrics_node*.txt`) at
  13B/10u. Confirmation = short spikes of queue depth on the cache-warm backend(s) coincident with
  the P99 tail, on the prefix arm only. Refutation = flat per-backend queues (then the tail is
  service-time, not queueing — points to (c)/service variance).
- **(a) confirm via the fix:** re-run 13B/10u with `cross_shard_load_sync=true`. Regression
  shrinking materially = (a)-transient confirmed as the driver. Regression unchanged = it is not
  cross-visibility (look at intra-node bursts / GPU-scrape lag / service variance instead).
- **(b):** the per-node Ranvier metrics expose `router_routing_latency` / `router_art_lookup_latency`
  histograms and tokenizer thread-pool queue-wait vs FFI-time. Confirmation that (b) is *not* the
  driver = the P99 of Ranvier's own added latency is single-digit ms while end-to-end P99 moved by
  ~1 s. (Already strongly implied by the 14 ms arithmetic.)
- **Diversion attribution:** `router_load_aware_fallbacks_total` and the residency-downgrade
  counter, per node, tell you how much of the 30–47% is load-cap vs residency, and whether the
  diverted requests are the ones landing in the tail (cross-reference with cache-miss counts).

---

## 2. Options for making prefix routing load-adaptive

Since (a) is a real contributor, here are the design options. They split into **"fix the signal"**
(cheap, attacks the mechanism) and **"gate the mode"** (expensive, works around it). Ordered
cheapest-first.

### Option 0 — Fix the load signal (NOT a gate; attacks the mechanism)

- **Control signal:** existing per-backend in-flight (`active_requests`) made cross-visible.
- **Where:** enable `cross_shard_load_sync` (exists, `config_schema.hpp:168`); optionally tighten
  the GPU-load scrape staleness so cross-node load is fresher at low load.
- **Hysteresis:** N/A — it makes an existing signal fresher, not a new mode decision.
- **Failure modes:** SMP cost is O(shards²) per broadcast (documented at `config_schema.hpp:157–164`
  — 8 shards @ 100 ms ≈ 1,120 msgs/s, fine; do **not** drop the interval to single-digit ms).
  Does **not** address cross-*node* visibility (only cross-shard within a node); node-to-node still
  relies on gossip/GPU-scrape. Still, it directly attacks the client-tokenize case the doc names.
- **Why first:** zero new mechanism, cannot hurt the high-load win, and if it works the whole
  "should we gate?" question dissolves.

### Option A — Cluster-throughput gate → fall back to random/hash below a threshold

- **Control signal:** cluster aggregate throughput (req/s) or aggregate in-flight. This is the
  variable the effect is actually monotonic in.
- **Where:** at the top of `route_request()` (`router_service.cpp:2518`) — before the PREFIX branch,
  if `cluster_load < gate_threshold`, route as RANDOM/HASH (skip ART entirely). A clean mode switch,
  not an allowance tweak.
- **Hysteresis:** **mandatory.** Dual thresholds (enter-random below `T_low`, resume-prefix above
  `T_high`, `T_high > T_low`) + an EWMA of the load signal, or you flap across the crossover band
  (which the data says is wide: "somewhere between ~16 and ~28 req/s" — *a band, not a number*).
- **Failure modes:** (i) *measuring cluster throughput is the hard part* — each node/shard sees only
  local traffic; a true cluster rate needs cross-node aggregation (gossip) or a local proxy
  (per-shard req/s), and a local proxy disagrees across nodes → nodes gate inconsistently. (ii) The
  crossover is hardware- and model-dependent ("treat the crossover as a band"), so a fixed
  `gate_threshold` is a per-deployment tuning burden. (iii) At the crossover, random and prefix are
  ~equal, so the gate buys nothing there and only risks flapping. (iv) Cold-start: after a gate flip
  to prefix, the ART is cold and the first requests miss — a transient the gate itself induces.

### Option B — Extend the load-aware allowance (in-flight signal) instead of a mode switch

- **Control signal:** the per-backend in-flight/queue depth the load-aware path *already reads*.
- **Where:** `compute_load_allowance()` (`router_service.cpp:1314`) — make the allowance
  load-adaptive so that below a cluster-load floor the affinity anchor is *never* preferred (forcing
  the scorer to spread), i.e. drive the allowance to 0 for the anchor at low load.
- **Reality check:** this is subtle and probably wrong-headed. At low load the `BOUNDED_LOAD`
  allowance is *already ~0* (§1.2), so the anchor already diverts as soon as it has ≥1 in-flight.
  The problem is not "affinity too sticky" — it is "concurrent deciders can't see each other." An
  allowance tweak cannot fix a visibility problem. **Rejected** as a primary fix; folded into
  Option 0.
- **Failure mode if pursued anyway:** you'd be diverting *more* to cold backends at low load —
  paying more cache-miss prefills — which is the very cost that already fails to pay off at low
  load. Likely makes it worse.

### Recommended shape

**Option 0 first, then Option A only if 0 is insufficient.** If a gate is built, it must be a
distinct mechanism from load-aware fallback (see §3.2), signal off a hysteretic cluster-throughput
EWMA, and be documented as a **timeout-protection** valve (not a latency optimizer), because the
latency it "fixes" is already small (§4).

---

## 3. Interaction with the existing diversion mechanisms

### 3.1 KEY FINDING — the threshold leg, as written, tests an inert knob

`load_imbalance_factor`/`load_imbalance_floor` feed the allowance **only for `JUMP`/`MODULAR`**
(`compute_load_allowance`, `router_service.cpp:1336–1346`). The **shipped** `hash_strategy` is
`BOUNDED_LOAD` (`config_schema.hpp:80`), whose allowance is `cap − 1` from `bounded_load_epsilon`
(`:1324–1331`) and **ignores factor/floor**. The benchmark compose sets
`RANVIER_LOAD_IMBALANCE_FACTOR/FLOOR` but **does not** set `RANVIER_HASH_STRATEGY`
(`docker-compose.benchmark-real.yml`), so it runs `BOUNDED_LOAD`.

Consequences, in order of importance:

1. **The `2.0/2` vs `3.0/4` threshold leg (BACKLOG §25 item 5, run files under
   `docs/benchmarks/rebaseline/`) will show ~no difference on the shipped strategy**, because those
   flags don't feed the active allowance. The one place factor/floor *do* bite under `BOUNDED_LOAD`
   is the hardware-price guard threshold (`router_service.cpp:3298–3299`), which the benchmark never
   exercises (no `cost_per_hour` set). So the leg as specified is at risk of measuring nothing and
   being misread as "thresholds don't matter."
2. **The 30–47% fallback rate is an `epsilon`=0.25 artifact, not a factor/floor artifact.** The
   jump from D2's 7.8% fallbacks (`.dev-context/next-benchmark-checklist.md`) to 30–47% is largely
   explained by the **strategy change** (D2 was factor/floor-driven → `JUMP`; current is
   `BOUNDED_LOAD`), independent of any threshold value. Attributing the fallback rate to "2.0/2
   thresholds" in the current write-up is imprecise.
3. **Fix before running:** either (a) run the leg with `RANVIER_HASH_STRATEGY=jump` so factor/floor
   actually govern, keeping the pre-registered `≥10% P99, no incomplete regression` rule; **or**
   (b) reframe it as a `bounded_load_epsilon` sweep (e.g. 0.25 shipped vs 0.5/1.0 looser), which is
   the knob that actually moves diversion on the shipped strategy. Prefer (b) — it tests the
   *shipped* code path.

### 3.2 Load-aware fallback vs a load gate — opposite ends, distinct mechanisms

The load-aware fallback diverts **more under high load** (allowance is exceeded when backends are
busy). The low-load regression is at the **opposite** end. So a load gate is **not** an extension
of load-aware fallback — it is a separate mechanism operating in the regime where the fallback is
quiescent. Confirmed by the direction of `compute_load_allowance`: at high load the anchor exceeds
allowance and diverts (fallback active); at low load the anchor mostly sits under allowance except
for the ~0-allowance brittleness. A gate lives *before* the affinity decision; the fallback lives
*after* it. They compose cleanly (a gate that routes RANDOM below the crossover simply means the
fallback never runs there — there's no anchor to divert from).

### 3.3 Threshold choice × load gate — and why the threshold leg runs first

Task's concern: *raised thresholds = less diversion = more concentration = could worsen the
low-load case.* Under `BOUNDED_LOAD` the equivalent statement is **raising `epsilon` = looser cap =
less diversion = more concentration**. Interaction with a gate:

- **Below the gate** (random/hash): epsilon/threshold is **irrelevant** — no affinity anchor, no
  divert. So a clean gate makes the threshold choice orthogonal in exactly the regime we're worried
  about.
- **Above the gate** (prefix): epsilon/threshold governs the high-load win, where *more* diversion
  historically helped (D1 no-divert +19% → D2 with-divert −6%). So the threshold choice is a
  **high-load** tuning question, and the gate is a **low-load** switch. Largely independent.

**Run the (corrected) threshold/epsilon leg FIRST**, because: (i) it is cheap (one existing knob);
(ii) it may improve the high-load win on its own and is the highest-value remaining GPU run per
BACKLOG §25; (iii) it tells you whether varying diversion aggressiveness moves the low-load
regression *at all* before you invest in a gate. If looser diversion (higher epsilon) *worsens*
13B/10u and tighter *doesn't* fix it, that is direct evidence the low-load problem is
visibility/transient (Option 0), not diversion-tuning — and you skip the gate.

### 3.4 Cache-residency downgrades (#527)

Inert on 8B (no cache pressure), active on 13B (as noted). Interaction: a residency downgrade
(`router_service.cpp:2934`) diverts an ART hit to a cold backend — **same directional cost** as the
low-load problem (it manufactures a cache-miss prefill). At low load there is little cache pressure,
so residency should fire rarely (gated at `cache_residency_threshold=0.2`, i.e. only when a
backend's effective cache is >~80% full). It is therefore a **minor** contributor to the low-load
regression, but it is *another* diversion source that a gate would bypass below the crossover (no
ART hit → no residency check). Worth isolating in the data (the residency-downgrade counter per
node) but not a design driver here. Note the residency A/B caveat from the checklist: confirm the
"prefix" arms actually ran residency at 0.2, or the attribution blurs.

---

## 4. Should we even do this?

**Framing honestly:**

- At 13B/10u the absolute P99 is ~3–4 s and there are **8 idle A100s** (16 req/s is ~1/3 the
  throughput of the 8B/20u win config). By definition there is spare capacity. A +29% relative
  regression on an already-small, under-utilized operating point is the *least* important corner of
  the matrix for a latency SLO.
- **The SLO that matters is the high-load tail** — that is when users feel latency and when you are
  near capacity and *can't* just scale down. Prefix routing is a **reliable −9 to −13% win** there.
  That is the product value, and it must not be risked to chase a low-load number.
- **Two honest caveats against "low load never matters":**
  1. Real clusters spend most wall-clock time at **low utilization** (bursty traffic, off-peak,
     overprovisioning for peak). "Low load is irrelevant" only holds if you autoscale aggressively
     to keep utilization high — many deployments don't.
  2. 13B/10u shows **worse incompletes (timeouts)**, not just latency. Timeouts at low load with
     idle capacity are a **reliability** embarrassment and are user-visible in a way a slower-but-
     completed request is not. This is the single low-load signal I would act on.

**Recommendation: gate only if cheap — and expect not to need a gate.**

1. **Do Option 0 + the corrected threshold/epsilon leg first** (both cheap, both use existing
   config). Most likely one or both recover the regression, because the mechanism (§1.2) is a known,
   already-mitigable transient-visibility problem, not a fundamental flaw.
2. **Do not build a new cluster-throughput-gated mode** unless (1) fails to recover the regression
   **and** the timeout increase persists. A gate adds a cross-node aggregation dependency, a
   hysteresis surface, a per-deployment threshold to tune, and a cold-ART transient on every
   flip — real cost against a benefit (a few hundred ms at an idle operating point) that is small.
3. **If a gate is ever built,** scope it narrowly as a **timeout-protection valve** (route random
   below a hysteretic cluster-load floor to reclaim round-robin's free parallelism and avoid the
   transient-burst timeouts), not as a latency optimizer, and keep it a distinct mechanism from
   load-aware fallback (§3.2).

---

## 5. If we proceed — GPU validation before any code change

All legs ride current shipped defaults (50 prefixes, ratio 0.9, stress, residency 0.2, 20 ms
flush) and use the existing repeat/aggregate machinery, worded like the campaign runbook
(`docs/benchmarks/benchmark-rebaseline-campaign.md` §2). Each leg is median-of-3 with the
IQR-spans-zero verdict rule. **Run in this order; stop as soon as the regression is recovered.**

### Leg V0 — Fix the signal (cheapest, no code): `cross_shard_load_sync` A/B at the regression point

The decisive test for (a)-transient. Add to a run file under `docs/benchmarks/rebaseline/` and run
each arm's routing config as an A/B vs round-robin:

```
# V0a — shipped (sync OFF), reproduce the regression on this box
--compare --model meta-llama/CodeLlama-13b-Instruct-hf --warmup --duration 10m --users 10 \
    --max-model-len 8192 --prefix-ratio 0.9 --prompt-dist stress
# V0b — same, but cross_shard_load_sync ON (env: RANVIER_CROSS_SHARD_LOAD_SYNC=true,
#        RANVIER_CROSS_SHARD_LOAD_SYNC_INTERVAL_MS=100)
--compare --model meta-llama/CodeLlama-13b-Instruct-hf --warmup --duration 10m --users 10 \
    --max-model-len 8192 --prefix-ratio 0.9 --prompt-dist stress
```
Run: `./scripts/bench-runner.sh --suite custom --file <file> --repeat 3 --output-dir
benchmark-reports/rebaseline/xshard_sync`.
**Pre-registered rule:** if V0b's 13B/10u prefix-vs-RR P99 regression is ≥10 percentage-points
smaller than V0a's (and incompletes no worse), transient cross-visibility is confirmed as the
driver → ship `cross_shard_load_sync=true` as the default for client-tokenize deployments and
**stop** (no gate needed). Also capture per-node time-series `active_requests` to *see* the bursts.

### Leg V1 — Corrected threshold/epsilon leg (replaces the inert §3.1 run file)

Test the knob that actually moves diversion on the shipped `BOUNDED_LOAD` strategy, across the
matrix (esp. the low- and high-load ends):

```
# epsilon 0.25 (shipped) vs 0.5 (looser = less diversion = more concentration)
--compare --model meta-llama/CodeLlama-13b-Instruct-hf --warmup --duration 30m --users 30 \
    --max-model-len 8192 --prefix-ratio 0.9 --prompt-dist stress   # env RANVIER_BOUNDED_LOAD_EPSILON=0.5
--compare --model meta-llama/CodeLlama-13b-Instruct-hf --warmup --duration 10m --users 10 \
    --max-model-len 8192 --prefix-ratio 0.9 --prompt-dist stress   # env RANVIER_BOUNDED_LOAD_EPSILON=0.5
```
(Baseline arms are the already-collected shipped-epsilon 30u and 10u runs.) If the factor/floor
question must be answered on its own terms, run the existing `threshold-3.0-4.runs` file **with
`RANVIER_HASH_STRATEGY=jump`** so the flags are live. **Pre-registered rule (unchanged intent):**
adopt a looser default only if median P99 improves ≥10% with no incomplete-rate regression;
additionally record whether looser diversion *worsens* 13B/10u (evidence for Option 0 over a gate).

### Leg V2 — Only if V0 and V1 both fail: prototype load-gate A/B

Before writing the gate into `route_request`, emulate it with the existing modes to bound the
upside: run **RANDOM** mode vs **PREFIX** mode head-to-head at 13B/10u (both already supported via
`RANVIER_ROUTING_MODE`), which is the *ceiling* a perfect gate could achieve at that operating
point (a gate can at best pick the better of the two per-request). If PREFIX−RANDOM P99 at 13B/10u
is within noise once V0/V1 are applied, **a gate cannot pay for itself** — do not build it. Only if
RANDOM is still materially better at low load *and* the timeout gap persists is a hysteretic
cluster-throughput gate (Option A, §2) justified; design its threshold from the measured crossover
band, not a guessed constant.

---

## Appendix — code anchors verified against the current tree

- `src/config_schema.hpp:59` RoutingMode enum; `:80` `hash_strategy = BOUNDED_LOAD` (shipped);
  `:86` `bounded_load_epsilon = 0.25`; `:123–125` `load_aware_routing` / `load_imbalance_factor 2.0`
  / `load_imbalance_floor 2`; `:168` `cross_shard_load_sync = false`; `:230` `cache_residency_threshold 0.2`.
- `src/router_service.cpp:1314` `compute_load_allowance` — **factor/floor only in the JUMP/MODULAR
  branch (`:1336`); BOUNDED_LOAD uses epsilon (`:1324`)**; `:1117` `get_composite_backend_load`;
  `:1067` cross-shard load add; `:870` `BackendRequestGuard` in-flight increment; `:2518`
  `route_request` entry; `:2934` residency downgrade; `:3144` load-term applicability; `:3298`
  factor/floor's only BOUNDED_LOAD use (hardware-price guard).
- `src/http_controller.cpp:1993–1996` speculative-increment comment naming the transient hot-spot;
  `:2081–2082` `X-Backend-ID` / `X-Routing-Mode` headers.
- `docker-compose.benchmark-real.yml` — `ROUTING_MODE=prefix`, `ACCEPT_CLIENT_TOKENS=1`,
  `ENABLE_TOKEN_FORWARDING=1`, `CROSS_SHARD_LOAD_SYNC=false`, `LOAD_IMBALANCE_FACTOR/FLOOR=2.0/2`,
  **no `RANVIER_HASH_STRATEGY`** (→ BOUNDED_LOAD).
- `docs/benchmarks/rebaseline/{standard-matrix,threshold-2.0-2,threshold-3.0-4}.runs`,
  `docs/benchmarks/benchmark-results-current.md`, `.dev-context/next-benchmark-checklist.md`
  (D1 +19% / D2 −6%), BACKLOG.md §25 item 5.
