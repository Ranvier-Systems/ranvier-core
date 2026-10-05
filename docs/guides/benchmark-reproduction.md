# Benchmark Reproduction Guide

Validate Ranvier's performance claims on your own hardware.

## Requirements

- **Quick benchmark (mock backends):** Docker, Docker Compose — no GPUs needed
- **Real benchmark:** 8x A100 GPUs (40GB or 80GB), Docker, Python 3.10+
- **Time:** 5 minutes (mock) or 30–60 minutes (real)

## Quick Benchmark (Mock Backends, 5 Minutes)

Uses mock vLLM backends to measure Ranvier's routing overhead and throughput ceiling.

```bash
# Start 3 Ranvier nodes + 2 mock backends
docker compose -f docker-compose.test.yml up -d

# Wait for health checks to pass
docker compose -f docker-compose.test.yml ps

# Run the benchmark
./tests/integration/run-benchmark.sh
```

### Expected Results (Mock)

| Metric | Expected |
|--------|----------|
| Throughput | ~473 rps |
| P99 latency | <140ms |
| P50 latency | <50ms |
| Error rate | 0% |

These numbers measure Ranvier overhead only — mock backends respond instantly. Real TTFT improvements depend on your GPU backends and workload.

```bash
# Clean up
docker compose -f docker-compose.test.yml down
```

## Real vLLM Benchmark (Requires GPUs)

For validated end-to-end performance with real LLM inference:

### Setup

See [`docs/benchmarks/benchmark-guide-8xA100.md`](../benchmarks/benchmark-guide-8xA100.md) for the full methodology, including:

- Hardware configuration and NUMA-aware CPU pinning
- vLLM server setup with `--enable-prefix-caching`
- Tensor parallelism settings by model size (TP=1 for 8B/13B, TP=2 for 70B)

### Run

```bash
# Automated benchmark runner
./scripts/bench.sh --duration 30m --users 20

# Or use Locust directly for more control
cd tests/integration
locust -f locustfile_real.py \
  --host http://ranvier:8080 \
  --users 20 --spawn-rate 2 \
  --run-time 30m --headless
```

### Expected Results (8x A100, 50-prefix workload, July 2026)

These are the citable figures (median of three `--compare` repeats; see
[benchmark-results-current.md](../benchmarks/benchmark-results-current.md)). The
February 2026 numbers quoted in older copies of this guide came from a five-prefix
workload the project has since deprecated and are not expected results.

| Config | P99 TTFT vs round-robin | Verdict | KV hit rate, RR → prefix |
|--------|-------------------------|---------|--------------------------|
| CodeLlama-13B, 20 users, fitted 16-prefix set (2026-10-05 defaults) | **−57.5%** median (−60.4, −57.5, −55.2) | consistent improvement, 3/3 | 12–14% → 69–73% |
| CodeLlama-13B, 10 users, fitted (2026-10-05 defaults, 1 repeat) | −34.5% | confirmation | 17% → 82% |
| Llama-3.1-8B, 20 users (2026-10-05 defaults, 1 repeat) | −22.6% | confirmation | 72% → 96% |
| Llama-3.1-8B, 20 users (2026-10-01 re-baseline, previous defaults) | **−17.0%** median | consistent improvement, 3/3 | 72% → 94% |
| CodeLlama-13B, 20 users (2026-10-01 re-baseline, previous defaults) | +11.0% median | consistent regression, 3/3 | 4% → 19% |

The 2026-10-01 13B regression was the load-divert policy reading a stale signal at too tight a
threshold, not the affinity; the 2026-10-05 defaults (node-local in-flight signal, ε 1.0,
least-loaded placement) resolved it. Expect route consistency to rise about fourfold and the
per-backend request distribution to stay uneven (Gini 0.09–0.12) in every prefix arm; the tail
is set by queue depth, which the divert policy now sees live. The 13B 30-user row (50 prefixes,
eviction regime) times out in both arms and is directional only.

## Interpreting Results

### Key Metrics

| Metric | What It Tells You |
|--------|------------------|
| **Route consistency** | % of requests that landed on the same backend as the previous request with that prefix. A client-side affinity proxy, ~1/N under round-robin; roughly 3× higher with prefix routing. Logs before 2026-09-30 label this "cache hit rate". |
| **KV prefix-cache hit rate** | vLLM's own `prefix_cache_hits/queries` counters differenced over the run (token-level). The real cache signal; reported since 2026-09-30 when backends expose it. |
| **TTFT (Time-To-First-Token)** | Latency until the first SSE chunk. Lower = better. Quote the "raw samples" line, not Locust's approximated table (±50 ms above 1 s). |
| **P99 TTFT** | Tail latency. −22% (8B) to −58% (13B, fitted set) vs round-robin under the 2026-10-05 defaults; the July 2026 light-load regression and the October 1 13B regression were the previous divert policy. |
| **Throughput (req/s)** | HTTP requests/sec from the Aggregated row. Before 2026-09-30 this row also counted derived samples and read ~6× high. |
| **Incomplete rate** | Requests that got HTTP 200 but no first token. Compare across arms; a difference here changes how to read P99. |

### What "Good" Looks Like

- **Saturated fleet (queue-bound):** P99 TTFT 9–13% lower than round-robin, every repeat agreeing; route consistency ~3× higher.
- **Moderately loaded fleet:** no reliable P99 effect; route consistency still ~3× higher.
- **Idle fleet:** P99 TTFT worse than round-robin and more incompletes; prefix affinity concentrates load the GPUs did not need relieved.
- **Any load:** the aggregate's verdict line says "CONSISTENT" only when every repeat moved the same way with at least three repeats; treat "MIXED" or "NO RELIABLE EFFECT" as exactly that.

### Detailed Results

See [benchmark-results-current.md](../benchmarks/benchmark-results-current.md) for the citable numbers with manifests, [benchmark-methodology.md](../benchmarks/benchmark-methodology.md) for how to run, and the [history notebook](../benchmarks/history/benchmark-history-8xA100.md) for the dated run-by-run tables.
