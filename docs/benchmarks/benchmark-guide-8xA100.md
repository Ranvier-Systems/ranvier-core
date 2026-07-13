# Benchmark Guide: 8x A100 Single-Host Setup

This page is the **index** for Ranvier's 8x A100 benchmark documentation. The former
single-file guide (~2,500 lines, ~60% dated lab notebook) was split on 2026-07-06 so that
*how to run* is separated from *what the numbers are* and from *the historical record*.

## Where to go

| I want to… | Read |
|------------|------|
| Run a benchmark — setup, knobs, warm-up, A/B scenarios, validation, monitoring, export | **[benchmark-methodology.md](benchmark-methodology.md)** |
| See results valid on the **current** defaults | **[benchmark-results-current.md](benchmark-results-current.md)** |
| Run the 50-prefix re-baseline campaign (matrix done; threshold leg pending) | **[benchmark-rebaseline-campaign.md](benchmark-rebaseline-campaign.md)** |
| Read the dated per-instance runs, invalidated sections, and re-run plans (append-only) | **[history/benchmark-history-8xA100.md](history/benchmark-history-8xA100.md)** |
| Understand how to read TTFT / cache-hit numbers honestly | [interpreting-benchmark-numbers.md](interpreting-benchmark-numbers.md) |

## TL;DR (read this before quoting any number)

**The representative-workload headline is measured** (50 prefixes, current defaults, commit
`817a1b5`, median-of-3): prefix-aware routing's P99 effect is **load-dependent, not a uniform
win** — a reliable **−9 to −13%** improvement under load (8B/20u, 13B/30u), a **wash** at
moderate load (13B/20u), and a reliable **+29% regression** at light load (13B/10u) — tracking
cluster throughput. Cache-hit rate rises ~3× in every config but is **decoupled** from P99. Full
table + interpretation: **[benchmark-results-current.md](benchmark-results-current.md)**.

Every headline the OLD guide advertised — P99 TTFT **−80% to −85%**, **+13-22%** throughput —
was measured on the now-**deprecated 5-prefix workload** (all source runs, Instances 1–9,
Jan–Apr 2026, predate the `NUM_LARGE_PREFIXES` 5→50 default change). With 8 backends, a prefix
pool ≤ backend count pigeonholes every prefix onto ≤ 5 backends under pure affinity, which
*manufactures* both the failure modes and, by concentration, much of the win. **Those numbers
are not reproducible on current defaults and must not be quoted as current** — at low load the
representative workload **flips their sign** (10u is a +29% regression, not a −60…−79% win). They
are preserved, as recorded, in the [history archive](history/benchmark-history-8xA100.md#detailed-results-by-instance).

The one open question is the **threshold leg** (shipped `2.0/2` vs raised `3.0/4` load-aware) —
the ~30–47% fallback rates make it the highest-value follow-up. See
[benchmark-results-current.md](benchmark-results-current.md) and the
[campaign runbook](benchmark-rebaseline-campaign.md) §2.

The May 2026 affinity-thrashing investigation that the old TL;DR flagged as "unresolved" was
**closed 2026-05-26 as a workload artifact** (concentration, not a routing defect) — see the
[history archive](history/benchmark-history-8xA100.md) and
[kv-cache-prefix-routing-benchmark.md](kv-cache-prefix-routing-benchmark.md).

## Current defaults (quick reference)

- `NUM_LARGE_PREFIXES` = **50** (≥ backend count; not the deprecated 5) — set in
  `tests/integration/locustfile_real.py`, the single source of truth.
- `SHARED_PREFIX_RATIO` = **0.9**.
- Route-batch flush interval = **20ms** (shipped; the old "10ms is correct" note is
  superseded — the contradiction is reconciled in
  [benchmark-results-current.md](benchmark-results-current.md#current-defaults-the-workload-these-numbers-must-be-measured-under)).
- Load-aware routing **ON** (factor 2.0 / floor 2), cache-residency **0.2**.

See [benchmark-methodology.md](benchmark-methodology.md) for the full knob reference and how
the wrapper scripts forward these (pass-through only when set).
