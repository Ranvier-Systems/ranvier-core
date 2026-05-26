# Next Benchmark Checklist — May 22 2026 Affinity-Thrashing Follow-up

**Branch:** `claude/dreamy-hypatia-b1Xjo` (or whichever ships these checklist items)
**Reference commit:** `5079f9f` (May 22, 2026 reproduction)
**Reading order:** [investigation-289](investigation-289-routing-regression.md)
→ [investigation-may22](investigation-may22-affinity-thrashing-reproduction.md)
→ this file.

The May 22 13B/30m runs at 10u/20u/30u reproduced the affinity-thrashing
failure mode investigation #289 predicted. This checklist runs the
smoking-gun A/B experiments needed to commit to a fix path (revert, threshold
tune, or hysteresis). **Do these experiments first; do not write a code fix
until the data points to one.**

## ⚠️ Two things changed since this checklist was first written (read before running)

**1. The env-var-prefix approach does NOT work — use bench.sh flags.**
`bench.sh` defaults `LOAD_AWARE=true` and *unconditionally* exports
`RANVIER_LOAD_AWARE_ROUTING=true` (scripts/bench.sh, "Export load-aware routing
settings" block), which overwrites any `RANVIER_LOAD_AWARE_ROUTING=… ./bench.sh`
prefix. This is why the first Exp A run (f1c70ee, May 25) came up with
load-aware ON. The supported overrides are CLI flags:
- `--no-load-aware` → exports `RANVIER_LOAD_AWARE_ROUTING=false`
- `--load-imbalance-factor <N>` → exports `RANVIER_LOAD_IMBALANCE_FACTOR`
- `--load-imbalance-floor <N>` → exports `RANVIER_LOAD_IMBALANCE_FLOOR`

bench.sh now prints an "Effective Routing Config" banner at startup; **confirm
it reads what you intended before letting a 30m run proceed.**

**2. Commit `f1c70ee` (#527) added cache-residency routing — a SECOND diversion
mechanism.** On an ART hit, if the owning backend's gossiped KV-cache residency
is below `cache_residency_threshold` (default `0.2`, i.e. cache >80% full), the
hit is downgraded to a miss and re-routed via the hash strategy, excluding the
cold backend. This fires **independently of load-aware** and is on by default.
At 30u/13B under cache pressure it can fire constantly. Consequences:
- The May 22 `5079f9f` data had NO residency routing. Any run on `f1c70ee`+ is
  a *different routing algorithm*, so "did the regression reproduce?" is only
  apples-to-apples if you neutralize residency.
- To get a true "pure affinity, no diversion" baseline you must disable BOTH
  mechanisms: `--no-load-aware` **and** `RANVIER_CACHE_RESIDENCY_THRESHOLD=0.0`
  (threshold 0.0 disables downgrades; verified router_service.cpp:2464,
  `if (state.config.cache_residency_threshold > 0.0)`).
- New diagnostic counter: `ranvier_router_residency_route_downgrades_total`
  (the parser now scrapes and prints it next to load_aware_fallbacks_total).
  Note: unlike the load-aware divert, a residency downgrade re-learns the ART
  to the new backend (it sets `art_hit=false` so `original_selected` becomes
  the hash pick), so residency routing has the self-healing escape valve the
  load-aware path lacks — see investigation-may22 for why that matters.

## Pre-flight (every run)

- Confirm the deployed commit. `RANVIER_CACHE_RESIDENCY_THRESHOLD` only matters
  on `f1c70ee`+; on `5079f9f` it's a no-op (feature absent).
- **Read the "Effective Routing Config" banner** bench.sh prints at startup and
  confirm `RANVIER_LOAD_AWARE_ROUTING` / imbalance factor+floor match the
  experiment. A missing flag is now caught at second 0, not minute 30.
- Capture `nvidia-smi --query-gpu=clocks.sm,clocks.mem,clocks_throttle_reasons.active --format=csv`
  at start and end (bench.sh does this; verify
  `$REPORT_DIR/nvidia_smi_{start,end}.csv` exist; container scrape uses
  `ranvier-bench1..3`).
- Curl `/metrics` at end of run to `$REPORT_DIR/prometheus_metrics.txt`
  (bench.sh does this; verify it exists and is non-empty). The parser surfaces
  `ranvier_routing_load_aware_fallbacks_total`,
  `ranvier_router_residency_route_downgrades_total`, and per-backend
  `ranvier_backend_active_requests` from this dump.
- Always pair each prefix-aware run with a round-robin baseline of the same
  duration on the same instance — no cross-instance comparisons.

## Status (2026-05-26) — INVESTIGATION COMPLETE

The full 2×2 (diversion on/off × 5/50 prefixes) is done. **Verdict: the 30u
"regression" is dominated by the 5-prefix benchmark workload, not the router.**

| | 5 prefixes | 50 prefixes |
|---|---|---|
| No diversion (A / D1) | P99 -76% but **9.4% timeouts**, Gini 0.534 | P99 +19%, 2.2% timeouts (≈base), Gini 0.197 |
| Load-aware (B / D2) | **P99 +167%**, miss-tail +219% | **P99 -6%**, 1.9% timeouts (<base), hit 60%, fallbacks 7.8% |

- **A — DONE** (`f2066e6`): pure affinity, 5 prefixes. P99 -76% but 9.4% timeouts
  (pigeonhole over-concentration). Both diversion counters 0.
- **B — DONE** (`f2066e6`): load-aware, 5 prefixes. P99 +167%, miss-tail +219%.
- **D1 — DONE** (`f708de5`): pure affinity, 50 prefixes. Timeouts fall to 2.2%
  (≈ round-robin 2.1%), Gini 0.197 — the 9.4% was a 5-prefix pigeonhole.
- **D2 — DONE** (`f708de5`): load-aware, 50 prefixes. **P99 -6% (net win)**,
  incompletes below baseline. The +167% in B was *also* largely a 5-prefix
  artifact.
- **Final verdict:** at realistic prefix counts the routing system works as
  designed; load-aware diversion earns its keep (D2). `#442`'s cold-divert is
  reclassified from correctness bug to a cache-efficiency optimization
  (~30 pp hit-rate left on the table by cold diverts). #441 and residency (#527)
  ruled out. Full write-up:
  [investigation-may22](investigation-may22-affinity-thrashing-reproduction.md)
  § "Empirical results — Experiments D1 & D2".
- **Superseded:** Experiment C (20u repeats) is now low-value — the 5-prefix
  timeout it was meant to investigate is a known pigeonhole artifact. Run only
  if a 20u-specific question arises. Experiment A2 remains moot (residency inert).

### Action taken from this investigation

- **bench.sh default changed: `NUM_LARGE_PREFIXES` now defaults to 50** (was the
  locust default of 5). 5 prefixes against 8 backends manufactures the false
  regression; 50 is representative. Pass `NUM_LARGE_PREFIXES=5` explicitly only
  to stress-test prefix concentration. Banner warns when count ≤ backend count.
- **No routing-config default change** is justified by this data: D2 used raised
  thresholds (`factor=3.0, floor=4`); the shipped defaults (`2.0`/`2`) were never
  tested at 50 prefixes. If a routing-default change is ever considered, run a
  default-threshold leg at ≥50 prefixes first.
- **`#442` warm-diversion fix:** optional optimization, human decision; not urgent.

## Experiments

Experiments A and B are DONE (see Status above); their specs are retained below
for reproducibility. C and D are the remaining runs.

### Experiment A — Disable ALL diversion (smoking gun) [DONE]

**Hypothesis:** If diversion-breaking-affinity is the root cause, turning off
*both* load-aware routing and residency downgrades collapses P99 back to
baseline-or-better at 30u and the Mar 5 doc numbers (`08e5a93`: P99 -79.6%,
+13.2% throughput) reappear.

```bash
# 30u/30m, prefix mode, BOTH diversion mechanisms OFF.
# --no-load-aware sets RANVIER_LOAD_AWARE_ROUTING=false (env prefix would be
# clobbered). RANVIER_CACHE_RESIDENCY_THRESHOLD=0.0 disables #527 downgrades.
RANVIER_CACHE_RESIDENCY_THRESHOLD=0.0 \
RANVIER_ROUTING_MODE=prefix \
./scripts/bench.sh --compare --warmup --duration 30m --users 30 \
  --no-load-aware \
  --model meta-llama/CodeLlama-13b-Instruct-hf \
  --prompt-dist stress --prefix-ratio 0.9 \
  --output-dir benchmark-reports/expA-no-divert
```

**Verify before letting it run:** the "Effective Routing Config" banner must show
`RANVIER_LOAD_AWARE_ROUTING = false`. After the run, both
`load_aware_fallbacks_total` and `residency_route_downgrades_total` should be ≈0.

**Expected if hypothesis is correct:**
- P99 TTFT collapses from +44% to ≤0% vs RR.
- Cache Miss P99 collapses from +69% to ≤+10%.
- Both diversion counters ≈ 0.
- 0 incompletes on both sides.

**Expected if hypothesis is wrong:** something else regressed. Open a new
investigation.

> Note: `RANVIER_CACHE_RESIDENCY_THRESHOLD` is a plain env var (no bench.sh
> clobber — bench.sh doesn't export it), so the prefix form works for it. Only
> `RANVIER_LOAD_AWARE_ROUTING` / imbalance vars are flag-controlled.

### Experiment A2 — Isolate residency routing (load-aware off, residency ON) [LIKELY MOOT]

**Status:** probably unnecessary. Exp B already measured
`residency_route_downgrades_total = 0` with residency at its default 0.2, so
residency routing is inert in this topology (signal never crosses threshold /
not populated). A2 would almost certainly just confirm residency does nothing.
Only run it if you specifically need to prove residency's null effect with
load-aware also off.

**Why (original rationale):** Experiment A turns off both mechanisms, so it
can't tell you which one mattered. A2 keeps residency routing at its default to
measure its solo contribution.

```bash
# 30u/30m, load-aware OFF, residency routing at default (0.2)
RANVIER_ROUTING_MODE=prefix \
./scripts/bench.sh --compare --warmup --duration 30m --users 30 \
  --no-load-aware \
  --model meta-llama/CodeLlama-13b-Instruct-hf \
  --prompt-dist stress --prefix-ratio 0.9 \
  --output-dir benchmark-reports/expA2-residency-only
```

**Read:** compare against A. If A is healthy but A2 regresses, residency
downgrades are the culprit (check `residency_route_downgrades_total` %). If A
and A2 are both healthy, residency routing is benign at this load and load-aware
was the whole story.

Flags verified: `--no-load-aware` (scripts/bench.sh arg parse → `LOAD_AWARE=false`
→ exports `RANVIER_LOAD_AWARE_ROUTING=false`). Toggle honored uniformly across
hash strategies (`src/router_service.cpp` load_aware_routing gates).

### Experiment B — Raise the median-comparison threshold (production-shaped fix) [DONE]

**Hypothesis:** Investigation #289 Fix 1 was right in spirit but the parameter
names are stale; today's equivalents are `load_imbalance_factor` (default 2.0)
and `load_imbalance_floor` (default 2). Raising both should make load-aware
fire only on genuine outliers, not on every request at 3.75 req/GPU.

```bash
# 30u/30m, prefix mode, load-aware ON with raised thresholds (via flags),
# residency routing left at default 0.2.
RANVIER_ROUTING_MODE=prefix \
./scripts/bench.sh --compare --warmup --duration 30m --users 30 \
  --load-imbalance-factor 3.0 \
  --load-imbalance-floor 4 \
  --model meta-llama/CodeLlama-13b-Instruct-hf \
  --prompt-dist stress --prefix-ratio 0.9 \
  --output-dir benchmark-reports/expB-thresholds-raised
```

(`LOAD_AWARE` defaults to true, so omit `--no-load-aware` here. The banner
should show `RANVIER_LOAD_AWARE_ROUTING = true`, factor 3.0, floor 4.)

**Expected if hypothesis is correct:**
- P99 better than the f1c70ee default-threshold run, possibly close to A —
  load-aware still protects against single-backend hotspots.
- `load_aware_fallbacks_total / total_requests` < 0.05.
- 0 incompletes.

**Expected if hypothesis is wrong:** P99 still elevated — the no-hysteresis
problem is structural and threshold tuning can only soften, not solve, the
ART/load-aware misalignment investigation-may22 attributes to `#442`. Move to
investigating the ART learning path (`#289 Fix 2` / proper hysteresis). Also
check `residency_route_downgrades_total` — if it's high, residency routing
(not load-aware) is the dominant diversion and Experiment B can't fix it.

**Flag / env-var note (verified May 25, 2026):**
- `--no-load-aware` → `RANVIER_LOAD_AWARE_ROUTING=false` → `routing.load_aware_routing`
- `--load-imbalance-factor N` → `RANVIER_LOAD_IMBALANCE_FACTOR` → `routing.load_imbalance_factor` (default 2.0)
- `--load-imbalance-floor N` → `RANVIER_LOAD_IMBALANCE_FLOOR` → `routing.load_imbalance_floor` (default 2)
- `RANVIER_CACHE_RESIDENCY_THRESHOLD` → `routing.cache_residency_threshold` (default 0.2; 0.0 disables; env prefix works, no flag)

Investigation #289's `queue_depth_threshold` / `queue_diff_threshold` names
**do not exist** in the current code — it uses a multiplicative-factor-on-median
scheme. Above is the current equivalent. No YAML overlay or code change needed.

### Experiment C — 20u repeats of A and B (zombie-timeout test) [SUPERSEDED — see Status]

The 20u/30m run on `5079f9f` had 639 incompletes (6.0%) — the "diverted
request never completes" failure mode predicted at lines 200-220 of
investigation #289. Both A and B should drive this to zero.

```bash
# C1: 20u/30m, all diversion OFF (mirror of Experiment A)
RANVIER_CACHE_RESIDENCY_THRESHOLD=0.0 RANVIER_ROUTING_MODE=prefix \
./scripts/bench.sh --compare --warmup --duration 30m --users 20 \
  --no-load-aware \
  --model meta-llama/CodeLlama-13b-Instruct-hf \
  --prompt-dist stress --prefix-ratio 0.9 \
  --output-dir benchmark-reports/expC1-no-divert-20u

# C2: 20u/30m, load-aware ON with raised thresholds (mirror of Experiment B)
RANVIER_ROUTING_MODE=prefix \
./scripts/bench.sh --compare --warmup --duration 30m --users 20 \
  --load-imbalance-factor 3.0 --load-imbalance-floor 4 \
  --model meta-llama/CodeLlama-13b-Instruct-hf \
  --prompt-dist stress --prefix-ratio 0.9 \
  --output-dir benchmark-reports/expC2-thresholds-20u
```

**Expected:** incomplete count goes to 0 in both. If only A clears the timeouts
and B doesn't, the flapping zone is wider than #289 modeled and threshold
tuning alone isn't enough — hysteresis becomes the next experiment.

### Experiment D — Prefix-count sensitivity (is the timeout an artifact of 5 prefixes?) [DONE — YES, see Status]

**Why:** Experiments A and B both ran the default `NUM_LARGE_PREFIXES=5` against 8
backends. Under pure affinity that is a pigeonhole — at most 5 of 8 backends can
serve a prefix, so 3 sit idle and the hot ones overload (Exp A measured Gini
0.534, 9.4% timeouts). That concentration may be a property of the *workload*,
not the router. Raising the prefix count relaxes the pigeonhole: with 50
prefixes spread by consistent hash, all 8 backends get work even under pure
affinity.

```bash
# D1: 30u/30m, all diversion OFF, 50 prefixes (mirror of Exp A but un-pigeonholed)
NUM_LARGE_PREFIXES=50 \
RANVIER_CACHE_RESIDENCY_THRESHOLD=0.0 RANVIER_ROUTING_MODE=prefix \
./scripts/bench.sh --compare --warmup --duration 30m --users 30 \
  --no-load-aware \
  --model meta-llama/CodeLlama-13b-Instruct-hf \
  --prompt-dist stress --prefix-ratio 0.9 --max-model-len 8192 \
  --output-dir benchmark-reports/expD1-noDivert-50prefix

# D2 (optional): same but load-aware ON (mirror of Exp B at 50 prefixes)
NUM_LARGE_PREFIXES=50 \
RANVIER_ROUTING_MODE=prefix \
./scripts/bench.sh --compare --warmup --duration 30m --users 30 \
  --load-imbalance-factor 3.0 --load-imbalance-floor 4 \
  --model meta-llama/CodeLlama-13b-Instruct-hf \
  --prompt-dist stress --prefix-ratio 0.9 --max-model-len 8192 \
  --output-dir benchmark-reports/expD2-la-50prefix
```

`NUM_LARGE_PREFIXES` is forwarded to the locust container by bench.sh and echoed
in the "Effective Routing Config" banner (with a pigeonhole warning when it is
≤ backend count). It is a workload knob, not a routing config — verify it shows
`50` in the banner.

**Expected if the timeouts are workload concentration (most likely):**
- D1 (no diversion, 50 prefixes): per-backend Gini drops from ~0.53 toward
  ~0.1, **incompletes fall toward 0**, and P99 stays strongly negative — i.e.
  pure affinity *works* once the prefix set can fill all backends.
- This would localize the Exp A timeout failure to the artificial 5-prefix
  pigeonhole, and reframe the 30u "regression" as: diversion is only needed
  when the prefix set is smaller than the backend count, and #442 makes that
  diversion cold. Real workloads with many prefixes may not need diversion at
  all at this concurrency.

**Expected if timeouts persist at 50 prefixes:** the over-concentration is not
purely a pigeonhole — some prefixes are hotter than others (Zipfian), and a few
backends still overload. That strengthens the case that *some* load-aware
diversion is genuinely needed, which puts the #442 cold-divert fix back on the
critical path.

### Experiment E — Cache-residency-aware routing (#527) under cache pressure [OPEN]

**Prerequisite finding (2026-05-26):** residency routing was confirmed LIVE but
INERT in every A/B/C/D run — `residency_route_downgrades_total = 0` while
`cache_states_received_total ≈ 5776` and `residency_cache_size = 8`. The signal
flows end-to-end; the KV cache simply never crossed the 80%-full threshold
(`residency_weight < 0.2`), so the downgrade never triggered. **A residency
benchmark is meaningless until the downgrade counter is > 0.** The parser now
prints the residency-signal health line and flags "LIVE but never fired."

**Goal:** create real cache pressure so residency fires, then A/B residency-on
vs residency-off under identical pressure.

**Step 1 — make it fire (verification, not yet an A/B).** Shrink the KV cache so
`vllm:gpu_cache_usage_perc` exceeds 0.8 on the hot backends. Primary knob is
`--gpu-mem-util` (default 0.85 → drives vLLM `--gpu-memory-utilization`); lower
it so less memory is left for KV cache after weights. For 13B on 40GB A100,
weights are ~26GB (~0.65 of card), so stay above ~0.70 or vLLM won't start.

```bash
# E0: confirm residency fires. Watch the banner + the parser's residency-signal
# line; success = residency_route_downgrades_total > 0.
NUM_LARGE_PREFIXES=50 \
RANVIER_CACHE_RESIDENCY_THRESHOLD=0.2 RANVIER_ROUTING_MODE=prefix \
./scripts/bench.sh --compare --warmup --duration 30m --users 30 \
  --gpu-mem-util 0.72 \
  --model meta-llama/CodeLlama-13b-Instruct-hf \
  --prompt-dist stress --prefix-ratio 0.9 --max-model-len 8192 \
  --output-dir benchmark-reports/expE0-residency-pressure-probe
```

If downgrades stay 0 at `--gpu-mem-util 0.72`, push pressure harder: lower toward
0.70, raise `--users` (40–50), and/or raise `--prefix-max-tokens`. As a last
resort raise `RANVIER_CACHE_RESIDENCY_THRESHOLD` toward 0.5 (fires at 50% full)
— but that changes what you're testing, so prefer real pressure.

**Step 2 — the A/B (only once Step 1 shows downgrades > 0).** Same pressured
config, residency ON vs OFF, compare the two prefix-aware legs:

```bash
# E_on: residency ON (0.2)
NUM_LARGE_PREFIXES=50 RANVIER_CACHE_RESIDENCY_THRESHOLD=0.2 RANVIER_ROUTING_MODE=prefix \
./scripts/bench.sh --compare --warmup --duration 30m --users 30 --gpu-mem-util 0.72 \
  --model meta-llama/CodeLlama-13b-Instruct-hf --prompt-dist stress --prefix-ratio 0.9 \
  --max-model-len 8192 --output-dir benchmark-reports/expE-on

# E_off: residency OFF (0.0), identical otherwise
NUM_LARGE_PREFIXES=50 RANVIER_CACHE_RESIDENCY_THRESHOLD=0.0 RANVIER_ROUTING_MODE=prefix \
./scripts/bench.sh --compare --warmup --duration 30m --users 30 --gpu-mem-util 0.72 \
  --model meta-llama/CodeLlama-13b-Instruct-hf --prompt-dist stress --prefix-ratio 0.9 \
  --max-model-len 8192 --output-dir benchmark-reports/expE-off

# Compare the two PREFIX legs directly (off = baseline, on = new):
./tests/integration/results_parser.py compare \
  benchmark-reports/expE-off/*_prefix/benchmark.log \
  benchmark-reports/expE-on/*_prefix/benchmark.log
```

**What residency-on is supposed to do** (per `docs/internals/cache-residency-routing.md`):
honor an ART hit only when the backend likely still holds the prefix; when the
owning backend's cache is full (prefix likely evicted), treat as a miss and
divert. So under pressure, residency-on should **reduce the stale-hit penalty** —
fewer "believed hits" that are actually cold — improving Cache Hit P99 / the gap
between believed-hit and actual-hit latency.

**The confound to watch:** a residency downgrade diverts to the load-based
fallback, i.e. it is itself a (cold) diversion — the same #442 family cost. So
residency-on trades stale-hit-misses for cold-divert-misses. The benchmark
measures the *net*. Read both `residency_route_downgrades_total` (did it fire,
and how much) and Cache Miss P99 (did the diverts pay off or just add cold cost).

**Success criteria:**
- Step 1: `residency_route_downgrades_total > 0` (ideally a few % of requests).
- Step 2: residency-on improves Cache Hit P99 / overall P99 vs residency-off
  *without* a worse net Cache Miss tail. If on ≈ off on every metric, residency
  routing is neutral at this pressure; if on is worse, the cold-divert cost
  dominates (same lesson as #442) and the threshold/eviction model needs work.

## Methodology notes

- **Minimum duration:** 30m at 20u+. 10m runs at 20u-30u are not long enough
  to distinguish steady state from warm-up variance — the Mar 5 `08e5a93`
  result that didn't reproduce was a 30m run, so use that as the floor.
- **Always run `--compare`** so prefix-aware and round-robin runs share the
  same backend state and warm-up. Bench.sh does this.
- **Save the Prometheus dump.** Two questions now matter: "did load-aware fire
  0%, 5%, or 90% of the time?" (`load_aware_fallbacks_total`) AND "did residency
  routing divert ART hits?" (`residency_route_downgrades_total`). A 30u miss-tail
  can be driven by either. The May 22 runs captured neither (wrong container
  name in the scrape); the bench.sh fixes ensure future runs do.
- **Save nvidia-smi.** Throttle reasons silently destroy benchmarks. If
  `clocks_throttle_reasons.active` is anything other than `0x0000000000000000`
  on multiple GPUs, the run is suspect regardless of routing numbers.
- **Compare using the updated parser.** The Validation/incompletes banner is at
  the top of the report; the ROUTING COUNTERS block prints both
  `load_aware_fallbacks_total` and `residency_route_downgrades_total` (with
  per-total-request %), plus the per-backend distribution. Read all of them
  before quoting a headline.

## Out of scope for this session

- Writing a code fix. The fix path (revert `#442`, add hysteresis, or just
  raise thresholds in `config_schema.hpp` defaults) must follow from
  Experiments A/B/C data, not from this static analysis. Flag for human
  decision per the task brief.
- Adding `nvidia-smi`/Prometheus scraping to the locust-only path
  (`tests/integration/run-benchmark.sh`). The harness fix is in
  `scripts/bench.sh`, which is the production path. Anyone running the legacy
  script will need a parallel change.
