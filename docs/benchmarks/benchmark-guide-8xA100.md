# Benchmark Guide: 8x A100 Single-Host Setup

This guide provides specific test scenarios, expected results, and validation criteria for benchmarking Ranvier on a Lambda Labs 8x A100 instance.

## TL;DR Results

Prefix-aware routing with load-aware fallback vs round-robin baseline:

| Model | Cache Hit Rate | XLarge TTFT Improvement | P99 Latency | Throughput |
|-------|----------------|------------------------|-------------|------------|
| CodeLlama-13b | 12% → **98%** | **93%** faster | **-76%** | **+17%** |
| Llama-3.1-8B | 12% → **98%** | **37%** faster | — | ~same |

*Results from February 2026 with load-aware routing enabled (10-minute runs, 30 users, 8x A100).*

**Key wins:**
- **8x more cache hits** — Requests routed to backends with cached KV data
- **37-93% faster TTFT** — For large prefixes (4K+ tokens), time-to-first-token drops significantly
- **Up to 76% lower tail latency** — P99 response times improve dramatically
- **Up to 17% higher throughput** — Load-aware routing prevents backend hotspots

**Works across prefix sharing levels:**

| Prefix Sharing | Cache Hit Rate | Improvement |
|----------------|----------------|-------------|
| 90% | 98% | 39% |
| 70% | 93% | 42% |
| 50% | 90% | 41% |

*Cache efficiency benchmarks use synthetic workloads simulating RAG/system-prompt patterns with large prefixes.*

---

## Hardware Configuration

| Component | Specification |
|-----------|---------------|
| GPUs | 8x NVIDIA A100 (40GB SXM4) |
| vCPUs | 124 |
| RAM | 1800 GiB |
| Storage | 6 TiB NVMe SSD |
| Cost | ~$10/hour |

## Quick Start

```bash
# 1. SSH to your Lambda instance
ssh ubuntu@<instance-ip>

# 2. Clone and setup
git clone git@github.com:Ranvier-Systems/ranvier-core.git
cd ranvier-core
./scripts/bench.sh --setup

# 2b. If prompted about docker group, run this then re-run setup:
newgrp docker
./scripts/bench.sh --setup

# 3. Set HuggingFace token (required for Llama models)
export HF_TOKEN="hf_your_token_here"

# 4. Run benchmark with warm-up
./scripts/bench.sh \
  --model meta-llama/Llama-3.1-8B-Instruct \
  --warmup \
  --duration 10m \
  --users 30 \
  --prompt-dist stress \
  --prefix-ratio 0.9
```

---

## Warm-up Mode

The `--warmup` flag runs a short preliminary benchmark before the main test:

| Parameter | Value |
|-----------|-------|
| Duration | 1 minute |
| Users | 2 concurrent |
| Pause after | 10 seconds |

**What it primes:**
1. **vLLM's KV cache** - Populates prefix patterns so subsequent requests benefit from cached key-value computations
2. **Ranvier's ART** - Learns routes in the Adaptive Radix Tree so prefix-aware routing can direct traffic to backends that already have the prefix cached

**When to use:**
- Always recommended for consistent, reproducible results
- Essential on cold starts (fresh vLLM instances)
- Required for accurate A/B comparisons

**Without warm-up:** First requests will all be cache misses, skewing early results and reducing measured improvement percentages.

```bash
# With warm-up (recommended)
./scripts/bench.sh --warmup --duration 10m --users 30

# Without warm-up (faster, but less consistent)
./scripts/bench.sh --duration 10m --users 30
```

---

## Benchmark Parameters Reference

### Command-Line Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `--duration` | 5m | Test duration (e.g., `5m`, `10m`, `1h`) |
| `--users` | 10 | Concurrent simulated clients |
| `--spawn-rate` | 2 | Users to spawn per second at start |
| `--warmup` | off | Run 1-minute warmup before main test |
| `--compare` | off | Run both round-robin and prefix modes for A/B comparison |
| `--prompt-dist` | stress | Prompt size distribution (see below) |
| `--prefix-ratio` | 0.9 | Ratio of requests sharing prefixes (0.0-1.0) |
| `--model` | Llama-3.1-8B | Model to benchmark |
| `--client-tokenize` | off | Tokenize on client side (see [Client Tokenization](#client-tokenization)) |
| `--debug` | off | Build with debug symbols for crash investigation |

### Prompt Distribution (`--prompt-dist`)

Controls the **size distribution** of generated prompts:

| Value | Distribution | Use Case |
|-------|-------------|----------|
| `short` | 100% tiny (<100 tokens) | Fast iteration, minimal GPU load |
| `medium` | 100% small (100-500 tokens) | Moderate workload |
| `long` | 100% small (100-500 tokens) | Similar to medium |
| `mixed` | 30% short, 50% medium, 20% long | Production-like variety |
| `large_prefix` | 100% large/xlarge (2000-8000 tokens) | Maximum cache benefit |
| **`stress`** | 10% small, 20% medium, 30% large, 40% xlarge | **Recommended for benchmarking** |

**Why `stress` is the default:** Large prefixes (2000-8000 tokens) benefit most from KV cache hits. The `stress` distribution biases toward these sizes (70% are 2000+ tokens) to maximize measurable improvement from prefix-aware routing.

### Prefix Ratio (`--prefix-ratio`)

Controls what **percentage of requests share a common system prompt**:

| Value | Meaning | Cache Hit Potential |
|-------|---------|---------------------|
| 0.1 | 10% share prefixes | Low (mostly unique requests) |
| 0.5 | 50% share prefixes | Medium |
| 0.7 | 70% share prefixes | Good |
| **0.9** | 90% share prefixes | **High (recommended)** |
| 1.0 | 100% share prefixes | Maximum (unrealistic) |

**How it works:** When generating a prompt, there's a 90% chance (at 0.9) it picks from a pool of shared system prompts. This simulates real-world patterns where many users share the same system prompt (e.g., "You are a helpful assistant...") but have different user queries.

**Expected cache hit rates with prefix-aware routing:**
- `--prefix-ratio 0.5` → ~50% cache hit rate
- `--prefix-ratio 0.9` → ~90%+ cache hit rate
- Round-robin baseline → ~12.5% (1/8 chance with 8 backends)

### Token Size Buckets

The benchmark categorizes prompts into size buckets for analysis:

| Bucket | Token Range | Typical Content |
|--------|-------------|-----------------|
| tiny | 0-100 | Simple queries |
| small | 100-500 | Short conversations |
| medium | 500-2000 | Moderate context |
| large | 2000-4000 | RAG documents, long system prompts |
| xlarge | 4000-8000 | Large context windows |

**Key insight:** XLarge prefixes show the most dramatic improvement (37-93% TTFT reduction with load-aware routing) because they save the most KV cache computation.

### Using Custom Prompt Files (`--prompt-file`)

Instead of synthetic prompts, you can benchmark with real conversation data using JSONL files.

**Basic usage:**
```bash
./scripts/bench.sh --compare --prompt-file data/prompts.jsonl --duration 10m --users 10
```

**Supported formats:**
- **OpenAI:** `{"messages": [{"role": "system", "content": "..."}, {"role": "user", "content": "..."}]}`
- **ShareGPT:** `{"conversations": [{"from": "human", "value": "..."}, {"from": "gpt", "value": "..."}]}`

**Validating prompt files:**

Before benchmarking, verify your prompt file has good prefix sharing characteristics:

```bash
# Check prefix distribution
python3 tests/integration/prompt_loader.py prefixes data/prompts.jsonl

# Example output for a well-structured file:
# Total prompts:      9950
# Unique prefixes:    3
# Avg per prefix:     3316.7
#
# Top 3 most common prefixes:
#   1.  3342 prompts (33.6%): "You are a coding expert..."
#   2.  3324 prompts (33.4%): "You are a customer service agent..."
#   3.  3284 prompts (33.0%): "You are a helpful assistant..."
```

**What makes a good prompt file for cache benchmarking:**

| Characteristic | Good | Poor |
|----------------|------|------|
| Unique prefixes | 3-50 | 1000+ |
| Prompts per prefix | 100+ | <10 |
| System message presence | Yes | No |

**Important:** The benchmark tracks cache hits based on **system messages only** (the shared prefix). Multi-turn conversation history is unique per conversation and doesn't contribute to cache hits.

**Included test files:**
```bash
# Synthetic with shared prefixes (good for benchmarking)
tests/integration/data/lmsys/lmsys_10k_shared_prefix.jsonl

# Validate before use
python3 tests/integration/prompt_loader.py prefixes tests/integration/data/lmsys/lmsys_10k_shared_prefix.jsonl
python3 tests/integration/prompt_loader.py stats tests/integration/data/lmsys/lmsys_10k_shared_prefix.jsonl
```

---

## Client Tokenization

The `--client-tokenize` flag moves tokenization from Ranvier to the benchmark client. This simulates production deployments where clients send pre-tokenized requests.

### Trade-offs by Model Size

Results from 30-minute benchmarks at 20 users:

#### CodeLlama-13b

| Metric | Server Tokenize | Client Tokenize | Difference |
|--------|-----------------|-----------------|------------|
| Routing overhead | ~17ms | ~0.4ms | **44x lower** |
| Throughput (req/s) | 25.4 | 31.1 | **+22%** |
| XLarge Improvement % | 38.9% | 4.1% | See below |
| XLarge Hit P50 | 886ms | 733ms | -17% |
| XLarge Miss P50 | 1451ms | 764ms | -47% |

#### Llama-3.1-8B

| Metric | Server Tokenize | Client Tokenize | Difference |
|--------|-----------------|-----------------|------------|
| Routing overhead | ~16ms | ~0.4ms | **40x lower** |
| Throughput (req/s) | 33.1 | 32.8 | ~same |
| XLarge Improvement % | 43.7% | 36.0% | See below |
| XLarge Hit P50 | 453ms | 451ms | ~same |
| XLarge Miss P50 | 804ms | 705ms | -12% |

**Why throughput differs by model:** The 8B model is faster overall (~450ms vs ~900ms for 13B), so the 15-17ms tokenization overhead is a smaller percentage of total request time. For slower models, client tokenization provides a bigger throughput boost.

### Why XLarge Improvement % Drops

The "XLarge Improvement" metric measures the relative benefit of cache hits vs misses:

```
Improvement = (miss_latency - hit_latency) / miss_latency
```

With client tokenization, **cache misses also get faster** because the server skips tokenization. The **absolute latencies are better**—both hits and misses are faster. The relative improvement shrinks because the denominator (miss latency) dropped significantly.

### When to Use

**Use `--client-tokenize` when:**
- Simulating production deployments with pre-tokenized requests
- Benchmarking larger/slower models where throughput gains are significant
- You want to measure Ranvier's pure routing overhead

**Use server tokenization (default) when:**
- Measuring the benefit of prefix-aware routing vs round-robin
- Clients send raw text (Ranvier handles tokenization)
- You want higher "improvement %" numbers for comparison

### Example

```bash
# Server tokenization (default) - measures routing benefit
./scripts/bench.sh --compare --duration 30m --users 20

# Client tokenization - measures production throughput
./scripts/bench.sh --compare --client-tokenize --duration 30m --users 20
```

---

## Test Scenarios

### Scenario 1: Baseline Validation (Quick)

**Purpose:** Verify setup is working correctly before longer tests.

```bash
./scripts/bench.sh \
  --model meta-llama/Llama-3.1-8B-Instruct \
  --duration 2m \
  --users 8 \
  --prompt-dist mixed
```

**Expected Results:**
| Metric | Expected | Acceptable Range |
|--------|----------|------------------|
| Error Rate | 0% | < 1% |
| P99 TTFT | < 3000ms | < 5000ms |
| Requests/sec | > 1.0 | > 0.5 |
| All backends registered | 8 | 8 |

**Validation:**
- All 8 GPUs should be utilized (check `nvidia-smi`)
- No connection errors in logs
- Cache hit rate should start appearing after warm-up

---

### Scenario 2: A/B Comparison (Standard)

**Purpose:** Measure prefix-aware routing improvement over round-robin.

```bash
./scripts/bench.sh \
  --model meta-llama/Llama-3.1-8B-Instruct \
  --compare \
  --duration 10m \
  --users 30 \
  --prompt-dist stress \
  --prefix-ratio 0.9
```

**Expected Results (8B model, 8x A100, load-aware routing):**

| Metric | Round-Robin | Prefix-Aware | Notes |
|--------|-------------|--------------|-------|
| Cache Hit Rate | ~12% | ~98% | 8x more cache hits |
| XLarge Improvement | ~0% | **~37%** | Hit vs miss within each run |
| Large Improvement | ~0% | **~37%** | Consistent across large buckets |
| Incomplete Rate | ~36% | ~31% | Prefix routing more stable |
| Throughput | ~57 req/s | ~57 req/s | ~same for 8B |

**For 13B models, expect even larger improvements:**

| Metric | Round-Robin | Prefix-Aware | Notes |
|--------|-------------|--------------|-------|
| XLarge Improvement | ~0% | **~93%** | Load-aware prevents queue buildup |
| Overall P99 TTFT | ~4100ms | ~1000ms | **-76%** tail latency |
| Throughput | ~41 req/s | ~47 req/s | **+17%** higher |

**How to interpret:**
- **Cache hit rate** is the headline metric (12% → 98%)
- **Per-bucket improvement** shows real benefit (37% for 8B, 93% for 13B)
- **Overall TTFT** may look worse for 8B due to small prefix overhead in aggregate
- Focus on **Large/XLarge buckets** where caching matters most
- **Larger models benefit dramatically** from load-aware routing under heavy concurrency

---

### Scenario 3: High Concurrency Stress Test

**Purpose:** Test system under heavy load with 8 GPUs.

```bash
./scripts/bench.sh \
  --model meta-llama/Llama-3.1-8B-Instruct \
  --warmup \
  --duration 15m \
  --users 64 \
  --spawn-rate 4 \
  --prompt-dist stress \
  --prefix-ratio 0.85
```

**Expected Results:**
| Metric | Expected | Alert Threshold |
|--------|----------|-----------------|
| Error Rate | < 1% | > 5% |
| P99 TTFT | < 2000ms | > 5000ms |
| Requests/sec | > 5.0 | < 2.0 |
| Cache Hit Rate | > 60% | < 40% |
| Memory Usage | < 90% per GPU | > 95% |

**Monitoring Commands:**
```bash
# Watch GPU utilization
watch -n 1 nvidia-smi

# Check Ranvier metrics
curl -s http://localhost:9181/metrics | grep -E "ranvier_(requests|latency|cache)"

# Check vLLM logs for errors
tail -f /tmp/vllm_gpu*.log
```

---

### Scenario 4: Long Prefix Workload

**Purpose:** Maximize KV-cache benefit with large shared prefixes (2000-8000 tokens).

```bash
./scripts/bench.sh \
  --model meta-llama/Llama-3.1-8B-Instruct \
  --warmup \
  --duration 10m \
  --users 20 \
  --prompt-dist stress \
  --prefix-ratio 0.95
```

**Expected Results:**
| Metric | Expected | Notes |
|--------|----------|-------|
| Cache Hit Rate | > 80% | High prefix sharing |
| P50 TTFT (Hit) | < 200ms | Near-instant with warm cache |
| P50 TTFT (Miss) | 2000-4000ms | Full prefill required |
| TTFT Improvement | > 10x | Most dramatic with large prefixes |

**This scenario demonstrates the core value proposition:** When prefixes are large and shared across requests, routing to the backend with the warm KV-cache provides order-of-magnitude latency improvement.

---

### Scenario 5: Mixed Workload (Production-like)

**Purpose:** Simulate realistic production traffic patterns.

```bash
./scripts/bench.sh \
  --model meta-llama/Llama-3.1-8B-Instruct \
  --warmup \
  --duration 15m \
  --users 40 \
  --prompt-dist mixed \
  --prefix-ratio 0.7
```

**Expected Results:**
| Metric | Expected | Notes |
|--------|----------|-------|
| Cache Hit Rate | 50-70% | Mixed workload dilutes hits |
| P50 TTFT | 400-800ms | Blend of hits and misses |
| P99 TTFT | 1500-2500ms | Tail from cache misses |
| Error Rate | < 0.5% | Stable under mixed load |

---

### Scenario 6: Model Size Comparison

**Purpose:** Compare performance across model sizes.

#### 6a. Small Model (1B parameters)
```bash
./scripts/bench.sh \
  --model meta-llama/Llama-3.2-1B-Instruct \
  --duration 5m \
  --users 50 \
  --prompt-dist stress
```

**Expected:** Very fast, may be CPU-bound on tokenization.

> ⚠️ **Note:** 1B models show **minimal KV cache benefit** because prefill computation is already trivial (~10-20ms even for 8K token prefixes). Use 8B+ models for meaningful cache hit vs miss comparisons.

#### 6b. Medium Model (8B parameters)
```bash
./scripts/bench.sh \
  --model meta-llama/Llama-3.1-8B-Instruct \
  --duration 10m \
  --users 30 \
  --prompt-dist stress
```

**Expected:** Good balance of speed and quality. Recommended for most tests.

#### 6c. Code Model (13B parameters)
```bash
./scripts/bench.sh \
  --model meta-llama/CodeLlama-13b-Instruct-hf \
  --duration 10m \
  --users 10 \
  --max-model-len 8192 \
  --prompt-dist stress
```

**Expected:** Much higher cache benefit than 8B (~93% TTFT improvement with load-aware routing at 30 users). Requires `--max-model-len 8192` on 40GB GPUs to fit in memory.

#### 6d. Large Model (70B parameters)
```bash
./scripts/bench.sh \
  --model meta-llama/Llama-3.1-70B-Instruct \
  --duration 15m \
  --users 16 \
  --prompt-dist stress
```

**Expected:** Slower but higher cache benefit. May need tensor parallelism across GPUs.

---

## Validation Checklist

### Pre-Benchmark Checks

```bash
# 1. Verify all GPUs are visible
nvidia-smi --list-gpus
# Expected: 8 GPUs listed

# 2. Check GPU memory is free
nvidia-smi --query-gpu=memory.free --format=csv
# Expected: ~40GB free per GPU

# 3. Verify Docker is running
docker ps
# Expected: No conflicting containers

# 4. Check system limits
cat /proc/sys/fs/aio-max-nr
# Expected: 1048576 (set by --setup)

# 5. Verify HF token is set
echo $HF_TOKEN | head -c 10
# Expected: "hf_xxxxx" (first 10 chars)
```

### During Benchmark

```bash
# Watch GPU utilization (all 8 should be active)
watch -n 2 'nvidia-smi --query-gpu=index,utilization.gpu,memory.used --format=csv'

# Check for errors in Ranvier logs
docker logs ranvier-bench1 2>&1 | grep -i error

# Monitor cache hit rate (should increase over time)
curl -s http://localhost:9181/metrics | grep cache_hit
```

### Post-Benchmark Validation

```bash
# 1. Parse results
./tests/integration/results_parser.py summary benchmark-reports/*/benchmark.log

# 2. Check for errors
grep -i "error\|fail" benchmark-reports/*/benchmark.log | head -20

# 3. Verify cache was effective
grep "Cache" benchmark-reports/*/benchmark.log

# 4. Compare A/B results (if --compare was used)
./tests/integration/results_parser.py compare \
  benchmark-reports/*round_robin*/benchmark.log \
  benchmark-reports/*prefix*/benchmark.log
```

---

## Expected Metrics Reference

### TTFT (Time To First Token)

Real-world results from 8x A100 40GB benchmarks (stress distribution):

| Model | Users | Prefix Size | Cache Miss | Cache Hit | Improvement | Notes |
|-------|-------|-------------|------------|-----------|-------------|-------|
| **13B** | 30 | XLarge (4-8K tokens) | ~687ms | **~50ms** | **~93%** | Feb 2026, load-aware |
| **8B** | 30 | XLarge (4-8K tokens) | ~738ms | ~462ms | **~37%** | Feb 2026, load-aware |
| 13B | 30 | XLarge (4-8K tokens) | ~1800ms | ~1030ms | ~43% | Jan 2026 |
| 13B | 20 | XLarge (4-8K tokens) | ~1451ms | ~886ms | ~39% | Jan 2026 |
| 13B | 10 | XLarge (4-8K tokens) | ~1575ms | ~816ms | ~48% | Jan 2026 |
| 8B | 30 | XLarge (4-8K tokens) | ~655ms | ~499ms | ~26% | Jan 2026 |
| 8B | 20 | XLarge (4-8K tokens) | ~804ms | ~453ms | ~44% | Jan 2026 |
| 8B | 10 | XLarge (4-8K tokens) | ~580ms | ~333ms | ~43% | Jan 2026 |
| 1B | 30 | XLarge (4-8K tokens) | ~130ms | ~130ms | ~0% | Jan 2026 |
| 70B | - | XLarge (4-8K tokens) | TBD | TBD | Expected 50-60%+ | |

**Key insights:**
- **Load-aware routing dramatically improves heavy-load results** — 13B at 30 users jumped from 43% → 93%
- **1B models show no benefit** — KV cache computation is already trivial (~10-20ms)
- **Small prefixes have overhead** — Routing cost exceeds cache benefit
- **Large prefixes (4K+ tokens) show real improvement** — 37-93% TTFT reduction with load-aware routing
- **Larger models amplify benefits** — 13B benefits far more from load-aware routing than 8B
- **Tail latency is the big win** — P99 TTFT drops up to 76% with prefix + load-aware routing

### Cache Hit Rate

| Prefix Ratio | Round-Robin | Prefix-Aware |
|--------------|-------------|--------------|
| 0.5 (50% shared) | ~12.5% | ~50% |
| 0.7 (70% shared) | ~12.5% | ~70% |
| 0.9 (90% shared) | ~12.5% | ~85% |
| 0.95 (95% shared) | ~12.5% | ~90% |

### Throughput Scaling

| GPUs | Expected RPS (8B model) | Notes |
|------|------------------------|-------|
| 1 | 0.5-1.0 | Baseline |
| 2 | 1.0-2.0 | Linear scaling |
| 4 | 2.0-4.0 | Near-linear |
| 8 | 3.5-7.0 | Some overhead |

---

## Real Benchmark Results

### February 2026 — With Load-Aware Routing

> Load-aware routing (added Feb 2026) prevents backend hotspots by checking queue depth
> before routing. If the prefix-preferred backend is overloaded, requests fall back to the
> least-loaded backend — accepting a cache miss for lower latency.

#### Test Configuration
```
Duration:     10 minutes
Users:        30 concurrent
Distribution: stress (70% large/xlarge prefixes)
Prefix Ratio: 0.9
Warm-up:      yes
```

#### CodeLlama-13b (8192 max-model-len)

| Metric | Round-Robin | Prefix-Aware | Change |
|--------|-------------|--------------|--------|
| Cache Hit Rate | 12.7% | 98.0% | **+85.3%** |
| Cache Hits | 481 | 4,570 | +850% |
| Cache Misses | 3,308 | 95 | -97.1% |
| XLarge Hit P50 | 897.7ms | **50.3ms** | **-94.4%** |
| XLarge Hit P99 | 4,819ms | 1,045ms | **-78.3%** |
| XLarge Improvement | -7.2% | **92.7%** | **+99.9pp** |
| Large Improvement | -2.6% | **43.6%** | **+46.2pp** |
| Overall P50 TTFT | 800ms | 750ms | **-6.2%** |
| Overall P99 TTFT | 4,100ms | 1,000ms | **-75.6%** |
| Throughput (req/s) | 40.6 | 47.3 | **+16.6%** |
| Incomplete Rate | 35.9% | 25.6% | **-28.8%** |
| Error Rate | 0% | 0% | — |

The 50.3ms XLarge Hit P50 means the KV cache is doing nearly all the work — prefill
computation is almost entirely skipped. This is a 94.4% reduction from the round-robin
baseline.

#### Llama-3.1-8B

| Metric | Round-Robin | Prefix-Aware | Change |
|--------|-------------|--------------|--------|
| Cache Hit Rate | 11.7% | 97.8% | **+86.1%** |
| Cache Hits | 609 | 5,197 | +753% |
| Cache Misses | 4,589 | 117 | -97.5% |
| XLarge Hit P50 | 424.9ms | 462.1ms | +8.8% |
| XLarge Hit P99 | 465.1ms | 569.9ms | +22.5% |
| XLarge Improvement | -0.1% | **37.4%** | **+37.5pp** |
| Large Improvement | -0.6% | **37.4%** | **+38.0pp** |
| Overall P50 TTFT | 420ms | 450ms | +7.1% |
| Overall P99 TTFT | 490ms | 570ms | +16.3% |
| Throughput (req/s) | 56.8 | 56.9 | ~same |
| Incomplete Rate | 36.2% | 30.7% | **-15.4%** |
| Error Rate | 0% | 0% | — |

The 8B model shows strong per-bucket improvement (37.4%) but modest overall TTFT increase.
This is expected — the 8B is fast enough that routing overhead is noticeable in aggregate,
but the per-bucket cache hit vs miss comparison reveals the real benefit.

#### Why 13B Benefits More Than 8B

| Factor | 8B | 13B |
|--------|-----|------|
| Prefill time per token | ~0.05ms | ~0.08ms (1.6x) |
| XLarge prefill cost | ~300ms | ~500ms |
| Cache hit savings | Moderate | Large |
| Queue buildup under load | Mild | Severe |
| Load-aware routing impact | Small | **Dramatic** |

The 13B model's slower inference creates more backend queuing under 30 concurrent users.
Load-aware routing prevents pile-up, letting cache hits actually deliver their latency
benefit instead of waiting in queue.

### January 2026 — Prefix-Aware Only (No Load-Aware)

<details>
<summary>Previous baseline results (click to expand)</summary>

#### Test Configuration
```
Model:       meta-llama/Llama-3.1-8B-Instruct
Duration:    10 minutes
Users:       30 concurrent
Distribution: stress (70% large/xlarge prefixes)
Prefix Ratio: 0.9
```

#### Summary Results

| Metric | Round-Robin | Prefix-Aware | Change |
|--------|-------------|--------------|--------|
| Cache Hit Rate | 12.7% | 98.0% | **+85.3%** |
| Cache Hits | 713 | 5,532 | +676% |
| Cache Misses | 4,908 | 111 | -97.7% |

#### Per-Bucket TTFT Improvement (The Key Metric)

| Bucket | Round-Robin Hit vs Miss | Prefix-Aware Hit vs Miss |
|--------|-------------------------|--------------------------|
| Tiny (<100 tokens) | 0% improvement | 2% improvement |
| Small (100-500) | 0% improvement | -10% (overhead) |
| **XLarge (4K-8K)** | -0.3% improvement | **+23.7% improvement** |

</details>

### Interpretation

1. **Cache hit rate is the headline**: 12% → 98% means nearly every request benefits from cached KV
2. **Per-bucket improvement is the real metric**: Overall TTFT can be misleading due to small prefix overhead
3. **Load-aware routing amplifies benefits for larger models**: 13B went from 42.9% → 92.7% XLarge improvement
4. **Tail latency is the big win**: P99 drops 76% for 13B under heavy load
5. **Throughput improves under load**: +16.6% for 13B because requests aren't stuck behind overloaded backends

### Value Proposition

For workloads with **large shared prefixes** (RAG, system prompts, few-shot):

#### Performance by Model Size and Load

| Model | Load | Users | Duration | XLarge TTFT Improvement | Cache Hit Rate | Notes |
|-------|------|-------|----------|-------------------------|----------------|-------|
| **CodeLlama-13b** | Heavy | 30 | 10m | **92.7%** | 98.0% | Feb 2026, load-aware |
| **Llama-3.1-8B** | Heavy | 30 | 10m | **37.4%** | 97.8% | Feb 2026, load-aware |
| CodeLlama-13b | Moderate | 20 | 30m | 38.9% | 97.6% | Jan 2026 |
| CodeLlama-13b | Normal | 10 | 10m | 48.2% | 96.4% | Jan 2026 |
| CodeLlama-13b | Heavy | 30 | 10m | 42.9% | 97.5% | Jan 2026 |
| Llama-3.1-8B | Moderate | 20 | 30m | 43.7% | 97.8% | Jan 2026 |
| Llama-3.1-8B | Normal | 10 | 10m | 42.7% | 95.6% | Jan 2026 |
| Llama-3.1-8B | Heavy | 30 | 10m | 25.9% | 97.8% | Jan 2026 |

**Impact of load-aware routing (comparing Heavy/30-user runs):**
- **CodeLlama-13b**: 42.9% → **92.7%** (+49.8pp) — load-aware eliminates queue-induced latency
- **Llama-3.1-8B**: 25.9% → **37.4%** (+11.5pp) — moderate improvement, less queuing to begin with

**Key takeaways:**
- **Load-aware routing is a major win for larger models** under heavy load (92.7% for 13B)
- **P99 tail latency** drops up to 76% — worst-case response times improve dramatically
- **Throughput increases** up to 17% with load-aware routing preventing backend hotspots
- **Cache hit rate** is excellent (97%+) regardless of load or model size

#### Impact of Prefix Sharing Ratio

Not all workloads have 90% prefix sharing. These tests show how improvement scales:

| Prefix Ratio | Cache Hit Rate | XLarge Improvement | P99 TTFT | Notes |
|--------------|----------------|-------------------|----------|-------|
| 0.9 (90%) | 97.6% | 38.9% | 1200ms | High sharing (single system prompt) |
| 0.7 (70%) | 93.2% | 41.5% | — | Moderate sharing |
| 0.5 (50%) | 89.9% | 40.9% | 970ms | Low sharing (many system prompts) |

**Key finding:** Improvement holds up remarkably well across prefix ratios. Even at 50% sharing, the system delivers 90% cache hit rate and 41% XLarge improvement. The 0.5 and 0.7 tests show *higher* improvement than 0.9 because they had 0% incomplete rate (less system load).

This demonstrates prefix-aware routing benefits workloads even when prefix sharing is moderate—you don't need 90%+ sharing to see real gains.

#### When Prefix-Aware Routing Helps (and When It Doesn't)

Tested with real LMSYS conversation data (natural short-to-medium prompts, 3 shared prefixes):

| Metric | Round-Robin | Prefix-Aware | Notes |
|--------|-------------|--------------|-------|
| Cache Hit Rate | 12.9% | **99.8%** | Excellent with only 3 prefixes |
| P50 TTFT | 410ms | 460ms | +12% worse |
| Incomplete Rate | 35.1% | **15.2%** | Much better load handling |

**Why TTFT got worse:** The LMSYS conversations have **shorter prefixes** than RAG/system-prompt workloads. With short prompts, the routing overhead (~3ms) exceeds the cache benefit.

**When prefix-aware routing helps:**
- RAG with large context documents (2K-8K tokens)
- Long system prompts (few-shot examples, detailed instructions)
- Workloads where prefill dominates latency

**When it doesn't help much:**
- General chat with short prompts (<500 tokens)
- Highly unique requests with no prefix sharing
- Small models (1B) where prefill is already fast

The cache hit rate will always improve with prefix-aware routing. The TTFT benefit depends on prefix size.

---

## Recommended Next Benchmarks

The February 2026 results establish a new baseline with load-aware routing for two configurations (8B/30u, 13B/30u). The following benchmarks would fill out the comparison matrix.

### Running with bench-runner.sh

All of the runs below are built into `scripts/bench-runner.sh` as the `high`, `medium`, and `all` suites. You can run them in one go instead of invoking each manually:

```bash
# Preview what will run
./scripts/bench-runner.sh --dry-run --suite all

# Run high-priority only (~1.5h)
./scripts/bench-runner.sh --suite high

# Run everything except the 70B test (no 70B support yet)
./scripts/bench-runner.sh --suite all --skip 10

# Resume from run #4 if something failed
./scripts/bench-runner.sh --suite all --resume 4
```

The runner produces a `runner_summary_*.md` report and logs to `benchmark-reports/`. See `./scripts/bench-runner.sh --help` for all options.

### High Priority

These re-run previously documented January configurations to measure load-aware routing impact:

```bash
# 1. 13B at moderate load (Jan baseline: 38.9%)
./scripts/bench.sh --compare --model meta-llama/CodeLlama-13b-Instruct-hf \
  --warmup --duration 10m --users 20 --max-model-len 8192

# 2. 8B at moderate load (Jan baseline: 43.7%)
./scripts/bench.sh --compare --model meta-llama/Llama-3.1-8B-Instruct \
  --warmup --duration 10m --users 20

# 3. 13B at low load (Jan baseline: 48.2%)
./scripts/bench.sh --compare --model meta-llama/CodeLlama-13b-Instruct-hf \
  --warmup --duration 10m --users 10 --max-model-len 8192
```

### Medium Priority

Longer duration runs for statistical confidence, and prefix ratio variations:

```bash
# 4. 13B 30-minute validated run (for TL;DR "validated runs" claim)
./scripts/bench.sh --compare --model meta-llama/CodeLlama-13b-Instruct-hf \
  --warmup --duration 30m --users 30 --max-model-len 8192

# 5. 8B 30-minute validated run
./scripts/bench.sh --compare --model meta-llama/Llama-3.1-8B-Instruct \
  --warmup --duration 30m --users 30

# 6-7. Prefix ratio sweep (update the prefix sharing table)
for ratio in 0.7 0.5; do
  ./scripts/bench.sh --compare --model meta-llama/CodeLlama-13b-Instruct-hf \
    --warmup --duration 10m --users 20 --prefix-ratio $ratio --max-model-len 8192
done
```

### Lower Priority

```bash
# 8. Client tokenization comparison (update client-tokenize section)
./scripts/bench.sh --compare --client-tokenize \
  --model meta-llama/CodeLlama-13b-Instruct-hf \
  --warmup --duration 10m --users 30 --max-model-len 8192

# 9. High concurrency stress test (Scenario 3)
./scripts/bench.sh --warmup --duration 15m --users 64 --spawn-rate 4 \
  --model meta-llama/Llama-3.1-8B-Instruct

# 10. 70B model (still TBD in the docs)
./scripts/bench.sh --compare --model meta-llama/Llama-3.1-70B-Instruct \
  --warmup --duration 15m --users 16
```

---

## Cleanup & Restarts

### Running Multiple Benchmarks

You can run benchmarks multiple times without manual cleanup. The script handles this automatically:

1. **Automatic cleanup** — `cleanup()` runs on EXIT, Ctrl+C (INT), or TERM signals
2. **vLLM processes** — Gracefully killed, then force-killed if needed
3. **Docker containers** — Stopped with `docker compose down -v --remove-orphans`
4. **Output directories** — Timestamped (`benchmark-reports/20260117_143052_*/`), no conflicts

### Manual Cleanup (if needed)

If a previous run crashed badly:

```bash
# Kill any leftover vLLM processes
pkill -f "vllm.entrypoints" || true

# Remove any leftover containers
docker compose -f docker-compose.benchmark-real.yml -p ranvier-benchmark-real down -v --remove-orphans

# Clean old benchmark reports (optional)
rm -rf benchmark-reports/
```

---

## Live Monitoring

While a benchmark runs, you can monitor the system from a separate terminal.

### rvctl Commands

```bash
# Check backend health and registration
./tools/rvctl -u http://localhost:8081 inspect backends

# View learned routes in the radix tree
./tools/rvctl -u http://localhost:8081 inspect routes

# Check cluster quorum status
./tools/rvctl -u http://localhost:8081 cluster status
```

### Docker Logs

```bash
# Watch Ranvier logs in real-time
docker logs -f ranvier-bench1

# Check for errors
docker logs ranvier-bench1 2>&1 | grep -i error

# View all Ranvier container logs
for i in 1 2 3; do echo "=== ranvier-bench$i ===" && docker logs ranvier-bench$i 2>&1 | tail -20; done
```

### GPU and System Monitoring

```bash
# Watch GPU utilization (all 8 should be active)
watch -n 2 'nvidia-smi --query-gpu=index,utilization.gpu,memory.used --format=csv'

# Check vLLM process logs
tail -f /tmp/vllm_gpu*.log

# Check vLLM health endpoints
for port in 8000 8001 8002 8003 8004 8005 8006 8007; do
  curl -s http://localhost:$port/health && echo " :$port OK" || echo " :$port FAIL"
done
```

### Prometheus Metrics

```bash
# Cache hit/miss rates
curl -s http://localhost:9181/metrics | grep -E "ranvier_(cache|route)"

# Request latency
curl -s http://localhost:9181/metrics | grep -E "ranvier_response_latency"

# Active requests
curl -s http://localhost:9181/metrics | grep -E "ranvier_active_requests"
```

---

## Troubleshooting

### Problem: Routing Mode Mismatch

**Symptoms:** Locust logs show:
```
ROUTING MODE MISMATCH DETECTED!
  BENCHMARK_MODE (client label): random
  X-Routing-Mode (server actual): prefix
```

**Why this happens:** The `BENCHMARK_MODE` environment variable controls how the locustfile **labels** results, but `RANVIER_ROUTING_MODE` controls how the **server** actually routes requests. If they don't match, your benchmark results will be mislabeled.

**Solution:** Set BOTH variables consistently:
```bash
# For prefix routing benchmark:
export BENCHMARK_MODE=prefix
export RANVIER_ROUTING_MODE=prefix

# For random/round-robin baseline:
export BENCHMARK_MODE=random
export RANVIER_ROUTING_MODE=random
```

Or use `bench.sh --compare` which handles this automatically.

---

### Problem: `inspect routes` Shows 0 Routes

**Symptoms:** Running `./tools/rvctl inspect routes` shows "(empty tree)" even with `RANVIER_ROUTING_MODE=prefix`, and Prometheus shows:
```
router_cache_hits{shard="0"} 0
router_cache_misses{shard="0"} 1756
router_prefix_affinity_routes{shard="0"} 1756
```

**Why this happens:** Routes are learned **after successful responses**, not on routing decision. The flow is:
1. Request arrives → Ranvier tokenizes and routes (via hash or existing ART entry)
2. Backend responds successfully (HTTP 200 detected via header snoop)
3. Route is learned and added to radix tree

If cache_misses equals prefix_affinity_routes and cache_hits is 0, routing IS working (via hash fallback) but routes aren't being learned.

**Causes & Solutions:**

1. **Check too early:** Wait for some requests to complete first
   ```bash
   # Wait until you see requests completing in Locust output, then check
   ./tools/rvctl -u http://localhost:8081 inspect routes
   ```

2. **Tokenizer not loaded:** Check Ranvier logs for tokenizer errors
   ```bash
   docker logs ranvier-bench1 2>&1 | grep -i tokenizer
   ```

3. **Routing mode not prefix:** Verify environment variable
   ```bash
   docker exec ranvier-bench1 env | grep ROUTING_MODE
   # Should show: RANVIER_ROUTING_MODE=prefix
   ```

4. **Requests failing or not returning HTTP 200:** Route learning requires `header_snoop_success`
   ```bash
   # Look for "First byte received" which indicates successful response
   docker logs ranvier-bench1 2>&1 | grep "First byte" | head -5

   # Look for "Learning route" messages
   docker logs ranvier-bench1 2>&1 | grep -i "learning route" | head -5
   ```

5. **min_token_length too high:** Routes only learned when token count >= min_token_length
   ```bash
   # Check current setting (default: 10)
   docker exec ranvier-bench1 env | grep MIN_TOKEN
   ```

6. **Tokens not being generated:** Check if tokenization is working
   ```bash
   # Look for token-related debug messages
   docker logs ranvier-bench1 2>&1 | grep -i "token" | head -20
   ```

**Verify routing is working:**
```bash
# Check Prometheus metrics for route learning
curl -s http://localhost:9181/metrics | grep -E "router_prefix_affinity_routes|router_cache"

# Interpretation:
# - prefix_affinity_routes > 0: Prefix routing is active
# - cache_hits > 0: ART is being used (routes were learned)
# - cache_hits = 0, cache_misses > 0: Hash fallback only (routes not learned)
```

**Note on client-side "cache hit rate":** The locustfile tracks hits based on whether the same prefix_hash goes to the same backend. With hash-based prefix routing, this will show high hit rates (~70-90%) even when the ART has 0 routes, because the hash is deterministic. This is different from Ranvier's `router_cache_hits` metric which tracks actual ART lookups.

### Problem: Low Cache Hit Rate

**Symptoms:** Cache hit rate < 50% even with high prefix ratio.

**Causes & Solutions:**
1. **Warm-up incomplete:** Run `--warmup` before main benchmark
2. **Prefix too short:** Use `--prompt-dist stress` for longer prefixes
3. **Too many unique prefixes:** Reduce `--users` to concentrate traffic
4. **Routing not working:** Check Ranvier logs for route learning

```bash
# Check if routes are being learned
curl -s http://localhost:9181/metrics | grep route_count
```

### Problem: High Error Rate

**Symptoms:** Error rate > 1%.

**Causes & Solutions:**
1. **vLLM OOM:** Reduce `--users` or use smaller model
2. **Connection timeout:** Check network between Docker containers
3. **Backend unhealthy:** Check vLLM logs

```bash
# Check vLLM health
for port in 8000 8001 8002 8003 8004 8005 8006 8007; do
  curl -s http://localhost:$port/health && echo " :$port OK" || echo " :$port FAIL"
done
```

### Problem: Inconsistent Results

**Symptoms:** Results vary significantly between runs.

**Causes & Solutions:**
1. **No warm-up:** Always use `--warmup` for consistent results
2. **Cold start effects:** Discard first 2 minutes of results
3. **Background processes:** Check for other GPU workloads

```bash
# Ensure clean state
pkill -f vllm || true
docker compose -f docker-compose.benchmark-real.yml down -v
```

### Problem: vLLM Won't Start

**Symptoms:** vLLM processes die immediately.

**Causes & Solutions:**
1. **CUDA version mismatch:** Check CUDA compatibility
2. **Insufficient GPU memory:** Use smaller model
3. **HF token invalid:** Re-export `HF_TOKEN`

```bash
# Check vLLM startup logs
cat /tmp/vllm_gpu0.log | head -50
```

### Debugging: Capturing Full Output

For debugging setups or sharing logs with others, use `--log-all` to capture everything:

```bash
# Run with full logging
./scripts/bench.sh --log-all --duration 10m --users 30

# This creates: benchmark-reports/run_YYYYMMDD_HHMMSS.log
# Contains: all script output, system info, timestamps, and errors
```

The run.log includes:
- System information (host, user, pwd)
- Full script status messages
- vLLM startup progress
- Ranvier cluster health checks
- Complete benchmark output

When troubleshooting, share both `run.log` and the per-benchmark `benchmark.log`.

### Problem: Ranvier Crashes (Segfault)

**Symptoms:** Ranvier container exits with segmentation fault, especially under high concurrency.

**Solution:** Run with debug build to get readable stack traces:

```bash
# Force rebuild with debug symbols
docker rmi ranvier:latest 2>/dev/null

# Run benchmark with debug build
./scripts/bench.sh --debug --log-all --duration 15m --users 64 --prompt-dist stress
```

**What `--debug` does:**
- Passes `--build-arg BUILD_TYPE=Debug` to docker build
- Includes debug symbols in the binary for readable backtraces
- Forces image rebuild (even if ranvier:latest exists)

**Collecting crash info:**
```bash
# Check container exit logs
docker logs ranvier-bench1 2>&1 | tail -100

# If you have coredumps enabled
coredumpctl list
coredumpctl debug <pid>  # Opens in gdb
```

**Common crash causes:**
1. **High concurrency + large requests:** Radix tree hash collisions (fixed in recent versions)
2. **Memory exhaustion:** Reduce `--users` or use smaller model
3. **Tokenizer issues:** Check HF_TOKEN is valid and model is accessible

### Problem: Tokenizer Crashes with --smp > 1

**Symptoms:** Ranvier crashes in the tokenizer (Rust FFI) when running with multiple shards (`--smp 2` or higher).

**Why this happens:** The tokenizers-cpp library (Rust FFI) has global state that conflicts with Seastar's per-shard memory allocator. This is a known incompatibility.

**Solutions:**

1. **Use `--std-alloc`** (recommended):
   ```yaml
   # In docker-compose.benchmark-real.yml
   command: ["./ranvier_server", "--smp", "2", "--std-alloc"]
   ```
   This uses standard malloc instead of Seastar's per-shard allocator, allowing Rust FFI to work correctly.

2. **Use `--smp 1`** and scale horizontally:
   ```yaml
   command: ["./ranvier_server", "--smp", "1", "--memory", "1G"]
   ```
   Run more containers with 1 shard each instead of fewer with multiple shards.

3. **Use client-side tokenization** (bypasses ranvier tokenization entirely):
   ```bash
   ./scripts/bench.sh --client-tokenize --duration 10m --users 30
   ```
   See "Client-Side Tokenization" section below.

---

## Client-Side Tokenization

The benchmark supports client-side tokenization, which pre-tokenizes prompts in the locust client and sends `prompt_token_ids` in requests. This bypasses ranvier's local tokenization.

### When to Use

- **Tokenizer crashes:** If ranvier crashes during tokenization with `--smp > 1`
- **Performance isolation:** To benchmark routing/proxy performance without tokenization overhead
- **Production architecture:** When clients already have tokens (e.g., from a separate tokenizer service)

### Configuration

**Option 1: CLI flag (recommended)**
```bash
./scripts/bench.sh --client-tokenize --duration 10m --users 30
```

**Option 2: Environment variable**
```bash
CLIENT_TOKENIZE=true ./scripts/bench.sh --duration 10m --users 30
```

**Optional: Specify tokenizer path** (default: assets/gpt2.json or downloads from model)
```bash
export TOKENIZER_PATH=/path/to/tokenizer.json
./scripts/bench.sh --client-tokenize --duration 10m
```

### Requirements

1. **Install tokenizers package** (automatically installed by `--setup`):
   ```bash
   ./scripts/bench.sh --setup   # Installs tokenizers automatically
   # Or manually:
   pip install tokenizers
   ```

2. **Ensure ranvier accepts client tokens** (enabled by default in benchmark config):
   ```yaml
   environment:
     - RANVIER_ACCEPT_CLIENT_TOKENS=1
     - RANVIER_ACCEPT_CLIENT_PREFIX_BOUNDARY=1
   ```

### What Happens

| CLIENT_TOKENIZE | Ranvier Tokenizes | Client Tokenizes | Prefix Hints |
|-----------------|-------------------|------------------|--------------|
| false (default) | Yes | No | Auto-detected from system messages |
| true | No | Yes | Calculated and sent by client |

When `CLIENT_TOKENIZE=true`:
1. Locust pre-tokenizes prompts using Python `tokenizers` library
2. Locust tokenizes system messages separately to calculate `prefix_token_count`
3. Sends `prompt_token_ids` and `prefix_token_count` in request body
4. Ranvier uses client-provided prefix boundary for routing (no local tokenization)
5. Ranvier forwards tokens to vLLM (vLLM also skips tokenization)

**Prefix Boundary Benefit**: By sending `prefix_token_count`, clients tell Ranvier exactly where their "shared prefix" ends. This enables optimal routing for multi-turn conversations where requests share the same system prompt but have different user queries.

### Comparing Both Paths

```bash
# Test with ranvier tokenization (default)
./scripts/bench.sh --duration 5m --users 30

# Test with client tokenization (bypasses ranvier tokenizer)
./scripts/bench.sh --client-tokenize --duration 5m --users 30
```

---

## Recommended Test Sequence

For a complete benchmark session, run tests in this order:

```bash
# 1. Setup (once)
./scripts/bench.sh --setup

# 2. Quick validation (2 min)
./scripts/bench.sh --duration 2m --users 8

# 3. A/B comparison (25 min)
./scripts/bench.sh --compare --warmup --duration 10m --users 30

# 4. High concurrency test (20 min)
./scripts/bench.sh --warmup --duration 15m --users 64 --prompt-dist stress

# 5. Parse and compare all results
./tests/integration/results_parser.py summary benchmark-reports/*/benchmark.log
```

**Total time:** ~50 minutes + vLLM startup (~5 min per scenario)

### Multi-Run Suites

For running the full benchmark matrix automatically, use `bench-runner.sh`:

```bash
# Preview what will run, with time estimates
./scripts/bench-runner.sh --dry-run --suite all

# Run the high-priority suite (~1.5h)
./scripts/bench-runner.sh --suite high

# Run everything, skip 70B (no support yet)
./scripts/bench-runner.sh --suite all --skip 10
```

The runner handles GPU cooldown pauses between runs, tracks per-run metrics
inline, produces a `runner_summary_*.md` report, and supports `--resume`
to pick up where you left off after failures. See `--help` for all options.

---

## Exporting Results

### Generate Markdown Report

```bash
./tests/integration/results_parser.py export \
  benchmark-reports/*/benchmark.log \
  --format markdown > results-summary.md
```

### Generate JSON for Analysis

```bash
./tests/integration/results_parser.py export \
  benchmark-reports/*/benchmark.log \
  --format json > results.json
```

### Compare Multiple Runs

```bash
# Compare baseline vs optimized
./tests/integration/results_parser.py compare \
  benchmark-reports/20250117_*_round_robin/benchmark.log \
  benchmark-reports/20250117_*_prefix/benchmark.log
```
