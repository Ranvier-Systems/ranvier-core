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

## Representative-workload headline: **TBD**

**There is currently no citable headline for prefix-aware vs round-robin under the
representative (50-prefix) workload.** Every -80%…-85% P99 / +13-22% throughput figure that
the old guide advertised was measured on the now-deprecated **5-prefix** workload (see the
[history archive](history/benchmark-history-8xA100.md)), where a prefix pool ≤ backend count
pigeonholes affinity onto too few backends and *manufactures* both the failure modes and, by
concentration, much of the headline win. Those numbers are **not reproducible on current
defaults** and are not quoted here.

The headline will be filled in by the **50-prefix re-baseline campaign** (BACKLOG §25, items
4–6): the standard matrix (13B 30u/30m, 13B 20u/10m, 13B 10u/10m, 8B 20u/10m — each
`--compare`, ×3 repeats), reported as median-of-3 with an explicit "no reliable effect"
verdict when the IQR spans zero on the discriminating metric. The turnkey procedure —
copy-paste commands, run files, pre-registered decision rules, and validity gates — is in the
**[re-baseline campaign runbook](benchmark-rebaseline-campaign.md)**. All the supporting tooling
(repeats + verdicts, manifests, 3-node telemetry, fair A/B ordering) is in place; only GPU time
remains.

## Only current-workload data points we have so far

From the affinity-thrashing investigation close-out (`.dev-context/next-benchmark-checklist.md`,
2026-05-26), at **50 prefixes / 8 backends** (single runs, no repeats — treat as directional,
not a headline):

| Leg | Config | P99 TTFT vs round-robin | Notes |
|-----|--------|-------------------------|-------|
| D1 | Pure affinity (load-aware OFF) | **+19%** | Affinity alone regresses the tail at 50 prefixes |
| D2 | Load-aware ON, raised thresholds (factor 3.0 / floor 4) | **−6%** | Small improvement; single run |

Note the shipped defaults (factor **2.0 / floor 2**) were **never tested at 50 prefixes** —
D2 validated only the *raised* thresholds. Closing that gap is the pre-registered threshold
leg of the re-baseline (BACKLOG §25, item 5): adopt 3.0/4 as default only if median P99
improves ≥10% with no incomplete-rate regression.

## Adding an entry here

Each result must record: commit SHA, full `bench.sh` argv, the effective routing config
(the run banner), workload knobs (`NUM_LARGE_PREFIXES`, `SHARED_PREFIX_RATIO`, distribution),
GPU type/count, vLLM version, and — once the P1 machinery lands — median/IQR across repeats.
Do not hand-copy a single run's best number into a headline; that is the failure mode this
split exists to end.
