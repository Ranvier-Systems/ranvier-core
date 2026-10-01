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

| Config | P99 TTFT vs round-robin | Verdict | Route consistency |
|--------|-------------------------|---------|-------------------|
| Llama-3.1-8B, 20 users | **−13.3%** | consistent improvement | 12% → 48% |
| CodeLlama-13B, 30 users | **−9.1%** | consistent improvement | 12% → 38% |
| CodeLlama-13B, 20 users | +3.8% | no reliable effect | 12% → 43% |
| CodeLlama-13B, 10 users | **+29%** | consistent regression | 12% → 49% |

The effect tracks cluster throughput: prefix affinity pays when the GPUs are
queue-bound and costs tail latency when they are idle. Expect route consistency to
rise about threefold in every configuration regardless of the P99 outcome.

## Interpreting Results

### Key Metrics

| Metric | What It Tells You |
|--------|------------------|
| **Route consistency** | % of requests that landed on the same backend as the previous request with that prefix. A client-side affinity proxy, ~1/N under round-robin; roughly 3× higher with prefix routing. Logs before 2026-09-30 label this "cache hit rate". |
| **KV prefix-cache hit rate** | vLLM's own `prefix_cache_hits/queries` counters differenced over the run (token-level). The real cache signal; reported since 2026-09-30 when backends expose it. |
| **TTFT (Time-To-First-Token)** | Latency until the first SSE chunk. Lower = better. Quote the "raw samples" line, not Locust's approximated table (±50 ms above 1 s). |
| **P99 TTFT** | Tail latency. −9% to −13% vs round-robin under sustained load; a regression at light load (July 2026). |
| **Throughput (req/s)** | HTTP requests/sec from the Aggregated row. Before 2026-09-30 this row also counted derived samples and read ~6× high. |
| **Incomplete rate** | Requests that got HTTP 200 but no first token. Compare across arms; a difference here changes how to read P99. |

### What "Good" Looks Like

- **Saturated fleet (queue-bound):** P99 TTFT 9–13% lower than round-robin, every repeat agreeing; route consistency ~3× higher.
- **Moderately loaded fleet:** no reliable P99 effect; route consistency still ~3× higher.
- **Idle fleet:** P99 TTFT worse than round-robin and more incompletes; prefix affinity concentrates load the GPUs did not need relieved.
- **Any load:** the aggregate's verdict line says "CONSISTENT" only when every repeat moved the same way with at least three repeats; treat "MIXED" or "NO RELIABLE EFFECT" as exactly that.

### Detailed Results

See [benchmark-results-current.md](../benchmarks/benchmark-results-current.md) for the citable numbers with manifests, [benchmark-methodology.md](../benchmarks/benchmark-methodology.md) for how to run, and the [history notebook](../benchmarks/history/benchmark-history-8xA100.md) for the dated run-by-run tables.
