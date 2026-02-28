# KV Cache Prefix-Affinity Routing Benchmark

**Date:** January–February 2026
**Environment:** 8x A100 GPUs (40GB and 80GB), single host
**Models:** Llama-3.1-8B, CodeLlama-13b, Llama-3.1-70B
**vLLM:** Prefix caching enabled (`--enable-prefix-caching`)

> **For the comprehensive, authoritative benchmark results** (including 13B, 70B, prefix ratio sweeps, client tokenization, stress tests, and 30-minute validated runs), see: **[Benchmark Guide for 8x A100](benchmark-guide-8xA100.md)**
>
> This document contains the original 8B benchmarks from January 2026 plus updated summary data.

## Executive Summary

Prefix-affinity routing provides **4-7x better cache hit rate** and **up to 78% lower P99 tail latency** compared to round-robin routing when serving LLM inference requests with shared prefixes. Validated stable over 30-minute sustained load (b63c165).

### Validated 30-Minute Run (February 28, 2026 — b63c165)

| Metric | Round-Robin | Prefix-Aware | Change |
|--------|-------------|--------------|--------|
| **Cache Hit Rate** | 12.5% | **73.9%** | **+61.5pp (+493%)** |
| **P99 TTFT** | 4500ms | **980ms** | **-78.2%** |
| **P50 TTFT** | 780ms | **750ms** | -3.8% |
| **Throughput** | 30.6 req/s | **33.5 req/s** | **+9.6%** |
| **Large Prefix P99 (hit)** | 4317ms | **906ms** | **-79.0%** |
| **Errors / Timeouts** | 0 | 0 | — |
| **Total Requests** | 9,510 | 10,354 | +8.9% |

*CodeLlama-13b, 20 users, 30m duration, 8x GPU, `RANVIER_LOAD_IMBALANCE_FACTOR=2.0` (default).
vLLM v0.15.1. Routing overhead: 10.62ms (tokenization: 10.61ms, ART lookup: 0.01ms) — offloaded to thread pool, not blocking reactor.*

### Results Summary (February 21, 2026 — bb20555, full suite)

| Model | Cache Hit Rate | P99 Latency | Throughput | Key Benefit |
|-------|----------------|-------------|------------|-------------|
| **CodeLlama-13b** | 12% → **54-89%** | **-24% to -51%** | **+3% to +14%** | P99 + throughput |
| **Llama-3.1-8B** | 12% → **65-95%** | flat | ~same | Stable, no harm |
| **Llama-3.1-70B** | 49% → **75%** | flat | -18% | Reliability (0% inc) |

*All on 40GB A100s. vLLM v0.15.1 pinned. Thread pool enabled, speculative load increment,
batched route learning. 70B on TP=4 (2 backends). 13B/8B on 8 backends.*

<details>
<summary>Previous best results (Instance 3, Feb 10-14 — click to expand)</summary>

| Model | Cache Hit Rate | XLarge TTFT Improvement | P99 Latency | Throughput |
|-------|----------------|-------------------------|-------------|------------|
| **Llama-3.1-70B** | 25% → **98%** | **44%** faster | ~same | ~same |
| CodeLlama-13b | 12% → **98%** | **33%** faster | **-85%** | **+22%** |
| Llama-3.1-8B | 12% → **98%** | **40%** faster | +6.5% | ~same |

*70B on 80GB A100s (TP=2, 4 backends, 32K context). 13B/8B on 40GB A100s (8 backends).
Pre-batched-route-learning architecture with per-request SMP broadcast.*

</details>

## Test Configuration

| Parameter | Value |
|-----------|-------|
| Backends | 8 vLLM instances (one per GPU) |
| Ranvier Nodes | 3 (clustered) |
| Concurrent Users | 10 |
| Test Duration | 5 minutes per mode |
| Unique Prefixes | 50 |
| Prefix Size | 2,000 - 8,000 tokens |

## Results

### Cache Hit Rate

| Routing Mode | Cache Hits | Cache Misses | Hit Rate |
|--------------|------------|--------------|----------|
| Round-Robin | 116 | 684 | 14.5% |
| Prefix-Affinity | 797 | 50 | **94.1%** |

**Improvement: 6.5x better cache utilization**

### Time to First Token (TTFT)

| Metric | Round-Robin | Prefix-Affinity |
|--------|-------------|-----------------|
| Cache Hit P50 | 433.8ms | 433.5ms |
| Cache Hit P99 | 925.1ms | **487.4ms** |
| Cache Miss P50 | 546.6ms | 636.7ms |
| Cache Miss P99 | 1138.8ms | 1186.3ms |
| Overall Improvement | 20.6% | **31.9%** |

### TTFT by Prefix Size

| Prefix Size | Round-Robin | Prefix-Affinity |
|-------------|-------------|-----------------|
| Large (2-4K tokens) | 20.7% improvement | **27.7% improvement** |
| XLarge (4-8K tokens) | 26.9% improvement | **37.4% improvement** |

Larger prefixes show greater improvement because more computation is saved by cache hits.

### Throughput

| Metric | Round-Robin | Prefix-Affinity |
|--------|-------------|-----------------|
| Total Requests (5 min) | 800 | 847 |
| Avg Request Time | 1015.7ms | **943.2ms** |

**7% higher throughput with prefix-affinity routing**

### Latency Consistency

The P99 cache hit latency tells the story of consistency:
- Round-Robin P99: 925.1ms (high variance)
- Prefix-Affinity P99: 487.4ms (consistent)

Prefix-affinity provides **47% lower tail latency** for cache hits because requests consistently route to backends with cached prefixes.

---

## High Concurrency Results (January 2026)

The following results were obtained with higher concurrency to stress-test the system:

| Parameter | Value |
|-----------|-------|
| Concurrent Users | 30 |
| Test Duration | 10 minutes (with warmup) |
| Prompt Distribution | stress (large prefixes) |
| Prefix Ratio | 0.9 |

### Cache Hit Rate (High Concurrency)

| Routing Mode | Cache Hits | Cache Misses | Hit Rate |
|--------------|------------|--------------|----------|
| Round-Robin | 1,130 | 7,828 | 12.6% |
| Prefix-Affinity | 8,892 | 180 | **98.0%** |

**Improvement: 7.8x better cache utilization**

### TTFT by Prefix Size (High Concurrency)

| Prefix Size | Round-Robin | Prefix-Affinity |
|-------------|-------------|-----------------|
| Large (2-4K tokens) | -0.3% improvement | **3.7% improvement** |
| XLarge (4-8K tokens) | 0.7% improvement | **25.9% improvement** |

### Key Observations

1. **Cache hit rate scales well** - Even at 64 concurrent users, prefix-affinity maintains 98% hit rate
2. **TTFT improvements are lower at high concurrency** - Contention reduces the per-request benefit, but aggregate throughput remains higher
3. **XLarge prefixes still show significant gains** - 25.9% improvement demonstrates the value for long-context workloads

---

## How It Works

### Round-Robin Routing
- Requests distributed randomly across backends
- Cache hits only occur by chance (1/8 = 12.5% expected)
- Each backend caches all prefixes over time (memory inefficient)

### Prefix-Affinity Routing
- Same prefix always routes to same backend (via consistent hashing)
- First request is a cache miss, all subsequent are hits
- Each backend only caches its assigned prefixes (memory efficient)

## Routing Modes

Ranvier supports three routing modes configurable via `RANVIER_ROUTING_MODE`:

| Mode | Description | Use Case |
|------|-------------|----------|
| `prefix` | ART lookup + consistent hash (learns routes) | **Production** - best KV cache reuse |
| `hash` | Consistent hash only (no ART, no learning) | Measures hash baseline vs ART |
| `random` | Weighted random (no affinity) | Baseline comparison |

### Mode Comparison

| Scenario | `random` | `hash` | `prefix` |
|----------|----------|--------|----------|
| Same exact prefix | Different backend | Same backend ✓ | Same backend ✓ |
| Similar prefix (90% overlap) | Different backend | Likely different | Same backend ✓ (LPM) |
| Learning over time | No | No | Yes |
| Backend changes | Random redistribution | Hash redistribution | ART re-learns |

The `prefix` mode's **Longest Prefix Match (LPM)** via ART is the key differentiator - it handles prefix variations (same system prompt, different user queries) that pure hashing cannot.

### Configuration Variables

There are two separate environment variables to be aware of:

| Variable | Scope | Purpose |
|----------|-------|---------|
| `RANVIER_ROUTING_MODE` | **Server** | Controls actual routing behavior in Ranvier |
| `BENCHMARK_MODE` | **Client** | Labels test results in locust benchmark script |

**Important:** These must be synchronized when running benchmarks! If you set `BENCHMARK_MODE=random` but forget to set `RANVIER_ROUTING_MODE=random`, the server will use its default (`prefix`) while your test results are mislabeled.

### Verifying Configuration

Use the `X-Routing-Mode` response header to verify the actual routing mode:

```bash
# Quick verification - check the actual routing mode in response headers
curl -s -D - http://localhost:8081/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"test","messages":[{"role":"user","content":"test"}]}' 2>&1 | grep -i x-routing-mode

# Expected output shows actual server-side routing mode:
# X-Routing-Mode: prefix
# X-Routing-Mode: hash
# X-Routing-Mode: random
```

This header is invaluable for debugging configuration mismatches between client expectations and server behavior.

## Conclusions

1. **Prefix-affinity routing is essential for KV cache optimization** — 4-7x better cache utilization translates directly to lower latency and higher throughput.

2. **13B is the sweet spot for aggregate metrics** — Queue buildup under load makes routing dramatically effective: P99 -24% to -51% (bb20555), up to -85% (Instance 3). Throughput improves +3% to +22%.

3. **Tail latency improves under sustained load** — P99 improvements are strongest at high concurrency and long duration (13B 30u 30m: -51.3% on bb20555).

4. **8B is routing-neutral** — Inference too fast (~1400ms) for cache savings to affect aggregate TTFT. No harm, but no benefit.

5. **70B benefit is reliability** — Incompletes eliminated (0.6% → 0%), but throughput cost from concentrating load for cache affinity.

6. **Memory efficiency** — With prefix-affinity, each backend only needs to cache its assigned prefixes rather than potentially caching all prefixes.

7. **Prefix ratio 0.9 is the reliable operating point** — Lower ratios (0.5-0.7) may cause hot-spotting depending on architecture version and concurrency.

See the [Benchmark Guide for 8x A100](benchmark-guide-8xA100.md) for full methodology, per-run data, and detailed analysis.

## Reproducing This Benchmark

### Quick Method (Recommended)

Use the consolidated benchmark script which handles vLLM startup, Ranvier cluster, and Locust automatically:

```bash
# First-time setup
./scripts/bench.sh --setup

# Set HuggingFace token
export HF_TOKEN="hf_your_token_here"

# Run A/B comparison (prefix vs round-robin)
./scripts/bench.sh \
  --model meta-llama/Llama-3.1-8B-Instruct \
  --compare \
  --warmup \
  --duration 5m \
  --users 10 \
  --prompt-dist stress \
  --prefix-ratio 0.9

# Parse and compare results
./tests/integration/results_parser.py compare \
  benchmark-reports/*round_robin*/benchmark.log \
  benchmark-reports/*prefix*/benchmark.log
```

For detailed scenarios and expected results, see: [Benchmark Guide for 8x A100](benchmark-guide-8xA100.md)

### Manual Method (Advanced)

For more control over individual components:

```bash
# Start 8 vLLM backends with prefix caching
for i in {0..7}; do
  CUDA_VISIBLE_DEVICES=$i python -m vllm.entrypoints.openai.api_server \
    --model meta-llama/Llama-3.1-8B-Instruct \
    --port $((8000+i)) \
    --enable-prefix-caching &
done

# Start Ranvier with prefix-affinity (default)
export RANVIER_ROUTING_MODE=prefix
docker compose -f docker-compose.benchmark-real.yml up -d

# Register backends
for i in {1..8}; do
  curl -X POST "http://localhost:8081/admin/backends?id=$i&ip=172.17.0.1&port=$((7999+i))&weight=100"
done

# Run benchmark
BENCHMARK_MODE=prefix PROMPT_DISTRIBUTION=stress NUM_LARGE_PREFIXES=50 \
BACKEND1_IP=172.17.0.1 NUM_BACKENDS=8 \
locust -f tests/integration/locustfile_real.py \
  --headless --users 10 --spawn-rate 2 --run-time 5m \
  --host http://localhost:8081
```

## Optional Follow-up Tests

### Longer Duration Test
For more statistical confidence, run a 15-30 minute test:

```bash
# 15-minute test for more data points
./scripts/bench.sh --duration 15m --users 10 --prompt-dist stress
```

### Higher Concurrency
Test with more concurrent users to measure throughput under load:

```bash
# 50 concurrent users (high concurrency stress test)
./scripts/bench.sh --duration 10m --users 50 --spawn-rate 5 --prompt-dist stress
```

### Different Prefix Ratios
Test how cache hit rate varies with prefix sharing:

```bash
# Low prefix sharing (50%)
./scripts/bench.sh --duration 5m --users 10 --prefix-ratio 0.5

# High prefix sharing (95%)
./scripts/bench.sh --duration 5m --users 10 --prefix-ratio 0.95
```

### Different Prompt Distributions

```bash
# Short prompts (< 100 tokens)
./scripts/bench.sh --duration 5m --users 20 --prompt-dist short

# Mixed workload (production-like)
./scripts/bench.sh --duration 10m --users 30 --prompt-dist mixed

# Stress test with large prefixes (2000-8000 tokens)
./scripts/bench.sh --duration 10m --users 10 --prompt-dist stress
```

### Compare All Routing Modes (A/B Test)

The `--compare` flag automatically runs both round-robin and prefix-aware routing:

```bash
# Automatic A/B comparison
./scripts/bench.sh --compare --duration 10m --users 20 --prompt-dist stress

# View comparison
./tests/integration/results_parser.py compare \
  benchmark-reports/*round_robin*/benchmark.log \
  benchmark-reports/*prefix*/benchmark.log
```

### Multi-Depth Routing (Option C)

For workloads with conversation continuations or branching, test multi-depth route storage:

```bash
# Enable multi-depth routing - stores routes at each message boundary
./scripts/bench.sh --multi-depth --compare --duration 10m --users 10

# Best with conversation-style prompts that share history
./scripts/bench.sh --multi-depth --duration 10m \
  --prompt-file tests/integration/data/lmsys/lmsys_10k_shared_prefix.jsonl
```

Multi-depth routing is most beneficial when:
- Users continue existing conversations (routes learned at deeper depths)
- Multiple users branch from a common conversation point
- System prompts are combined with multi-turn dialogue

See [Prefix Affinity Routing Internals](../internals/prefix-affinity-routing.md) for details on Options A, B, and C.

### Manual Multi-Mode Comparison

For testing all three routing modes individually:

```bash
for mode in prefix hash random; do
  echo "Testing routing mode: $mode"

  # Set routing mode and run benchmark
  RANVIER_ROUTING_MODE=$mode ./scripts/bench.sh \
    --duration 5m \
    --users 10 \
    --prompt-dist stress \
    --output-dir "benchmark-reports/${mode}_test"
done

# Compare results
./tests/integration/results_parser.py summary benchmark-reports/*/benchmark.log
```

## Related Files

### Benchmark Tools
- `scripts/bench.sh` - Consolidated benchmark script (setup, run, compare)
- `tests/integration/results_parser.py` - Parse, summarize, and compare results
- `tests/integration/locustfile_real.py` - Locust load test for real vLLM
- `docs/benchmark-guide-8xA100.md` - Detailed test scenarios for 8x A100

### Implementation
- `src/router_service.cpp` - Routing implementation (ART + consistent hashing)
- `src/http_controller.cpp` - Request handling with routing mode selection
- `src/radix_tree.cpp` - Adaptive Radix Tree for prefix matching
- `docs/internals/prefix-affinity-routing.md` - Options A, B, C routing strategies
