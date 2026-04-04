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

### Expected Results (8x A100, 30-Minute Runs)

| Model | Cache Hits | P99 TTFT Improvement | Throughput Gain |
|-------|-----------|---------------------|-----------------|
| Llama-3.1-70B (TP=2) | 25% → 98% | **44% faster** | ~same |
| CodeLlama-13B | 12% → 58–98% | **-60% to -85%** | **+4% to +22%** |
| Llama-3.1-8B | 12% → 68–98% | **40% faster** | ~same |

13B is the sweet spot: queue-bound under load, so routing has the largest impact.

## Interpreting Results

### Key Metrics

| Metric | What It Tells You |
|--------|------------------|
| **Cache hit rate** | % of requests that found a warm KV cache. Target: >80% |
| **TTFT (Time-To-First-Token)** | Latency until first token streams back. Lower = better |
| **P99 latency** | Tail latency. Ranvier targets -60% to -85% vs baseline |
| **Throughput (rps)** | Requests/sec. Expect +4–22% on queue-bound models |
| **Incomplete rate** | Should be 0%. Ranvier retries stale connections |

### What "Good" Looks Like

- **8B models:** High cache hits (>90%), modest TTFT improvement. Compute-light, so less headroom.
- **13B models:** Best overall improvement. P99 drops 60–85%, throughput up 4–22%.
- **70B models:** Highest per-request TTFT savings (44%), but compute-bound — throughput stays flat.

### Detailed Results

See [CHANGELOG.md](../../CHANGELOG.md) and the full [Benchmark Guide](../benchmarks/benchmark-guide-8xA100.md) for run-by-run data, methodology notes, and hardware-specific tuning.
