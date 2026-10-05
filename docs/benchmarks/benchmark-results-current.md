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

**Consequences.** (1) The remedy was expected to be in dispatch: when an anchor is over cap,
divert to the *least-loaded* under-cap candidate rather than the first hash probe. **Tested the
same day and it did not hold; see the acceptance section below.** (2) Leg V1 as written sweeps
ε *looser* (0.5), which would strand more; the informative sweep is *tighter* (0.1). (3) The 13B
20-user row remains the regression test for whichever fix lands: a consistent improvement
there, on the fitted set, is the acceptance criterion. Archive:
`docs/benchmarks/results/2026-10-02-fitted/`.

### Least-loaded diversion: acceptance run (2026-10-02, same box, ❌ failed)

Branch `claude/sleepy-lamport-wxly0b` (commit `f47c998`) changed `bounded_load_select` and the
scorer tie order so an over-cap anchor diverts to the least-loaded candidate instead of the first
under-cap jump probe. Both binaries ran the fitted suite on the same 8×A100 instance the same
evening (old binary first, by accident of a stale `ranvier:latest`; the run was kept as the
control). Three repeats per row, alternating arm order, all arms valid.

| Config | Binary | P99 TTFT per repeat | Verdict | P50 TTFT | Diverts | Prefix-arm Gini per repeat |
|--------|--------|---------------------|---------|----------|---------|----------------------------|
| 13B 10u | old (first-under-cap) | −10.7, +7.6, +0.8 | mixed | −27% ×3 | 29–33% | 0.103, 0.081, 0.114 |
| 13B 10u | new (least-loaded) | −11.3, +8.3, −10.8 | mixed | −27% ×3 | 33% ×3 | 0.084, 0.098, 0.091 |
| 13B 20u | old (first-under-cap) | +11.7, +11.0, +3.9 | ❌ consistent regression | −24% ×3 | 29–33% | 0.075, 0.103, 0.105 |
| 13B 20u | new (least-loaded) | **+10.5, +24.1, +17.5** | ❌ consistent regression | −24% ×3 | 29–30% | 0.049, 0.075, 0.063 |

Round-robin arms: Gini 0.021–0.043 throughout. KV hit rates were unchanged between binaries
(10u ≈ 17% → 47–64%; 20u ≈ 11% → 31–44%).

**What the acceptance run settles.**

- **Where diverts land does not set the tail.** The new rule tightened the 20-user prefix-arm
  spread in two of three repeats (Gini 0.049/0.063 vs 0.075–0.105) and P99 did not move; in
  the one repeat where the spread stayed at the old level, P99 was the worst of the day. The
  "stranded capacity" reading of the fitted suite is therefore not the whole mechanism: the
  completion-count imbalance is a symptom that can be removed without touching the tail.
- **The tail is queueing on both hits and diverts.** At 20 users the prefix arm's
  route-consistent P99 was better than round-robin's in one repeat (−15%) and worse in two
  (+19%, +48%); route-changed P99 was worse in all three (+22% to +25%). Cache hits are faster
  at the median in every repeat (−3% to −5%) and slower at the tail in most.
- **"Load" in this deployment is not queue depth.** Three Ranvier nodes × 8 shards = 24
  shards with `RANVIER_CROSS_SHARD_LOAD_SYNC=false`: each shard sees ~1/24 of the in-flight
  requests, so the in-flight term of the composite load is almost always 0. The value the
  bounded-load cap and the least-loaded pick actually read is `gpu_load_weight` (10) × the
  scraped vLLM score (0.7 × queue pressure + 0.3 × KV usage) plus `capacity_headroom_weight` (5)
  × KV usage, refreshed by the 5 s health scrape and identical on every shard of a node. The
  shard-0 counters confirm it: 132/151 and 156/175 load diverts in the first two new-binary
  prefix arms (~88%) carried a GPU score at decision time. Consequences: diverts fire at ~30% of
  requests at every concurrency because the trigger is scraped KV pressure, not a queue; and
  "least loaded" is the same backend for every shard of a node for 5 s, so the new rule herds
  diverts where the old rule stranded a backend. Same P99 cost, different shape.
- **The fix is held on the branch, not merged.** The tooling changes beside it (stale-image
  refusal, `--build-image`, manifest `server_image`, load-signal knobs in the compose file)
  stand on their own.

**Next legs (configuration only, 20-user row ×3 each, ~80 min each):**

- **A. No-divert control** (`RANVIER_LOAD_AWARE_ROUTING=false`): pure affinity. If this is
  also ≈+10%, the regression is affinity concentration itself (16 hot prefixes pinned onto 8
  backends) and no divert-target rule can fix it; the levers are ε or a prefix-spread policy.
- **B. In-flight signal** (`RANVIER_ROUTING_GPU_LOAD_WEIGHT=0 RANVIER_CAPACITY_HEADROOM_WEIGHT=0
  RANVIER_CROSS_SHARD_LOAD_SYNC=true`): diverts triggered and targeted by node-local in-flight
  counts. If diverts stop being the tail, the signal was the problem and least-loaded is the
  right target; a fleet-wide in-flight view (gossip) is then the code change. If the tail
  persists, randomise the divert target among under-cap candidates (herd-breaker).

Raw runs were **not archived**: the instance was terminated on 2026-10-03 before the copy
step ran, so every number in this and the following sections is transcribed from the per-run
`compare_*.txt` files as they were read during the campaign. Manifests for later campaigns carry
`server_image`, so the binary behind a run is identifiable from now on.

### Leg A, no-divert control (2026-10-02/03): the regression is placement, not diversion

Same box, same binary, 13B 20 users on the fitted set, `--no-load-aware
--cache-residency-threshold 0.0`: zero load diverts, zero residency downgrades, 97% route
consistency. Three repeats, alternating arm order.

| Rep | P99 TTFT | P50 TTFT | KV hit RR → prefix | Throughput | Prefix-arm Gini | Busiest / idlest backend |
|-----|----------|----------|--------------------|------------|-----------------|--------------------------|
| 1 (rr-first) | **+8.2%** | −28.5% | 11 → 55% | +6.5% | 0.305 | b7 684 (23%) / b5 18 (0.6%) |
| 2 (prefix-first) | **+12.3%** | −28.5% | 14 → 61% | +7.2% | 0.295 | b7 669 (22%) / b5 20 (0.7%) |
| 3 (rr-first) | **+11.3%** | −28.9% | 14 → 56% | +4.9% | 0.296 | b7 674 (23%) / b5 18 (0.6%) |

**What leg A settles.** With every divert mechanism off, prefix affinity regresses P99 by the
same 8–12% as it did with either divert policy. The cause is hash placement itself. The
workload picks one of 16 prefixes uniformly; by hash they landed 4/3/2/2/2/1/1/0 on the eight
backends (the completion shares read it straight off: 23/18/14/14/11/8.5/6.5/0.6%), the same
placement in all three repeats because the hash is deterministic. The backend holding four
prefixes runs at twice the fleet's mean concurrency all run long, and P99 TTFT is the busiest
backend's queue. Round-robin keeps every backend at the mean. So affinity pays at the tail
exactly what it gains at the median: P50 −28%, throughput +5–7%, P99 +8–12%. Both divert
policies failed because they were treating the symptom (completion imbalance) downstream of
the cause (where prefixes are placed), on a signal that was not queue depth anyway.

**Fix under test: least-loaded cache-miss placement** (`routing.miss_placement: least_loaded`,
same branch). A miss has no cache to preserve, so the new prefix goes to the live candidate
holding the fewest learned-route tokens (then fewest routes, then lowest load, then probe
order); 16 uniform prefixes land two per backend by construction and hits are untouched.
First run (count-weighted, 2026-10-03, 20u rep 1): +6.6% P99, Gini 0.076, 203 misses placed
off their hash bucket — the ~70 short one-off prompts the stress mix learns as routes outvoted
the 16 long prefixes in a count tally, so the placement is now token-weighted. Acceptance:
`bench-runner.sh --suite placement` — the fitted 20u row turns negative with the prefix arm's
Gini near round-robin's (≤0.05) and P50 unchanged. Leg B (in-flight load signal for the
divert policy) is still informative but no longer decides the design. Known limit: this
balances prefix count, not popularity; a hot prefix carrying a quarter of the traffic will
need replication across backends, which is the next item.

### Leg B, in-flight load signal (2026-10-03): the divert policy with a real queue signal

Same row, default placement, `RANVIER_ROUTING_GPU_LOAD_WEIGHT=0 RANVIER_CAPACITY_HEADROOM_WEIGHT=0
RANVIER_CROSS_SHARD_LOAD_SYNC=true`, so bounded-load reads node-local in-flight requests instead
of the 5 s-stale scraped score. Three repeats scheduled; repeat 2 died two minutes in when a
per-arm `docker compose up` failed silently under `set -e` (fixed in `bench.sh` the same day).

| Rep | P99 TTFT | P50 TTFT | Route consistency | KV hit (prefix) | Diverts | Prefix-arm Gini |
|-----|----------|----------|-------------------|-----------------|---------|-----------------|
| 1 | +14.2% | −20.0% | 39.1% | 35.6% | 31.8% | 0.042 |
| 3 | **−1.0%** | −22.1% | 41.5% | 27.0% | 28.9% | 0.061 |

Verdict over two: mixed. Repeat 3 is the first 20-user prefix arm of the campaign whose tail
matched round-robin's, and it paid for it in affinity (consistency 41% vs the default's 48–53%,
KV hits 27% vs 36–44%, P50 −22% vs −25..−28%). The divert rate did not move: at ~0.8 in-flight
requests per backend per node, `bounded_load_epsilon` 0.25 gives a cap of 1–2, so nearly any
load at all diverts. Takeaways: a real-time signal lets diversion do its job (pull toward
round-robin's balance when a backend queues), epsilon 0.25 is uncalibrated for small-integer
loads, and the default scraped signal is not queue depth.

### Least-loaded placement, count-weighted (2026-10-03): ❌ not accepted; two defects found

`--suite placement` on the first placement build (`b66fa80`: fewest learned **routes**, then
load, then probe order), 13B 20 users ×3. The 10-user rows ran but were not read before the
instance was terminated.

| Rep | P99 TTFT | P50 TTFT | Route consistency | KV hit (prefix) | Diverts | Prefix-arm Gini | Misses placed off hash bucket |
|-----|----------|----------|-------------------|-----------------|---------|-----------------|-------------------------------|
| 1 (rr-first) | +6.6% | −23.7% | 41.6% | 32.0% | 24.9% | 0.076 | 203 |
| 2 (prefix-first) | +25.1% | −19.7% | 36.7% | 28.7% | 23.2% | 0.104 | — |
| 3 (rr-first) | +13.3% | −20.9% | 37.1% | 30.1% | 24.3% | 0.080 | — |

Consistent regression, and **worse affinity than the default** (consistency 37–42% vs 47–53%,
KV hits 29–32% vs 36–44%) in all three. Two defects, both fixed on the branch and pending a
rebuild (`d8bdde4`, `5769f4e`):

1. **Count-weighting.** 203 misses were placed, not 16: the stress mix is 30% short/medium
   one-off prompts, each learned as a route, so ~70 one-off routes outvoted the 16 long
   prefixes in a per-backend route count and the long prefixes landed almost as unevenly as
   by hash (Gini 0.08–0.10). Fix: weigh routes by key length in tokens
   (`RadixTree::route_tokens_by_backend`); a 3000-token prefix outweighs thirty 100-token
   one-offs.
2. **Divergent placement.** Hash placement needs no coordination; least-loaded placement is a
   local decision, and routes were learned only at first byte, ~1 TTFT after dispatch. In that
   window other shards and nodes placed the same new prefix elsewhere and each kept its LOCAL
   route, so prefixes were warm on two or three backends and hits were split — the consistency
   and KV-hit drop. First fix: under `least_loaded` the controller also learns the placed miss
   at dispatch, shrinking the window to one 20 ms route-batch flush plus gossip.

### Least-loaded placement v2, token-weighted + eager learn (2026-10-03): ❌ split persists

Fresh instance, GHCR image from main (`ba00cb9`), `--suite placement`, 13B 20 users.

| Rep | P99 TTFT | P50 TTFT | Route consistency | KV hit (prefix) | Diverts | Prefix-arm Gini | Gossip trust refusals (shard 0, 3 nodes) |
|-----|----------|----------|-------------------|-----------------|---------|-----------------|------------------------------------------|
| 1 (rr-first) | +11.4% | −20.6% | 37.9% | 29.8% | 22.4% | 0.048 | 860 |
| 2 (prefix-first) | +12.5% | −23.2% | 40.8% | 31.0% | 23.3% | 0.074 | 781 |
| 3 (rr-first) | +3.9% | −24.1% | 41.5% | 37.6% | 24.2% | 0.050 | 800 |

Consistent regression (+11.4, +12.5, +3.9; median +11.4, the mildest set of the campaign).
Balance improved (Gini 0.048–0.074 vs 0.08–0.10) but affinity did not recover: consistency
and KV hits are mostly where the count-weighted build left them. Arithmetic: default arms divert ~30%
and are ~50% consistent, so ~20% of requests change backend for other reasons (mostly first-seen
one-offs); v2 arms divert 23% and are 39% consistent, leaving 38% — the extra ~18 points are
pool prefixes served from more than one backend. The 20 ms intra-node window cannot produce
that; the cross-node path can. **Confirmed by the counter:** ~800 gossip REMOTE routes per arm
were refused on shard 0 alone because a LOCAL route to a *different* backend already held the
prefix (`router_remote_routes_trust_refused_total`; ~0 expected under hash placement, where
every node computes the same bucket). Two nodes that place the same new prefix differently each
keep their own LOCAL route forever under the trust ladder (invariant T7), so the prefix stays
warm on two or three backends and its hits are split for the whole run.

**10 users, same build (reps 1–2 of 3; rep 3 pending):**

| Rep | P99 TTFT | P50 TTFT | Route consistency | KV hit (prefix) | Diverts | Prefix-arm Gini | Large-hit P99 |
|-----|----------|----------|-------------------|-----------------|---------|-----------------|---------------|
| 1 (rr-first) | +0.9% | −28.1% | 54.6% | 59.2% | 26.0% | 0.128 | +8.7% |
| 2 (prefix-first) | −6.9% | −28.1% | 51.3% | 64.0% | 27.8% | 0.091 | +23.4% |

Mixed, like every 10-user set of the campaign (fitted default −7.3/−5.8/−0.1, leg B −11.3/+8.3/−10.8):
at ~1 in-flight per backend the queue that sets P99 at 20 users is mostly absent, so placement has
little to fix and these rows say nothing about it either way. Affinity is **not** split at 10
users (consistency 51–55%, KV 59–64%, both at the default-hash 10u level of 47–64%), which fits
the mechanism: with half the arrival rate a new prefix is far less likely to reach two nodes
inside the same first-byte window. Balance is no better than hash at this load (Gini 0.09–0.13);
the ~70 one-off routes dominate placement when only 16 pool prefixes are in play. In both reps the
large-prefix *hit* P99 is worse (+8.7, +23.4%) while miss P50 improves, the same shape as at 20
users: the hits that queue behind a hot backend are the tail.

**Second fix (branch, pending rebuild): converge conflicting routes by lowest backend id.**
Under `least_loaded`, a gossiped REMOTE route that meets a LOCAL or REMOTE route to a
different backend is settled by a total order — the lower backend id wins — on every node
alike (`RadixTree::insert_if_trusted(..., converge_local_conflicts)`). Both sides apply the
same rule, so the cluster converges on one backend and nothing flaps; PUSH routes are never
touched; the default (hash) path keeps the plain trust ladder. Counter:
`router_remote_routes_converged_total`. The tell on the next run: trust refusals near zero,
the converged counter in the tens (once per conflicting prefix per node, not per
re-announcement), route consistency back near 48%, KV hits near 40%.

### Least-loaded placement v3, + gossip convergence (2026-10-03): split fixed, balance not

Same instance, GHCR image from main (`48471f4`, convergence commit verified inside the image),
`--suite placement`, 13B 20 users.

| Rep | P99 TTFT | P50 TTFT | Route consistency | KV hit (prefix) | Diverts | Prefix-arm Gini | Busiest / quietest backend |
|-----|----------|----------|-------------------|-----------------|---------|-----------------|----------------------------|
| 1 (rr-first) | +8.1% | −26.6% | **56.5%** | **47.7%** | 26.4% | **0.113** | b5 555 / b8 267 (18.5% / 8.9%) |
| 2 (prefix-first) | +10.1% | −26.6% | **59.1%** | 42.4% | 25.3% | **0.106** | b5 474 / b8 250 (15.9% / 8.4%) |
| 3 (rr-first) | +3.4% | −25.7% | **58.2%** | **46.7%** | 26.8% | **0.090** | b7 480 / b5 263 (16.2% / 8.9%) |

**Verdict: ❌ not accepted** (+8.1, +10.1, +3.4; median +8.1). **Affinity is back and then
some:** consistency 56–59% and KV hits 42–48% are the best 20-user numbers of the campaign
(default hash ≈ 48% / 40%; v1–v2 ≈ 40% / 31%). The convergence rule did what it was built for.
**Balance is the worst of the campaign:** Gini 0.090–0.113 against 0.05–0.10 for every earlier
prefix arm, with the busiest backend taking 16–18.5% of requests and the quietest 8.4–8.9% in
every rep. The hot backend moves between reps (b5, b5, b7), so this is a placement draw, not a
hot prefix. P99 is that one queue again, and the three P99 deltas track the three Ginis.

**10 users, same build:**

| Rep | P99 TTFT | P50 TTFT | Route consistency | KV hit (prefix) | Diverts | Prefix-arm Gini | Busiest / quietest backend |
|-----|----------|----------|-------------------|-----------------|---------|-----------------|----------------------------|
| 1 (rr-first) | **−13.2%** | −29.1% | 57.2% | 66.0% | 25.3% | 0.083 | b1/b6 223 / b5 132 (13.8% / 8.2%) |
| 2 (prefix-first) | −3.0% | −28.3% | 55.2% | 66.1% | 27.2% | 0.118 | b1 280 / b5 133 (17.6% / 8.3%) |
| 3 (rr-first) | +0.4% | −28.8% | 55.5% | **69.2%** | 28.8% | 0.141 | b5 284 / b2 138 (17.7% / 8.6%) |

Verdict: ⚖️ no reliable effect on P99 (−13.2, −3.0, +0.4), the campaign's best 10-user P50 and
KV figures (−28 to −29%, 66–69%), and the worst 10-user balance (Gini 0.08–0.14, the busiest
backend about twice the quietest in every rep; hot backend b1/b6, b1, b5). The three P99 deltas
again track the three Ginis. At 10 users the hot backend's queue is short enough that affinity
wins or breaks even; at 20 it does not. Same mechanism, different regime: the placement draw is
the only thing between a clean 10-user win and a +8% 20-user tail. The large-prefix *hit* P99 is
worse in all three (+6.3, +18.4, +15.9%): the hits queuing behind the hot backend are the tail.

**Counters (prefix arm of rep 3, shard 0 of each node):** `router_remote_routes_trust_refused_total`
219 / 222 / 247, `router_remote_routes_converged_total` **0 / 0 / 0**,
`router_miss_placements_rebalanced_total` 34 / 36 / 27. Node 1 resident routes per backend (all
shards, all depths, one-offs included): b6 88, b7 72, b4 56, b2 56, b5 48, b1 32, b8 24, b3 24.

**The convergence rule never fired.** Zero moves on every node while a quarter of v2's refusals
remained means every conflict was being settled the other way round. Cause, found in the code:
`apply_local_batch_to_tree` inserted this node's own learns with the plain `insert` — latest
wins, no trust check, no convergence. A placed miss is learned at placement but *lands* at the
next 20 ms flush, on all eight shards; if a peer's lower-id route had arrived by gossip in that
window, the flush silently moved the prefix back to the higher id, announced it, the peer refused
the announcement (that is the 220), and since the peer never re-announces, the split was
permanent. The 57% consistency (up from 40%) came from the `learn_route_global` guard alone
(first-byte learns stopped re-opening conflicts); the remaining split is the flush. The route
counts above cannot say whether the leftover Gini is placement or popularity (one-off prompts are
routes too), so the token tally is now exported per backend.

**Fourth fix (branch):** under `least_loaded` the local flush goes through the same lowest-id
rule (`insert_if_trusted(..., LOCAL, converge)`): a learn that meets a lower-id LOCAL or REMOTE
route is dropped, removed from the batch before cross-shard fan-out and gossip, and counted in
`router_local_routes_converged_total`; a lower-id learn still displaces a higher-id route. PUSH and
the hash default are untouched. New gauge `backend_resident_route_tokens` (per backend, sum over
shards) is the placement weight itself. Tell for v4: trust refusals ≈ 0, local-converged in the
tens, consistency ≥ 57%, and the token gauge even to within one prefix across backends. If the
tokens are even and traffic is still skewed, the residue is popularity (replication, BACKLOG
§27); if the tokens are uneven, placement during the warm-up burst is still blind (24 shard-local
tallies, gossip-interval window) and the post-hoc rebalance is the next step: once the tally has
converged, move the heaviest prefix off the fullest backend when the gap exceeds one prefix,
same deterministic rule on every node, one cache miss per move.


### Least-loaded placement v4, + local-flush convergence (2026-10-05): ❌ split gone, tally blind

Fresh instance, GHCR image from main (`f272e6c`, local-flush fix verified inside the image),
13B 20 users, custom run file (20u only). Rep 3 died 47 s in: vLLM on GPU 2 failed engine-core
initialization at startup (vLLM, not Ranvier; the runner does not retry a backend start).

| Rep | P99 TTFT | P50 TTFT | Route consistency | KV hit (prefix) | Diverts | Prefix-arm Gini | Busiest / quietest backend |
|-----|----------|----------|-------------------|-----------------|---------|-----------------|----------------------------|
| 1 (rr-first) | +20.9% | −25.7% | 56.9% | 42.1% | 24.6% | 0.075 | b8 432 / b3 294 (14.6% / 9.9%) |
| 2 (prefix-first) | +13.1% | −25.4% | 56.2% | 43.8% | 28.5% | 0.097 | b3 466 / b5 257 (15.7% / 8.7%) |

**Counters (prefix arms, shard 0 of each node):** `router_remote_routes_trust_refused_total`
**0 / 0 / 0** (both reps), `router_local_routes_converged_total` 25 / 19 / 21 and 25 / 25 / 28,
`router_remote_routes_converged_total` 0, `router_miss_placements_rebalanced_total` 25–39.
**The split is gone**: every conflict is now settled at the local flush, nothing is refused, and
the three nodes agree on every prefix. Consistency did not rise above v3's 57%, so there was no
remaining split to recover; what is left of the 43% "route-changed" is the 25–28% divert rate
plus first-seen misses and one-offs.

**Token gauge (node 1, `backend_resident_route_tokens` summed over 8 shards):** rep 1
5888 / 5248 / 4736 / 4480 / 4352 / 4224 / 4096 / 4096; rep 2 5376 / 5376 / 4480 / 4352 / 4352 /
4352 / 4352 / 4224. Per shard that is ~4600 tokens in total across ~50 routes, about 90 tokens a
route, and the per-backend values are all multiples of 128 ± a few one-offs. **Routes are keyed
on the first 128 tokens of the prompt, not on the 2000–4000-token system prefix.** Partial
tokenization stops near `prefix_token_length` (128), the system-message boundary lies past the
tokenized span and is never found, and `learn_route_global` truncates to 128. So a pool prefix
and a one-off prompt weigh the same, the token-weighted tally *is* the count-weighted tally, and
it was balanced to within one route-unit in both reps while traffic ran 1.5–1.8× between the
busiest and quietest backend. The placement weight is uncorrelated with load by construction.
That is the finding of the whole placement line: a single-home placement that balances anything
derived from the route table cannot balance this workload, because the route table does not know
which routes carry traffic. v1–v4 and the hash default all sit in the same +3…+21% band at 20u.

**Verdict for the placement line: stop.** The two fixes that were real (eager learn, gossip and
local-flush convergence) stay: they remove a genuine split and cost nothing under `hash`. The
knob itself is not a default. What would still move the 20-user row is (a) a placement weight the
route table can carry — the request's estimated prompt tokens or observed hits per route, which
means a gossip wire change so every node weighs the same — or (b) replication of the hot prefixes,
or (c) a divert policy with a live queue signal (leg B rep 3 was the one near-zero 20u prefix arm).
(c) is the only one that is an experiment rather than a feature: the combo leg below.

### Combo leg: split-free placement + live in-flight signal + ε 1.0 (2026-10-05)

Same box and image as v4, 13B 20 users, `RANVIER_ROUTING_GPU_LOAD_WEIGHT=0
RANVIER_CAPACITY_HEADROOM_WEIGHT=0 RANVIER_CROSS_SHARD_LOAD_SYNC=true` with
`--miss-placement least_loaded --bounded-load-epsilon 1.0` (manifest checked: all four recorded).
Bounded-load reads node-local in-flight requests instead of the 5 s-stale scraped score, and the
cap is twice the average rather than 1.25×.

| Rep | P99 TTFT | P50 TTFT | Route consistency | KV hit (prefix) | Diverts | Prefix-arm Gini | Large hit / miss P99 | req/s |
|-----|----------|----------|-------------------|-----------------|---------|-----------------|----------------------|-------|
| 1 (rr-first) | **−60.4%** (3712 → 1471 ms) | −28.9% | 52.7% | **68.9%** | 22.6% | 0.116 | −55.6% / −53.0% | +10.1% |
| 2 (prefix-first) | **−57.5%** (3745 → 1592 ms) | −28.7% | 52.5% | **70.8%** | 24.2% | 0.102 | −63.4% / −53.0% | +11.3% |

Two reps agree across both arm orders; rep 3 pending. This is the acceptance for the 13B 20-user
row: the first 20-user prefix arms of the campaign to beat round-robin's tail, by a margin no
other configuration came within 50 points of. Read with the earlier legs: the request distribution is
*still* uneven (Gini 0.116, busiest backend 17% of requests, the same placement draw as v1–v4),
yet the tail collapsed. Request count per backend was never the tail; queue depth was, and a
divert policy that sees the queue live and only acts at 2× the mean removes the queue without
removing affinity (consistency 53% against 57% for the no-divert placement arms, KV hits the
highest of any 20-user arm). Leg B rep 3 (−1.0%) was the same signal with ε 0.25 and hash
placement, diverting 29% of requests and giving back the affinity; ε 1.0 is the calibration
that was missing. The prefix arm also completed 10% more requests in the same 10 minutes, so
the closed-loop confound ran *against* this result.

**Resume checklist (if the box is still up, else next GPU session):**

1. Combo leg, 20u ×3: placement (no split) + live in-flight load signal + calibrated epsilon.
   `RANVIER_ROUTING_GPU_LOAD_WEIGHT=0 RANVIER_CAPACITY_HEADROOM_WEIGHT=0 RANVIER_CROSS_SHARD_LOAD_SYNC=true`
   with `--miss-placement least_loaded --bounded-load-epsilon 1.0`. Check the manifest shows
   `cross_shard_load_sync: true` and `gpu_load_weight: 0`. Read diverts (expect well under 25%),
   then P99. A negative ×3 here is the acceptance; anything else closes the 13B 20u row as "no
   reliable effect, mechanism known" (BACKLOG §27) and the remaining levers are features (a)/(b).
2. Archive every run directory into `docs/benchmarks/results/<date>-<leg>/` (compare files,
   aggregates, manifests, `prometheus_metrics_node*.txt`, `ranvier_node*.log`) **before**
   terminating the instance.

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

- **13B 20-user fitted regression: re-run `--suite placement` on the token-weighted,
  eager-learn build** (BACKLOG §27; resume checklist above). Leg A settled the mechanism as
  hash placement; the least-loaded diversion fix and the count-weighted placement build both
  failed their acceptance runs; leg B showed a real-time load signal helps diversion at the
  cost of affinity and that ε 0.25 is uncalibrated for small-integer loads.
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
