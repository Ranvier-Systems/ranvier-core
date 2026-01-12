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
| `prefix` | Hybrid ART + consistent hashing | **Recommended for LLM inference** |
| `radix` | ART lookup + random fallback | Adaptive learning scenarios |
| `round_robin` | Pure random/weighted | Baseline, stateless routing |

### Configuration Variables

There are two separate environment variables to be aware of:

| Variable | Scope | Purpose |
|----------|-------|---------|
| `RANVIER_ROUTING_MODE` | **Server** | Controls actual routing behavior in Ranvier |
| `BENCHMARK_MODE` | **Client** | Labels test results in locust benchmark script |

**Important:** These must be synchronized when running benchmarks! If you set `BENCHMARK_MODE=round_robin` but forget to set `RANVIER_ROUTING_MODE=round_robin`, the server will use its default (`prefix`) while your test results are mislabeled.

### Verifying Configuration

Use the `X-Routing-Mode` response header to verify the actual routing mode:

```bash
# Quick verification - check the actual routing mode in response headers
curl -s -D - http://localhost:8081/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"test","messages":[{"role":"user","content":"test"}]}' 2>&1 | grep -i x-routing-mode

# Expected output shows actual server-side routing mode:
# X-Routing-Mode: prefix
# X-Routing-Mode: round_robin
# X-Routing-Mode: radix
```

This header is invaluable for debugging configuration mismatches between client expectations and server behavior.

## Conclusions

1. **Prefix-affinity routing is essential for KV cache optimization** - 6.5x better cache utilization translates directly to lower latency and higher throughput.

2. **Larger prefixes benefit more** - XLarge prefixes (4-8K tokens) see 37% TTFT improvement vs 28% for large prefixes.

3. **Tail latency improves dramatically** - P99 latency drops 47% because cache behavior is predictable, not random.

4. **Memory efficiency** - With prefix-affinity, each backend only needs to cache its assigned prefixes rather than potentially caching all prefixes.

## Reproducing This Benchmark

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
BENCHMARK_MODE=prefix PROMPT_DISTRIBUTION=large-prefix NUM_LARGE_PREFIXES=50 \
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
locust -f tests/integration/locustfile_real.py \
  --headless --users 10 --spawn-rate 2 --run-time 15m \
  --host http://localhost:8081
```

### More Prefixes
Test with 100+ unique prefixes to stress the routing and cache systems:

```bash
# 100 unique prefixes
NUM_LARGE_PREFIXES=100 \
locust -f tests/integration/locustfile_real.py \
  --headless --users 10 --spawn-rate 2 --run-time 5m \
  --host http://localhost:8081
```

### Higher Concurrency
Test with more concurrent users to measure throughput under load:

```bash
# 50 concurrent users
locust -f tests/integration/locustfile_real.py \
  --headless --users 50 --spawn-rate 5 --run-time 5m \
  --host http://localhost:8081
```

### Different Prefix Sizes
Adjust the prefix token range to test smaller or larger prefixes:

```bash
# Smaller prefixes (500-2000 tokens)
LARGE_PREFIX_MIN_TOKENS=500 LARGE_PREFIX_MAX_TOKENS=2000 \
locust -f tests/integration/locustfile_real.py \
  --headless --users 10 --spawn-rate 2 --run-time 5m \
  --host http://localhost:8081

# Larger prefixes (8000-16000 tokens) - requires sufficient GPU memory
LARGE_PREFIX_MIN_TOKENS=8000 LARGE_PREFIX_MAX_TOKENS=16000 \
locust -f tests/integration/locustfile_real.py \
  --headless --users 10 --spawn-rate 2 --run-time 5m \
  --host http://localhost:8081
```

### Compare All Three Routing Modes
Run the same test with each routing mode:

```bash
for mode in prefix radix round_robin; do
  echo "Testing routing mode: $mode"
  docker compose -f docker-compose.benchmark-real.yml down
  export RANVIER_ROUTING_MODE=$mode
  docker compose -f docker-compose.benchmark-real.yml up -d
  sleep 10

  # Register backends
  for i in {1..8}; do
    curl -s -X POST "http://localhost:8081/admin/backends?id=$i&ip=172.17.0.1&port=$((7999+i))&weight=100"
  done

  # Run benchmark
  BENCHMARK_MODE=$mode PROMPT_DISTRIBUTION=large-prefix NUM_LARGE_PREFIXES=50 \
  BACKEND1_IP=172.17.0.1 NUM_BACKENDS=8 \
  locust -f tests/integration/locustfile_real.py \
    --headless --users 10 --spawn-rate 2 --run-time 5m \
    --host http://localhost:8081 2>&1 | tee ${mode}_results.txt
done
```

## Related Files

- `tests/integration/locustfile_real.py` - Benchmark script
- `src/router_service.cpp` - Routing implementation
- `src/http_controller.cpp` - Request handling with routing mode selection
