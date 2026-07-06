# Benchmark Methodology (8x A100)

How to run Ranvier benchmarks against real vLLM backends: environment setup, workload
knobs, warm-up discipline, the A/B test scenarios, the validation checklist, expected
metric bands, live monitoring, troubleshooting, and result export. This is the
load-bearing **"how"** — it is kept free of dated run results on purpose.

> **Where the numbers live:**
> - Results valid on the **current** defaults → [benchmark-results-current.md](benchmark-results-current.md)
> - The dated, append-only lab notebook (per-instance tables, invalidated runs, re-run
>   plans) → [history/benchmark-history-8xA100.md](history/benchmark-history-8xA100.md)
> - How to read TTFT / cache-hit numbers without fooling yourself →
>   [interpreting-benchmark-numbers.md](interpreting-benchmark-numbers.md)
>
> **Workload defaults live in `tests/integration/locustfile_real.py` — the single source
> of truth.** The wrapper scripts (`bench.sh`, `bench-residency-ab.sh`) forward a workload
> knob only when you explicitly set it, so they never shadow the locustfile. In particular:
> - `NUM_LARGE_PREFIXES` defaults to **50**, never the historical 5 — with N backends a
>   prefix pool ≤ N pigeonholes every prefix onto ≤ N backends under pure affinity, which
>   manufactured a false "regression" in the May 2026 affinity-thrashing investigation. Use
>   a small value only to *deliberately* stress prefix concentration.
> - `SHARED_PREFIX_RATIO` defaults to **0.9** (aligned across the locustfile and `bench.sh`).
>
> **Current shipped server defaults:** route-batch **flush interval = 20ms**
> (`src/config_schema.hpp`, `docker-compose.benchmark-real.yml`), load-aware routing ON
> (`load_imbalance_factor` 2.0 / floor 2), cache-residency 0.2. The older "10ms is the
> correct default" note is historical and superseded — see the history archive.

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

### CPU Pinning & NUMA Topology

The benchmark compose file pins each Ranvier node to **dedicated, non-overlapping CPU cores**
via Docker `cpuset` and Seastar `--cpuset`. This is critical for accurate benchmarks because:

1. **No core overlap** — Without pinning, all 3 nodes' Seastar reactors compete for the same
   cores (default 0-7), causing context switches that defeat Seastar's thread-per-core model.
2. **NUMA-local L3 cache** — Cores within a NUMA node share an L3 cache. Spreading a single
   Ranvier node across NUMA boundaries causes remote memory access (~2x latency penalty).
3. **Deterministic scheduling** — Pinned reactors never get migrated by the scheduler,
   eliminating a source of tail latency jitter.

**Default core assignments** (env-overridable):

| Node | Docker cpuset | Seastar --cpuset | Env var |
|------|---------------|------------------|---------|
| ranvier1 | 0-7 | 0-7 | `RANVIER_CPUSET_1` |
| ranvier2 | 8-15 | 8-15 | `RANVIER_CPUSET_2` |
| ranvier3 | 16-23 | 16-23 | `RANVIER_CPUSET_3` |

**Verify your NUMA topology before benchmarking:**

```bash
# Show NUMA nodes and which cores belong to each
numactl --hardware

# Example output (dual-socket AMD EPYC):
#   node 0 cpus: 0-61
#   node 1 cpus: 62-123
# → Defaults (0-23) all fit in node 0. No changes needed.

# If your layout differs, override:
RANVIER_CPUSET_1=0-7 RANVIER_CPUSET_2=8-15 RANVIER_CPUSET_3=16-23 \
  docker compose -f docker-compose.benchmark-real.yml up -d
```

> **Note:** Previous benchmarks (before this change) ran without CPU pinning. All 3 nodes
> likely shared cores 0-7, which may have understated Ranvier's performance due to
> context-switching overhead and L3 cache thrashing between nodes.

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

**Expected Results (8B model, 8x A100, 08ba984+ post-fix):**

| Metric | Round-Robin | Prefix-Aware | Notes |
|--------|-------------|--------------|-------|
| Cache Hit Rate | ~12% | ~65-95% | Instance/config dependent |
| P99 TTFT | ~500-600ms | ~500-600ms | Flat for 8B (too fast) |
| Incomplete Rate | 0% | 0% | Fixed by stale connection retry |
| Throughput | ~44-50 req/s | ~44-47 req/s | ~same for 8B |

**For 13B models, expect larger improvements (especially P99 and throughput):**

| Metric | Round-Robin | Prefix-Aware | Notes |
|--------|-------------|--------------|-------|
| Cache Hit Rate | ~12% | ~53-63% | ~60% ceiling with batched route learning |
| Overall P99 TTFT | ~3000-9700ms | ~1900-4000ms | **-37% to -59%** tail latency |
| Throughput | ~33-47 req/s | ~34-48 req/s | **+3-23%** higher |

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
| **13B** | **20** | **1m** | XLarge | ~1142ms | ~846ms | **~26%** | Feb 27, post-SSE-fix (b63c165), 1m quick validation |
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
- **P99 tail latency is the strongest win for 13B** — -79% to -85% on Instance 3 (valid). ⚠ bb20555 showed -24% to -51% but baselines were inflated 3.3x.
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
