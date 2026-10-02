# Benchmark Results — Current Defaults

Only results measured on the **current shipped defaults** belong on this page. Every entry
must be stamped with the commit it was measured at and its run manifest (workload knobs +
routing config). When a default changes, prior entries move to
[history/benchmark-history-8xA100.md](history/benchmark-history-8xA100.md).

## Current defaults (the workload these numbers must be measured under)

| Axis | Current default | Source of truth |
|------|-----------------|-----------------|
| `NUM_LARGE_PREFIXES` | **50** (≥ backend count; not the deprecated 5) | `tests/integration/locustfile_real.py` |
| `SHARED_PREFIX_RATIO` | **0.9** | `tests/integration/locustfile_real.py` / `bench.sh --prefix-ratio` |
| Route-batch flush interval | **20ms** | `src/config_schema.hpp`, `docker-compose.benchmark-real.yml` |
| Load-aware routing | **ON**, `load_imbalance_factor` 2.0 / floor 2 | `src/config_schema.hpp` |
| Cache-residency weight | **0.2** | `src/config_schema.hpp` |
| Prompt distribution | `stress` (large-prefix) | `bench.sh --prompt-dist` |

> **Flush reconciliation:** 20ms is the shipped default. The historical guide contained a
> contradiction — an early section declared "10ms confirmed as the correct default," a later
> section changed it to 20ms, and the 20ms change shipped. Both dated sections are preserved
> in the history archive; **20ms is current.**

## Representative-workload headline (measured 2026-10-01, fixed tooling)

**Prefix-aware routing's P99 effect depends on whether the backends' KV cache can hold the hot
prefix set.** Where it can (Llama-3.1-8B on A100-40GB), prefix routing cut P99 TTFT by **17%**
against round-robin, every repeat agreeing, and lifted the real KV hit rate from 72% to 94%.
Where it cannot (CodeLlama-13B on the same cards: 11.6k tokens of KV per backend against a
~250k-token hot set), both arms run almost cache-cold under active preemption and the routing
policy's effect is small, unstable in sign across campaigns, and in one configuration a
consistent **+11% regression**. The July 2026 "monotonic in cluster throughput" story does not
survive: the 13B rows were measuring memory pressure, not routing.

First campaign on the tooling fixed by the 2026-09-30 audit: routing DB no longer carried across
arms, seeded prefix pool, exact raw-sample TTFT percentiles, HTTP-only request counts, route
consistency and vLLM KV hit rate reported, counters differenced over the run, arm order
alternated. Standard matrix on current defaults (50 prefixes, ratio 0.9, stress, load-aware
2.0/2 under `bounded_load` ε 0.25, residency 0.2, 20 ms flush), 8×A100 40GB, vLLM 0.15.1,
`--compare --warmup` ×3 via `bench-runner.sh --suite rebaseline`, 12/12 runs passed, 7h11m.
Verdict rule: CONSISTENT only when all three repeats share a sign; NO RELIABLE EFFECT when the
IQR of the per-repeat %change spans zero. Each run's commit, argv, effective config and KV
regime are in its `manifest.json`; the campaign's aggregates, manifests, compare files and
runner summary are archived under `docs/benchmarks/results/2026-10-01-rebaseline/`.

| Config | P99 TTFT, prefix vs RR (per repeat) | Verdict | KV hit rate, RR → prefix | KV per backend |
|--------|-------------------------------------|---------|--------------------------|----------------|
| **8B 20u/10m**  | **−17.0%** median (−17.0, −6.5, −17.4; IQR −17.2…−11.8) | ✅ consistent improvement, 3/3 | 72.1% → 94.4% | 142,144 tokens |
| **13B 30u/30m** | −2.4, −1.6, **+3.6** (IQR −2.0…+1.0) | ⚖️ no reliable effect | 5.3% → 14.4% | 11,648 tokens |
| **13B 20u/10m** | **+11.0%** median (+17.4, +11.0, +4.6; IQR +7.8…+14.2) | ❌ consistent regression, 3/3 | 3.9% → 19.0% | 11,648 tokens |
| **13B 10u/10m** | +12.2, **−10.8**, +6.1 (IQR −2.4…+9.1) | ⚖️ no reliable effect | 6.3% → 26.4% | 11,648 tokens |

KV hit rates are vLLM's own `prefix_cache_hits/queries` counters, token-level, differenced over
the main run, repeat 1 of each row. P99 TTFT is the exact percentile over every TTFT sample
(not Locust's approximated table). Absolute P99: ~870 ms RR vs ~730 ms prefix at 8B/20u;
~7.7 s both arms at 13B/30u.

**How to read the two models.**

- **8B is the regime where affinity can pay.** The 250k-token hot set does not fit in one
  backend's 142k-token cache but fits easily split eight ways. Round-robin is already 72%
  cache-warm; affinity recovers most of the remaining fifth of prefix prefill, and under a
  20-user queue that shows up as a sixth off the tail. This is the citable result.
- **13B is an eviction regime.** A backend's 11.6k tokens cannot hold its ~31k-token share of
  the set, and at 30 users in-flight requests alone exceed it; vLLM logged 216–277 preemptions
  per backend in 45 minutes. With both arms below 30% KV hits there is little for routing to
  recover, and P99 is governed by recompute storms. The 20-user regression is internally
  consistent and has a plausible mechanism: affinity concentrates large-prefix requests onto
  the same backends, and under KV pressure that concentration means more preemption there. The
  load-aware fallback watches in-flight counts, not KV occupancy, so it does not see it.
  Whether that holds is the question the `fitted` suite answers (below).

### Fitted suite (measured 2026-10-02): 13B inside its KV cache

`bench-runner.sh --suite fitted`: same fleet and router, 16 prefixes of 2000..4000 tokens
(~48k, ~6k per backend) so a backend's share fits beside in-flight requests. Six runs, 6/6
passed, three repeats per row, alternating arm order. Regime banner confirmed "fits when split
across backends but not in one" (11,680 KV tokens/backend vs 48k working set).

| Config | P99 TTFT per repeat | Verdict | KV hit rate, RR → prefix (rep 1) | Prefix-arm backend dist (min..max, Gini) |
|--------|---------------------|---------|----------------------------------|------------------------------------------|
| **13B 10u/10m, fitted** | −7.3, −5.8, −0.1 (IQR −6.6…−3.0) | ✅ consistent improvement, −5.8% median | 17.0% → 64.8% | 114..264, 0.151 / 132..292, 0.139 / 131..235, 0.119 |
| **13B 20u/10m, fitted** | +10.1, +8.2, +21.4 (IQR +9.1…+15.7) | ❌ consistent regression, +10.1% median | 14.2% → 44.9% | 237..388, 0.060 / 217..433, 0.096 / 238..431, 0.094 |

Round-robin arms in the same six runs: Gini 0.018–0.055, min..max within ±10% of the mean.
vLLM preemptions stayed in single and low double digits per backend throughout the 20-user
fitted runs (vs 216–277 per backend in 45 min on the default set at 30 users), so memory
pressure was largely relieved.

**What the fitted suite settles.**

- **Fitting the set restored the cache signal and turned the 10-user row from noise into a
  small consistent win** (KV hits 26% → 65%; P99 from ±12% sign-flips to −5.8% median). The
  13B mechanism works when the cache can hold the set; at 10 users the queue is short, so the
  tail benefit is single-digit, as the 8B story predicts.
- **The 20-user regression is not a memory-pressure artefact.** With the set fitting and
  preemptions low, the prefix arm still lost 8–21% on P99 while tripling its hit rate and
  completing ~9% more requests. More completions in a closed loop means the *mean* improved;
  the *tail* got worse.
- **The mechanism is stranded capacity, visible in every prefix arm.** All six prefix arms
  show one cold backend 35–45% below the mean (min 114–238 against means of ~190/~370) with the
  rest 5–20% above; every round-robin arm is flat. The arithmetic matches prefix assignment
  granularity: 16 prefixes learned onto 8 backends by first hit gives most backends two
  prefixes and at least one backend a single prefix. `router_service.cpp` bounded-load
  diversion probes the next consistent-hash buckets and takes the first candidate under cap
  (`bounded_load_select`): it pushes away from overloaded backends and never pulls toward the
  coldest one; with ε 0.25 the other seven may run 25% over average before any diversion
  fires, so a backend 40% under average is invisible to the policy. Load-aware fallbacks were
  30% of prefix-arm requests and still left the capacity stranded. At 10 users idle capacity
  is not the binding constraint, so hits win; at 20 users the fleet is queue-bound and a
  stranded eighth of it becomes tail latency. Real workloads have skewed prefix popularity and
  will produce the same effect without any benchmark artefact.

**Consequences.** (1) The remedy is in dispatch, not in cache sizing: when an anchor is over
cap, divert to the *least-loaded* under-cap candidate rather than the first hash probe, and
consider a pull rule for backends far below average. (2) Leg V1 as written sweeps ε *looser*
(0.5), which would strand more; the informative sweep is *tighter* (0.1). (3) The 13B 20-user
row is the regression test for whichever fix lands: a consistent improvement there, on the
fitted set, is the acceptance criterion. Archive: `docs/benchmarks/results/2026-10-02-fitted/`.

### Superseded: 2026-07-13 campaign (commit `817a1b5`)

Kept for the record; **do not cite.** The audit of 2026-09-30 found that these runs carried
the SQLite routing DB across arms on a host bind mount (every prefix arm started with the
previous run's routes), generated different prefix bytes per arm from an unseeded RNG, read
req/s from a Locust row inflated ~6× by derived samples, took P99 from Locust's approximated
table, and reported the client-side route-consistency proxy as "cache hit". Their verdicts
also used an IQR rule that at n=3 tolerated one contradicting repeat.

| Config | ~req/s (×6 too high) | P99 TTFT (median-of-3) | Verdict as recorded | "Cache hit" (= route consistency) |
|--------|---------------------:|------------------------|---------------------|-----------------------------------|
| 8B 20u/10m  | ~47 | −13.3% (IQR −15.6…−8.5) | reliable improvement | 12→48% |
| 13B 30u/30m | ~38 | −9.1% (IQR −13.2…−5.7)  | reliable improvement | 12→38% |
| 13B 20u/10m | ~28 | +3.8% (IQR −5.7…+8.0)   | no reliable effect   | 12→43% |
| 13B 10u/10m | ~16 | +29.0% (IQR +21…+34)    | reliable regression  | 12→49% |

Only the 8B row reproduced (and strengthened) on fixed tooling. The July reading that the
effect was "monotonic in cluster throughput" rested on the three 13B rows, which the KV data
now shows were measured in an eviction regime.

## Still open

- **Least-loaded diversion fix + re-run of the fitted 20-user row** as its acceptance test
  (BACKLOG §27). The fitted suite ran 2026-10-02 and localised the regression to stranded
  capacity in `bounded_load_select`; see the fitted section above.
- **Leg V1, epsilon** (`bench-runner.sh --suite epsilon`): the shipped file sweeps ε 0.5
  (looser). The fitted result says looser strands more capacity; sweep *tighter* (0.1) instead,
  on the fitted set, after the diversion fix. The factor/floor "threshold leg" (BACKLOG §25
  item 5) remains inert under `bounded_load`; `bench.sh` refuses it without `--hash-strategy jump`.
- **Four-arm design** (direct-to-vLLM, random, least-loaded without affinity, prefix): still
  the only way to separate affinity from load balancing and to measure Ranvier's own cost.
  Needs a least-loaded mode and a no-proxy arm in `bench.sh`.

## Adding an entry here

Each result must record: commit SHA, full `bench.sh` argv, the effective routing config
(the run banner), workload knobs (`NUM_LARGE_PREFIXES`, `SHARED_PREFIX_RATIO`, distribution),
GPU type/count, vLLM version, and — once the P1 machinery lands — median/IQR across repeats.
Do not hand-copy a single run's best number into a headline; that is the failure mode this
split exists to end.
