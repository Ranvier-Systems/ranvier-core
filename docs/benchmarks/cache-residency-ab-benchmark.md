# Cache-Residency Routing A/B Benchmark

**Status: MEASURED (2026-06-11).** First valid residency on-vs-off run; results
below. Methodology and tooling sections follow the results.

> ⚠️ **Do not** attribute the repository's existing "~48% faster TTFT" /
> "P99 -78%" figures to residency routing. Those measure **prefix-affinity
> routing vs round-robin** ([kv-cache-prefix-routing-benchmark.md](kv-cache-prefix-routing-benchmark.md))
> — a different feature. The residency numbers are the table below, measured
> under the saturation regime described in this document, and nothing else.

## Results (measured 2026-06-11)

**Environment:** Lambda Labs 8x NVIDIA A100-SXM4-40GB (1 host) ·
meta-llama/Llama-3.1-8B-Instruct · vLLM 0.15.1 (`--enable-prefix-caching`,
`--gpu-memory-utilization 0.50`, `--max-model-len 8192`) · 3-node Ranvier
cluster, commit `30fd329` (`--no-load-aware` both legs, scrape timeout 1000ms)
· churn workload (universe 200, active 24, step 8/20s, seed 42, prefix-ratio
0.9, prefixes 2000–8000 est. tokens) · 60 users · 400 output tokens/req ·
30m per leg + 1m warmup, vLLM cold-started per leg.

**Command:** `./scripts/bench-residency-ab.sh --skip-probe --users 60 --max-tokens 400`
(preceded by a passed pressure probe at the same config: 20 downgrades / 8m).

**Validity:** all gates passed. OFF downgrades 0; ON downgrades 39 (0.2% of
requests); scrapes 3024/0 and 3016/0 (success/failed); gossip received = 2x
sent on both legs; KV usage peak/mean 100%/35.6% (OFF) vs 99.8%/37.4% (ON);
zero errors, zero timeouts, throughput within 1.2%.

| Metric | Residency OFF | Residency ON | Delta |
|--------|---------------|--------------|-------|
| **TTFT P99 (all requests)** | **2800 ms** | **1200 ms** | **−57.1%** |
| TTFT P50 (all requests) | 470 ms | 470 ms | 0% |
| Cache Hit P99 † | 3074 ms | 968 ms | −68.5% |
| XLarge-bucket Hit P99 † | 4396 ms | 982 ms | −77.7% |
| Cache Miss P99 † | 986 ms | 5051 ms | +412% |
| Client cache hit rate † | 88.8% | 80.4% | −8.4 pp |
| Throughput | 57.7 req/s | 58.4 req/s | +1.2% |
| Errors / timeouts | 0 / 0 | 0 / 0 | — |
| `residency_route_downgrades_total` | 0 | 39 (0.2%) | — |
| Backend distribution (Gini) | 0.081 | 0.076 | slightly more even |

† Composition-caveated — see "How to read the split metrics" below. The
headline is the **overall P99**, which is computed over all requests and is
immune to hit/miss reclassification.

Raw artifacts: `benchmark-reports/residency-ab_20260611_162343/ab-summary.txt`
on the run host (validity block, per-bucket tables, Ranvier overhead, backend
distributions).

### How to read the split metrics

The client classifies a request as a "hit" when the prefix landed on the same
backend as its previous request. Residency-ON changes routing topology, so
pools shift between legs:

- **OFF leg = pure affinity, literally zero route changes** (misses 2054 ==
  unique prefixes 2054). Every returning evicted prefix re-prefilled on its
  original backend *inside the "hit" class* — that stale-hit population is
  what put OFF's Hit P99 at 3.1–4.4 s.
- **ON leg: 39 downgrades produced 1,485 route-change "misses"** (3617 misses
  − 2132 unique prefixes), a 38x amplification explained by **cross-node
  route flapping** (next section). Most of those "misses" ran at hit-grade
  latency (ON Miss P50 454–500 ms ≈ Hit P50), i.e. they were warm — the −8.4
  pp hit rate is mostly accounting, not lost cache efficiency. The genuinely
  cold population (the diverted requests + fresh prefills under saturation)
  forms ON's 5.1 s miss tail.
- Net: what the toggle actually buys is the **−57% overall P99** at equal
  throughput and error rates. Attribute it to the mechanism *as shipped* —
  the 39 diverts **plus** their re-learn side effects (including accidental
  dual-backend warming of flapped prefixes, which spreads hot-prefix load;
  Gini improved slightly) — not to the diverts alone.

### New finding: cross-node route flapping after a divert (open issue)

`learn_route_global()` dedups a route only when the local ART already maps
the prefix to the **same** backend (`router_service.cpp`, dedup block). After
a downgrade re-learns node1's ART to backend B, the other nodes still route
the prefix to A, each node keeps re-learning + re-broadcasting whatever it
last served, and `ROUTE_ANNOUNCEMENT`s carry no version/tie-break — so the
cluster sustains disagreement and the client (round-robining nodes) observes
the backend alternating: 39 diverts → 1,485 observed flips. Side effects cut
both ways: flapped prefixes end up warm on two backends (replication-like
latency benefit, visible in this run) at the cost of duplicate KV occupancy
and repeated announcement traffic. **This applies to every route-changing
mechanism** (load-aware fallback, cost divert, backend death), not just
residency — residency merely made it measurable because the OFF leg had zero
route changes. Flagged in `.dev-context/next-benchmark-checklist.md` for a
follow-up investigation (route-announcement convergence semantics).

### Scope caveats

- Measured in the **active-demand saturation regime** (60 users x 400-token
  decodes against a deliberately small 0.50-util KV cache) — the only regime
  where the shipped v1-usage signal fires (see semantics section below). The
  result says nothing about lighter regimes, where residency-ON ≡ OFF.
- Single run pair, one model/hardware combo. The May 2026 methodology notes
  (30m floor, paired legs, same instance) were followed; replicate before
  treating the exact percentages as stable.

---

# Methodology

## What is being measured

Cache-residency-aware routing
([internals doc](../internals/cache-residency-routing.md)): on an ART prefix
hit, the router consults the owning backend's gossiped `residency_weight`
(`1 - effective_cache_pressure`, from the vLLM `gpu_cache_usage_perc` scrape,
distributed via `CACHE_STATE` gossip packets). If it is below
`routing.cache_residency_threshold` (default 0.2, i.e. cache >80% full), the
hit is treated as a likely-evicted stale route: the request is diverted to the
hash/load fallback excluding the cache-cold backend, and the ART re-learns.

The A/B toggle is `RANVIER_CACHE_RESIDENCY_THRESHOLD`:

| Leg | Setting | Meaning |
|-----|---------|---------|
| OFF (baseline) | `0.0` | downgrades disabled — ART hits always honored (pre-#527 behavior) |
| ON | `0.2` | server default — downgrade when backend cache >80% full |

Supported ways to set it (both verified end-to-end as of this harness):
`./scripts/bench.sh --cache-residency-threshold 0.0 …` (preferred, shows in the
"Effective Routing Config" banner) or the
`RANVIER_CACHE_RESIDENCY_THRESHOLD=0.0 ./scripts/bench.sh …` env prefix.

> **History:** before June 2026, `docker-compose.benchmark-real.yml` did not
> pass this variable into the Ranvier containers, so the env prefix silently
> did nothing — every "residency off" leg actually ran at the 0.2 default.
> This did not corrupt past conclusions (the downgrade counter was 0
> everywhere, so on≡off), but any residency A/B **must** run on a build that
> includes the compose pass-through.

## Why a special workload is needed

Residency routing only *does* anything when three things hold simultaneously:

1. A backend's KV cache exceeds the threshold (>80% full at default 0.2);
2. The router gets an ART hit pointing at that backend (a learned route);
3. The prefix behind that route has actually been **evicted** — otherwise the
   downgrade trades a real cache hit for a cold divert and can only hurt.

The standard `stress` distribution cannot produce condition 3 in steady state:
it samples from a **static** prefix pool, so every prefix is continuously
re-requested and stays at the warm end of vLLM's LRU. Cache usage can be high,
but the *routed-to* prefixes are the resident ones. That is exactly the
warm-cache best case the May 2026 runs measured: signal flowing, zero
downgrades, nothing to A/B.

### The `churn` workload

`--prompt-dist churn` (in `locustfile_real.py`) rotates a sliding window of
active prefixes over a much larger universe:

| Knob (env var) | Default | Meaning |
|----------------|---------|---------|
| `CHURN_PREFIX_UNIVERSE` | 200 | total unique large prefixes (deterministic content) |
| `CHURN_ACTIVE_PREFIXES` | 24 | working-set window receiving traffic at any instant |
| `CHURN_ROTATION_SECONDS` | 20 | window advance period |
| `CHURN_ROTATION_STEP` | 8 | prefixes rotated out per advance |
| `CHURN_SEED` | 42 | universe content seed (identical bytes across A/B legs) |
| `LARGE_PREFIX_MIN/MAX_TOKENS` | 2000/8000 | per-prefix size range (shared with stress mode) |
| `SHARED_PREFIX_RATIO` (`--prefix-ratio`) | 0.9 | rest are unique one-shot fillers (pure pressure) |

Prefix lifecycle at defaults:
**hot ~60s → dormant ~440s → returns** (full cycle 500s; in a 30m run each
prefix returns 3–4 times). During dormancy the backend keeps absorbing new
prefixes under a deliberately small KV cache, evicting the dormant one; the
ART route (TTL 3600s) survives. The return is therefore a **stale ART hit** —
the precise event residency routing exists to intercept. The window position
is a pure function of wall-clock time, so all Locust users agree on the active
set without coordination, and `CHURN_SEED` makes the prompt universe
byte-identical between legs.

This is measured under **cache pressure**, not a warm-cache best case: the
point of the experiment is the regime where `gpu_cache_usage_perc` sits above
the threshold on hot backends and evictions are continuous.

## What `gpu_cache_usage_perc` actually measures (vLLM v1 — critical)

**Finding 0 — the gauge was renamed and the scrape parsed nothing.** vLLM v1
renamed `vllm:gpu_cache_usage_perc` → `vllm:kv_cache_usage_perc`. Against
0.15.1, a probe's KV-usage sampler found **zero** occurrences of the old name
while the same endpoints served `/health`, and Ranvier's health scrape looked
for the same single name — leaving `gpu_cache_usage_percent` at its 0.0
default, i.e. `residency_weight` broadcast a constant **1.0** in every
benchmark to date. The scrape now accepts both names (`health_service.cpp`);
the wrapper's sampler matches both and records `nomatch` rows when an endpoint
responds without either, so a blind-signal run is called out as INVALID
instead of read as "no pressure". **When running from a branch that carries
this C++ fix, pass `--build-image`** — otherwise bench.sh reuses a stale
`ranvier:latest` (or pulls GHCR, which tracks main) and the servers won't have
the parser. Confirm the live name on your vLLM build with:
`curl -s localhost:8000/metrics | grep -iE '^vllm:.*(cache|usage)'`.

**Empirical finding (2026-06-11, Lambda 8x A100-40GB, vLLM 0.15.1):** the
engine log printed, after minutes of heavy prefix traffic,

```
Running: 0 reqs, Waiting: 0 reqs, GPU KV cache usage: 0.0%, Prefix cache hit rate: 91.2%
```

i.e. in vLLM v1 the `vllm:gpu_cache_usage_perc` gauge counts **only blocks
held by RUNNING requests**. Freed-but-cached prefix blocks are reclaimable and
count as *free* — a cache physically full of warm prefixes reads 0% when idle.

Consequences for this benchmark:

- The residency signal (`residency = 1 − usage`) crosses the 0.2 threshold
  only when **active in-flight demand** (prefill + decode tokens of running
  requests) exceeds ~80% of KV capacity — not when the prefix cache is "full".
  Usage >80% does still imply rampant prefix-cache eviction (almost no
  reclaimable blocks survive), so the signal is *directionally* sound, but it
  fires far later than the "cache 80% populated" reading of the design doc.
- Workload knobs that grow the *dormant* working set (`--churn-active`,
  universe size) shape eviction realism and stale-hit frequency but **do not
  move the gauge**. Only live load does.
- The gauge is instantaneous and the health scrape samples it every 5s, so
  the router sees a noisy series that dips to ~0 between batches. Expect the
  downgrade to fire stochastically during pressure bursts, not continuously.
- **Spiky pressure does not fire** (measured: probe peak 88.6% with mean 7.8%
  and zero downgrades). Prefill holds blocks for an instant; a residency
  entry stays "cold" only ~5s until the next scrape rewrites it, so a
  transient spike must coincide with a scrape AND an ART hit on that backend.
  Two harness mitigations: the wrapper reports the **pressure shape**
  (samples over the firing line + longest streak) so spike-vs-sustained is
  visible, and the benchmark compose sets
  `RANVIER_HEALTH_VLLM_METRICS_TIMEOUT_MS=1000` — the shipped 200ms scrape
  timeout is exceeded by vLLM's Python `/metrics` endpoint exactly at busy
  instants, censoring the signal when it matters (watch the
  `vllm_scrapes failed` line in the leg counters). The cure for spikiness is
  **sustained decode load**: `--users 60 --max-tokens 400` — decode holds KV
  blocks for seconds, so usage plateaus instead of pulsing.
- **Feature-level open question (flagged for human decision, not changed
  here):** if the intent is "divert when the *prefix* was likely evicted", a
  better signal source than v1 usage would be the prefix-cache hit counters
  (`vllm:gpu_prefix_cache_{queries,hits}`) or per-prefix push eviction events
  ([push-cache-eviction.md](push-cache-eviction.md)). This benchmark measures
  the mechanism **as shipped**.

## Inducing real pressure (sizing guide)

Per the above: to make residency fire, size KV capacity *down toward live
demand* (or push demand up toward capacity). The wrapper samples the gauge
during every leg (`vllm_usage_samples.csv`) and, when a probe fails, prints a
**measured `--gpu-mem-util` suggestion** computed from this run's numbers — so
one failed probe yields the corrected setting, no guesswork.

The arithmetic it uses (also good for picking a starting point):

```
capacity_tokens   = KV_GiB / kv_bytes_per_token        (vLLM logs both at startup:
                                                        "Available KV cache memory" /
                                                        "GPU KV cache size: N tokens")
non_KV_GiB        = VRAM × util − KV_GiB               (weights + activations + graphs)
in_flight_tokens  ≈ concurrent_reqs_per_backend × avg_prompt_tokens
suggested_util    = (non_KV_GiB + in_flight_GiB / 0.85) / VRAM
```

Reference config (8x A100-40GB, Llama-3.1-8B, 30 users): non-KV measured
≈16.6 GiB/GPU; KV per token 128 KiB (GQA, 32 layers × 8 KV heads × 128 dim).
At util 0.80 capacity is ~126k tokens vs ~12–22k tokens of live demand — usage
peaks far below 80%, residency can never fire (this is exactly what the first
hardware probe measured). The firing region is **util ≈ 0.47–0.50**, hence the
wrapper's default of 0.50.

Levers, in order of preference:

1. **`--users` + `--max-tokens` together** (e.g. `--users 60 --max-tokens
   400`): the *sustained*-pressure recipe. Decode holds blocks for seconds —
   long decodes overlapping across many users produce a usage plateau; short
   outputs produce only prefill spikes that the 5s scrape misses.
2. **`--gpu-mem-util`** (default 0.50): sizes capacity toward demand. Keep
   enough KV for one `--max-model-len` sequence or vLLM refuses to start
   (8192 tokens of 8B GQA = 1.0 GiB; 13B MHA = 6.4 GiB — 13B needs much
   gentler clamping).
3. **Prefix sizes** (`LARGE_PREFIX_MIN_TOKENS`, `--prefix-max-tokens`).

`--churn-active` / universe stay workload-shape knobs: keep them for eviction
realism, don't use them to force firing. And avoid raising the threshold
(e.g. 0.5) to force firing — that changes the decision rule under test.

## Procedure

Everything is wrapped in **`scripts/bench-residency-ab.sh`** (run from the
repo root on the GPU box, `HF_TOKEN` exported):

```bash
# 1. Pressure probe (~8m + model load): verifies downgrades actually fire.
#    --build-image rebuilds ranvier:latest from this checkout — required while
#    the dual-name cache-usage parser fix lives on this branch.
./scripts/bench-residency-ab.sh --probe-only --build-image

# 2. Full A/B (probe again, then OFF and ON legs, 30m each + warmup):
./scripts/bench-residency-ab.sh --build-image

# Or in one go with explicit knobs (example for an 8x A100-40GB box):
./scripts/bench-residency-ab.sh \
  --model meta-llama/Llama-3.1-8B-Instruct \
  --duration 30m --users 30 --gpu-mem-util 0.50 \
  --churn-universe 200 --churn-active 24
```

The wrapper defaults `--max-model-len 8192` (the workload's 8k max prefix):
a 128k-native model like Llama-3.1 cannot fit one native-length sequence in a
deliberately-small KV cache, and vLLM refuses to start without the clamp.

What the wrapper does per leg (manual equivalent):

```bash
# OFF leg
./scripts/bench.sh --model <MODEL> --duration 30m --users 30 \
  --gpu-mem-util 0.50 --max-model-len 8192 --prompt-dist churn --warmup \
  --no-load-aware --cache-residency-threshold 0.0 --output-dir <ROOT>/off

# ON leg — identical except the threshold
./scripts/bench.sh ... --cache-residency-threshold 0.2 --output-dir <ROOT>/on

# Compare the two prefix legs (off = baseline, on = new)
python3 tests/integration/results_parser.py compare \
  <ROOT>/off/*_prefix/benchmark.log <ROOT>/on/*_prefix/benchmark.log
```

Design choices baked into the wrapper:

- **Load-aware routing OFF in both legs (default).** Residency and load-aware
  are independent diversion mechanisms; a backend with a full cache is often
  also load-hot, so load-aware would divert some of the same requests in the
  OFF leg and dilute the measured delta. `--no-load-aware` isolates residency.
  Run a second pair with `--with-load-aware` for the production-shaped net
  effect; report it separately.
- **Each leg cold-starts vLLM + 1m warmup** — legs are symmetric by
  construction (bench.sh tears everything down on exit).
- **A KV-usage sampler runs alongside every leg** (5s cadence, same gauge and
  cadence as the router's health scrape), leaving
  `<leg>/vllm_usage_samples.csv` as the pressure record. Probe failures report
  the sampled peak and a measured `--gpu-mem-util` suggestion.
- **30m minimum duration** at ≥20 users (shorter runs can't separate steady
  state from warm-up; established floor from the May 2026 investigations).
- **`--with-rr-baseline`** optionally adds a round-robin leg per run
  (bench.sh `--compare`) to anchor against environmental drift, at 2x runtime.

## Validity gates (all enforced/reported by the wrapper)

A run is quotable only if **all** of these hold:

1. **Probe:** `residency_route_downgrades_total > 0` before committing to the
   A/B (otherwise tune pressure and re-probe).
2. **Banner check:** each leg's "Effective Routing Config" banner shows the
   intended `RANVIER_CACHE_RESIDENCY_THRESHOLD` (and load-aware setting).
3. **OFF leg:** `residency_route_downgrades_total == 0` (toggle actually off).
4. **ON leg:** `residency_route_downgrades_total > 0` during the measured run
   (ideally ≥ ~1% of total requests; a handful of downgrades is noise).
5. **Signal plumbing both legs:** `gossip_cache_states_received_total > 0`
   and `residency_cache_size > 0`. (Sanity cross-check: with 3 nodes,
   `received` ≈ 2 × `sent` per node — each node's broadcasts reach 2 peers.)
6. **Pressure regime held:** sampled KV usage peak (in
   `vllm_usage_samples.csv`) above `100×(1 − threshold)%` during the ON leg —
   recorded in `ab-summary.txt`.
7. **No thermal confound:** `clocks_throttle_reasons.active` is
   `0x0000000000000000` in `nvidia_smi_{start,end}.csv` for both legs.
8. **Comparable error rates:** incomplete/timeout counts within ~1pp of each
   other — a leg drowning in timeouts invalidates its tail percentiles.

## Metrics to capture (per leg)

| Metric | Source | Why |
|--------|--------|-----|
| TTFT p50 / p99 | Locust (`benchmark.log`, parser) | headline client-side latency |
| TTFT p99 cache-hit / cache-miss split | Locust custom metrics | residency converts believed-hits-that-miss into honest misses; the hit tail should clean up |
| Client cache hit rate | Locust (prefix→backend tracking) | residency-on trades some hits for diverts; quantifies the cost side |
| `ranvier_router_residency_route_downgrades_total` (+ % of requests) | `prometheus_metrics.txt` | how often the feature fired |
| `ranvier_gossip_cache_states_sent_total` / `received_total` | `prometheus_metrics.txt` | signal plumbing health |
| `ranvier_router_residency_cache_size` | `prometheus_metrics.txt` | residency cache populated |
| `ranvier_routing_load_aware_fallbacks_total` | `prometheus_metrics.txt` | must be 0 with `--no-load-aware` (confound check) |
| `ranvier_health_vllm_scrapes_{success,failed,suppressed}` | `prometheus_metrics.txt` (printed in leg counters) | failed ≫ 0 = the /metrics fetch timed out at busy instants — the signal was censored |
| Incompletes / timeouts, per-backend distribution | parser report | validity gate 7, concentration effects |
| `vllm:gpu_cache_usage_perc` over the run | sampled automatically into `<leg>/vllm_usage_samples.csv` by the wrapper | proof the pressure regime held (vLLM v1: gauge = running-request blocks only) |

## Recording results

Measured results live in the **Results** section at the top of this document.
For repeat runs: fill from `ab-summary.txt` / `REPORT.md` produced by the
wrapper, and **do not quote any number until the validity gates pass**.

## Interpreting the outcome

Frame the result correctly: given the v1 usage semantics, the regime where
residency fires is **active-demand saturation** (backends near KV capacity,
prefix-cache eviction rampant, possible preemptions). The A/B answers "does
diverting stale ART hits away from saturated backends help TTFT *in that
regime*" — it says nothing about lighter regimes, where the shipped signal
simply never fires.

- **ON improves hit-tail / overall P99 without a worse miss tail** — residency
  is paying for itself: stale hits (full prefill billed as "hits") are being
  diverted to backends that can serve them faster.
- **ON ≈ OFF on everything despite downgrades firing** — the stale-hit penalty
  and the cold-divert penalty cancel at this pressure; the feature is neutral
  here. Report the downgrade rate alongside.
- **ON is worse** — the cold-divert cost dominates: a downgraded request pays
  full prefill *somewhere else* plus loses any partial-prefix reuse it might
  still have had on the original backend (vLLM evicts block-wise, so "cache
  >80% full" does not guarantee *this* prefix is gone). That outcome argues
  for a smarter eviction model or per-prefix signals (e.g. the
  [push cache eviction](push-cache-eviction.md) path), not threshold tuning.

A known structural confound either way: a residency downgrade **is itself a
cold divert** (same family as the #442 cost). The benchmark measures the net.
