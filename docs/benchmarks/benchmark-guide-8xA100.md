# Benchmark Guide: 8x A100 Single-Host Setup

This guide provides specific test scenarios, expected results, and validation criteria for benchmarking Ranvier on a Lambda Labs 8x A100 instance.

## TL;DR Results

Prefix-aware routing with load-aware fallback vs round-robin baseline (30-minute validated runs):

| Model | Cache Hit Rate | XLarge TTFT Improvement | P99 Latency | Throughput |
|-------|----------------|------------------------|-------------|------------|
| CodeLlama-13b | 12% → **98%** | **33%** faster | **-85%** | **+22%** |
| Llama-3.1-8B | 12% → **98%** | **40%** faster | +6.5% | ~same |

*Results from February 2026 with load-aware routing + stale connection fix (30-minute validated runs, 30 users, 8x A100).*

**Key wins:**
- **8x more cache hits** — Requests routed to backends with cached KV data
- **32-40% faster TTFT** — For large prefixes (4K+ tokens), time-to-first-token drops significantly
- **Up to 85% lower tail latency** — P99 response times improve dramatically for 13B
- **Up to 22% higher throughput** — Load-aware routing prevents backend hotspots
- **0% incomplete rate** — Stale connection retry ensures every request completes

**Works across prefix sharing levels (13B, 20 users, 10m):**

| Prefix Sharing | Cache Hit Rate | XLarge Improvement | P99 TTFT |
|----------------|----------------|--------------------|----------|
| 90% (default) | 98% | 36% | -79% |
| 70% | 90% | n/a† | -87% |
| 50% | 91% | 37% | -76% |

*†XLarge metric unreliable at 0.7 due to tiny miss sample. Benchmarks use synthetic workloads simulating RAG/system-prompt patterns with large prefixes. XLarge improvement varies by instance; P99/throughput improvements are more consistent.*

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
- `--prefix-ratio 0.5` → ~90% cache hit rate
- `--prefix-ratio 0.7` → ~93% cache hit rate
- `--prefix-ratio 0.9` → ~97-98% cache hit rate
- Round-robin baseline → ~11-13% (1/8 chance with 8 backends)

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

#### CodeLlama-13b (February 2026 — 30 users, 10 minutes, post-fix)

| Metric | Server Tokenize | Client Tokenize | Difference |
|--------|-----------------|-----------------|------------|
| Routing overhead | ~10ms | ~0.3ms | **32x lower** |
| Throughput change | +13.7% | **+19.4%** | Better with client tok |
| XLarge Improvement % | ~36% | 9.6% | Lower (see below) |
| XLarge Hit P50 | ~890ms | 943ms | ~same |
| XLarge Miss P50 | ~1300ms | 1043ms | -20% faster misses |
| P99 TTFT Change | **-79%** | **-83.6%** | Comparable |
| Cache Hit Rate | 97.6% | 97.4% | Same |

> **Note:** With the stale connection fix, client tokenization now shows its true benefit:
> **+19.4% throughput** (vs +13.7% server-side) and **-83.6% P99** (vs -79% server-side).
> Pre-fix data showed -6.4% throughput and -30% P99 due to stale connections disproportionately
> affecting the prefix-aware side.

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

**Expected Results (8B model, 8x A100, load-aware routing + stale connection fix):**

| Metric | Round-Robin | Prefix-Aware | Notes |
|--------|-------------|--------------|-------|
| Cache Hit Rate | ~12% | ~98% | 8x more cache hits |
| XLarge Improvement | ~0% | **~32%** | Consistent across instances |
| Incomplete Rate | 0% | 0% | Fixed by stale connection retry |
| Throughput | ~44 req/s | ~44 req/s | ~same for 8B |

**For 13B models, expect larger improvements (especially P99 and throughput):**

| Metric | Round-Robin | Prefix-Aware | Notes |
|--------|-------------|--------------|-------|
| XLarge Improvement | ~0% | **~33-39%** | Instance-dependent |
| Overall P99 TTFT | ~4500-6800ms | ~940-1000ms | **-79 to -85%** tail latency |
| Throughput | ~14-36 req/s | ~15-44 req/s | **+14-22%** higher |

**How to interpret:**
- **Cache hit rate** is the headline metric (12% → 98%) — always consistent
- **P99 tail latency** is the strongest win for 13B — consistently -79% or better
- **Per-bucket improvement** shows XLarge benefit (~32-39%)
- **Incomplete rate** should be 0% — if you see >1%, check for connection pool issues
- **Overall TTFT** may look worse for 8B due to small prefix overhead in aggregate
- Focus on **Large/XLarge buckets** where caching matters most

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

**Measured Results (February 2026, 8B, 64 users, 15m, prefix-only, post-fix):**

| Metric | Result | Alert Threshold |
|--------|--------|-----------------|
| Error Rate | **0%** | > 5% |
| P99 TTFT | **600ms** | > 5000ms |
| Requests/sec | **22.0** (successful TTFT) | < 2.0 |
| Cache Hit Rate | **98.0%** | < 40% |
| XLarge Improvement | **29.0%** | |
| Incomplete Rate | **0%** | |
| Validation | **PASSED** | |

**Notes:** XLarge improvement is lower than 20-user runs (~32-40%) because contention compresses
the hit/miss gap — even cache hits wait in backend queues at 64 users. Load distribution was
virtually even (~6600 across 3 Ranvier nodes). All 19,841 requests completed with zero errors
and zero incomplete. Post-fix run shows higher XLarge (29% vs pre-fix 18%) and 0% incomplete.

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

**Expected:** Higher cache benefit than 8B (~35% XLarge TTFT improvement, up to -87% P99 and +27% throughput at 30 users). Requires `--max-model-len 8192` on 40GB GPUs to fit in memory.

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
| 70B | - | - | XLarge | TBD | TBD | Expected 50-60%+ | |

**Key insights:**
- **P99 tail latency is the strongest win** — -79% or better for 13B
- **XLarge improvement typically 32-39%** — consistent across instances post-fix
- **0% incomplete rate** — stale connection retry (Feb 9 fix) eliminated phantom timeouts
- **1B models show no benefit** — KV cache computation is already trivial (~10-20ms)
- **Small prefixes have overhead** — Routing cost exceeds cache benefit
- **Larger models amplify benefits** — 13B sees bigger throughput/P99 gains than 8B
- **Throughput improves for 13B** — +14-15% with 0% wasted connections

### Cache Hit Rate

| Prefix Ratio | Round-Robin | Prefix-Aware | Source |
|--------------|-------------|--------------|--------|
| 0.5 (50% shared) | ~11% | **~90%** | Feb 2026 measured |
| 0.7 (70% shared) | ~13% | **~93%** | Feb 2026 measured |
| 0.9 (90% shared) | ~12% | **~97-98%** | Feb 2026 measured |
| 0.95 (95% shared) | ~12.5% | ~98%+ | Estimated |

### Throughput Scaling

| GPUs | Expected RPS (8B model) | Notes |
|------|------------------------|-------|
| 1 | 0.5-1.0 | Baseline |
| 2 | 1.0-2.0 | Linear scaling |
| 4 | 2.0-4.0 | Near-linear |
| 8 | 3.5-7.0 | Some overhead |

---

## Real Benchmark Results

### February 2026 — With Load-Aware Routing + Stale Connection Fix

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

#### Complete Results Matrix (All Runs)

**Post-fix runs (0% incomplete rate):**

| Model | Users | Duration | XLarge Improvement | P99 TTFT Change | Throughput | Cache Hit Rate | Incomplete | Instance |
|-------|-------|----------|-------------------|-----------------|------------|----------------|------------|----------|
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
benefit instead of waiting in queue. This shows most clearly in the 30-minute run where
round-robin P99 degrades to 9.2 seconds while prefix stays at 1.2 seconds.

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

1. **Cache hit rate is the headline**: 12% → 98% means nearly every request benefits from cached KV
2. **Per-bucket improvement is the real metric**: Overall TTFT can be misleading due to small prefix overhead
3. **P99 tail latency is the strongest win**: -85% under sustained 30-minute load, with round-robin failing validation
4. **Throughput improves for larger models**: +22% for 13B because requests aren't stuck behind overloaded backends
5. **XLarge improvement varies by instance**: Use 30-minute validated runs (33% for 13B, 40% for 8B) as the reliable reference

### Value Proposition

For workloads with **large shared prefixes** (RAG, system prompts, few-shot):

#### Performance by Model Size and Load

| Model | Load | Users | Duration | XLarge TTFT Improvement | Cache Hit Rate | P99 TTFT Change | Notes |
|-------|------|-------|----------|-------------------------|----------------|-----------------|-------|
| **CodeLlama-13b** | **Heavy** | **30** | **30m** | **32.8%** | 97.5% | **-85.3%** | Feb 2026, post-fix |
| **CodeLlama-13b** | **Moderate** | **20** | **10m** | **35.9%** | 97.6% | **-78.7%** | Feb 2026, post-fix |
| **CodeLlama-13b** | **Normal** | **10** | **10m** | **39.4%** | 96.8% | **-79.1%** | Feb 2026, post-fix |
| **Llama-3.1-8B** | **Heavy** | **30** | **30m** | **40.5%** | 98.0% | +6.5% | Feb 2026, post-fix |
| **Llama-3.1-8B** | **Moderate** | **20** | **10m** | **31.6%** | 98.1% | -13.8% | Feb 2026, post-fix |
| CodeLlama-13b | Heavy | 30 | 30m | 35.0% | 97.9% | -87.0% | Feb 2026, pre-fix |
| Llama-3.1-8B | Heavy | 30 | 30m | 30.9% | 98.1% | +10.6% | Feb 2026, pre-fix |
| CodeLlama-13b | Moderate | 20 | 30m | 38.9% | 97.6% | — | Jan 2026 |
| CodeLlama-13b | Normal | 10 | 10m | 48.2% | 96.4% | — | Jan 2026 |
| CodeLlama-13b | Heavy | 30 | 10m | 42.9% | 97.5% | — | Jan 2026 |
| Llama-3.1-8B | Moderate | 20 | 30m | 43.7% | 97.8% | — | Jan 2026 |
| Llama-3.1-8B | Normal | 10 | 10m | 42.7% | 95.6% | — | Jan 2026 |
| Llama-3.1-8B | Heavy | 30 | 10m | 25.9% | 97.8% | — | Jan 2026 |

**Key takeaways:**
- **P99 tail latency** is the strongest and most consistent win — -79 to -85% for 13B
- **Throughput increases** +14-22% for 13B with 0% wasted connections (post-fix)
- **Cache hit rate** is excellent (96-98%) regardless of load, model, or instance
- **XLarge improvement** typically ranges 32-40% post-fix
- **0% incomplete rate** after stale connection retry fix (Feb 9, 2026)

#### Impact of Prefix Sharing Ratio

Not all workloads have 90% prefix sharing. These tests show how improvement scales
(CodeLlama-13b, 20 users, 10 minutes, February 2026):

| Prefix Ratio | Cache Hit Rate | XLarge Improvement | P99 TTFT Change | Throughput | Validation |
|--------------|----------------|--------------------|-----------------|------------|------------|
| 0.9 (default) | 97.6% | 35.9% | -78.7% | +13.7% | PASSED (post-fix) |
| 0.7 | 90.0% | n/a† | -86.9% | +31.5% | PASSED (post-fix) |
| 0.5 | 91.0% | 37.3% | -76.1% | +10.0% | PASSED (post-fix) |

†XLarge improvement at 0.7 reported -1144% due to very few XLarge misses (P50 64ms from
tiny sample). The hit P50 of 798ms is reliable; the miss sample is too small.

**Key findings:**
- **Improvement holds up remarkably well** across prefix ratios — even at 50% sharing, the
  system delivers 91% cache hit rate and 37% XLarge improvement
- **P99 tail latency improvement is consistent** — -76% to -87% regardless of prefix ratio
- **Throughput improves at all levels** — +10% to +32%, with the strongest gains at moderate
  ratios where load-aware routing prevents more backend pile-ups

This demonstrates prefix-aware routing benefits workloads even when prefix sharing is
moderate — you don't need 90%+ sharing to see real gains.

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

# Run everything except the 70B test (no 70B support yet)
./scripts/bench-runner.sh --suite all --skip 10

# Resume from run #4 if something failed
./scripts/bench-runner.sh --suite all --resume 4
```

The runner produces a `runner_summary_*.md` report and logs to `benchmark-reports/`. See `./scripts/bench-runner.sh --help` for all options.

### Completed (February 2026)

**Post-fix (stale connection retry, Feb 9):**

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

**Pre-fix (runs 4-11 collected before stale connection fix — TTFT metrics valid):**

| # | Config | Status | Result |
|---|--------|--------|--------|
| ~~4~~ | ~~13B, 30u, 30m (validated)~~ | ~~Done~~ | ~~XLarge 35.0%, P99 -87.0%~~ (re-run above) |
| ~~5~~ | ~~8B, 30u, 30m (validated)~~ | ~~Done~~ | ~~XLarge 30.9%~~ (re-run above) |
| ~~6~~ | ~~13B, prefix ratio 0.7~~ | ~~Done~~ | ~~XLarge 33.4%, P99 -76.3%~~ (re-run above) |
| ~~7~~ | ~~13B, prefix ratio 0.5~~ | ~~Done~~ | ~~XLarge 43.5%, P99 -81.8%~~ (re-run above) |
| ~~8~~ | ~~13B, client tokenization~~ | ~~Done~~ | ~~XLarge 24.8%, P99 -30.0%~~ (re-run above) |
| ~~9~~ | ~~8B, 64u, 15m stress test~~ | ~~Done~~ | ~~XLarge 18.0%, 0 errors~~ (re-run above) |
| ~~11~~ | ~~8B, 20u, 10m, 16K prefix~~ | ~~Done~~ | ~~XLarge 43.6%, P99 -24.6%~~ (re-run above) |

### Remaining Benchmarks

```bash
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
