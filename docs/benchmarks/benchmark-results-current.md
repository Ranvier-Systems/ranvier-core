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
| Load-aware routing | **ON**, `bounded_load` ε **1.0** (was 0.25 until 2026-10-05), `load_imbalance_factor` 2.0 / floor 2 | `src/config_schema.hpp` |
| Divert load signal | **node-local in-flight count** (`cross_shard_load_sync` true, 100 ms); scraped GPU score and KV headroom weights **0** (were 10 / 5 until 2026-10-05) | `src/config_schema.hpp` |
| Cache-miss placement | **`least_loaded`** with eager learn and gossip + local-flush convergence (was `hash` until 2026-10-05) | `src/config_schema.hpp` |
| Cache-residency weight | **0.2** | `src/config_schema.hpp` |
| Prompt distribution | `stress` (large-prefix) | `bench.sh --prompt-dist` |

> **Routing defaults changed on 2026-10-05.** Every entry dated before that was measured under
> the previous divert signal and threshold (scraped GPU score + one shard's in-flight count,
> ε 0.25, hash placement). They stay on this page because they are the record of how the new
> defaults were arrived at, from the 2026-10-01 re-baseline through the placement line to the
> combo, isolation and confirmation legs. The full matrix under the shipped defaults at three
> repeats was measured on 2026-10-06 ("Standard matrix under the shipping defaults", below) and
> is the citable table; the 2026-10-01 table is the previous-defaults record.
>
> **Flush reconciliation:** 20ms is the shipped default. The historical guide contained a
> contradiction — an early section declared "10ms confirmed as the correct default," a later
> section changed it to 20ms, and the 20ms change shipped. Both dated sections are preserved
> in the history archive; **20ms is current.**

## Headline (2026-10-05): the 13B rows were the divert policy, not the affinity

Under the defaults shipped on 2026-10-05 (node-local in-flight load signal, divert only at twice
the mean, least-loaded placement with convergence) the configuration that had regressed in every
campaign, CodeLlama-13B at 20 users, improved P99 TTFT by **−60.4 / −57.5 / −55.2%** against
round-robin across three repeats and both arm orders, with P50 −28% and KV prefix hits 69–73%
(fitted 16-prefix set). One confirmation repeat each: 13B 10 users −34.5% (KV 82%), 8B 20 users
−22.6% (was −17%), 13B 30 users −16.4% in the eviction regime. The isolation leg (hash placement,
same signal) gave −51.8% median, so the divert policy is most of the win and placement the rest.
The request distribution across backends stayed uneven in every one of these arms; the tail was
queue depth, not request count. Full sections below: "Combo leg", "Confirmation rows",
"Isolation leg". *2026-10-06:* the standard 50-prefix matrix re-run at three repeats under the
shipping defaults improved every one of its twelve repeats: 8B 20u −26.7%, 13B 30u −14.1%,
13B 20u −21.0%, 13B 10u −38.8% (medians); the fitted suite at three repeats gave 13B 10u −28.6%,
20u −56.6%, 30u −42.0% with zero timeouts and +15% throughput at 30 users; next two sections.

## Standard matrix under the shipping defaults (measured 2026-10-06, release 2.2.0) — the citable table

`bench-runner.sh --suite rebaseline`, unflagged, image `ghcr.io/ranvier-systems/ranvier:2.2.0`
(commit `7925c8d` checked out on the host), fresh 8×A100 instance in a different region from the
2026-10-01..05 campaign (same instance type; the per-arm manifests record the GPU and KV capacity).
Same matrix, durations, user counts, prefix set (50 prefixes × 2000–8000 tokens, ratio 0.9) and
method (alternating arm order, per-arm warm-up, exact TTFT percentiles, vLLM KV counters) as the
2026-10-01 re-baseline. Only the routing defaults differ: ε 1.0, live node-local in-flight signal,
least-loaded placement. 12 of 12 configured runs completed; rep 2 of the 13B 20u row failed 47 s
into start-up (before vLLM was up) and was re-run by hand with the same arguments and arm order
after the suite, so every row has three repeats and both arm orders. Verdicts are the runner's
aggregate (median of per-repeat %change; CONSISTENT when all three share a sign).

| Config | P99 TTFT, prefix vs RR, per repeat | Median (IQR) | P50 TTFT | Throughput | KV hit, RR → prefix | Incompletes RR / prefix | Verdict |
|--------|------------------------------------|--------------|----------|------------|---------------------|-------------------------|---------|
| **8B 20u/10m** | −26.7, −17.7, −27.1 | **−26.7%** (−26.9…−22.2) | −0.7% | +1…+2% | 72–76% → 93–97% | 0 / 0 | ✅ consistent improvement, 3/3 |
| **13B 30u/30m** | −14.1, −17.9, −13.1 | **−14.1%** (−16.0…−13.6) | −18…−22% | +5…+6% | 5–6% → 17–21% | 1.3–1.5% / 1.4–1.8% | ✅ consistent improvement, 3/3; P99 of completed requests |
| **13B 20u/10m** | −18.4, −27.9, −21.0 | **−21.0%** (−24.4…−19.7) | −18…−24% | +5…+7% | 6–8% → 23–29% | 1.5–1.8% / 1.4–1.7% | ✅ consistent improvement, 3/3 |
| **13B 10u/10m** | −38.8, −46.8, −36.8 | **−38.8%** (−42.8…−37.8) | −7…−11% | +1…+4% | 6–8% → 26–41% | 1.7–1.8% / 1.0–1.9% | ✅ consistent improvement, 3/3 |

Against the 2026-10-01 table (same matrix, previous defaults): 8B −17.0% → −26.7%; 13B 30u
−1.6% (no reliable effect) → −14.1%; 13B 20u **+11.0%** (consistent regression) → −21.0%; 13B
10u +12.2 / −10.8 / +6.1 (no reliable effect) → −38.8%. The round-robin arms reproduce the
2026-10-01 box on every control (8B RR P50 400–402 ms, P99 842–892 ms; 13B KV hits 5–8%;
route consistency 12%), so the region change shows up nowhere in the baseline.

**What the counters say.** Diverts (`load_aware_fallbacks_total` / requests) fell from 30–49%
under the previous defaults to 15–26%: 8B 25–26%, 13B 30u 15–17%, 13B 20u 21–23%, 13B 10u
19–21%. Per-backend request Gini on the prefix arm is 0.09–0.16 against 0.01–0.04 for round-robin,
as in every leg of the campaign: affinity still concentrates requests, and the tail falls anyway.
Residency downgrades fired at 2.6% on the 30-user row (cache pressure present) and 0.5–1.6%
elsewhere. The hit buckets carry the win: Large/Xlarge *hit* P99 fell 20–53% in every 13B rep
while *miss* P50 was flat to +11% (an ART hit whose KV was already evicted lands on a warm, busy
backend). The 22 ms routing-decision P99 on every prefix arm is the boundary-detect tokenization
tail, unchanged since 2026-10-01 and about 3% of the 8B prefix arm's P99.

**Two caveats that belong next to the numbers.**

- *The 30-user row is still the eviction regime.* Both arms time out on more than 1% of requests,
  so the true P99 of either arm is the client timeout; the printed P99 is the 99th percentile of
  completed requests, and the prefix arm leaves 0.1–0.4 points more incomplete in all three reps.
  The figures that do not depend on that exclusion agree: P50 −18…−22%, 5–6% more requests
  served in the same 30 minutes. Cite this row as P50 and throughput with the P99 qualified, never
  as a bare P99. At 20 and 10 users the prefix arm timed out *less* in two of three reps each, so
  the extra timeouts are confined to full load, where the 2× cap lets a hot backend run a deeper
  queue while it is also evicting. The fitted 30u row (`fitted` suite, row 9) is the test of whether
  that persists when the cache fits; a tighter ε sweep (0.1) belongs there, not on the 10u rows.
- *Arm order matters where the cache fits.* vLLM's KV cache is not reset between arms, so the arm
  that runs second inherits the first arm's warm cache. On 8B the prefix-first rep was the weakest
  (−17.7 vs −26.7 / −27.1; RR KV hit 76.3% vs 72–73%, RR P99 843 vs 882–890 ms), exactly as on
  2026-10-01 (−6.5 prefix-first vs −17.0 / −17.4). In the 13B eviction regime there is nothing to
  inherit (RR KV hit 5.8% either order) and the prefix-first rep was the *strongest* at 20 users.
  With three repeats the median is always an rr-first value; report the range. Fix queued for the
  tooling pass: reset vLLM's prefix cache between arms and say so in the compare header.

**Tooling notes from this run.** The compare file's `Validation` column is an absolute gate (P99
over 5,000 ms fails the arm), not an incomplete-rate check: the 30u row fails both arms, the 10u row
passes both, and the 20u row reads FAILED → PASSED because the prefix arm pulled a 5.1 s tail under
5 s. `Xlarge Hit P50` printed `N/A` for the round-robin arm in five compares with 82–659 samples;
the value is `None` in that arm's per-bucket stats and the cause is not yet traced. The runner
summary's per-arm "improv" column is the locust hit-vs-miss improvement, not the A/B result.

Archive: `docs/benchmarks/results/2026-10-06-rebaseline-2.2.0/` (compare files, runner summary,
aggregates, manifests, prefix-arm Prometheus dumps); raw run directories for this suite and the
fitted suite below are on the v2.2.0 release as `ranvier-benchmark-runs-2026-10-06.tar.gz` (64 MB).

### Fitted suite under the shipping defaults (measured 2026-10-06/07, release 2.2.0)

`bench-runner.sh --suite fitted` on the same instance and image, straight after the matrix above:
16 prefixes × 2000–4000 tokens (~48k, ~6k per backend, fits beside in-flight requests), three rows
× three repeats, alternating arm order, 9/9 runs passed. The 30-user row is new (added 2026-10-06
so the high-load regime has a measurement without timeouts). **Zero incomplete requests in all
eighteen arms**, so every P99 here is a true P99.

| Config (fitted set) | P99 TTFT per repeat | Median (IQR) | P50 TTFT | Throughput | KV hit, RR → prefix | Diverts | Verdict |
|---------------------|---------------------|--------------|----------|------------|---------------------|---------|---------|
| **13B 10u/10m** | −28.6, −34.2, −23.6 | **−28.6%** (−31.4…−26.1) | −29.0 / −29.4 / −29.4% | +7…+9% | 16–19% → 76–82% | 17–23% | ✅ consistent improvement, 3/3 |
| **13B 20u/10m** | −56.6, −61.5, −55.8 | **−56.6%** (−59.1…−56.2) | −28.5 / −28.5 / −27.8% | +10…+12% | 12–15% → 67–72% | 22–25% | ✅ consistent improvement, 3/3 |
| **13B 30u/30m** | −43.1, −42.0, −34.1 | **−42.0%** (−42.6…−38.1) | −29.4 / −29.9 / −29.1% | **+14…+15%** | 10–11% → 46–50% | 16–17% | ✅ consistent improvement, 3/3 |

**What the three rows say together.** P50 is −29% at every load: that is the prefill a cache hit
saves, and it does not depend on queueing. P99 is what the divert policy adds on top, and it scales
with the queue: −29% at 10 users (little queue to remove), −57% at 20 users (the queue is the tail),
−42% at 30 users (the fleet is saturated and some eviction returns: KV hit 50% vs 70% at 20 users,
residency downgrades 1.4–1.6%). Throughput follows saturation the other way: +8% at 10 users,
+11% at 20, +15% at 30, where the prefix arm served ~1,750 more requests per 30-minute arm because
saved prefill becomes served requests rather than idle time. The 20u row reproduces the 2026-10-05
combo leg (−60.4 / −57.5 / −55.2 on the Arizona instance) on a different box within one point of
median: six repeats across two instances and both arm orders, all between −55 and −62%.

**The 30-user row closes the timeout question.** On the 50-prefix set at 30 users the prefix arm
left 0.1–0.4 points more requests incomplete than round-robin in every repeat. On the fitted set,
same load, same 30-minute arms, both arms completed every one of ~25,000 requests per repeat, in
both arm orders. The excess was the eviction regime (a hot backend running a deeper queue while it
is also evicting), not the 2× cap; no ε sweep is needed and the default stands. Round-robin's P99
sat at the 5 s validation gate (4.8–5.1 s; rep 3 reads FAILED → PASSED), the prefix arm's at 2.8–3.3 s.

Arm order made no difference on any 13B fitted row (round-robin KV hit 9.7–18.7% whichever arm ran
first), consistent with the reading above: round-robin never concentrates a prefix, so it inherits
nothing from a warm cache. Routing-decision P99 is 8–10 ms on this set against 22 ms on the 50-prefix
set: the boundary-detect tokenization tail scales with prefix length (4,000 vs 8,000 tokens max).

Archive: `docs/benchmarks/results/2026-10-07-fitted-2.2.0/`.

### The campaign at a glance (CodeLlama-13B, 20 users, 8×A100, 3 Ranvier nodes)

Every leg of the 2026-10-01..05 campaign on the one row that resisted, in the order it was run.
Fitted 16-prefix set (2000–4000 tokens) unless noted; prefix arm vs round-robin, 10-minute arms,
alternating arm order. Consistency, KV hits, diverts and Gini are the prefix arm's. P99 of the
previous-default rows had **+3…+21%** across twelve runs; the shipping defaults give **−55…−60%**.

| # | Leg (date) | What changed vs the row above | P99 TTFT per rep | Median | Consistency | KV hit | Diverts | Gini | Verdict |
|---|------------|-------------------------------|------------------|--------|-------------|--------|---------|------|---------|
| 1 | Re-baseline, 50 prefixes (10-01) | previous defaults, unfitted set | +17.4 / +11.0 / +4.6 | **+11.0%** | ~48% | 19% | ~30% | — | ❌ regression |
| 2 | Fitted set (10-02) | 16 prefixes that fit the 13B cache | +10.1 / +8.2 / +21.4 | +10.1% | 48–53% | 45% | ~30% | 0.06–0.10 | ❌ regression |
| 3 | Least-loaded diversion (10-02) | divert target = coldest instead of first-under-cap | +10.5 / +24.1 / +17.5 | +17.5% | — | 29–30% | ~30% | 0.05–0.08 | ❌ regression |
| 4 | Leg A, no diverts (10-02/03) | pure affinity, every divert mechanism off | +8.2 / +12.3 / +11.3 | +11.3% | — | 55–61% | 0% | 0.30 | ❌ placement is the mechanism |
| 5 | Leg B, in-flight signal (10-03) | live node-local in-flight count, ε 0.25, hash placement | +14.2 / lost / −1.0 | — | 39–42% | 27–36% | 29–32% | 0.04–0.06 | ⚖️ mixed, n=2; first near-zero |
| 6 | Placement v1 (10-03) | least-loaded placement, count-weighted | +6.6 / +25.1 / +13.3 | +13.3% | 37–42% | 29–32% | 23–25% | 0.08–0.10 | ❌ one-offs outvote prefixes |
| 7 | Placement v2 (10-03) | token-weighted + eager learn at dispatch | +11.4 / +12.5 / +3.9 | +11.4% | 38–42% | 30–38% | 22–24% | 0.05–0.07 | ❌ split: ~800 trust refusals/node |
| 8 | Placement v3 (10-03) | + gossip convergence (lowest id wins) | +8.1 / +10.1 / +3.4 | +8.1% | 56–59% | 42–48% | 25–27% | 0.09–0.11 | ❌ converged = 0 (local flush bypassed it) |
| 9 | Placement v4 (10-05) | + local-flush convergence | +20.9 / +13.1 / (vLLM start failure) | — | 56–57% | 42–44% | 25–29% | 0.08–0.10 | ❌ split gone; token tally blind (128-token keys) |
| 10 | Isolation (10-05) | live signal + ε 1.0, **hash** placement | −48.4 / −51.8 / −53.5 | **−51.8%** | 39–45% | 49–55% | 30–33% | 0.05–0.10 | ✅ divert policy is most of the win |
| 11 | **Combo = shipping defaults (10-05)** | live signal + ε 1.0 + **least-loaded** placement with convergence | **−60.4 / −57.5 / −55.2** | **−57.5%** | 50–53% | **69–73%** | 23–27% | 0.09–0.12 | ✅ **accepted** |

The other rows, previous defaults vs shipping defaults (confirmation repeat of 2026-10-05, then
the three-repeat matrix of 2026-10-06 where the row is in it):

| Row | Previous defaults, P99 per rep | Shipping defaults, confirmation (1 rep) | Shipping defaults, matrix (3 reps, 10-06) | KV hit, RR → prefix (shipping) |
|-----|-------------------------------|------------------------------------------|--------------------------------------------|--------------------------------|
| 13B 10u, fitted | −7.3 / −5.8 / −0.1 (default); −13.2 / −3.0 / +0.4 (v3) | **−34.5%** | **−28.6%** (fitted suite 10-06); −38.8% on the 50-prefix row | 16–19% → 76–82% |
| 13B 20u, fitted | +10.1 / +8.2 / +21.4 | −57.5% (combo, 3 reps) | **−56.6%** (fitted suite 10-06) | 12–15% → 67–72% |
| 13B 30u/30m, fitted | not measured | — | **−42.0%**, zero timeouts | 10–11% → 46–50% |
| 8B 20u, 50 prefixes | **−17.0 / −6.5 / −17.4** | −22.6% | **−26.7%** | 72% → 96% |
| 13B 20u, 50 prefixes | **+17.4 / +11.0 / +4.6** | — | **−21.0%** | 7% → 26% |
| 13B 30u/30m, 50 prefixes (eviction regime) | −2.4 / −1.6 / +3.6 | −16.4%, timeouts in both arms | **−14.1%**, timeouts in both arms | 6% → 20% |

Reading down the first table: rows 2–9 all balance *something derived from the route table or the
hash* and all land in the same band, because the per-backend request Gini (0.05–0.12) was never
the tail; row 4 shows that with zero diverts, and row 9 shows that even a tally balanced to one
route-unit leaves traffic 1.8× apart. Rows 10–11 change what the divert policy *sees* and *when it
acts*, and the tail collapses while Gini stays where it was. Placement earns its place in row 11
over row 10 by needing fewer diverts: 15–20 points more KV hits for 6 points more P99.

## Baseline suite: prefix vs least-loaded, the no-affinity control (measured 2026-10-08)

**Question.** Every number above is prefix routing against round-robin (the server's uniform random
mode under the `round_robin` alias), the weakest baseline there is. How much of the win is affinity,
and how much is load balancing that any least-loaded router provides? **Pre-registered reading
(written before the run, resume checklist item 4):** if prefix beats least-loaded by less than 10
points of P99 on the fitted 20u row, the campaign's P99 win was mostly load balancing and the README
must say so.

**Method.** `bench-runner.sh --suite baseline`: `bench.sh --compare --baseline-mode least_loaded` on
the fitted 13B 20u/10m and 30u/30m rows, three repeats each, alternating arm order, fresh 8×A100
instance, image built from `573fbf6` (2.3.0-dev, the commit that adds the mode). The baseline arm
runs `RANVIER_ROUTING_MODE=least_loaded`: every request to the live backend with the lowest
capacity-adjusted composite load (under the shipping defaults, the node's in-flight count summed
across shards, the same signal the prefix mode's divert policy reads), ties uniformly at random, no
ART, no learning, tokenization skipped as in `random`. Locust verified `X-Routing-Mode:
least_loaded` on every arm. The between-arm KV reset returned 404 on every backend (vLLM serves
`/reset_prefix_cache` only with `VLLM_SERVER_DEV_MODE=1`, fixed afterwards), so the arms carry KV
over exactly as every run before them; the 13B rows have shown no order effect. Zero incomplete
requests in all twelve arms. Round-robin figures below are the fitted suite's (2026-10-06/07), same
rows, same set, same image lineage, one instance earlier.

| Row (fitted set) | Arm | P99 TTFT | P50 TTFT | Throughput | KV hit | Route consistency | Gini |
|---|---|---|---|---|---|---|---|
| **13B 20u/10m** | Round-robin | 3.40–3.76 s | 1.02–1.03 s | 4.6–4.7 rps | 12–15% | 12–13% | 0.02–0.04 |
| | **Least-loaded** | **1.68–1.85 s** | 1.00–1.01 s | 4.9 rps | 17–21% | 11–12% | **0.008–0.015** |
| | Prefix | 1.45–1.50 s | 0.73–0.74 s | 5.2 rps | 64–72% | 50–53% | 0.07–0.11 |
| | *Prefix vs round-robin* | *−56.6%* | *−28%* | *+10…+12%* | | | |
| | **Prefix vs least-loaded** | **−14.6 / −13.8 / −20.8%** (median −14.6, IQR −17.7…−14.2; 3/3 agree) | **−26.5 / −26.4 / −27.1%** | **+6.3 / +5.7 / +7.1%** | | | |
| **13B 30u/30m** | Round-robin | 4.79–5.06 s | 1.09 s | 6.5–6.6 rps | 10–11% | 13% | 0.01 |
| | **Least-loaded** | **2.80–2.89 s** | 1.07 s | 6.9 rps | 13–15% | 11–12% | **0.002–0.005** |
| | Prefix | 2.68–3.33 s | 0.76–0.77 s | 7.5–7.6 rps | 44–54% | 53–56% | 0.08–0.11 |
| | *Prefix vs round-robin* | *−42.0%* | *−29%* | *+14…+15%* | | | |
| | **Prefix vs least-loaded** | **−6.5 / +6.6 / +11.3%** (median +6.6; MIXED, no reliable effect) | **−28.9 / −27.8 / −28.5%** | **+9.1 / +7.9 / +8.8%** | | | |
| **13B 20u/10m, 50-prefix set** (eviction regime, one repeat) | Round-robin (matrix, 10-06) | 5.07–5.38 s of completed; 1.5–1.8% timeouts | 0.97–1.00 s | 5.0–5.1 rps | 6–8% | 12% | 0.01–0.04 |
| | **Least-loaded** | **3.66 s**; 1.7% timeouts | 0.92 s | 5.3 rps | 6% | 12% | **0.011** |
| | Prefix (today / matrix) | 3.66 s / 3.66–4.39 s; 1.3% timeouts | 0.77 s | 5.4 rps | 28% / 23–29% | 46% | 0.107 |
| | **Prefix vs least-loaded** | **+0.2%** (same) | **−16.6%** | **+2.5%** | | | |

The one-repeat 50-prefix row (the set the campaign's regression was found on) says the same thing
in the eviction regime: least-loaded matches prefix on P99 exactly (3.66 s both; round-robin 5.1–5.4 s),
prefix keeps P50 (−16.6%, smaller than on the fitted set because only 28% of prefills hit), KV (6% →
28%), throughput (+2.5%) and fewer timeouts (1.3% vs 1.7%).

**Outcome against the pre-registered rule.** Cleared at 20 users on the fitted set (14.6 points,
every repeat agreeing), not at 30 (mixed, median slightly against prefix), and not on the 50-prefix
set (one repeat, parity). So:

- **On P99, load balancing is most of the win over round-robin.** Least-loaded alone, with no
  affinity, takes the 20-user tail from ~3.5 s to ~1.75 s (about −50%) and the 30-user tail from
  ~4.9 s to ~2.85 s (−41%). Of prefix routing's 57 points against round-robin at 20 users, about 50
  are load balancing and about 15 are affinity (the two overlap, so they do not add). At 30 users
  prefix has no reliable P99 edge over least-loaded.
- **On P50, KV hit rate and throughput, affinity is the whole effect.** Least-loaded's P50 equals
  round-robin's (~1.0 s at 20 users, ~1.07 s at 30): it balances queues but every request still pays
  full prefill. Prefix cuts P50 by 26–29% against *either* baseline at every load, lifts the KV hit
  rate from 13–21% to 44–72%, and serves 6–9% more requests than least-loaded (10–15% more than
  round-robin).
- **Why prefix loses its tail edge at saturation.** Least-loaded's backend distribution is almost
  perfectly flat (Gini 0.002–0.005). The prefix arm runs at 0.08–0.11, with two backends carrying
  ~2,100–2,460 requests against ~1,300–1,360 at the low end; at 30 users that queue depth is the tail
  (prefix route-consistent P99 2.1–2.65 s vs least-loaded's 2.0–2.1 s; prefix miss P99 worse in all
  three reps). The 2× divert cap (ε 1.0) is what permits the concentration. It was cheap at 20 users
  and is not at 30.

**What this changes in the headline.** The round-robin numbers stay true and stay stated, with the
caveat attached: a live least-loaded router already removes most of the tail. What prefix-aware
routing adds on top of a competent balancer, at every load measured, is P50 −27…−29%, throughput
+6…+9% and a 3–4× KV hit rate; plus a further −15% P99 at moderate load and nothing reliable at
saturation. The product's case is prefill saved and capacity recovered, not the tail. README and the
2026-10-02 strategic assessment updated to say so.

**What it reopens.** The ε sweep closed on 2026-10-07 (both arms completed every request at 30
users) is reopened on fairer grounds: least-loaded shows what a flat distribution buys at saturation.
Next session: the fitted 30u row vs least-loaded at ε 0.5 and 0.25 (one config each, ×3, ~7 h), and
the 50-prefix 20u row vs least-loaded two more times (one repeat ran 2026-10-08: P99 parity, P50 −16.6%). The reading to
pre-register: an ε that matches least-loaded's P99 at 30u while keeping P50 within 5 points of
−28% and KV above 40% becomes the default; if no ε does both, the default stays 1.0 and the docs
say the trade-off out loud. *Measured 2026-10-09 ("Saturation suite", next): no ε recovers the
tail, 1.0 stays; and with the KV reset working the 50-prefix row is a consistent +23% P99 against
least-loaded, so the parity above was the carry-over cache, not the regime.*

Archive: `docs/benchmarks/results/2026-10-08-baseline-least-loaded/`; raw run directories on the
v2.2.0 release as `ranvier-benchmark-runs-2026-10-08.tar.gz`.

## Saturation suite: a tighter divert cap vs least-loaded, and the eviction regime (measured 2026-10-09)

**Question.** The baseline suite left prefix routing with no reliable P99 edge over least-loaded at
30 users, and read the prefix arm's concentration (Gini 0.08–0.11 under the 2× cap) as the tail.
Does a tighter cap (ε 0.5: divert at 1.5× the mean; ε 0.25: at 1.25×) recover that tail without
giving back P50 and KV? And on the 50-prefix set at 20 users, where the fleet's cache cannot hold
the prefix set, is the one carry-over repeat's P99 parity real? **Pre-registered reading (resume
checklist item 5, written 2026-10-08):** an ε that matches least-loaded's P99 at 30u (MIXED or
better, no CONSISTENT REGRESSION) while keeping P50 within 5 points of −28% and KV above 40%
becomes the default; if none does both, 1.0 stays and the docs state the trade-off.

**Method.** `bench-runner.sh --suite saturation`: `bench.sh --compare --baseline-mode least_loaded`
on the fitted 13B 30u/30m row with the prefix arm at `--bounded-load-epsilon 0.5` and `0.25`, and
on the 50-prefix 13B 20u/10m row at the shipping ε 1.0; three repeats each, alternating arm order,
fresh 8×A100 instance, image built from `5f8be8b` (2.3.0-dev). First suite with the between-arm KV
reset working: every compare header records `8/8` backends acknowledging `/reset_prefix_cache`
before each arm, so no arm inherits the other's cache. Zero incomplete requests in all eighteen
30-user arms. The ε 1.0 row quoted for comparison is the baseline suite's (2026-10-08, same row,
same set, carry-over KV; the 13B rows have shown no order effect).

| 13B 30u/30m, fitted set | P99 TTFT, least-loaded → prefix | P99 vs least-loaded | P50 | Throughput | KV hit, LL → prefix | Route consistency | Diverts | Gini (prefix) | Route-consistent P99 |
|---|---|---|---|---|---|---|---|---|---|
| ε 1.0 (shipping; 10-08, carry-over KV) | 2.80–2.89 s → 2.68–3.33 s | −6.5 / +6.6 / +11.3% (MIXED) | −28.9 / −27.8 / −28.5% | +9.1 / +7.9 / +8.8% | 13–15% → 44–54% | 53–56% | | 0.08–0.11 | +5…+26% worse |
| **ε 0.5** | 2.97 / 2.91 / 3.09 s → 2.75 / 3.09 / 3.13 s | **−7.3 / +6.0 / +1.3%** (median +1.3; MIXED) | −27.7 / −27.3 / −27.3% | +7.1 / +7.8 / +6.8% | 13% → 44 / 43 / 42% | 46 / 48 / 45% | 24.2 / 24.6 / 23.7% | 0.067 / 0.070 / 0.079 | +14.9 / +40.2 / +22.6% worse |
| **ε 0.25** | 2.98 / 2.93 / 2.75 s → 3.31 / 3.08 / 3.45 s | **+11.1 / +4.8 / +25.4%** (median +11.1; CONSISTENT REGRESSION) | −24.1 / −25.4 / −24.8% | +4.5 / +5.5 / +4.8% | 13–15% → 32 / 37 / 33% | 37 / 40 / 38% | 30.8 / 29.8 / 29.9% | 0.059 / 0.056 / 0.044 | +60.6 / +26.2 / +71.5% worse |

Least-loaded's own P99 sat at 2.75–3.09 s in all six runs (2.80–2.89 s the day before), its Gini
at 0.003–0.006. Read down the table: each halving of the cap diverts more (24% → 30%), spreads
the prefix arm flatter (Gini 0.07 → 0.05), and gives back KV (43% → 33%), route consistency (47%
→ 38%), P50 (−27% → −25%) and throughput (+7% → +5%), and the tail gets *worse*, not better. The
loss is in the hits, not only the misses: the prefix arm's route-consistent P99 is above
least-loaded's at every ε (the "hit" P99 of a prefix router at saturation is a request queued
behind other hits on a warm backend), and at ε 0.25 the large-bucket hit P99 is +9 … +53% above
least-loaded's in all three repeats. The divert policy can only move a request to the momentarily
lowest backend, where it arrives as a miss; at 30 users every backend is within a request or two of
the mean already (least-loaded's per-backend counts 1,474–1,534), so the move buys no queue position
and costs the prefill the hit would have saved. The 2026-10-08 reading, that the prefix arm's
concentration *is* the tail, was wrong in its remedy: flattening the distribution by diverting
more is what least-loaded does with 13% KV, and prefix routing cannot reach least-loaded's tail by
imitating it while keeping any affinity.

**Outcome against the pre-registered rule.** ε 0.5 satisfies the rule as written (MIXED, P50
−27.3…−27.7%, KV 42–44%), but so does the shipping ε 1.0 the rule was meant to improve on (MIXED,
P50 −28%, KV 44–54%), and the rule did not say "better than 1.0". Between the two, ε 0.5 has a P99
median 5 points nearer parity (+1.3 vs +6.6) inside overlapping ranges (−7.3…+6.0 vs −6.5…+11.3),
and measurably less affinity: 2–10 points of KV, 6–9 points of route consistency, 1–2 points of
throughput. Changing a default for a tail gain inside the noise at a measured affinity cost is not
justified by this suite, and the rule's premise, that some ε recovers the saturation tail, is not
met by any ε: **the default stays 1.0, and the trade-off is now stated out loud: at saturation
the 2× cap gives prefix routing least-loaded's tail within about ±10% while keeping P50 −28%, KV
3–4× and throughput +8%; tightening it moves the tail the wrong way.** ε 0.25, the pre-2.2.0
default, is a consistent regression against least-loaded at this load, which is the mechanism
behind the 13B rows that regressed under the previous defaults.

| 13B 20u/10m, 50-prefix set (eviction regime) | Arm | P99 TTFT (of completed) | Incomplete | P50 TTFT | Throughput | KV hit | Route consistency | Diverts | Gini |
|---|---|---|---|---|---|---|---|---|---|
| rep 1 (rr-first) | Least-loaded | 3.31 s | 66 (2.1%) | 0.86 s | 5.3 rps | 7.2% | 11% | | 0.014 |
| | Prefix | 4.06 s (**+22.9%**) | 47 (1.5%) | 0.78 s (−9.3%) | 5.3 rps (−0.2%) | 22.9% | 46% | 21.6% | 0.087 |
| rep 2 (prefix-first) | Least-loaded | 3.40 s | 68 (2.1%) | 0.93 s | 5.3 rps | 6.4% | 11% | | 0.013 |
| | Prefix | 4.24 s (**+24.9%**) | 50 (1.6%) | 0.79 s (−15.2%) | 5.3 rps (−1.5%) | 32.4% | 48% | 22.1% | 0.148 |
| rep 3 (rr-first) | Least-loaded | 3.35 s | 59 (1.8%) | 0.91 s | 5.3 rps | 7.6% | 13% | | 0.020 |
| | Prefix | 4.08 s (**+21.7%**) | 54 (1.7%) | 0.77 s (−15.8%) | 5.4 rps (+0.4%) | 23.7% | 45% | 23.3% | 0.165 |
| **Prefix vs least-loaded** | | **+22.9 / +24.9 / +21.7%** (median +22.9; CONSISTENT REGRESSION) | fewer in 3/3 | **−9.3 / −15.2 / −15.8%** | −0.2 / −1.5 / +0.4% | 3–5× | | | |

The one carry-over repeat of 2026-10-08 (KV reset 404 on every backend, P99 3.66 s in both arms)
does not survive three clean repeats: with the cache reset before each arm, least-loaded's tail is
3.3–3.4 s and prefix's 4.1–4.2 s, both arm orders, every repeat agreeing. The clean reps supersede
it. Where the fleet's cache cannot hold the shared prefixes (50 prefixes of 2,000–8,000 tokens
against ~11,600 KV tokens per 13B backend), prefix routing keeps the hit rate 3–5× (7% → 23–32%),
P50 −9…−16% and fewer timeouts (1.5–1.7% vs 1.8–2.1%), serves the same number of requests, and
loses the tail to least-loaded by 22–25%. The counters say why: the prefix arm's large-bucket
*hit* P99 (3.7–4.4 s) is above least-loaded's *miss* P99 (3.1–3.6 s) in every repeat, and its
Gini (0.09–0.17, the highest of any prefix arm in the campaign) shows the hits piling onto the
few backends that happen to hold a prefix at the moment. Against round-robin on the same row
(standard matrix, 2026-10-06) prefix is −21.0% P99; against a least-loaded router it is +23%. This
is the eviction regime's honest number and the README now carries it.

**What it closes and what it opens.** The ε question is closed on measured grounds: the cap
trades affinity for distribution monotonically and never buys the saturation tail; ε 1.0 stays.
Nothing further on this hardware distinguishes the shipping defaults from least-loaded at
saturation, so the next routing change is not a knob. What the counters point at is a divert that
keeps the hit: a hit that would queue past the cap on its home backend should be offered to
*another backend that also holds the prefix* (the residency gossip already carries holders per hot
prefix, BACKLOG §20.1 P3) before being sent to the least-loaded stranger as a miss, and a hot
prefix that no second backend holds should be replicated rather than diverted (placement balances
prefix count, not popularity). Backlog §27 carries the item with these numbers attached.

Archive: `docs/benchmarks/results/2026-10-09-saturation/`; raw run directories on the v2.2.0
release as `ranvier-benchmark-runs-2026-10-09.tar.gz`.

## Representative-workload re-baseline (measured 2026-10-01, fixed tooling, previous defaults)

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
  Whether that holds is the question the `fitted` suite answers (below). *Resolved 2026-10-05:*
  the mechanism was the divert policy's signal and threshold, not KV occupancy; the fallback was
  reading a 5 s-stale score and one shard's share of the queue, at a threshold that fired on
  nearly any load. With the live node-local count and ε 1.0 the same row is −57.5% (headline).

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

Every leg of the campaign is archived (the run directories had been copied off each instance
before termination after all): summaries in this tree, raw data as a release asset.

| Leg | Summaries (compare files, manifests, aggregates, prefix-arm Prometheus dumps) |
|-----|-------------------------------------------------------------------------------|
| Re-baseline, 50 prefixes (2026-10-01) | `results/2026-10-01-rebaseline/` |
| Fitted set + diversion acceptance (2026-10-02) | `results/2026-10-02-fitted/` |
| Leg A, no diverts | `results/2026-10-02-legA-nodivert/` |
| Leg B, in-flight signal | `results/2026-10-03-legB-inflight/` |
| Placement v1, v2, v3 | `results/2026-10-03-placement-v1/`, `-v2/`, `-v3/` |
| Placement v4 | `results/2026-10-05-placement-v4/` |
| Combo (the shipping defaults) | `results/2026-10-05-combo/` |
| Isolation (hash placement + live signal) | `results/2026-10-05-isolation/` |
| Confirmation rows | `results/2026-10-05-confirm/` |
| Standard matrix under 2.2.0 defaults (2026-10-06) | `results/2026-10-06-rebaseline-2.2.0/` |
| Fitted suite under 2.2.0 defaults, incl. new 30u row (2026-10-06/07) | `results/2026-10-07-fitted-2.2.0/` |
| Baseline suite: prefix vs least-loaded (2026-10-08) | `results/2026-10-08-baseline-least-loaded/` |
| Saturation suite: ε 0.5 / 0.25 vs least-loaded at 30u, 50-prefix 20u vs least-loaded (2026-10-09) | `results/2026-10-09-saturation/` |

The raw run directories (Locust CSVs, per-request logs, vLLM logs, per-node Ranvier logs where
captured; 4.6 GB, 610 MB compressed) are attached to the
[v2.2.0 release](https://github.com/Ranvier-Systems/ranvier-core/releases/tag/v2.2.0) as
`ranvier-benchmark-runs-2026-10.tar.gz` (the 2026-10-01..05 campaign) and
`ranvier-benchmark-runs-2026-10-06.tar.gz` (the 2026-10-06/07 matrix and fitted suite, 64 MB) and
`ranvier-benchmark-runs-2026-10-08.tar.gz` (the baseline suite vs least-loaded).
New runs are archived with
`scripts/bench-archive.sh`. Manifests from 2026-10-02 on carry `server_image`, so the binary
behind a run is identifiable.

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
| 3 (rr-first) | **−55.2%** (3277 → 1469 ms) | −28.2% | 49.6% | **73.2%** | 26.7% | 0.092 | −60.7% / −54.5% | +10.3% |

**Verdict: ✅ consistent improvement, −57.5% median P99 (IQR −58.9…−56.3), 3/3 across both arm
orders.** This is the acceptance for the 13B 20-user row and the first 20-user prefix arms of the
campaign to beat round-robin's tail, by a margin no other configuration came within 50 points of.
P50 −28 to −29% (the campaign's usual), KV hits 69–73% (the highest of any 20-user arm), both the
large-hit and large-miss P99 down by more than half, and the prefix arm completed 10–11% more
requests in the same ten minutes, so the closed-loop confound ran against the result. Read with the earlier legs: the request distribution is
*still* uneven (Gini 0.116, busiest backend 17% of requests, the same placement draw as v1–v4),
yet the tail collapsed. Request count per backend was never the tail; queue depth was, and a
divert policy that sees the queue live and only acts at 2× the mean removes the queue without
removing affinity (consistency 53% against 57% for the no-divert placement arms, KV hits the
highest of any 20-user arm). Leg B rep 3 (−1.0%) was the same signal with ε 0.25 and hash
placement, diverting 29% of requests and giving back the affinity; ε 1.0 is the calibration
that was missing. The prefix arm also completed 10% more requests in the same 10 minutes, so
the closed-loop confound ran *against* this result.

### Confirmation rows under the combo settings (2026-10-05, one rep each)

Same env and ε 1.0 with `--miss-placement least_loaded`, on the rows the campaign already has
baselines for. The question was whether the new divert policy costs anything where prefix
routing already won or broke even.

| Row | P99 TTFT | P50 TTFT | Route consistency | KV hit RR → prefix | Diverts | Prefix-arm Gini | Previous verdict for this row |
|-----|----------|----------|-------------------|--------------------|---------|-----------------|-------------------------------|
| 13B 10u/10m, fitted | **−34.5%** | −29.8% | 58.0% | 16.9% → **82.4%** | 19.7% | 0.076 | mixed (−7.3/−5.8/−0.1 default; −13.2/−3.0/+0.4 v3) |
| 8B 20u/10m (50 prefixes) | **−22.6%** | −1.3% | 50.7% | 71.8% → 95.9% | 25.5% | 0.078 | −17.0% median (−17.0, −6.5, −17.4) |
| 13B 30u/30m (50 prefixes, eviction regime) | −16.4% (excl. timeouts) | −20.1% | 47.3% | 6.1% → 23.1% | 15.4% | 0.103 | no reliable effect (−2.4, −1.6, +3.6) |

Nothing regressed; every row improved on its previous verdict. The 10-user fitted row, which
ε 0.25 over-diverted into a coin flip, is now the second-largest tail win of the campaign with
the highest KV hit rate measured (82%). The 8B row kept its flat P50 (the 8B fleet is not
prefill-bound) and widened its tail win from −17% to −23% with KV hits at 96%. The 30-user row
is the eviction regime (50 prefixes × 2000–8000 tokens against 11.6k tokens of KV per backend):
**validation FAILED in both arms** with 1.4% / 1.6% timeouts, so its P99 excludes incompletes
and the prefix arm timed out 29 more requests while completing 684 more; read it as "the direction
is right, the regime is still wrong", not as a result. One rep each: enough to show no regression,
not enough to re-verdict the rows.

### Isolation leg: hash placement + live in-flight signal + ε 1.0 (2026-10-05)

The combo minus placement: `--miss-placement hash`, same env and ε 1.0, 13B 20 users ×3.

| Rep | P99 TTFT | P50 TTFT | Route consistency | KV hit (prefix) | Diverts | Prefix-arm Gini | Large hit / miss P99 |
|-----|----------|----------|-------------------|-----------------|---------|-----------------|----------------------|
| 1 (rr-first) | −48.4% | −27.4% | 42.4% | 49.2% | 30.6% | 0.062 | −52.7% / −30.4% |
| 2 (prefix-first) | −51.8% | −27.7% | 38.8% | 54.7% | 32.7% | 0.054 | −60.2% / −45.1% |
| 3 (rr-first) | −53.5% | −27.6% | 45.2% | 55.4% | 29.6% | 0.099 | −55.0% / −51.5% |

**Verdict: ✅ consistent improvement, −51.8% median P99 (3/3).** Side by side with the combo:

| | Hash placement | Least-loaded placement (combo) |
|---|---|---|
| P99 TTFT, 3 reps | −48.4 / −51.8 / −53.5 (median −51.8) | −60.4 / −57.5 / −55.2 (median **−57.5**) |
| Route consistency | 39–45% | 50–53% |
| KV hit (prefix) | 49–55% | **69–73%** |
| Diverts | 30–33% | 23–27% |
| P50 TTFT | −27.5% | −28.5% |

The live divert policy is most of the tail win: about 50 of the 57 points. Split-free placement
adds the rest and does it by needing fewer diverts: one home per prefix means fewer hot-backend
collisions to divert away from, so 6–8 points more consistency, 15–20 points more KV hits, 5–7
points fewer diverts, and 4–9 points more P99, with the ranges not overlapping (hash's best
−53.5 vs the combo's worst −55.2) on the same box, image and day. Placement alone could not move
the tail (v1–v4); the divert policy alone leaves a third of requests diverted. Together is the
measured configuration, and every confirmation row above was run with it.

**Shipping (branch): all three.** `cross_shard_load_sync` true, `gpu_load_weight` 0,
`capacity_headroom_weight` 0, `bounded_load_epsilon` 1.0 **and** `miss_placement` `least_loaded`
become the defaults. The hash placement stays one env var away (`RANVIER_MISS_PLACEMENT=hash`) and
is the right choice on a single-node deployment with no gossip, where the convergence rules have
nothing to do.

**Resume checklist (next GPU session):**

1. ~~Rebaseline suite unflagged under the shipping defaults.~~ Done 2026-10-06 ("Standard matrix
   under the shipping defaults", above).
2. ~~Fitted suite under the shipping defaults, with the new 13B 30u/30m row.~~ Done 2026-10-06/07
   ("Fitted suite under the shipping defaults", above): 30u fitted −42.0% median with zero
   incompletes in both arms, so the 50-prefix timeout excess was the eviction regime and no ε sweep
   is needed. Nothing on this hardware remains unmeasured under the shipping defaults.
3. Tooling, before the next campaign. Done 2026-10-07: `bench.sh --compare` now POSTs
   `/reset_prefix_cache` to every vLLM backend before each arm (`--no-kv-reset` restores the
   carry-over) and the compare header says which it was, so the 8B order effect (second arm
   inherits a warm cache; prefix-first reps ~9 points weaker) cannot recur unnoticed. *First
   hardware contact 2026-10-08:* vLLM 0.15.1 answered 404, because the endpoint is served only
   with `VLLM_SERVER_DEV_MODE=1`; bench.sh now sets that on the vLLM it launches, and the
   baseline suite's compare headers honestly record `0/8` acknowledged (carry-over, as every
   run before it; the 13B rows showed no order effect). Still unverified on hardware: a run
   whose header says `8/8`; the
   aggregate prints rr-first and prefix-first medians beside the overall one and records them in
   its JSON. Run 8 of the rebaseline suite died because vLLM instance 5 failed engine-core
   initialisation 40 s into start-up (same failure class as placement-v4 rep 3); its log was
   overwritten by the next run, so bench.sh now keeps a dead instance's start-up log under the
   output dir and bench-runner retries a run once when it fails within `--startup-retry` seconds
   (default 180; nothing was measured). The `Xlarge Hit P50 = N/A` on round-robin arms does not
   reproduce: the arm's stats JSON holds the value (887.3 ms, 576 samples for the 13B 30u rep 1
   arm), and the same parser on the same `benchmark.log` prints it on another machine (Python
   3.11+). It was printed as N/A by the compare run on the instance (Python 3.10) at the end of the
   arm; the archived compare files carry that cell as printed, the headline rows are unaffected,
   and the cause is not chased further. Not yet verified on GPUs: the reset's effect on the 8B row
   (expected: prefix-first and rr-first repeats converge; 13B rows unchanged).
4. ~~`bench-runner.sh --suite baseline`: prefix vs least-loaded.~~ Done 2026-10-08 ("Baseline suite",
   above). Pre-registered rule: cleared at 20u (−14.6% P99, 3/3), not at 30u (mixed). Load
   balancing is most of the P99 win over round-robin; affinity owns P50 (−27…−29%), KV (3–4×) and
   throughput (+6…+9% over least-loaded). README and strategic assessment updated.
5. ~~`bench-runner.sh --suite saturation`: the fitted 30u row vs least-loaded at ε 0.5 and 0.25,
   plus the 50-prefix 20u row vs least-loaded, ×3 each.~~ Done 2026-10-09 ("Saturation suite",
   above). Pre-registered rule: ε 0.5 passes it (−7.3/+6.0/+1.3, P50 −27%, KV 42–44%) but so does
   the shipping 1.0, and no ε recovers the tail (ε 0.25 is +11.1/+4.8/+25.4, a consistent
   regression); 1.0 stays and the trade-off is stated. The 50-prefix row is +22.9/+24.9/+21.7%
   P99 against least-loaded with the KV reset working (the carry-over repeat's parity was the
   cache), P50 −9…−16%, KV 3–5×, fewer timeouts. The KV reset verified on hardware: every header
   `8/8`. Still unverified: the reset's effect on the 8B row's order effect (no 8B row in this
   suite).
6. Archive every run directory's summaries with
   `./scripts/bench-archive.sh <run-dir> <date>-<leg>` (compare files, runner summary,
   aggregates, per-arm manifests, prefix-arm Prometheus dumps; `--with-logs` for the per-node
   Ranvier logs, `--date-prefix YYYYMMDD` for a directory holding several campaigns) **before**
   terminating the instance, and tar the raw run directories onto the release as an asset
   (`gh release upload vX.Y.Z <tarball>`): a suite is 50–100 MB raw and a few hundred KB summarised.
7. Next GPU session: `bench-runner.sh --suite kvevents --output-dir benchmark-reports-kvevents`
   (~2h50m, 6 runs; needs an image built from the commit that adds `kv_events_port` to
   `POST /admin/backends`, so `--build-image` on the first run or a merged main). The native
   KV-event subscriber has never run on GPUs: every `router_native_*` counter in every
   archived dump is 0, because the benchmark registers backends through the admin API,
   which had no way to opt them in. `bench.sh --kv-events` launches each vLLM with
   `--kv-events-config` and registers every backend with its publisher port, for both arms.
   **Read first:** the compare header's "Native KV events" line must show
   `router_native_kv_ops_total` > 0 on both arms and the counter rows `stream_resets` near 0;
   a 0 means the stream never connected and the run measured probabilistic residency again
   (bench.sh logs an error; stop the suite and read `/tmp/vllm_gpu*.log` and the node logs
   for "KV-event subscriber"). **Pre-registered reading:** row 17 (fitted 13B 20u vs
   least-loaded; −14.6/−13.8/−20.8% P99 without the stream) must not regress. Row 18
   (50-prefix 13B 20u vs least-loaded; +22.9/+24.9/+21.7% P99 on 2026-10-09) is the
   eviction-regime acceptance row: a CONSISTENT IMPROVEMENT is the finding (verified
   evictions stopped hits queueing on backends that had already evicted the prefix); MIXED
   with KV above today's 23–32% is progress worth a second look at `verified_evictions`;
   an unchanged CONSISTENT REGRESSION means block-exact residency alone does not fix the
   eviction regime, and the holder-aware divert (BACKLOG §27) is the next change, now with
   its signal live. Either way the counters say how often the stream changed a decision
   (`verified_hits`, `verified_evictions`, `routes_materialized`), which no run has shown yet.
   *First contact 2026-10-10:* the guard fired. Runs 1–2 (fitted 20u, −13.2 / −12.5% P99 vs
   least-loaded, zero incompletes; valid as control repeats without the stream) ended with
   `kv_ops=0` on both arms. Three defects, found from vLLM's own logs and source: the publisher
   endpoint `tcp://0.0.0.0:5557` made vLLM connect() instead of bind() (nothing listened on
   5557 while the replay socket on 5657 did); the replay request lacked the empty delimiter
   frame vLLM's ROUTER expects ("Invalid replay request" in the vLLM log, every backfill failed);
   and the decoder would have rejected vLLM's default byte-string block hashes once messages
   arrived. All three fixed the same day (CHANGELOG, Fixed); bench.sh now verifies each
   publisher port is listening after vLLM start. The suite restarts from run 1 on a rebuilt
   image.
8. Same session, after 7: `bench-runner.sh --suite kvreset --output-dir benchmark-reports-kvreset`
   (~1h15m): the 8B 20u row with the between-arm KV reset acknowledged, which the reset was
   built for and has never had (404 on 10-08; 13B rows only on 10-09). Expected: the rr-first
   and prefix-first repeats converge on the −27% median; the compare header must read
   `backends acked: 8/8` for both arms. If the order effect persists with the reset acked,
   it was never the vLLM cache and the 8B row's write-up changes.

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

- **13B 20-user fitted regression: resolved by the combo leg (2026-10-05): −60.4 / −57.5 /
  −55.2% P99** with the node-local in-flight load signal (`RANVIER_CROSS_SHARD_LOAD_SYNC=true`,
  GPU-score and headroom weights 0) and `bounded_load_epsilon` 1.0 on top of split-free
  least-loaded placement. Open: the isolation leg (same without placement) decides whether the
  shipping change is two defaults or three; then one rep each of 8B 20u, 13B 10u and 13B 30u
  under the new defaults (resume checklist above).
- **Epsilon: closed 2026-10-09** ("Saturation suite" above). Against least-loaded at 30 users,
  ε 0.5 and 0.25 both move the tail the wrong way while giving back KV; 1.0 stays. The
  factor/floor "threshold leg" (BACKLOG §25 item 5) remains inert under `bounded_load`;
  `bench.sh` refuses it without `--hash-strategy jump`.
- **Four-arm design** (direct-to-vLLM, random, least-loaded without affinity, prefix): the
  least-loaded arm exists since 2.3.0-dev (`--baseline-mode least_loaded`, measured 2026-10-08/09);
  the no-proxy arm, which measures Ranvier's own cost, still needs a direct-to-vLLM mode in
  `bench.sh`.
- **Holder-aware divert** (BACKLOG §27): the eviction-regime and saturation rows say the next
  routing change is a divert that keeps the hit, not a knob. Acceptance: the 50-prefix 20u row
  vs least-loaded (now +23% P99) and the fitted 30u row (now MIXED) turn to improvements without
  giving back KV. **Blocked on a signal:** nothing in the benchmark deployment knows which
  backends hold a prefix. The cluster `holders_of` index counts Ranvier *nodes* reporting a
  hot prefix (telemetry sink, off), the route table holds one home per prefix by design, and
  the native KV-event subscriber, the one block-exact per-backend source, has never been on
  (resume checklist item 7 turns it on first).

## Adding an entry here

Each result must record: commit SHA, full `bench.sh` argv, the effective routing config
(the run banner), workload knobs (`NUM_LARGE_PREFIXES`, `SHARED_PREFIX_RATIO`, distribution),
GPU type/count, vLLM version, and — once the P1 machinery lands — median/IQR across repeats.
Do not hand-copy a single run's best number into a headline; that is the failure mode this
split exists to end.
