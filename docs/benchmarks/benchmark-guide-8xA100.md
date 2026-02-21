# Benchmark Guide: 8x A100 Single-Host Setup

This guide provides specific test scenarios, expected results, and validation criteria for benchmarking Ranvier on a Lambda Labs 8x A100 instance.

## TL;DR Results

Prefix-aware routing with speculative load increment + batched route learning vs round-robin baseline:

| Model | Cache Hit Rate | P99 Latency | Throughput | Key Benefit |
|-------|----------------|-------------|------------|-------------|
| **CodeLlama-13b** | 12% → **87-89%** | **-24% to -51%** | **+3% to +14%** | P99 + throughput |
| **Llama-3.1-8B** | 12% → **65-95%** | flat | ~same | Stable, no harm |
| **Llama-3.1-70B** | 49% → **75%** | flat | -18% | Reliability (0% inc) |

*Results from February 21, 2026 (commit bb20555) on 8xA100 40GB with vLLM v0.15.1 pinned.
70B on 40GB (TP=4, 2 backends). 13B/8B on 40GB (8 backends). Thread pool enabled, speculative
load increment, batched route learning.*

**Key wins:**
- **4-7x more cache hits** — Requests routed to backends with cached KV data
- **Up to 51% lower P99 tail latency** — 13B at 30 users shows strongest improvement
- **Up to 14% higher throughput** — Load-aware routing prevents backend hotspots for 13B
- **0% incomplete rate** — Stale connection retry ensures every request completes
- **Zero hot-spotting at default prefix ratio** — Speculative load increment prevents burst routing
- **Stable under stress** — 8B at 64 concurrent users with zero errors

**Performance at default config (prefix-ratio 0.9):**

| Model | Users | Duration | P99 TTFT | Throughput | Cache Hits |
|-------|-------|----------|----------|------------|------------|
| 13B | 10 | 10m | **-26.8%** | flat | 87.7% |
| 13B | 20 | 10m | **-24% to -48%** | +3% to +13% | 87-89% |
| 13B | 30 | 30m | **-51.3%** | **+13.9%** | 53.6% |
| 8B | 20 | 10m | flat | -2.8% | 94.6% |
| 8B | 30 | 30m | flat | -4.6% | 64.9% |
| 70B | 16 | 15m | +2.6% | -17.7% | 75.4% |

**Known limitations at lower prefix sharing (13B 20u 10m):**

| Prefix Sharing | Cache Hit Rate | P99 TTFT | Status |
|----------------|----------------|----------|--------|
| 90% (default) | 87-89% | **-24% to -48%** | Stable |
| 70% | 76.8% | +86.4% | Hot-spotting |
| 50% | 66.7% | +64.4% | Hot-spotting |

*At prefix ratios below 0.9, more unique prefixes overwhelm the load balancer and cause
hot-spotting at medium concurrency (20 users). This is a known limitation being tracked
in backlog item 20.11 (cross-shard speculative load synchronization).*

> **Historical context:** Earlier benchmarks (Instance 3, Feb 10-14) showed stronger results:
> P99 -79% to -87% for 13B, 97-98% cache hit rates, and no hot-spotting at any prefix ratio.
> Those runs used per-request SMP route broadcasts (before batched route learning 21.1), which
> provided tighter cross-shard synchronization. The current architecture trades some cache
> affinity for lower SMP overhead — a net positive for throughput at default settings, but
> edge cases (low prefix ratio, client-tokenize, high concurrency) show reduced improvement.

---

## Hardware Configuration

| Component | Specification |
|-----------|---------------|
| GPUs | 8x NVIDIA A100 (40GB or 80GB SXM4) |
| vCPUs | 124 |
| RAM | 1800 GiB |
| Storage | 6 TiB NVMe SSD |
| Cost | ~$10/hour |

*8B/13B benchmarks run on 40GB A100s (8 backends). 70B requires tensor parallelism:
TP=4 on 40GB (2 backends, 4K context) or TP=2 on 80GB (4 backends, 32K context).*

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
| `--prefix-max-tokens` | 8000 | Maximum prefix size in tokens (increase for longer prefix tests) |
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

**Measured cache hit rates with prefix-aware routing (Feb 2026):**
- `--prefix-ratio 0.5` → ~67-91% cache hit rate (instance-dependent)
- `--prefix-ratio 0.7` → ~77-93% cache hit rate (instance-dependent)
- `--prefix-ratio 0.9` → ~87-98% cache hit rate
- Round-robin baseline → ~11-13% (1/8 chance with 8 backends)

> **Note:** Cache hit rates at lower prefix ratios vary significantly between instances and
> code versions. The batched route learning optimization (21.1) trades some cache affinity
> for reduced SMP overhead, which can lower hit rates at sub-0.9 ratios.

### Token Size Buckets

The benchmark categorizes prompts into size buckets for analysis:

| Bucket | Token Range | Typical Content |
|--------|-------------|-----------------|
| tiny | 0-100 | Simple queries |
| small | 100-500 | Short conversations |
| medium | 500-2000 | Moderate context |
| large | 2000-4000 | RAG documents, long system prompts |
| xlarge | 4000-8000 | Large context windows |

**Key insight:** XLarge prefixes show the most improvement (typically 30-43% TTFT reduction) because they save the most KV cache computation.

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

#### CodeLlama-13b — Client Tokenization Results

**bb20555 (Feb 21, Instance 5 — 30 users, 10 minutes):**

| Metric | Server Tokenize (30u 30m) | Client Tokenize (30u 10m) | Notes |
|--------|--------------------------|--------------------------|-------|
| Routing overhead | ~10ms | ~0.4ms | **25x lower** |
| P99 TTFT Change | **-51.3%** | +18.3% | Client-tok hot-spots |
| Throughput change | **+13.9%** | +5.6% | Server-tok better |
| Cache Hit Rate | 53.6% | 54.0% | Same |

> **Warning:** On bb20555, client tokenization causes **hot-spotting** at 30 users. Without
> the ~10ms server-side tokenization stagger, cross-shard routing decisions happen
> near-simultaneously, causing burst routing to the same backends. This is tracked in
> backlog 20.11 (cross-shard speculative load synchronization).

**Instance 3 (Feb 14, pre-batched-routes — 30 users, 10 minutes):**

| Metric | Server Tokenize | Client Tokenize | Difference |
|--------|-----------------|-----------------|------------|
| Routing overhead | ~10ms | ~0.3ms | **32x lower** |
| Throughput change | +13.7% | **+19.4%** | Better with client tok |
| P99 TTFT Change | **-79%** | **-83.6%** | Comparable |
| Cache Hit Rate | 97.6% | 97.4% | Same |

> **Note:** On Instance 3 (with per-request SMP broadcast), client tokenization performed
> excellently: **+19.4% throughput** and **-83.6% P99**. The per-request SMP broadcast
> kept all shards synchronized, preventing the cross-shard burst routing that causes
> hot-spotting on bb20555.

#### Llama-3.1-8B (January 2026 — 20 users, 30 minutes)

| Metric | Server Tokenize | Client Tokenize | Difference |
|--------|-----------------|-----------------|------------|
| Routing overhead | ~16ms | ~0.4ms | **40x lower** |
| Throughput (req/s) | 33.1 | 32.8 | ~same |
| XLarge Improvement % | 43.7% | 36.0% | See below |
| XLarge Hit P50 | 453ms | 451ms | ~same |
| XLarge Miss P50 | 804ms | 705ms | -12% |

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

**Expected Results (8B model, 8x A100, bb20555+ with speculative load increment):**

| Metric | Round-Robin | Prefix-Aware | Notes |
|--------|-------------|--------------|-------|
| Cache Hit Rate | ~12% | ~65-95% | Instance/config dependent |
| P99 TTFT | ~500-600ms | ~500-600ms | Flat for 8B (too fast) |
| Incomplete Rate | 0% | 0% | Fixed by stale connection retry |
| Throughput | ~44-50 req/s | ~44-47 req/s | ~same for 8B |

**For 13B models, expect larger improvements (especially P99 and throughput):**

| Metric | Round-Robin | Prefix-Aware | Notes |
|--------|-------------|--------------|-------|
| Cache Hit Rate | ~12% | ~54-89% | Varies with concurrency |
| Overall P99 TTFT | ~4500-6800ms | ~2500-3500ms | **-24% to -51%** tail latency |
| Throughput | ~14-36 req/s | ~15-41 req/s | **+3-14%** higher |

**How to interpret:**
- **Cache hit rate** improves 4-7x over round-robin — always a significant gain
- **P99 tail latency** is the strongest win for 13B — -24% to -51% at default prefix ratio
- **Incomplete rate** should be 0% — if you see >1%, check for connection pool issues
- **Overall TTFT** may look worse for 8B due to small prefix overhead in aggregate
- Focus on **Large/XLarge buckets** where caching matters most
- Results are instance-dependent; 13B benefits most under sustained load (30u 30m)

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

**Measured Results (February 2026, 8B, 64 users, 15m, prefix-only):**

| Metric | Instance 3 (Feb 14) | Instance 5 (Feb 21, bb20555) | Alert Threshold |
|--------|---------------------|------------------------------|-----------------|
| Error Rate | **0%** | **0%** | > 5% |
| P99 TTFT | **600ms** | **1700ms** | > 5000ms |
| Requests/sec | **22.0** | **24.1** | < 2.0 |
| Cache Hit Rate | **98.0%** | **40.5%** | < 40% |
| Incomplete Rate | **0%** | **0%** | |
| Validation | **PASSED** | **PASSED** | |

**Notes:** The bb20555 run shows lower cache hit rate (40.5% vs 98.0%) at 64 users — the
batched route learning flush interval (10ms) allows more cross-shard routing divergence under
high concurrency. Despite lower cache affinity, the system remains completely stable: zero
errors, zero incompletes, and near-perfect 3-way node balance (7256/7235/7213). This validates
that the speculative load increment prevents hot-spotting even at extreme concurrency.

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

**Expected:** Higher cache benefit than 8B. At default prefix ratio 0.9: P99 -24% to -51%,
throughput +3% to +14% at 10-30 users. Best results under sustained high concurrency (30u 30m).
Requires `--max-model-len 8192` on 40GB GPUs to fit in memory.

#### 6d. Large Model (70B parameters)

A 70B model in FP16 requires ~140GB VRAM and cannot fit on a single A100 40GB. Use
`--tp` (tensor parallelism) to shard the model across multiple GPUs.

**Auto-detection:** `bench.sh` automatically detects GPU VRAM via `nvidia-smi` and
selects the appropriate TP size, GPU memory utilization, and max context length. Just run:

```bash
./scripts/bench.sh --compare \
  --model meta-llama/Llama-3.1-70B-Instruct \
  --warmup --duration 15m --users 16
```

The script will auto-configure based on detected GPU VRAM:
- **40GB A100**: `--tp 4 --gpu-mem-util 0.92 --max-model-len 4096` (2 backends)
- **80GB A100**: `--tp 2` (4 backends, full context)

You can override any auto-detected value with explicit flags.

**On 8xA100 40GB** — auto-detects TP=4, giving 2 backends (each using 4 GPUs):

```bash
# Explicit equivalent of auto-detected config:
./scripts/bench.sh --compare \
  --model meta-llama/Llama-3.1-70B-Instruct \
  --warmup --duration 15m --users 16 \
  --tp 4 --max-model-len 4096 --gpu-mem-util 0.92
```

**Memory constraints (40GB A100s with TP=4):**
- Model weights per GPU: ~35GB (140GB / 4)
- Available VRAM at 0.92 util: ~36.8GB per GPU
- KV cache headroom: ~1.8GB per GPU — limits context to ~4096 tokens
- `--max-model-len 4096` is auto-set to prevent OOM

> **2 backends vs. more:** With only 2 backends, every request is a binary routing choice.
> Cache hit rates will actually be *higher* than 8-backend configs (each backend owns half
> the prefix space), so TTFT improvement percentages should be strong. The limitation is
> throughput — fewer concurrent users before saturation. Use 16 users (not 30+).

**On 8xA100 80GB** — auto-detects TP=2, giving 4 backends with full context:

```bash
# Auto-detected on 80GB GPUs. Explicit equivalent:
./scripts/bench.sh --compare \
  --model meta-llama/Llama-3.1-70B-Instruct \
  --warmup --duration 15m --users 16 \
  --tp 2 --max-model-len 8192
```

4 backends provides a better routing surface for Ranvier — more routing decisions
and a more realistic production-like topology.

Or force TP=4 for 2 backends with 32K+ context headroom:

```bash
./scripts/bench.sh --compare \
  --model meta-llama/Llama-3.1-70B-Instruct \
  --warmup --duration 15m --users 16 \
  --tp 4 --max-model-len 32768
```

**Tensor parallelism options:**

| TP | Backends | GPUs/Backend | Min GPU VRAM | Context (70B FP16) | Notes |
|----|----------|-------------|--------------|-------------------|----|
| 2  | 4        | 2           | ~80GB        | 8K+ | Best for routing benchmarks (80GB GPUs) |
| 4  | 2        | 4           | ~40GB        | ~4K (tight) | Works on 40GB, auto-detected |
| 8  | 1        | 8           | ~20GB        | 32K+ | No routing — single-backend perf only |

**Expected:** Slower inference but higher per-request cache benefit. The 70B model's
longer prefill time (~0.15ms/token) means KV cache hits save more compute than with 8B/13B.
Expected XLarge TTFT improvement: 50-60%+.

> **Note:** 70B model loading with TP takes significantly longer than single-GPU models
> (~5-10 minutes for weight sharding and distribution). The script uses an extended 10-minute
> timeout for TP>1 configurations.

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

| Model | Users | Duration | Prefix Size | Cache Miss | Cache Hit | Improvement | Notes |
|-------|-------|----------|-------------|------------|-----------|-------------|-------|
| **13B** | **20** | **10m** | XLarge | ~1301ms | ~834ms | **~36%** | Feb 2026, post-fix |
| **13B** | **10** | **10m** | XLarge | ~1394ms | ~845ms | **~39%** | Feb 2026, post-fix |
| **8B** | **20** | **10m** | XLarge | ~649ms | ~444ms | **~32%** | Feb 2026, post-fix |
| 13B | 30 | 30m | XLarge | ~1525ms | ~992ms | ~35% | Feb 2026, pre-fix validated |
| 8B | 30 | 30m | XLarge | ~674ms | ~465ms | ~31% | Feb 2026, pre-fix validated |
| 13B | 20 | 10m | XLarge | ~1158ms | ~769ms | ~34% | Feb 2026, pre-fix |
| 13B | 10 | 10m | XLarge | ~1318ms | ~751ms | ~43% | Feb 2026, pre-fix |
| 8B | 20 | 10m | XLarge | ~638ms | ~448ms | ~30% | Feb 2026, pre-fix |
| 8B (16K pfx) | 20 | 10m | XLarge | ~639ms | ~461ms | **~28%** | Feb 2026, post-fix, `--prefix-max-tokens 16000` |
| 8B (64u stress) | 64 | 15m | XLarge | ~668ms | ~474ms | ~29% | Feb 2026, post-fix, high concurrency |
| 13B (ratio 0.7) | 20 | 10m | XLarge | ~1123ms | ~748ms | ~33% | Feb 2026, `--prefix-ratio 0.7` |
| 13B (ratio 0.5) | 20 | 10m | XLarge | ~1523ms | ~861ms | ~44% | Feb 2026, `--prefix-ratio 0.5` |
| 13B (client tok) | 30 | 10m | XLarge | ~1065ms | ~802ms | ~25% | Feb 2026, `--client-tokenize` |
| 13B | 30 | 10m | XLarge | ~1800ms | ~1030ms | ~43% | Jan 2026 |
| 13B | 20 | 10m | XLarge | ~1451ms | ~886ms | ~39% | Jan 2026 |
| 13B | 10 | 10m | XLarge | ~1575ms | ~816ms | ~48% | Jan 2026 |
| 8B | 30 | 10m | XLarge | ~655ms | ~499ms | ~26% | Jan 2026 |
| 8B | 20 | 10m | XLarge | ~804ms | ~453ms | ~44% | Jan 2026 |
| 8B | 10 | 10m | XLarge | ~580ms | ~333ms | ~43% | Jan 2026 |
| 1B | 30 | 10m | XLarge | ~130ms | ~130ms | ~0% | Jan 2026 |
| 70B (TP=2) | 16 | 30m | XLarge | ~2665ms | ~1498ms | **~44%** | Feb 2026, 80GB A100s, 4 backends, 32K context |
| 70B (TP=2) | 16 | 15m | XLarge | ~2924ms | ~1520ms | ~48% | Feb 2026, 80GB, warm-up effects inflate P99 |
| 70B (TP=4) | 16 | 15m | XLarge | ~2108ms | ~1069ms | ~49% | Feb 2026, 40GB A100s, 2 backends, 4K context |

**Key insights:**
- **P99 tail latency is the strongest win for 13B** — -24% to -51% on bb20555, -79% to -85% on Instance 3
- **0% incomplete rate** — stale connection retry (Feb 9 fix) eliminated phantom timeouts
- **Results are instance/architecture dependent** — cache hit rates range 54-98% across runs
- **1B models show no benefit** — KV cache computation is already trivial (~10-20ms)
- **Small prefixes have overhead** — Routing cost exceeds cache benefit
- **Larger models amplify benefits** — 13B sees bigger throughput/P99 gains than 8B
- **Throughput improves for 13B** — +3% to +22% depending on instance and concurrency
- **Batched route learning trades cache affinity for SMP efficiency** — lower cache hit rates but less overhead

### Cache Hit Rate

| Prefix Ratio | Round-Robin | Prefix-Aware (Instance 3) | Prefix-Aware (bb20555) | Notes |
|--------------|-------------|--------------------------|----------------------|-------|
| 0.5 (50% shared) | ~11% | **~91%** | **~67%** | Lower with batched routes |
| 0.7 (70% shared) | ~13% | **~90%** | **~77%** | Lower with batched routes |
| 0.9 (90% shared) | ~12% | **~97-98%** | **~87-95%** | Still strong |
| 0.95 (95% shared) | ~12.5% | ~98%+ | ~95%+ | Estimated |

### Throughput Scaling

| GPUs | Expected RPS (8B model) | Notes |
|------|------------------------|-------|
| 1 | 0.5-1.0 | Baseline |
| 2 | 1.0-2.0 | Linear scaling |
| 4 | 2.0-4.0 | Near-linear |
| 8 | 3.5-7.0 | Some overhead |

---

## Real Benchmark Results

### February 21, 2026 — bb20555: Speculative Load Increment + Batched Route Learning

> **Commit:** bb20555. **vLLM:** v0.15.1 (pinned). **Instance:** Lambda Labs 8xA100 40GB.
>
> This run includes all optimizations: speculative load increment (20.9), batched local
> route learning (21.1), tokenizer thread pool (enabled by default), and pinned vLLM version.
> Full `bench-runner.sh --suite all` execution.

#### Core Results (prefix-ratio 0.9, default config)

**CodeLlama-13b:**

| Config | P99 TTFT Change | Throughput | Cache Hit Rate | Status |
|--------|-----------------|------------|----------------|--------|
| 10u 10m | **-26.8%** | flat | 87.7% | Stable |
| 20u 10m run 1 | **-47.5%** | +3.2% | 86.8% | Stable |
| 20u 10m run 2 | **-23.8%** | +12.8% | 89.1% | Stable |
| 30u 30m | **-51.3%** | **+13.9%** | 53.6% | Excellent |

**Llama-3.1-8B:**

| Config | P99 TTFT Change | Throughput | Cache Hit Rate | Status |
|--------|-----------------|------------|----------------|--------|
| 20u 10m | flat | -2.8% | 94.6% | Neutral |
| 30u 30m | flat | -4.6% | 64.9% | Slight negative |
| 64u 15m (stress) | — | 65.1 tok/s | 40.5% | Stable (standalone) |
| 20u 10m 16K pfx | 0.0% | -5.2% | 96.2% | Neutral |

**Llama-3.1-70B (TP=4, 2 backends, 40GB):**

| Config | P99 TTFT Change | Throughput | Cache Hit Rate | Incompletes |
|--------|-----------------|------------|----------------|-------------|
| 16u 15m | +2.6% | -17.7% | 75.4% | 26 → **0** |

70B continues the reliability pattern — incompletes eliminated (0.6% → 0%) at the cost of
throughput. The 40GB TP=4 config only provides 2 backends, limiting routing surface.

#### Variant Configurations

**Prefix ratio sensitivity (13B 20u 10m):**

| Prefix Ratio | P99 TTFT | Throughput | Cache Hits | Status |
|--------------|----------|------------|------------|--------|
| 0.9 (default) | **-24% to -48%** | +3% to +13% | 87-89% | Stable |
| 0.7 | +86.4% | flat | 76.8% | **Hot-spotting** |
| 0.5 | +64.4% | -8.4% | 66.7% | **Hot-spotting** |

At prefix ratios below 0.9, more unique prefixes cause unpredictable load distribution.
The speculative load increment (shard-local) cannot prevent cross-shard burst routing when
prefix diversity is high. This is tracked in backlog 20.11.

**Client-side tokenization (13B 30u 10m):**

| Mode | P99 TTFT | Throughput | Cache Hits | Status |
|------|----------|------------|------------|--------|
| Server tokenize (30u 30m) | **-51.3%** | **+13.9%** | 53.6% | Excellent |
| Client tokenize (30u 10m) | +18.3% | +5.6% | 54.0% | **Hot-spotting** |

Client-tokenize removes the natural ~10ms stagger from server-side tokenization. Without
this stagger, cross-shard routing decisions happen near-simultaneously, causing burst routing
to the same backends. Tracked in backlog 20.11.

#### Comparison with Instance 3 (Feb 10-14) Results

Cache hit rates are systematically lower on bb20555 vs Instance 3 (87-89% vs 97-98% for
13B 20u). The primary architectural change is batched route learning (21.1) — routes flush
every 10ms instead of per-request SMP broadcast. This reduces cross-shard synchronization
frequency, which lowers cache affinity but eliminates SMP overhead on the hot path.

P99 improvements are also weaker (-24% to -51% vs -79% to -85%) but still positive for the
primary use case. Throughput improvements are comparable (+3% to +14% vs +14% to +22%).

The most significant regression is at lower prefix ratios (0.7, 0.5) which now show hot-spotting
instead of the -76% to -87% P99 improvements previously documented. Instance variance,
different GPU thermals, and the batched route learning change all contribute.

---

### February 2026 — With Load-Aware Routing + Stale Connection Fix (Instance 3)

> Load-aware routing (added Feb 2026) prevents backend hotspots by checking queue depth
> before routing. If the prefix-preferred backend is overloaded, requests fall back to the
> least-loaded backend — accepting a cache miss for lower latency.
>
> **Stale connection retry** (added Feb 9, 2026) detects when the connection pool returns a
> dead TCP socket (vLLM closed its end) and transparently retries on a fresh connection.
> This eliminated the 30-37% "incomplete" request rate that was present in earlier runs.

Results collected across three Lambda Labs 8x A100 instances. All runs use stress
distribution (70% large/xlarge prefixes), prefix ratio 0.9, and warmup enabled.

#### Post-Fix 10-Minute Runs (Instance 3)

These runs were collected after the stale connection retry fix. Incomplete rate dropped
from 30-37% to **0%** across all configurations.

**CodeLlama-13b (20 users, 10 minutes):**

| Metric | Round-Robin | Prefix-Aware | Change |
|--------|-------------|--------------|--------|
| Cache Hit Rate | 13.2% | 97.6% | **+84.5%** |
| XLarge Improvement | 2.2% | **35.9%** | **+33.7pp** |
| Large Improvement | -2.7% | **35.9%** | **+38.6pp** |
| Overall P50 TTFT | 1,000ms | 820ms | **-18.0%** |
| Overall P99 TTFT | 4,700ms | 1,000ms | **-78.7%** |
| Throughput (req/s) | 25.6 | 29.1 | **+13.7%** |
| Incomplete Rate | 0% | 0% | **0% (was 30-38%)** |
| Validation | PASSED | PASSED | |

**CodeLlama-13b (10 users, 10 minutes):**

| Metric | Round-Robin | Prefix-Aware | Change |
|--------|-------------|--------------|--------|
| Cache Hit Rate | 11.2% | 96.8% | **+85.6%** |
| XLarge Improvement | -2.3% | **39.4%** | **+41.8pp** |
| Overall P50 TTFT | 1,000ms | 750ms | **-25.0%** |
| Overall P99 TTFT | 4,500ms | 940ms | **-79.1%** |
| Throughput (req/s) | 13.3 | 15.3 | **+14.6%** |
| Incomplete Rate | 0% | 0% | **0% (was 33%)** |
| Validation | PASSED | PASSED | |

**Llama-3.1-8B (20 users, 10 minutes):**

| Metric | Round-Robin | Prefix-Aware | Change |
|--------|-------------|--------------|--------|
| Cache Hit Rate | 11.9% | 98.1% | **+86.1%** |
| XLarge Improvement | 0.0% | **31.6%** | **+31.6pp** |
| Overall P50 TTFT | 410ms | 430ms | +4.9% |
| Overall P99 TTFT | 580ms | 500ms | **-13.8%** |
| Throughput (req/s) | 44.2 | 43.7 | -1.2% |
| Incomplete Rate | 0% | 0% | **0% (was 37%)** |
| Validation | PASSED | PASSED | |

The 8B model is fast enough (~410-430ms) that routing overhead is noticeable in aggregate
TTFT, but per-bucket XLarge improvement remains strong at 31.6%.

#### 30-Minute Validated Runs (Post-Fix)

**CodeLlama-13b (30 users, 30 minutes):**

| Metric | Round-Robin | Prefix-Aware | Change |
|--------|-------------|--------------|--------|
| Cache Hit Rate | 12.1% | 97.5% | **+85.4%** |
| XLarge Improvement | -1.1% | **32.8%** | **+33.9pp** |
| Large Improvement | -9.9% | **16.3%** | **+26.1pp** |
| Overall P50 TTFT | 1,100ms | 870ms | **-20.9%** |
| Overall P99 TTFT | 6,800ms | 1,000ms | **-85.3%** |
| Throughput (req/s) | 36.3 | 44.4 | **+22.3%** |
| Incomplete Rate | 0% | 0% | **0%** |
| Validation | **FAILED** | **PASSED** | |
| Error Rate | 0% | 0% | — |

Under sustained 30-minute load, round-robin degrades badly (P99 hits 6.8 seconds) while
prefix-aware stays at 1.0 seconds. The round-robin baseline **fails validation** while
prefix routing passes. With the stale connection fix, incomplete rate is 0% on both sides.

**Llama-3.1-8B (30 users, 30 minutes):**

| Metric | Round-Robin | Prefix-Aware | Change |
|--------|-------------|--------------|--------|
| Cache Hit Rate | 11.9% | 98.0% | **+86.0%** |
| XLarge Improvement | 0.2% | **40.5%** | **+40.3pp** |
| Large Improvement | 0.3% | 3.9% | +3.6pp |
| Overall P50 TTFT | 420ms | 440ms | +4.8% |
| Overall P99 TTFT | 460ms | 490ms | +6.5% |
| Throughput (req/s) | 66.2 | 65.4 | -1.1% |
| Incomplete Rate | 0% | 0% | **0%** |
| Validation | PASSED | PASSED | |
| Error Rate | 0% | 0% | — |

The 8B model shows strong XLarge improvement (40.5%) now that stale connections no longer
mask the miss penalty (miss P50 763ms vs hit P50 454ms). Overall TTFT is slightly worse
due to routing overhead (~11ms tokenization), but the model is fast enough (~420-440ms)
that this is expected. Throughput is essentially flat.

#### 70B Model Test

**Llama-3.1-70B (TP=2, 4 backends, 8xA100 80GB, 32K context, 16 users, 30 minutes):**

| Metric | Round-Robin | Prefix-Aware | Change |
|--------|-------------|--------------|--------|
| Cache Hit Rate | 25.2% | 97.8% | **+72.6%** |
| XLarge Improvement | 0.3% | **43.8%** | **+43.5pp** |
| Large Improvement | 0.2% | **38.8%** | **+38.7pp** |
| Overall P50 TTFT | 1,500ms | 1,500ms | 0% |
| Overall P99 TTFT | 1,600ms | 1,600ms | 0% |
| Throughput (req/s) | 14.8 | 14.8 | 0% |
| Incomplete Rate | 0% | 0% | **0%** |
| Validation | PASSED | PASSED | |
| Error Rate | 0% | 0% | — |

Under sustained 30-minute load, the 70B model's benefit is **per-request cache efficiency**
rather than P99 or throughput. XLarge requests are 44% faster when cache-hit (hit P50
~1498ms vs miss P50 ~2665ms), but this doesn't translate to aggregate P99/throughput wins
because the model is compute-bound at 16 users with 4 backends — neither side is queue-overloaded.

This contrasts with 13B, where queue buildup under 30 concurrent users causes round-robin
P99 to degrade to 6.8 seconds. With 70B, the longer per-request inference time means fewer
concurrent requests in-flight, preventing queue saturation.

<details>
<summary>15-minute and 40GB reference runs</summary>

**15-minute run (same 80GB config):** XLarge 48.0%, P99 -33.3%. The higher P99 improvement
reflects warm-up effects — under sustained 30-minute load, round-robin P99 stabilizes from
2400ms to 1600ms, equalizing with prefix-aware.

**40GB A100 test (TP=4, 2 backends, 4K context):** XLarge 49.3%, but round-robin had 63.9%
incomplete rate (only 36% of requests completed). With only 2 backends, queuing was
catastrophic. The 80GB test (4 backends) is the reliable reference.

</details>

**Key findings for 70B:**
- **XLarge improvement is the highest of any model (44-49%)** — 70B has the most compute per
  token, so KV cache reuse saves the most (~1498ms hit vs ~2665ms miss)
- **P99 and throughput are flat under sustained load** — unlike 13B, 70B at 16 users with 4
  backends is compute-bound, not queue-bound. The 15m run showed -33% P99 but this was a
  warm-up effect that stabilized over 30 minutes
- **The benefit is per-request, not aggregate** — individual large/XLarge requests are 39-44%
  faster on cache hit, but aggregate metrics don't shift because the system isn't overloaded
- **Backend count matters** — 2 backends (40GB) causes 64% timeouts; 4 backends (80GB) handles
  the load cleanly. Production deployments should target TP=2 on 80GB GPUs
- **Baseline cache hit rate scales with 1/backends** — 49% with 2 backends (1/2 chance),
  25% with 4 backends (1/4 chance), vs 12% with 8 backends for 8B/13B

#### Complete Results Matrix (All Runs)

**bb20555 runs (Feb 21, 2026 — speculative load increment + batched route learning):**

| Model | Users | Duration | P99 TTFT Change | Throughput | Cache Hit Rate | Incomplete | Instance |
|-------|-------|----------|-----------------|------------|----------------|------------|----------|
| **13B** | **30** | **30m** | **-51.3%** | **+13.9%** | 53.6% | **0%** | 5 |
| **13B** | **20** | **10m** | **-47.5%** | +3.2% | 86.8% | **0%** | 5 |
| **13B** | **20** | **10m** | **-23.8%** | +12.8% | 89.1% | **0%** | 5 |
| **13B** | **10** | **10m** | **-26.8%** | flat | 87.7% | **0%** | 5 |
| **8B** | **20** | **10m** | flat | -2.8% | 94.6% | **0%** | 5 |
| **8B** | **30** | **30m** | flat | -4.6% | 64.9% | **0%** | 5 |
| 8B (64u stress) | 64 | 15m | — | 65.1 tok/s | 40.5% | 0% | 5 |
| 8B (16K pfx) | 20 | 10m | 0.0% | -5.2% | 96.2% | 0% | 5 |
| 70B (TP=4, 40GB) | 16 | 15m | +2.6% | -17.7% | 75.4% | 0% (RR: 0.6%) | 5 |
| 13B (ratio 0.7) | 20 | 10m | +86.4% | flat | 76.8% | 0% | 5 |
| 13B (ratio 0.5) | 20 | 10m | +64.4% | -8.4% | 66.7% | 0% | 5 |
| 13B (client tok) | 30 | 10m | +18.3% | +5.6% | 54.0% | 0% | 5 |

**Instance 3 post-fix runs (Feb 10-14, 2026 — 0% incomplete rate, pre-batched-routes):**

| Model | Users | Duration | XLarge Improvement | P99 TTFT Change | Throughput | Cache Hit Rate | Incomplete | Instance |
|-------|-------|----------|-------------------|-----------------|------------|----------------|------------|----------|
| **70B (TP=2, 80GB)** | **16** | **30m** | **43.8%** | 0% | 0% | 97.8% | **0%** | 4 |
| 70B (TP=2, 80GB) | 16 | 15m | 48.0% | -33.3% | -1.4% | 96.9% | 0% | 4 |
| 70B (TP=4, 40GB) | 16 | 15m | 49.3% | n/a‡ | n/a‡ | 97.6% | 0% (RR: 64%) | 3 |
| **13B** | **30** | **30m** | **32.8%** | **-85.3%** | **+22.3%** | 97.5% | **0%** | 3 |
| **8B** | **30** | **30m** | **40.5%** | +6.5% | -1.1% | 98.0% | **0%** | 3 |
| **13B** | **20** | **10m** | **35.9%** | **-78.7%** | **+13.7%** | 97.6% | **0%** | 3 |
| **13B** | **10** | **10m** | **39.4%** | **-79.1%** | **+14.6%** | 96.8% | **0%** | 3 |
| **8B** | **20** | **10m** | **31.6%** | -13.8% | -1.2% | 98.1% | **0%** | 3 |
| **13B (ratio 0.7)** | **20** | **10m** | n/a† | **-86.9%** | **+31.5%** | 90.0% | **0%** | 3 |
| **13B (ratio 0.5)** | **20** | **10m** | **37.3%** | **-76.1%** | **+10.0%** | 91.0% | **0%** | 3 |
| **13B (client tok)** | **30** | **10m** | 9.6% | **-83.6%** | **+19.4%** | 97.4% | **0%** | 3 |
| **8B (64u stress)** | **64** | **15m** | **29.0%** | — | — | 98.0% | **0%** | 3 |
| **8B (16K pfx)** | **20** | **10m** | **27.9%** | -11.5% | ~same | 97.6% | **0%** | 3 |

†XLarge metric unreliable at ratio 0.7: only a handful of XLarge misses (P50 64ms from tiny sample).

**Pre-fix runs (30-37% incomplete from stale connections — TTFT/routing metrics valid, throughput understated):**

| Model | Users | Duration | XLarge Improvement | P99 TTFT Change | Throughput | Cache Hit Rate | Instance |
|-------|-------|----------|-------------------|-----------------|------------|----------------|----------|
| 13B | 30 | 30m | 35.0% | -87.0% | +27.1% | 97.9% | 2 |
| 8B | 30 | 30m | 30.9% | +10.6% | +1.6% | 98.1% | 2 |
| 13B | 30 | 10m | 92.7% | -75.6% | +16.6% | 98.0% | 1 |
| 13B | 30 | 10m | -7.4% | -68.6% | +7.1% | 97.8% | 2 |
| 13B | 20 | 10m | 33.6% | -81.2% | +12.9% | 97.3% | 2 |
| 13B | 10 | 10m | 43.1% | -68.5% | +1.4% | 96.4% | 2 |
| 8B | 30 | 10m | 37.4% | +16.3% | ~same | 97.8% | 1 |
| 8B | 30 | 10m | 15.8% | 0% | +1.2% | 97.5% | 2 |
| 8B | 20 | 10m | 29.7% | -18.3% | +1.2% | 97.9% | 2 |
| 8B (16K pfx) | 20 | 10m | 43.6% | -24.6% | +0.9% | 97.7% | 2 |
| 8B (64u stress) | 64 | 15m | 18.0% | — | — | 98.3% | 2 |
| 13B (ratio 0.7) | 20 | 10m | 33.4% | -76.3% | +19.4% | 93.4% | 2 |
| 13B (ratio 0.5) | 20 | 10m | 43.5% | -81.8% | +17.6% | 89.5% | 2 |
| 13B (client tok) | 30 | 10m | 24.8% | -30.0% | -6.4% | 97.8% | 2 |

#### Instance-to-Instance Variance

The XLarge Improvement metric shows significant variance between instances (e.g., 13B/30u
ranges from -7.4% to 92.7%). This metric compares cache hit vs miss latency *within* a
single prefix-aware run, making it sensitive to backend thermal state, GPU clock speeds,
and transient queuing patterns.

**Metrics that are consistent across instances:**
- **Cache hit rate**: 96.4-98.1% (always excellent)
- **13B P99 TTFT improvement**: -69% to -87% (always strong)
- **13B throughput improvement**: +7% to +27% (always positive)
- **Incomplete rate**: 0% (post-fix) — previously 30-37% from stale TCP connections
- **Error rate**: 0% (always clean)

**Metrics that vary significantly:**
- **XLarge TTFT Improvement**: 16-93% (use multiple-instance averages as reference)

#### Benefits Scale With Model Size

| Factor | 8B | 13B | 70B |
|--------|-----|------|------|
| Prefill time per token | ~0.05ms | ~0.08ms | ~0.2ms+ |
| XLarge prefill cost | ~300ms | ~500ms | ~1500-2000ms |
| Cache hit savings | Moderate | Large | **Huge** |
| Queue buildup under load | Mild | Severe | **Severe** (2 backends) |
| Load-aware routing impact | Small | **Dramatic** | **Large** |
| XLarge improvement | 28-40% | 33-39% | **44-49%** |
| P99 TTFT change | +6.5% | -85% | ~same (sustained) |
| RR incomplete rate | 0% | 0% | 0% (4 backends) / 64% (2 backends) |

Larger models have higher per-token compute cost, so cache misses are more expensive.
The 70B model shows the highest XLarge improvement (48-49%) because the hit/miss gap is
widest (~1520ms vs ~2924ms). P99 improves by 33% — less dramatic than 13B (-85%) because
70B inference is inherently slower (~1500ms even for hits) and throughput is compute-bound.
Backend count is critical for 70B: 2 backends (TP=4 on 40GB) causes 64% timeouts, while
4 backends (TP=2 on 80GB) handles the load cleanly.

#### Impact of Prefix Size (8K vs 16K tokens)

The default benchmark uses prefixes up to 8K tokens. Testing with `--prefix-max-tokens 16000`
shows that longer prefixes amplify the benefit of prefix-aware routing:

| Max Prefix | XLarge Miss P50 | XLarge Hit P50 | XLarge Improvement | P99 TTFT Change |
|-----------|-----------------|----------------|-------------------|-----------------|
| 8K (default) | 638ms | 448ms | 29.7% | -18.3% |
| **16K** | 817ms | 461ms | **43.6%** | **-24.6%** |

*Both runs: 8B model, 20 users, 10 minutes, Instance 2.*

Cache hits stay fast (~450-460ms) regardless of prefix length — the KV cache eliminates
prefill computation. But cache misses get slower with longer prefixes (more tokens to
compute), widening the gap. The +13.9pp improvement demonstrates that **production RAG
workloads with 16K+ token contexts will see larger benefits than the default benchmarks
suggest**.

> **Note:** Only the 8B model was tested at 16K because the 13B model is limited to
> `--max-model-len 8192` on 40GB A100s due to VRAM constraints.

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

1. **Cache hit rate always improves**: 12% → 54-98% depending on config and instance
2. **Per-bucket improvement is the real metric**: Overall TTFT can be misleading due to small prefix overhead
3. **P99 tail latency is the strongest win for 13B**: -24% to -85% depending on architecture version
4. **Throughput improves for 13B**: +3% to +22% depending on instance and concurrency level
5. **Results are instance/architecture dependent**: Cache hit rates, P99 improvements, and hot-spotting behavior vary
6. **Default prefix ratio (0.9) is the reliable operating point**: Lower ratios may hot-spot on current architecture
7. **8B is routing-neutral**: Inference too fast for cache savings to matter in aggregate TTFT
8. **70B benefit is reliability, not speed**: Incompletes eliminated, but throughput cost from cache affinity

### Value Proposition

For workloads with **large shared prefixes** (RAG, system prompts, few-shot):

#### Performance by Model Size and Load

**bb20555 (Feb 21, Instance 5 — latest architecture):**

| Model | Load | Users | Duration | Cache Hit Rate | P99 TTFT Change | Throughput | Notes |
|-------|------|-------|----------|----------------|-----------------|------------|-------|
| **CodeLlama-13b** | **Heavy** | **30** | **30m** | 53.6% | **-51.3%** | **+13.9%** | Best sustained result |
| **CodeLlama-13b** | **Moderate** | **20** | **10m** | 87-89% | **-24% to -48%** | +3% to +13% | Consistent across runs |
| **CodeLlama-13b** | **Normal** | **10** | **10m** | 87.7% | **-26.8%** | flat | Stable |
| **Llama-3.1-8B** | **Heavy** | **30** | **30m** | 64.9% | flat | -4.6% | Routing-neutral |
| **Llama-3.1-8B** | **Moderate** | **20** | **10m** | 94.6% | flat | -2.8% | Routing-neutral |
| **Llama-3.1-70B** | **Moderate** | **16** | **15m** | 75.4% | +2.6% | -17.7% | Reliability (0% inc) |

**Instance 3 (Feb 10-14 — pre-batched-routes, higher cache affinity):**

| Model | Load | Users | Duration | Cache Hit Rate | P99 TTFT Change | Throughput | Notes |
|-------|------|-------|----------|----------------|-----------------|------------|-------|
| **Llama-3.1-70B** | **Heavy** | **16** | **30m** | 97.8% | 0% | 0% | TP=2, 80GB, 4 backends |
| **CodeLlama-13b** | **Heavy** | **30** | **30m** | 97.5% | **-85.3%** | **+22.3%** | |
| **CodeLlama-13b** | **Moderate** | **20** | **10m** | 97.6% | **-78.7%** | **+13.7%** | |
| **CodeLlama-13b** | **Normal** | **10** | **10m** | 96.8% | **-79.1%** | **+14.6%** | |
| **Llama-3.1-8B** | **Heavy** | **30** | **30m** | 98.0% | +6.5% | -1.1% | |
| **Llama-3.1-8B** | **Moderate** | **20** | **10m** | 98.1% | -13.8% | -1.2% | |

<details>
<summary>Earlier results (Jan 2026, pre-fix — click to expand)</summary>

| Model | Load | Users | Duration | Cache Hit Rate | Notes |
|-------|------|-------|----------|----------------|-------|
| CodeLlama-13b | Heavy | 30 | 30m | 97.9% | Pre-fix |
| Llama-3.1-8B | Heavy | 30 | 30m | 98.1% | Pre-fix |
| CodeLlama-13b | Moderate | 20 | 30m | 97.6% | Jan 2026 |
| CodeLlama-13b | Normal | 10 | 10m | 96.4% | Jan 2026 |
| CodeLlama-13b | Heavy | 30 | 10m | 97.5% | Jan 2026 |
| Llama-3.1-8B | Moderate | 20 | 30m | 97.8% | Jan 2026 |
| Llama-3.1-8B | Normal | 10 | 10m | 95.6% | Jan 2026 |
| Llama-3.1-8B | Heavy | 30 | 10m | 97.8% | Jan 2026 |

</details>

**Key takeaways:**
- **P99 tail latency** is the strongest win for 13B — -24% to -51% (bb20555), -79% to -85% (Instance 3)
- **Throughput increases** for 13B across both architectures (+3-14% on bb20555, +14-22% on Instance 3)
- **Cache hit rates** are always much higher than round-robin — 54-98% vs 11-13% baseline
- **8B is routing-neutral** — inference too fast (~1400ms) for cache savings to matter in aggregate
- **70B benefit is reliability** — incompletes eliminated, but throughput drops with cache affinity
- **Backend count matters for 70B** — TP=4 (2 backends, 40GB) limits routing surface
- **0% incomplete rate** on all post-fix runs (Feb 9, 2026 onwards)

#### Impact of Prefix Sharing Ratio

Not all workloads have 90% prefix sharing. These tests show how improvement scales
(CodeLlama-13b, 20 users, 10 minutes):

**bb20555 (Feb 21, Instance 5 — speculative load increment + batched route learning):**

| Prefix Ratio | Cache Hit Rate | P99 TTFT Change | Throughput | Validation |
|--------------|----------------|-----------------|------------|------------|
| 0.9 (default) | 87-89% | **-24% to -48%** | +3% to +13% | PASSED |
| 0.7 | 76.8% | +86.4% (hot-spotting) | flat | PASSED→FAILED |
| 0.5 | 66.7% | +64.4% (hot-spotting) | -8.4% | PASSED→FAILED |

**Instance 3 (Feb 10-14 — per-request SMP broadcast, pre-batched-routes):**

| Prefix Ratio | Cache Hit Rate | P99 TTFT Change | Throughput | Validation |
|--------------|----------------|-----------------|------------|------------|
| 0.9 (default) | 97.6% | -78.7% | +13.7% | PASSED |
| 0.7 | 90.0% | -86.9% | +31.5% | PASSED |
| 0.5 | 91.0% | -76.1% | +10.0% | PASSED |

**Key findings:**
- **At 0.9 prefix ratio**, prefix-aware routing consistently delivers P99 improvements and
  positive or neutral throughput. This is the recommended operating point.
- **At lower prefix ratios (0.5-0.7)**, results are highly instance/architecture dependent.
  Instance 3 (with per-request SMP broadcast) showed strong improvements at all ratios.
  bb20555 (with batched route learning) shows hot-spotting at these ratios — more unique
  prefixes overwhelm the shard-local speculative load increment (backlog 20.11).
- **Cache hit rates are always much higher than round-robin** — even bb20555 at 0.5 ratio
  achieves 66.7% vs ~11% baseline. The benefit is real, but load distribution suffers.
- **Recommendation:** Use `--prefix-ratio 0.9` (default) for production workloads. If your
  workload has lower natural prefix sharing, monitor for hot-spotting via node request
  distribution in Prometheus metrics.

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

The February 2026 results cover the core matrix with load-aware routing across two instances. The following benchmarks would complete remaining gaps.

### Running with bench-runner.sh

The benchmark runs below are built into `scripts/bench-runner.sh` as suites. You can run them in one go instead of invoking each manually:

```bash
# Preview what will run
./scripts/bench-runner.sh --dry-run --suite all

# Run high-priority only (~1.5h)
./scripts/bench-runner.sh --suite high

# Run everything including the 70B test (requires 8xA100)
./scripts/bench-runner.sh --suite all

# Resume from run #4 if something failed
./scripts/bench-runner.sh --suite all --resume 4
```

The runner produces a `runner_summary_*.md` report and logs to `benchmark-reports/`. See `./scripts/bench-runner.sh --help` for all options.

### Completed (February 2026)

**bb20555 full suite (Feb 21, Instance 5 — speculative load increment + batched route learning):**

| # | Config | Status | Result |
|---|--------|--------|--------|
| 1 | 13B, 20u, 10m | **Done (2x)** | P99 -47.5%/-23.8%, throughput +3%/+13%, 87-89% cache |
| 2 | 8B, 20u, 10m | **Done** | P99 flat, throughput -2.8%, 94.6% cache |
| 3 | 13B, 10u, 10m | **Done** | P99 -26.8%, throughput flat, 87.7% cache |
| 4 | 13B, 30u, 30m (validated) | **Done** | P99 **-51.3%**, throughput **+13.9%**, 53.6% cache |
| 5 | 8B, 30u, 30m (validated) | **Done** | P99 flat, throughput -4.6%, 64.9% cache |
| 6 | 13B, prefix ratio 0.7 | **Done** | P99 +86.4% **hot-spotting**, 76.8% cache |
| 7 | 13B, prefix ratio 0.5 | **Done** | P99 +64.4% **hot-spotting**, 66.7% cache |
| 8 | 13B, client tokenization | **Done** | P99 +18.3% hot-spotting, throughput +5.6%, 54% cache |
| 9 | 8B, 64u, 15m stress test | **Done** | P99 1700ms, 24.1 req/s, 40.5% cache, 0 errors |
| 10 | 70B, 16u, 15m (TP=4, 40GB) | **Done** | P99 +2.6%, throughput -17.7%, incompletes 0.6%→0% |
| 11 | 8B, 20u, 10m, 16K prefix | **Done** | P99 0%, throughput -5.2%, 96.2% cache |

<details>
<summary>Instance 3 post-fix results (Feb 10-14, pre-batched-routes — click to expand)</summary>

| # | Config | Status | Result |
|---|--------|--------|--------|
| 1 | 13B, 20u, 10m | **Re-run** | XLarge 35.9%, P99 -78.7%, **0% incomplete** |
| 2 | 8B, 20u, 10m | **Re-run** | XLarge 31.6%, P99 -13.8%, **0% incomplete** |
| 3 | 13B, 10u, 10m | **Re-run** | XLarge 39.4%, P99 -79.1%, **0% incomplete** |
| 4 | 13B, 30u, 30m (validated) | **Re-run** | XLarge 32.8%, P99 -85.3%, +22.3% throughput, **0% incomplete** |
| 5 | 8B, 30u, 30m (validated) | **Re-run** | XLarge 40.5%, P99 +6.5%, ~same throughput, **0% incomplete** |
| 6 | 13B, prefix ratio 0.7 | **Re-run** | P99 -86.9%, +31.5% throughput, **0% incomplete** |
| 7 | 13B, prefix ratio 0.5 | **Re-run** | XLarge 37.3%, P99 -76.1%, +10.0% throughput, **0% incomplete** |
| 8 | 13B, client tokenization | **Re-run** | XLarge 9.6%, P99 -83.6%, +19.4% throughput, **0% incomplete** |
| 9 | 8B, 64u, 15m stress test | **Re-run** | XLarge 29.0%, P99 600ms, 0 errors, **0% incomplete** |
| 11 | 8B, 20u, 10m, 16K prefix | **Re-run** | XLarge 27.9%, P99 -11.5%, **0% incomplete** |
| 10 | 70B, 16u, 15m (TP=4, 40GB) | **Done** | XLarge 49.3%, RR 64% incomplete, prefix 0% |
| 10b | 70B, 16u, 15m (TP=2, 80GB) | **Done** | XLarge 48.0%, P99 -33.3%, 0% incomplete both sides |
| 10c | 70B, 16u, 30m (TP=2, 80GB) | **Done** | XLarge 43.8%, P99 ~same (sustained), 0% incomplete |

</details>

<details>
<summary>Pre-fix results (before stale connection retry — click to expand)</summary>

| # | Config | Status | Result |
|---|--------|--------|--------|
| ~~4~~ | ~~13B, 30u, 30m (validated)~~ | ~~Done~~ | ~~XLarge 35.0%, P99 -87.0%~~ |
| ~~5~~ | ~~8B, 30u, 30m (validated)~~ | ~~Done~~ | ~~XLarge 30.9%~~ |
| ~~6~~ | ~~13B, prefix ratio 0.7~~ | ~~Done~~ | ~~XLarge 33.4%, P99 -76.3%~~ |
| ~~7~~ | ~~13B, prefix ratio 0.5~~ | ~~Done~~ | ~~XLarge 43.5%, P99 -81.8%~~ |
| ~~8~~ | ~~13B, client tokenization~~ | ~~Done~~ | ~~XLarge 24.8%, P99 -30.0%~~ |
| ~~9~~ | ~~8B, 64u, 15m stress test~~ | ~~Done~~ | ~~XLarge 18.0%, 0 errors~~ |
| ~~11~~ | ~~8B, 20u, 10m, 16K prefix~~ | ~~Done~~ | ~~XLarge 43.6%, P99 -24.6%~~ |

</details>

### All Benchmarks Complete

Three full benchmark rounds have been completed:
1. **Pre-fix** (Jan-Feb 2026, Instance 1-2) — baseline with stale connection issues (30-37% incomplete)
2. **Post-fix** (Feb 10-14, Instance 3) — stale connection retry fixed, per-request SMP broadcast
3. **bb20555** (Feb 21, Instance 5) — speculative load increment + batched route learning + vLLM v0.15.1 pinned

Every run on bb20555 shows 0% incomplete rate. Default config (prefix-ratio 0.9) delivers
consistent P99 improvements for 13B (-24% to -51%). Lower prefix ratios and client-tokenize
show hot-spotting — tracked in backlog 20.11 (cross-shard speculative load synchronization).

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
# Kill any leftover vLLM processes (parent launcher + renamed children like VLLM::EngineCore)
pkill -9 -f "vllm.entrypoints" || true
pkill -9 -f "vllm.engine" || true
pkill -9 -f "VLLM::" || true

# Remove any leftover containers
docker compose -f docker-compose.benchmark-real.yml -p ranvier-benchmark-real down -v --remove-orphans

# Verify GPUs are free
nvidia-smi --query-compute-apps=pid --format=csv,noheader

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

# Run everything including the 70B test (TP=4 on 8xA100 40GB)
./scripts/bench-runner.sh --suite all
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
