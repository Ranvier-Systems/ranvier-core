# KV Cache Prefix-Affinity Routing Benchmark

**Date:** January 2026
**Environment:** 8x A100 40GB GPUs, single host
**Model:** meta-llama/Llama-3.1-8B-Instruct
**vLLM:** Prefix caching enabled (`--enable-prefix-caching`)

## Executive Summary

Prefix-affinity routing provides **6.5x better cache hit rate** and **7% faster overall throughput** compared to round-robin routing when serving LLM inference requests with shared prefixes.

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

1. **Prefix-affinity routing is essential for KV cache optimization** - 6.5x better cache utilization translates directly to lower latency and higher throughput.

2. **Larger prefixes benefit more** - XLarge prefixes (4-8K tokens) see 37% TTFT improvement vs 28% for large prefixes.

3. **Tail latency improves dramatically** - P99 latency drops 47% because cache behavior is predictable, not random.

4. **Memory efficiency** - With prefix-affinity, each backend only needs to cache its assigned prefixes rather than potentially caching all prefixes.

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
