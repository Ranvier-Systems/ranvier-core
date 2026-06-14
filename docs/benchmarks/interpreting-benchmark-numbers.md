# Interpreting Benchmark Numbers

**Purpose:** When comparing Ranvier to other LLM routers you will find headline
numbers that look far larger than ours (e.g. "86% faster TTFT"). Before
concluding another router is faster, check *how* the number was produced. A
routing benchmark's headline percentage is set mostly by the **baseline** and
the **metric chosen**, not by router quality. This note states how Ranvier
measures, and the questions to ask of any cache-aware-routing benchmark —
including our own.

## How Ranvier's numbers are measured

| Choice | Ranvier |
|--------|---------|
| Backends | Real GPUs (8×A100), real vLLM (v0.15.1) — not a latency simulator |
| Backend caching | **Enabled in both arms** (`--enable-prefix-caching` / RadixAttention) |
| Baseline arm | `random` routing (no affinity) on those same real backends |
| Headline metric | **P99 TTFT** and **cache-hit rate** |
| Disclosure | Neutral cases and regressions are documented, not dropped |

Two consequences worth internalizing:

- **Our baseline already gets cache hits.** With prefix caching on, `random`
  routing lands a request on a backend that already holds the prefix roughly
  **1/N** of the time — ~12.5% at 8 backends, ~49% at 2. Affinity's job is to
  push that toward ~1.0. The baseline is *not* "cold on every request."
- **Our P50 is often flat; the win is in the tail.** In a validated 30-minute
  run, P50 TTFT moved −3.8% while P99 moved −78% and hit rate went 12.5% → 74%.
  On real backends the median request frequently already benefits from
  backend-side caching, so the honest gains surface as **tail latency** and
  **hit-rate**, not median TTFT. On fast models (8B) aggregate TTFT is
  routing-neutral — no benefit, no harm.

## Questions to ask of any cache-aware-routing benchmark

1. **Real backends or a simulator?** A simulator that sleeps a fixed "hit" vs
   "miss" latency, with no per-replica cache state, pins the baseline at **0%
   hit** — every non-routed request is a full cold prefill. Every percentage
   then looks enormous by construction.
2. **Is backend prefix caching enabled in the baseline?** Real vLLM/SGLang cache
   per replica regardless of the router. A "cold every time" baseline inflates
   the win; a realistic baseline floors at ≈1/N.
3. **Is the headline P50 or P99 / hit-rate?** P50 on a high-locality workload
   flatters cache routing — the median request is a hit by construction. Tail
   latency and hit-rate are the harder, more honest figures.
4. **How diverse is the workload?** A trace that is, say, 70% one identical
   prompt across a handful of backends makes *any* affinity router look great.
   Real value shows on high-cardinality prompts, partial-prefix overlap, and
   higher backend fan-out.
5. **One best-case run, or steady state with regressions disclosed?** A single
   cherry-picked run is not a result.

## Why the baseline dominates the headline

"% faster" is largely a function of how far you moved off the baseline you
chose. The same optimized system, measured against two baselines:

| Baseline assumption | Baseline hit rate | Typical headline shape |
|---------------------|-------------------|------------------------|
| Cold every request (simulated, no backend cache) | 0% | Large P50 cut (e.g. 800ms → 100ms ≈ −88%) |
| Round-robin, backend caching on (realistic) | ≈1/N (12–49%) | Small P50 move; gain is in P99 + hit-rate |

A "−88% over a 0% baseline" and a "−78% P99 over a 12.5% baseline" are not the
same claim measured two ways — they are different baselines, and the second is
the harder one to earn.

## Comparing against a specific third-party router

Don't compare headline numbers across benchmarks. Put both routers on the
**same workload and the same real backends (caching on)** and compare **P99
TTFT plus cache-hit rate**. `tests/integration/run_benchmark_comparison.py`
runs the `random` vs `prefix` A/B against `docker-compose.benchmark-real.yml`;
pointing a third-party router's load generator at the same backends yields an
apples-to-apples number. Anything else is comparing a lap time to a dyno run.

## See also

- [KV Cache Prefix-Affinity Routing Benchmark](kv-cache-prefix-routing-benchmark.md) — methodology, baseline definition, validated runs
- [Benchmark Guide for 8x A100](benchmark-guide-8xA100.md) — full per-run data and scenarios
- [Prefix Affinity Routing Internals](../internals/prefix-affinity-routing.md) — why the baseline floors at ≈1/N
