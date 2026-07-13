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

## Representative-workload headline (measured 2026-07-13, commit `817a1b5`)

**Prefix-aware routing's P99 effect is operating-point-dependent — it is not a uniform win.**
On the representative 50-prefix workload it ranges from a **reliable −9 to −13% P99 improvement
under load** to a **reliable +29% regression at light load**, and the outcome tracks **cluster
throughput (queue pressure), not user count or model size**. Cache-hit rate improved **~3× in
every single config** (12% → 43–52%) yet is **decoupled** from the P99 result — the campaign's
clearest lesson.

Standard matrix on current defaults (50 prefixes, ratio 0.9, stress, load-aware 2.0/2, residency
0.2, 20ms flush), 8×A100 40GB, vLLM 0.15.1, `--compare` ×3 repeats, **median-of-3** (verdict =
"no reliable effect" when the IQR of the per-repeat %change spans zero). Manifests + logs under
`benchmark-reports/rebaseline/matrix/`.

| Config | ~req/s | P99 TTFT (median-of-3) | Verdict | Cache hit | Incompletes |
|--------|-------:|------------------------|---------|-----------|-------------|
| **8B 20u/10m**  | ~47 | **−13.3%** (IQR −15.6…−8.5) | ✅ reliable improvement | 12→48% | 0% both |
| **13B 30u/30m** | ~38 | **−9.1%** (IQR −13.2…−5.7) | ✅ reliable improvement | 12→38% | ~2% both |
| **13B 20u/10m** | ~28 | **+3.8%** (IQR −5.7…+8.0) | ⚖️ no reliable effect | 12→43% | ~2% both |
| **13B 10u/10m** | ~16 | **+29.0%** (IQR +21…+34) | ❌ reliable regression | 12→49% | prefix worse |

**Read this as a gradient, not four labels.** Sorted by throughput the P99 effect slides
monotonically from −13% (busiest) to +29% (idlest): under sustained load, routing to cache-warm
backends relieves the tail; below a crossover (somewhere between ~16 and ~28 req/s on this
hardware) the concentration it induces *adds* tail latency and timeouts while the cache-hit
prefill savings are too small to offset it. Throughput — not the 20-user count — is why 8B/20u
*wins* and 13B/20u doesn't: 8B pushes ~47 req/s, 13B/20u only ~28. (4-point interpolation; treat
the crossover as a band, not a precise number.)

Secondary signals, consistent across the matrix:
- **Cache hit rate up ~3× everywhere** yet uncorrelated with the P99 outcome — a high hit rate
  is not evidence of a latency win.
- **Throughput +1–7%** on every config (prefix never lost throughput).
- **Load-aware fallbacks 30–47%** on every prefix run (highest on 8B, which drains fastest): a
  third to a half of requests are diverted off affinity to balance load. That — not affinity
  thrash (Gini stayed low, 0.06–0.13) — is what caps the win, and it is the strongest case for
  the threshold leg below.

This **confirms and supersedes** the earlier directional D2 result (−6% at 13B/30u with *raised*
thresholds): 13B/30u here measures **−9.1% on the shipped `2.0/2` thresholds**. It also **flips
the deprecated 5-prefix guide's sign at low load** — that guide advertised 10u as a −60…−79%
win; on the representative workload 13B/10u is a **+29% regression**.

## Still open: threshold leg (BACKLOG §25 item 5)

The shipped `2.0/2` vs raised `3.0/4` load-aware decision is **not yet run**, and given the
30–47% fallback rates it is now the highest-value follow-up. Pre-registered rule (unchanged):
adopt `3.0/4` as default only if median P99 improves **≥10%** with no incomplete-rate regression.
Procedure: [re-baseline campaign runbook](benchmark-rebaseline-campaign.md) §2. Superseded
single-run predecessors (`.dev-context/next-benchmark-checklist.md`, directional only): D1
pure-affinity **+19%**, D2 raised-thresholds **−6%**.

## Adding an entry here

Each result must record: commit SHA, full `bench.sh` argv, the effective routing config
(the run banner), workload knobs (`NUM_LARGE_PREFIXES`, `SHARED_PREFIX_RATIO`, distribution),
GPU type/count, vLLM version, and — once the P1 machinery lands — median/IQR across repeats.
Do not hand-copy a single run's best number into a headline; that is the failure mode this
split exists to end.
