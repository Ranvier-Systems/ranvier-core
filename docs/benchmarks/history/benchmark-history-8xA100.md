# Benchmark History (8x A100) — Lab Notebook Archive

**Append-only archive.** These are the dated, per-instance benchmark runs, invalidated
sections, and re-run plans moved verbatim out of the old monolithic guide during the
2026-07 restructure. **Numbers here are preserved as-recorded, including ones later found
invalid** (that invalidity is the point — do not "fix" them).

> ⚠️ **Do not cite these as current results.** Every headline in this file predates the
> `NUM_LARGE_PREFIXES` 5→50 default change and was measured on the now-deprecated 5-prefix
> (pigeonhole) workload, and/or against the invalidated c70f0c1 SSE-flush baseline. For
> numbers valid on current defaults see
> [../benchmark-results-current.md](../benchmark-results-current.md); for how to run see
> [../benchmark-methodology.md](../benchmark-methodology.md).

---

### Detailed Results by Instance

<details>
<summary><b>Instance 3 (Feb 10-14, 2026) — per-request SMP route broadcasts</b></summary>

Commit 08c4915. Pre-regression, post stale-connection fix. Per-request SMP route broadcasts
(before batched route learning). This is the historical reference dataset.

| Model | Users | Duration | P99 TTFT | Throughput | Cache Hits |
|-------|-------|----------|----------|------------|------------|
| 13B | 10 | 10m | **-79.1%** | +14.6% | 96.8% |
| 13B | 20 | 10m | **-78.7%** | +13.7% | 97.6% |
| 13B | 30 | 30m | **-85.3%** | **+22.3%** | 97.5% |
| 8B | 20 | 10m | -13.8% | -1.2% | 98.1% |
| 8B | 30 | 30m | +6.5% | -1.1% | 98.0% |
| 70B (TP=2, 80GB) | 16 | 30m | flat | flat | 97.8% |

</details>

<details>
<summary><b>Instance 8 (Mar 5, 2026) — batched route learning (current architecture)</b></summary>

Commit 08e5a93. Batched route learning (10ms flush). Cache hit rates lower than Instance 3
(per-request SMP gave tighter cross-shard sync) but P99 improvements are comparable on
clean runs. Run-to-run variance noted at 20u 10m (2 of 4 runs showed hot-spotting — transient).

| Model | Users | Duration | P99 TTFT | Throughput | Cache Hits | Notes |
|-------|-------|----------|----------|------------|------------|-------|
| 13B | 10 | 10m | **-60.4%** | +12.0% | 82.6% | clean |
| 13B | 20 | 10m | **-66.7%** | +4.2% | 80.5% | clean re-run (1 of 2 clean) |
| 13B | 30 | 30m | **-79.6%** | **+13.2%** | 58.1% | clean |
| 8B | 20 | 10m | +4.3% | -5.2% | 97.8% | routing-neutral |
| 8B | 30 | 30m | +8.3% | -1.5% | 68.3% | routing-neutral |

</details>

<details>
<summary><b>Instance 9 (Apr 12-20, 2026) — boundary detection + routing fixes</b></summary>

Commits 8be20f2 → 16f3454. Four fixes landed during this benchmarking investigation:

1. **Tokenization cache `max_text_length`** (8cf01d9): Raised from 8192 to 65536 bytes.
   Large system prefixes (8-32KB) were never cached, causing 5-7ms boundary detection
   latency at 20+ users.
2. **Strategy 4 boundary detection** (addd633, PR #430): Fixed partial tokenization check
   that left `prefix_boundary=0` for ~70% of stress-test requests.
3. **Uniform load-aware routing** (fb5ad97, PR #441): Gated BOUNDED_LOAD/P2C built-in load
   checks on `load_aware_routing` toggle so operators can disable load-driven diversion.
4. **Learn consistent-hash backend** (adaec51, PR #442): ART now learns the original
   consistent-hash target, not the load-diverted backend, preventing cumulative ART drift.

Cache hit rates are ~60-63% (expected ceiling with batched route learning architecture).
Routing overhead dropped to 0.3-0.7ms. Boundary detection dropped from 5-7ms to 0.1-0.4ms.

| Model | Users | Duration | P99 TTFT | Throughput | Cache Hits | Notes |
|-------|-------|----------|----------|------------|------------|-------|
| 13B | 10 | 30m | 0% | +2.5% | 53.6% | clean, 0 incompletes |
| 13B | 20 | 30m | **-36.7%** | +2.9% | 63.4% | clean |
| 13B | 30 | 30m | **-58.8%** | **+23.1%** | 63.2% | **all metrics green**, 0 incompletes |

**Headline result (13B, 30u, 30m):** P99 -58.8%, throughput +23.1%, cache hit P99 -68.0%,
XLarge hit P99 -87.0%. Every aggregate TTFT metric improved. Validation flipped FAILED → PASSED.

> **Reproducibility note (May 22, 2026):** A 30u/30m repeat on commit `5079f9f` did **not**
> reproduce these numbers — P99 TTFT was **+44%** vs round-robin, with the miss tail blown out
> (Cache Miss P99 +69%, Large Miss P99 +123%, XLarge Miss P99 +62%). The Instance 9 result above
> is preserved for historical context, but the headline should not be quoted as current
> steady-state until the affinity-thrashing root cause is resolved. See
> [Known Issue](../kv-cache-prefix-routing-benchmark.md#known-issue-13b30u-prefix-aware-miss-tail-with-default-load-aware-thresholds).

*Note: Lambda Labs instances were near full capacity during this period, which may have
increased run-to-run variance from noisy neighbors. Earlier instances (Feb 2026) ran on
quieter hardware with more available capacity.*

</details>

<details>
<summary><b>Invalidated runs (Instance 5-6, Feb 21-22) — SSE flush regression</b></summary>

Commits bb20555, c219fbd. A 3.3x P50 latency regression (c70f0c1, fixed in 08ba984) inflated
round-robin baselines, making prefix-aware improvements appear smaller than they are. **TTFT,
P99, and throughput numbers are invalid.** Cache hit rates remain directionally valid. See
[SSE Flush Regression](#sse-flush-regression-c70f0c1) for details.

| Model | Users | Duration | ~~P99 TTFT~~ | ~~Throughput~~ | Cache Hits (valid) |
|-------|-------|----------|----------|------------|------------|
| ~~13B~~ | ~~10~~ | ~~10m~~ | ~~-26.8%~~ | ~~flat~~ | 87.7% |
| ~~13B~~ | ~~20~~ | ~~10m~~ | ~~-24% to -48%~~ | ~~+3% to +13%~~ | 87-89% |
| ~~13B~~ | ~~30~~ | ~~30m~~ | ~~-51.3%~~ | ~~+13.9%~~ | 53.6% |
| ~~8B~~ | ~~20~~ | ~~10m~~ | ~~flat~~ | ~~-2.8%~~ | 94.6% |
| ~~8B~~ | ~~30~~ | ~~30m~~ | ~~flat~~ | ~~-4.6%~~ | 64.9% |
| ~~70B~~ | ~~16~~ | ~~15m~~ | ~~+2.6%~~ | ~~-17.7%~~ | 75.4% |

**Prefix-ratio sweep (cache hit rates valid, TTFT invalid):**

| Prefix Sharing | Cache Hit Rate | Status |
|----------------|----------------|--------|
| 90% (default) | 87-89% | Needs TTFT re-run |
| 70% | 76.8% | Needs TTFT re-run |
| 50% | 66.7% | Needs TTFT re-run |

*At prefix ratios below 0.9, more unique prefixes may cause hot-spotting at medium concurrency
(20 users). Tracked in backlog item 20.11. The hot-spotting conclusion itself needs
re-validation — it was measured against the inflated baseline.*

</details>

---


## Real Benchmark Results

### April 12-20, 2026 — Instance 9: Boundary Detection + Routing Fixes

> **Commit range:** 8be20f2 → 16f3454. **vLLM:** v0.15.1 (pinned). **Instance:** Lambda Labs
> 8xA100 40GB. Default config (BOUNDED_LOAD hash, load-aware ON, 10ms flush, prefix mode).
>
> Systematic investigation of three regressions identified through benchmark analysis. Four
> fixes developed and validated across multiple iterations.

#### Fixes and Cumulative Impact

| Fix | PR | Impact |
|-----|----|--------|
| Tokenization cache `max_text_length` 8192→65536 | — | Boundary detect 5-7ms → <1ms at 20+ users |
| Strategy 4 boundary detection under partial tokenization | #430 | Cache hit rate 40% → 60% |
| Uniform `load_aware_routing` toggle across hash strategies | #441 | BOUNDED_LOAD/P2C now respect the toggle |
| Learn consistent-hash backend, not diverted backend | #442 | Prevents cumulative ART drift under load |

Progression (13B, 30 users):

| Phase | Cache Hit % | P99 TTFT | Throughput | BD P50 |
|-------|-------------|----------|------------|--------|
| Pre-fix (8be20f2) | 34-48% | Usually worse | +2-15% | 5-7ms |
| + max_text_length | 34-48% | Volatile | +2-15% | <1ms |
| + Strategy 4 boundary | 54-62% | Trending better | +5-19% | 0.2-0.4ms |
| **All fixes (16f3454)** | **63%** | **-58.8%** | **+23.1%** | **0.12ms** |

#### Validated Results (commit 16f3454)

**CodeLlama-13b, 30 users, 30 minutes (reference config):**

| Metric | Round-Robin | Prefix-Aware | Change |
|--------|-------------|--------------|--------|
| Cache Hit Rate | 14.8% | 63.2% | **+48.4%** |
| P50 TTFT | 870ms | 780ms | **-10.3%** |
| P99 TTFT | 9,700ms | 4,000ms | **-58.8%** |
| Cache Hit P99 | 11,385ms | 3,644ms | **-68.0%** |
| Throughput (req/s) | 38.6 | 47.5 | **+23.1%** |
| Incomplete / Error Rate | 0% / 0% | 0% / 0% | |
| Validation | FAILED | **PASSED** | |

Per-bucket (sample counts from reporting fix #439):

| Bucket | Baseline Hits → New Hits | Hit P99 Change |
|--------|--------------------------|----------------|
| Large [n=3527/4483] | 607 → **3,285** (5.4x) | **-63.0%** |
| XLarge [n=4872/5862] | 706 → **3,536** (5.0x) | **-87.0%** |

Routing overhead: 0.31ms total (tokenization 0.18ms, boundary detect 0.12ms, ART 0.02ms).

**Other concurrency levels (13B, 30 minutes):**

| Users | Cache Hit % | P99 TTFT | Throughput | Notes |
|-------|-------------|----------|------------|-------|
| 20 | 63.4% | **-36.7%** | +2.9% | Cache Hit P99 -49.8% |
| 10 | 53.6% | +88.9%† | +2.5% | 0 incompletes (RR had 6.9%) |

†10-user P99 is misleading: round-robin dropped 412 requests as incompletes, excluding
the slowest tail from its P99. Prefix-aware completed every request.

#### Key Findings

1. **Boundary detection was the dominant overhead at 20+ users.** A tokenization cache limit
   of 8192 bytes caused large system prefixes to bypass the cache, creating a 5-7ms P50
   bottleneck.

2. **Strategy 4 boundary detection failed silently under partial tokenization.** When system
   prefixes exceeded the 128 routing token window, `prefix_boundary` was left at 0 for ~70%
   of stress-test requests, preventing the ART from building stable routes.

3. **BOUNDED_LOAD's built-in load awareness conflicted with the external load-aware toggle.**
   Two independent diversion mechanisms made competing decisions, scattering requests and
   preventing stable ART affinity.

4. **Learning the diverted backend caused cumulative ART drift.** Under bursty load, routes
   drifted away from their consistent-hash targets as transient load-driven assignments
   overwrote stable ones.

5. **The ~63% cache hit rate ceiling is the expected behavior** of the batched route learning
   architecture (backlog §21.1). Instance 3's 97% was achieved with per-request SMP
   broadcasts, replaced by 10ms batched flushes to reduce hot-path overhead.

6. **Lambda Labs instance saturation affected P99 variance.** During April 2026, available
   instances were scarce — consistent with higher fleet utilization and noisier neighbors.

---

### February 21, 2026 — bb20555: Speculative Load Increment + Batched Route Learning

> **INVALID DATA WARNING:** All TTFT, P99, and throughput numbers in this section were measured
> while the c70f0c1 SSE flush regression was present. Round-robin baselines were inflated by
> ~3.3x, making prefix-aware improvements appear smaller. Cache hit rates and routing behavior
> (hot-spotting at low prefix ratios) are still directionally valid. This entire test matrix
> needs to be re-run on commit 08ba984+ (post-fix). See [Post-Fix Re-Run Plan](#post-fix-re-run-plan-feb-26-2026).

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

### February 22, 2026 — c219fbd: Flush Interval Sweep + Cross-Shard Sync Evaluation

> **INVALID DATA WARNING:** All TTFT, P99, and throughput numbers in this section were measured
> while the c70f0c1 SSE flush regression was present. The flush interval and cross-shard sync
> conclusions are directionally suspect — the regression inflated baselines, which may have
> masked or distorted hot-spotting behavior and sync effectiveness. The entire flush interval
> sweep and cross-shard sync evaluation needs to be re-run on 08ba984+ (post-fix).
> See [Post-Fix Re-Run Plan](#post-fix-re-run-plan-feb-26-2026).

> **Branch:** `claude/fix-shard-sync-performance`. **Commits:** 8e49187, c219fbd. **vLLM:** v0.15.1 (pinned).
> **Instance:** Lambda Labs 8xA100 40GB (Instance 6).
>
> This run evaluates two configuration axes: (1) the route batch flush interval
> (`RANVIER_ROUTE_BATCH_FLUSH_INTERVAL_MS`), and (2) the cross-shard load sync
> feature (`RANVIER_CROSS_SHARD_LOAD_SYNC`). The flush interval controls how often
> locally-learned routes propagate cross-shard. The cross-shard sync broadcasts
> per-shard `active_requests` to give routing decisions a global view of backend load.
>
> The cross-shard sync feature (575dd78) was found to cause a 3x tokenization
> regression (12ms → 40ms) due to SMP reactor congestion. The fix branch reduces
> this to 2x (20ms) when enabled and disables the feature by default. See backlog 20.11.

#### Flush Interval Sweep (13B 20u 10m, sync disabled)

Testing on d1f4bd1 (575dd78 reverted) and c219fbd (fix branch, sync disabled by default):

| Flush Interval | P99 TTFT Change | Throughput | Cache Hit Rate | Tokenization | Validation | Notes |
|---------------|-----------------|------------|----------------|--------------|------------|-------|
| 2ms | +53.1% | -10.7% | 54.1% | — | FAILED | Hot-spotting |
| 5ms | +21.5% | +1.3% | 56.1% | — | FAILED | Hot-spotting |
| **10ms (default)** | **-29.5%** | +0.8% | 45.5% | 11.13ms | **PASSED** | Reliable |
| 20ms (run 1) | **-41.8%** | -4.4% | 61.7% | 8.56ms | PASSED | Best single run |
| 20ms (run 2) | +31.3% | -8.7% | 54.4% | 11.33ms | FAILED | High variance |
| 50ms | **-30.5%** | +5.3% | 51.0% | 11.50ms | **PASSED** | No benefit over 10ms |

Sustained load at 2ms (13B 30u 30m): P99 +131.7%, throughput -20.5% — catastrophic hot-spotting
confirmed. Tighter flush intervals concentrate load on fewer backends.

**Flush interval conclusions:**
- **<10ms**: Consistently causes hot-spotting. Route decisions cluster on the same backends.
- **10ms (default)**: Reliable. Passes validation consistently.
- **20ms**: High run-to-run variance on this instance. Best individual result (-41.8%) but
  also produced the worst (+31.3%). Not recommended as default.
- **50ms**: Comparable to 10ms with no measurable benefit. Delays route propagation without gain.
- **10ms confirmed as the correct default.**

#### Cross-Shard Load Sync Evaluation (13B 20u 10m, c219fbd)

| Config | P99 TTFT Change | Throughput | Cache Hit Rate | Tokenization | Validation |
|--------|-----------------|------------|----------------|--------------|------------|
| Sync disabled, 10ms flush | -37.3% | +10.1% | 80.9% | 12.64ms | PASSED |
| Sync disabled, 20ms flush (run 1) | -37.3% | +10.1% | 80.9% | 12.64ms | PASSED |
| Sync disabled, 20ms flush (run 2) | +31.3% | -8.7% | 54.4% | 11.33ms | FAILED |
| **Sync enabled, 10ms flush** | **-52.2%** | +11.2% | **86.8%** | 20.17ms | **PASSED** |
| Sync enabled, 20ms flush | +6.1% | +12.4% | 24.6% | 14.15ms | FAILED |

**Cross-shard sync conclusions:**
- **Sync enabled @ 10ms flush** delivers the best cache affinity (86.8%) and P99 (-52.2%),
  but with 2x tokenization overhead (20ms vs ~11ms). The cache improvement more than
  compensates — an extra 9ms is negligible in a 2-4 second inference cycle.
- **Sync enabled @ 20ms flush** catastrophically destroys cache affinity (24.6%). The load
  sync interval is tied to the flush interval — at 20ms, stale load information causes the
  load-balancing signal to override prefix affinity with outdated data.
- **Sync disabled** shows high run-to-run variance (45-81% cache hits). The 80.9% result
  appears to be an outlier; most sync-disabled runs cluster around 45-62%.
- **The cross-shard sync feature needs its interval decoupled from the flush interval** to
  allow independent tuning. The route batching benefits from 10ms, but the load sync may
  need a separate (potentially faster or slower) cadence.
- **Feature disabled by default** until the interval coupling is resolved and the 2x
  tokenization overhead is further reduced.

#### Run-to-Run Variance on Instance 6

Identical config (sync disabled, 20ms flush, c219fbd) produced vastly different results
back-to-back: cache hits 80.9% vs 54.4%, P99 -37.3% vs +31.3%. The baseline P99 itself
shifted from 5,900ms to 6,700ms between runs, suggesting instance-level instability
(noisy neighbors, thermal throttling, GPU memory pressure).

**Implication:** Single 10-minute runs on Lambda Labs instances have a wide confidence
interval. The most reliable conclusions are directional (e.g., 2ms causes hot-spotting,
sync @ 20ms breaks affinity) rather than precise numbers.

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

**Instance 9 runs (Apr 12-20, 2026 — boundary detection + routing fixes, 16f3454):**

| Model | Users | Duration | P99 TTFT Change | Throughput | Cache Hit Rate | Incomplete | Instance |
|-------|-------|----------|-----------------|------------|----------------|------------|----------|
| **13B** | **30** | **30m** | **-58.8%** | **+23.1%** | 63.2% | **0%** | 9 |
| **13B** | **20** | **30m** | **-36.7%** | +2.9% | 63.4% | 5.4% | 9 |
| **13B** | **10** | **30m** | +88.9%† | +2.5% | 53.6% | **0%** | 9 |

†10-user P99 is misleading: round-robin baseline dropped 412 requests as incompletes (6.9%),
excluding the slowest tail. Prefix-aware completed every request.

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

> **Note:** Items marked with ⚠ are based on bb20555/c219fbd data (invalid baselines) and need
> re-validation. Items without ⚠ are from Instance 3 (valid) or are cache-hit-rate observations
> that are unaffected by the regression.

1. **Cache hit rate always improves**: 12% → 45-98% depending on config, instance, and sync mode
2. **Per-bucket improvement is the real metric**: Overall TTFT can be misleading due to small prefix overhead
3. **P99 tail latency is the strongest win for 13B**: -79% to -85% (Instance 3, valid). ⚠ bb20555 showed -24% to -51% but against inflated baselines — re-run needed.
4. **Throughput improves for 13B**: +14% to +22% (Instance 3, valid). ⚠ bb20555 showed +3% to +14% — re-run needed.
5. **Results are instance/architecture dependent**: Cache hit rates, P99 improvements, and hot-spotting behavior vary significantly across Lambda Labs instances — single 10m runs have wide confidence intervals
6. **Default prefix ratio (0.9) is the reliable operating point**: ⚠ Hot-spotting at lower ratios on bb20555 needs re-validation with correct baselines
7. **8B is routing-neutral**: Inference too fast for cache savings to matter in aggregate TTFT
8. **70B benefit is per-request cache efficiency**: XLarge requests 44-49% faster on cache hit (Instance 3, valid). Incompletes eliminated.
9. ⚠ **Flush interval 10ms may still be optimal**: Directionally sound (tighter = hot-spotting) but exact numbers from c219fbd are invalid — re-run needed
10. ⚠ **Cross-shard load sync results need re-validation**: The 86.8% cache hits / -52.2% P99 result was measured against inflated baselines. The 2x tokenization overhead is real, but the net benefit needs re-measurement

### Value Proposition

For workloads with **large shared prefixes** (RAG, system prompts, few-shot):

#### Performance by Model Size and Load

~~**bb20555 (Feb 21, Instance 5 — TTFT/P99/throughput INVALID due to c70f0c1 regression):**~~

| Model | Load | Users | Duration | Cache Hit Rate | ~~P99 TTFT Change~~ | ~~Throughput~~ | Notes |
|-------|------|-------|----------|----------------|-----------------|------------|-------|
| **CodeLlama-13b** | **Heavy** | **30** | **30m** | 53.6% | ~~-51.3%~~ | ~~+13.9%~~ | **Re-run needed** |
| **CodeLlama-13b** | **Moderate** | **20** | **10m** | 87-89% | ~~-24% to -48%~~ | ~~+3% to +13%~~ | **Re-run needed** |
| **CodeLlama-13b** | **Normal** | **10** | **10m** | 87.7% | ~~-26.8%~~ | ~~flat~~ | **Re-run needed** |
| **Llama-3.1-8B** | **Heavy** | **30** | **30m** | 64.9% | ~~flat~~ | ~~-4.6%~~ | **Re-run needed** |
| **Llama-3.1-8B** | **Moderate** | **20** | **10m** | 94.6% | ~~flat~~ | ~~-2.8%~~ | **Re-run needed** |
| **Llama-3.1-70B** | **Moderate** | **16** | **15m** | 75.4% | ~~+2.6%~~ | ~~-17.7%~~ | **Re-run needed** |

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

**Key takeaways (valid data only — Instance 3 and earlier):**
- **P99 tail latency** is the strongest win for 13B — -79% to -85% (Instance 3, valid)
- **Throughput increases** for 13B — +14% to +22% (Instance 3, valid)
- **Cache hit rates** are always much higher than round-robin — 96-98% vs 11-13% baseline (valid across all instances)
- **8B is routing-neutral** — inference too fast (~420ms) for cache savings to matter in aggregate
- **70B benefit is per-request cache efficiency** — XLarge 44-49% faster on hit, incompletes eliminated
- **Backend count matters for 70B** — TP=4 (2 backends, 40GB) limits routing surface
- **0% incomplete rate** on all post-stale-connection-fix runs (Feb 9, 2026 onwards)

**Pending re-validation (bb20555/c219fbd data invalidated by c70f0c1 regression):**
- ⚠ bb20555 P99/throughput numbers (-24% to -52% P99, +3-14% throughput) need re-run
- ⚠ Cross-shard sync (86.8% cache hits, -52.2% P99) needs re-testing with valid baselines
- ⚠ Flush interval 10ms conclusion is directionally sound but needs re-measurement
- ⚠ Hot-spotting at low prefix ratios and client-tokenize needs re-validation

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

#### Impact of Load Imbalance Factor

The `RANVIER_LOAD_IMBALANCE_FACTOR` controls how aggressively the BOUNDED_LOAD strategy
diverts requests away from the cache-optimal backend. Higher values allow more imbalance
(preserving cache affinity), lower values spread load more evenly (sacrificing cache hits).

**factor=1.25 vs factor=2.0 (default) — 74055a1, March 1 2026:**

| Config | Metric | factor=2.0 | factor=1.25 | Winner |
|--------|--------|-----------|-------------|--------|
| **13B 10u 10m** | Cache Hits | 96.8% | 81.3% | 2.0 |
| | P99 TTFT Change | **-79.1%** | -42.5% | 2.0 |
| | Throughput | **+14.6%** | +6.0% | 2.0 |
| **13B 20u 10m** | Cache Hits | 81.2% | 61.8% | 2.0 |
| | P99 TTFT Change | **-79.6%** | +2.9% | 2.0 |
| | Throughput | **+22.1%** | -5.7% | 2.0 |
| **13B 30u 30m** | Cache Hits | 64.0% | 55.7% | 2.0 |
| | P99 TTFT Change | +84.6% | +127.3% | 2.0 |
| | Hit P99 | **1,237ms** | 4,301ms | 2.0 |
| | Throughput | **+3.7%** | -1.9% | 2.0 |
| **8B 20u 10m** | Cache Hits | 98.1% | 80.2% | 2.0 |
| | XLarge Impr. | **31.6%** | 1.8% | 2.0 |
| **8B 30u 30m** | Cache Hits | 98.0% | 47.5% | 2.0 |
| | XLarge Impr. | **40.5%** | -0.8% | 2.0 |
| | Throughput | -1.1% | -7.4% | 2.0 |

*factor=2.0 data from Instance 3 (Feb 10-14) and b63c165/d6b97a6 (Feb 27-28).
factor=1.25 data from 74055a1 (March 1, 2026). Same hardware (8xA100 40GB), vLLM v0.15.1.
Note: 13B 30u 30m factor=2.0 row uses d6b97a6 data (miss-tail blowout). Instance 8 (08e5a93)
shows P99 **-79.6%** at factor=2.0 for the same config — the blowout was transient.*

**Key findings:**
- **factor=2.0 wins every comparison.** Factor=1.25 reduces cache affinity by 15-50pp across
  all configs, eliminating the P99 and throughput benefits that make prefix routing worthwhile.
- **At 13B 20u (the primary comparison point):** factor=1.25 turns a -79.6% P99 improvement
  into +2.9% (effectively no benefit). Cache hits drop from 81% to 62%.
- **At low concurrency (10u):** factor=1.25 still shows positive results (P99 -42.5%), but
  roughly half the benefit of factor=2.0 (-79.1%). Even with minimal contention, the
  aggressive diversion hurts.
- **At high concurrency (30u):** factor=1.25 makes things worse than factor=2.0 in every
  dimension — including the cache-hit path (Hit P99 4,301ms vs 1,237ms) that factor=2.0
  preserves. Spreading load doesn't help when it destroys the cache affinity that makes
  prefix routing effective.
- **8B XLarge improvement collapses:** 40.5% → -0.8% at 30u. The per-request cache benefit
  that 8B shows at factor=2.0 is entirely eliminated.

**Why lower factors don't help the miss tail:** The 30u miss-tail problem (P99 9,477ms at
factor=2.0) is caused by cache misses queuing behind cache hits on popular-prefix backends.
Factor=1.25 reduces the number of cache hits on those backends, but also reduces hits
everywhere else — the misses don't get faster (Miss P99 12,796ms at 1.25 vs 9,477ms at 2.0),
they get worse because the overall system loses cache efficiency. The fix requires
hit/miss-aware load weighting, not a blunter imbalance factor.

**Recommendation:** Keep `RANVIER_LOAD_IMBALANCE_FACTOR=2.0` (default). Do not reduce below
1.5 — the cache affinity loss outweighs any load distribution benefit.

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

### Benchmark Validity Status

Five benchmark rounds have been completed. Rounds 3-4 contain invalid latency data:

1. **Pre-fix** (Jan-Feb 2026, Instance 1-2) — **VALID** (pre-regression). Baseline with stale connection issues (30-37% incomplete). TTFT/routing metrics valid, throughput understated by incompletes.
2. **Post-fix** (Feb 10-14, Instance 3) — **VALID** (pre-regression). Stale connection retry fixed, per-request SMP broadcast. This is the historical reference dataset.
3. **bb20555** (Feb 21, Instance 5) — **TTFT/P99/THROUGHPUT INVALID** (c70f0c1 regression present). Cache hit rates and routing behavior (hot-spotting patterns) are directionally valid.
4. **c219fbd** (Feb 22, Instance 6) — **TTFT/P99/THROUGHPUT INVALID** (c70f0c1 regression present). Flush interval sweep and cross-shard sync conclusions need re-validation.
5. **Instance 9** (Apr 12-20, 2026, commits 8be20f2→16f3454) — **VALID**. Boundary detection and routing fixes. 13B 30u 30m: P99 -58.8%, throughput +23.1%, cache 63.2%. This is the current reference for the batched route learning architecture.

**What was invalidated:** The c70f0c1 commit changed `stream_backend_response()` to flush SSE
only on first/last chunk instead of every chunk. Without per-chunk flush, intermediate tokens
sat in Seastar's output_stream buffer until buffer fill or stream end, inflating P50 latency
by ~3.3x. This made round-robin baselines artificially slow, causing prefix-aware improvements
to appear smaller than they are. The fix (08ba984) restored per-chunk flush.

**Fix validation (same instance, before/after 08ba984):**

| Metric | Before Fix | After Fix | Improvement |
|--------|-----------|-----------|-------------|
| Baseline P50 | 2,900ms | 890ms | 3.3x faster |
| Baseline P99 | 6,000ms | 4,300ms | 1.4x faster |
| Prefix P50 | 2,700ms | 740ms | 3.6x faster |
| Prefix P99 | 9,700ms | 1,200ms | 8.1x faster |

**What remains valid from rounds 3-4:**
- Cache hit rates (87-89% for 13B at 0.9 ratio, 53.6% at 30u 30m, etc.)
- Hot-spotting at low prefix ratios (directionally — hot-spotting may differ with correct baselines)
- 0% incomplete rate on all runs
- Tokenization overhead measurements (11-20ms depending on sync config)
- Cross-shard sync's effect on cache affinity (86.8% vs ~45-62% without sync)

---

### Post-Fix Re-Run Plan (Feb 26, 2026)

The c70f0c1 SSE flush regression has been fixed in 08ba984. All TTFT/P99/throughput benchmarks
from Instance 5 (bb20555) and Instance 6 (c219fbd) need to be re-run. The re-runs should be
on commit 08ba984 or later (HEAD of main), which includes all architecture changes (speculative
load increment, batched route learning, configurable flush interval, cross-shard sync) plus
the SSE flush fix.

#### Priority 1: Core Comparison Points (~2.5 hours)

These establish the new valid baseline for the current architecture with the fix applied.

| # | Config | Command | Why |
|---|--------|---------|-----|
| 1 | **13B 20u 10m** | `./scripts/bench.sh --compare --model meta-llama/CodeLlama-13b-Instruct-hf --warmup --duration 10m --users 20 --max-model-len 8192` | Primary comparison point — most historical data. Run 2x for variance. |
| 2 | **13B 10u 10m** | `./scripts/bench.sh --compare --model meta-llama/CodeLlama-13b-Instruct-hf --warmup --duration 10m --users 10 --max-model-len 8192` | Low-concurrency reference. |
| 3 | **13B 30u 30m** | `./scripts/bench.sh --compare --model meta-llama/CodeLlama-13b-Instruct-hf --warmup --duration 30m --users 30 --max-model-len 8192` | Sustained load — the strongest previous result (-51.3% P99 was invalid). |
| 4 | **8B 20u 10m** | `./scripts/bench.sh --compare --model meta-llama/Llama-3.1-8B-Instruct --warmup --duration 10m --users 20` | Confirm 8B is routing-neutral with correct baselines. |
| 5 | **8B 30u 30m** | `./scripts/bench.sh --compare --model meta-llama/Llama-3.1-8B-Instruct --warmup --duration 30m --users 30` | Sustained load reference for 8B. |

**Expected outcomes with fix:**
- 13B P99 improvements should be **stronger** than bb20555 numbers (-24% to -51%) because the
  round-robin baseline will no longer be inflated. With correct baselines, we expect results
  closer to Instance 3 (-79% to -85%), though batched route learning may reduce cache affinity.
- 8B should remain routing-neutral — the model is too fast for cache savings to matter in aggregate.
- Cache hit rates should be comparable to bb20555 (87-89% for 20u, 53-65% for 30u 30m) since
  the fix only affects SSE flushing, not routing decisions.

#### Priority 2: Flush Interval Re-Validation (~2 hours)

The flush interval sweep on c219fbd showed 10ms as optimal, but with inflated baselines.
The hot-spotting at 2ms/5ms may have been exaggerated or understated.

| # | Config | Command | Why |
|---|--------|---------|-----|
| 6 | **13B 20u 10m, 2ms flush** | `RANVIER_ROUTE_BATCH_FLUSH_INTERVAL_MS=2 ./scripts/bench.sh --compare --model meta-llama/CodeLlama-13b-Instruct-hf --warmup --duration 10m --users 20 --max-model-len 8192` | Confirm 2ms still causes hot-spotting. |
| 7 | **13B 20u 10m, 5ms flush** | `RANVIER_ROUTE_BATCH_FLUSH_INTERVAL_MS=5 ./scripts/bench.sh --compare --model meta-llama/CodeLlama-13b-Instruct-hf --warmup --duration 10m --users 20 --max-model-len 8192` | Boundary between hot-spotting and stability. |
| 8 | **13B 20u 10m, 20ms flush** | `RANVIER_ROUTE_BATCH_FLUSH_INTERVAL_MS=20 ./scripts/bench.sh --compare --model meta-llama/CodeLlama-13b-Instruct-hf --warmup --duration 10m --users 20 --max-model-len 8192` | High-variance result on c219fbd — run 2x. |
| 9 | **13B 20u 10m, 50ms flush** | `RANVIER_ROUTE_BATCH_FLUSH_INTERVAL_MS=50 ./scripts/bench.sh --compare --model meta-llama/CodeLlama-13b-Instruct-hf --warmup --duration 10m --users 20 --max-model-len 8192` | Confirm no benefit over 10ms. |

**Key question:** Does 10ms remain the optimal flush interval with correct baselines?
The previous conclusion was sound directionally (tighter = hot-spotting), but the exact
crossover point and magnitude of improvements need re-measurement.

#### Priority 3: Cross-Shard Sync Re-Evaluation (~1.5 hours)

The cross-shard sync result (86.8% cache hits, -52.2% P99 at 10ms flush with sync enabled)
was the strongest individual result but measured against the inflated baseline. Re-testing
will reveal whether sync genuinely provides better cache affinity or if the result was an
artifact of the regression.

| # | Config | Command | Why |
|---|--------|---------|-----|
| 10 | **13B 20u 10m, sync OFF, 10ms** | `RANVIER_CROSS_SHARD_LOAD_SYNC=false ./scripts/bench.sh --compare --model meta-llama/CodeLlama-13b-Instruct-hf --warmup --duration 10m --users 20 --max-model-len 8192` | Baseline without sync (compare to run #1). |
| 11 | **13B 20u 10m, sync ON, 10ms** | `RANVIER_CROSS_SHARD_LOAD_SYNC=true ./scripts/bench.sh --compare --model meta-llama/CodeLlama-13b-Instruct-hf --warmup --duration 10m --users 20 --max-model-len 8192` | Re-test the 86.8% / -52.2% result. |
| 12 | **13B 20u 10m, sync ON, 20ms** | `RANVIER_CROSS_SHARD_LOAD_SYNC=true RANVIER_ROUTE_BATCH_FLUSH_INTERVAL_MS=20 ./scripts/bench.sh --compare --model meta-llama/CodeLlama-13b-Instruct-hf --warmup --duration 10m --users 20 --max-model-len 8192` | Re-test the 24.6% cache catastrophe. |

**Key question:** Does cross-shard sync still push cache hits to 87%+ and P99 to -52%?
The 2x tokenization overhead (20ms vs 11ms) is a real cost — need to confirm the cache
improvement compensates. The 24.6% cache hit result at 20ms flush also needs re-testing
to confirm the sync/flush interval coupling problem is real.

#### Priority 4: Variant Configurations (~2 hours)

Lower priority — these can be deferred if instance time is limited.

| # | Config | Command | Why |
|---|--------|---------|-----|
| 13 | **13B 20u 10m, ratio 0.7** | `./scripts/bench.sh --compare --model meta-llama/CodeLlama-13b-Instruct-hf --warmup --duration 10m --users 20 --prefix-ratio 0.7 --max-model-len 8192` | Re-test hot-spotting at 0.7 ratio. |
| 14 | **13B 20u 10m, ratio 0.5** | `./scripts/bench.sh --compare --model meta-llama/CodeLlama-13b-Instruct-hf --warmup --duration 10m --users 20 --prefix-ratio 0.5 --max-model-len 8192` | Re-test hot-spotting at 0.5 ratio. |
| 15 | **13B 30u 10m, client-tok** | `./scripts/bench.sh --compare --client-tokenize --model meta-llama/CodeLlama-13b-Instruct-hf --warmup --duration 10m --users 30 --max-model-len 8192` | Re-test client-tokenize hot-spotting. |
| 16 | **8B 64u 15m stress** | `./scripts/bench.sh --model meta-llama/Llama-3.1-8B-Instruct --warmup --duration 15m --users 64 --prompt-dist stress` | Re-test high-concurrency stability. |

**Total estimated time: ~8 hours** (including vLLM startup, warmup, and cooldown between runs).

#### How to Run

```bash
# On Lambda Labs 8xA100 instance, after setup:
git checkout main && git pull  # Ensure commit 08ba984+ is present

# Option A: Use bench-runner.sh (if updated with post-fix suite)
./scripts/bench-runner.sh --suite high  # Priority 1-2

# Option B: Run manually in order
# Priority 1 first, then 2, etc. Allow 2-3 min GPU cooldown between runs.
```

#### Recording Results

As results come in, update the tables below. Use this template for each run:

```markdown
| Config | P99 TTFT Change | Throughput | Cache Hit Rate | Tokenization | Validation | Notes |
|--------|-----------------|------------|----------------|--------------|------------|-------|
| 13B 20u 10m (run 1) | TBD | TBD | TBD | TBD | TBD | Post-fix, 08ba984 |
```

#### Post-Fix Results (to be filled in as runs complete)

**Quick Validation (13B 20u 1m, b63c165, Feb 27 — Instance 7):**

| Metric | Round-Robin | Prefix-Aware | Change | Notes |
|--------|-------------|--------------|--------|-------|
| Cache Hit Rate | 11.0% | 85.2% | **+74.2%** | Consistent with bb20555 (87-89%) |
| P99 TTFT | 3,700ms | 1,200ms | **-67.6%** | Much stronger than invalidated -24% to -48% |
| P50 TTFT | 800ms | 770ms | -3.8% | |
| Throughput (req/s) | 23.9 | 26.9 | **+12.6%** | Consistent with previous data |
| Incompletes | 0 | 0 | 0% | Clean |
| Errors | 0 | 0 | 0% | Clean |
| XLarge Hit P50 | 1,319ms | 846ms | -35.8% | Cache hits much faster |
| XLarge Miss P50 | 1,142ms | 771ms | -32.5% | Misses also faster (fewer in queue) |
| Tokenization P50 | — | 8.14ms | — | Consistent with previous measurements |
| Validation | PASSED | PASSED | | |

> **Key finding:** The 1-minute quick validation showed P99 -67.6%, but the 10-minute run
> converged to **-79.6%** — matching Instance 3's -78.7% almost exactly. Throughput +22.1%
> is the best recorded, exceeding Instance 3 (+13.7%). The architecture is validated:
> batched route learning trades cache affinity (81% vs 98%) for better baseline performance,
> with equivalent or better net P99 improvement.

**Pre-regression baseline comparison (3218554 vs b63c165, same instance):**

| Metric | 3218554 (old, pre-regression) | b63c165 (main, post-fix) | Notes |
|--------|-------------------------------|--------------------------|-------|
| P99 TTFT change | -88.3% | **-79.6%** | Main's RR baseline is healthier |
| P50 TTFT change | -14.4% | -16.4% | ~same |
| Throughput change | +20.9% | **+22.1%** | Main slightly better |
| Cache Hit Rate | 97.4% | 81.2% | Batched routes vs per-request SMP |
| Prefix P99 | 890ms | 960ms | ~same absolute performance |
| RR P99 | 7,600ms | 4,700ms | Main's RR 38% faster (less SMP overhead) |
| Tokenization P50 | 8.0ms | 16.4ms | Main higher — needs investigation |
| RR Validation | FAILED | PASSED | Main's RR is healthier |

> **No hidden regressions in the prefix path.** The prefix-aware path performs identically
> on both commits (P99 ~890-960ms on clean runs). The difference in relative improvement
> (-88% vs -80%) comes from the round-robin denominator: main's reduced SMP overhead makes
> RR less bad (4,700ms vs 7,600ms P99).
>
> **However, run-to-run variance is a real issue on main.** Back-to-back 10m runs produced
> P99 -79.6% (run 1a) and +34.3% (run 1b) — hot-spotting on the second run caused prefix
> P99 to spike to 4,700ms while the RR baseline was actually healthier (3,500ms). This
> matches the c219fbd pattern. The batched route learning 10ms flush interval creates
> windows where cross-shard routing decisions diverge under load, occasionally concentrating
> traffic on fewer backends. 3218554 (per-request SMP) showed no such variance.
>
> **Tokenization overhead was 2x higher on Instance 7** (~16.5ms vs 8.0ms on 3218554).
> This was a real code issue, investigated and mitigated between b63c165 and 08e5a93.
> Instance 8 runs confirm fix: all tokenization P50 measurements back below 10ms.

**Priority 1 — Core runs (commit 08ba984+):**

| # | Config | P99 TTFT Change | Throughput | Cache Hit Rate | Validation | Notes |
|---|--------|-----------------|------------|----------------|------------|-------|
| 1a | 13B 20u 10m (run 1) | **-79.6%** | **+22.1%** | 81.2% | PASSED | b63c165, Instance 7, clean run |
| 1b | 13B 20u 10m (run 2) | +34.3% | -2.2% | 78.6% | PASSED | b63c165, Instance 7, **hot-spotting** — prefix P99 4,700ms |
| 1c | 13B 20u 10m (run 3) | +155.6% | -10.3% | 87.2% | FAILED | 08e5a93, Instance 8, **hot-spotting** — transient |
| 1d | 13B 20u 10m (run 4) | **-66.7%** | +4.2% | 80.5% | PASSED | 08e5a93, Instance 8, clean re-run |
| 2 | 13B 10u 10m | **-60.4%** | **+12.0%** | 82.6% | PASSED | 08e5a93, Instance 8 |
| 3a | 13B 30u 30m | +84.6% | +3.7% | 64.0% | FAILED | d6b97a6, Instance 7, **miss tail blowout** |
| 3b | 13B 30u 30m (re-run) | **-79.6%** | **+13.2%** | 58.1% | PASSED | 08e5a93, Instance 8, clean |
| 4 | 8B 20u 10m | +4.3% | -5.2% | 97.8% | PASSED | 08e5a93, Instance 8, routing-neutral |
| 5 | 8B 30u 30m | +8.3% | -1.5% | 68.3% | PASSED | 08e5a93, Instance 8, routing-neutral |

> **Instance 8 full suite (March 5, 2026, commit 08e5a93):** All 5 Priority 1 configs plus
> Priority 4 variants completed in a single session on 8xA100 40GB. Key findings:
>
> **Run-to-run variance is the primary concern.** The 13B 20u 10m hot-spotting seen on Instance 7
> (run 1b) reproduced once (run 1c, P99 +155.6%) but the immediate re-run (run 1d) came back
> clean at P99 -66.7%, confirming the hot-spotting is transient and non-systematic. 2 of 4 runs
> at this config showed hot-spotting across two instances.
>
> **30u miss-tail blowout is resolved.** The d6b97a6 result (#3a, P99 +84.6%) is not reproduced
> on 08e5a93 — run 3b shows P99 -79.6% with +13.2% throughput, matching Instance 3's validated
> -85.3%. Cache hit rate is lower (58.1% vs 97.5% on Instance 3 with per-request SMP), but
> the miss tail no longer explodes. This suggests 08e5a93 includes improvements that prevent
> miss-tail amplification at 30 users.
>
> **8B confirms routing-neutral.** Both 20u and 30u show flat P99 and throughput with 68-98%
> cache hits. The model's inference is too fast (~1400ms) for cache savings to affect aggregates.
>
> **Comparison to validated runs:**
> - **20u 10m (b63c165 run 1a):** 81.2% hits, P99 -79.6% — Instance 7
> - **20u 10m (08e5a93 run 1d):** 80.5% hits, P99 -66.7% — Instance 8 (clean re-run)
> - **20u 30m (b63c165):** 73.9% hits, P99 980ms (**-78.2%**) — Instance 7
> - **30u 30m (Instance 3):** 97.5% hits, P99 1,000ms (**-85.3%**) — per-request SMP
> - **30u 30m (08e5a93):** 58.1% hits, P99 940ms (**-79.6%**) — Instance 8, batched routes
>
> **Run #3a analysis (13B 30u 30m, d6b97a6 — Instance 7, now superseded by 3b):** Cache affinity
> dropped to 64% at 30u, meaning 36% of requests were cache misses landing on hot backends.
> Cache-hit path remained excellent (Hit P99 1,237ms, 68% better than RR), but cache-miss tail
> exploded (Miss P99 9,477ms vs RR 3,886ms). The BOUNDED_LOAD strategy treats all in-flight
> requests equally, but a cache miss on a hot backend waits much longer than a cache hit.
> This failure mode did not reproduce on 08e5a93 (Instance 8).

**Priority 2 — Flush interval sweep (13B 20u 10m, sync disabled):**

| # | Flush Interval | P99 TTFT Change | Throughput | Cache Hit Rate | Validation | Notes |
|---|---------------|-----------------|------------|----------------|------------|-------|
| 6 | 2ms | **-63.1%** | -3.4% | 79.9% | PASSED | 08e5a93, Instance 8; P50 +8.5% (tokenization 7.67ms) |
| 7 | 5ms | **-70.9%** | +0.9% | 77.5% | PASSED | 08e5a93, Instance 8; P50 +2.7% (tokenization 5.96ms) |
| 8a | 20ms (run 1) | **-76.9%** | +11.5% | 67.8% | PASSED | 08e5a93, Instance 8; P50 **-6.6%** (tokenization 5.95ms) |
| 8b | 20ms (run 2) | **-86.3%** | +17.5% | 69.4% | PASSED | 1749f05, Instance 9 + NUMA; P50 **-5.0%** (tokenization 5.82ms); **best result ever** |
| 8c | 20ms (run 3) | **-78.6%** | +6.1% | 85.4% | PASSED | 1749f05, Instance 9 + NUMA; P50 +4.2% (tokenization 6.04ms) |
| 9 | 50ms | **-69.7%** | +1.9% | 61.2% | PASSED | 08e5a93, Instance 8; P50 +2.9% (tokenization 6.74ms); cache hit P50 -10.2% |
| 10ms+NUMA | 10ms (NUMA run 1) | +462.5% | -24.9% | 94.3% | FAILED | 1749f05, Instance 9 + NUMA; **hot-spotting**, XLarge Hit P99 20.5s, 43 unique prefixes |
| 10ms+NUMA | 10ms (NUMA run 2) | **-71.1%** | +6.3% | 76.9% | PASSED | 1749f05, Instance 9 + NUMA; P50 +4.1% (tokenization 7.16ms); hot-spotting not reproduced |

> **Priority 2 sweep analysis (2ms–50ms flush, #6-9, plus NUMA re-runs):**
>
> | Flush | P99 | P50 | Throughput | Cache | Tokenization | Instance |
> |-------|-----|-----|------------|-------|--------------|----------|
> | 2ms (#6) | -63.1% | +8.5% | -3.4% | 79.9% | 7.67ms | 8 |
> | 5ms (#7) | -70.9% | +2.7% | +0.9% | 77.5% | 5.96ms | 8 |
> | 10ms (1d) | -66.7% | — | +4.2% | 80.5% | — | 8 |
> | 10ms+NUMA (run 1) | **+462.5%** | +14.7% | -24.9% | 94.3% | 7.89ms | 9 NUMA **FAILED** |
> | 10ms+NUMA (run 2) | -71.1% | +4.1% | +6.3% | 76.9% | 7.16ms | 9 NUMA |
> | 20ms (#8a) | -76.9% | -6.6% | +11.5% | 67.8% | 5.95ms | 8 |
> | **20ms (#8b)** | **-86.3%** | **-5.0%** | **+17.5%** | 69.4% | 5.82ms | **9 NUMA** |
> | 20ms (#8c) | -78.6% | +4.2% | +6.1% | 85.4% | 6.04ms | 9 NUMA |
> | 50ms (#9) | -69.7% | +2.9% | +1.9% | 61.2% | 6.74ms | 8 |
>
> **Conclusion: 20ms confirmed as optimal flush interval (3 runs, 2 instances, 0 failures).**
>
> Three 20ms runs across two instances show consistent P99 improvement (-77% to -86%) with
> run-to-run variance in P50 and throughput:
>
> | Run | P99 | P50 | Throughput | Cache |
> |-----|-----|-----|------------|-------|
> | 8a (Inst 8) | -76.9% | -6.6% | +11.5% | 67.8% |
> | 8b (Inst 9 NUMA) | -86.3% | -5.0% | +17.5% | 69.4% |
> | 8c (Inst 9 NUMA) | -78.6% | +4.2% | +6.1% | 85.4% |
>
> Run 8c shows higher cache (85.4%) but with P50 regression (+4.2%), while 8a/8b had lower
> cache (68-69%) with P50 improvement. This suggests variance in workload prefix distribution
> across runs rather than a systematic difference. P99 is consistently strong across all three.
>
> **10ms + NUMA hot-spotting is transient, not systematic.** The first 10ms NUMA run failed
> catastrophically (P99 +462.5%, 18s) with 94.3% cache hits concentrated on 43 prefixes. The
> re-run passed cleanly: P99 -71.1%, 76.9% cache, 100 unique prefixes. This matches the
> transient hot-spotting pattern seen on Instance 8 (runs 1b/1c vs 1d) — a race condition in
> early route learning, not a NUMA-specific failure mode.
>
> However, 10ms remains inferior to 20ms even when it passes: P99 -71.1% vs -78% to -86%,
> and is susceptible to transient hot-spotting (1-in-2 NUMA, 2-in-4 Instance 8).
>
> **Sweep curve (final):**
> - **2-5ms:** High cache affinity (77-80%) but P50 regresses (+2.7% to +8.5%).
> - **10ms:** P99 -67% to -71% when stable, but susceptible to transient hot-spotting.
> - **20ms (recommended default):** P99 -77% to -86% (best), throughput +6% to +17%,
>   zero hot-spotting failures across 3 runs on 2 instances.
> - **50ms:** Diminishing returns — stale load signals, P99 reverts to -70%.
>
> **Recommendation:** Change default `RANVIER_ROUTE_BATCH_FLUSH_INTERVAL_MS` from 10 to 20.

**Priority 3 — Cross-shard sync (13B 20u 10m):**

| # | Config | P99 TTFT Change | Throughput | Cache Hit Rate | Tokenization | Validation | Notes |
|---|--------|-----------------|------------|----------------|--------------|------------|-------|
| 10 | Sync OFF, 10ms | — | — | — | — | — | Pending |
| 11 | Sync ON, 10ms | — | — | — | — | — | Pending |
| 12 | Sync ON, 20ms | — | — | — | — | — | Pending |

**Priority 4 — Variant configs (08e5a93, Instance 8, March 5):**

| # | Config | P99 TTFT Change | Throughput | Cache Hit Rate | Validation | Notes |
|---|--------|-----------------|------------|----------------|------------|-------|
| 13 | 13B 20u 10m, ratio 0.7 | +22.1% | -3.8% | 71.4% | FAILED | Hot-spotting at lower prefix sharing |
| 14 | 13B 20u 10m, ratio 0.5 | +58.9% | -8.7% | 62.1% | FAILED | More unique prefixes overwhelm load balancer |
| 15 | 13B 30u 10m, client-tok | +31.2% | +1.4% | 56.8% | FAILED | Client-tok hot-spotting confirmed |
| 16 | 8B 64u 15m stress | +12.6% | -2.3% | 94.2% | PASSED | Stable — 0 errors, routing-neutral |

> **Priority 4 analysis (Instance 8):** Lower prefix ratios (0.5-0.7) confirm hot-spotting at
> 20 users with the batched route learning architecture. This matches Instance 5/6 directionally
> (those cache hit rates were valid). Client tokenization at 30u (#15) also confirms hot-spotting
> — without the ~10ms server-side tokenization stagger, cross-shard routing decisions happen
> near-simultaneously. 8B at 64u (#16) remains stable with zero errors, confirming the stress
> resilience from Instance 3.

---

## SSE Flush Regression (c70f0c1)

Commit c70f0c1 (#280, "improve http service") introduced a **3.3x P50 latency regression** by
changing the SSE flush strategy in `stream_backend_response()`. Two changes were made:

1. **SSE flush strategy (root cause):** Changed from flushing on every SSE chunk to only flushing
   on the first and last chunk. Without per-chunk flush, intermediate tokens sat in Seastar's
   `output_stream` buffer (default 8KB), never hitting the socket until buffer fill or stream
   end. This caused TTFT to appear as the time until the buffer filled rather than the time
   until the first token was generated.

2. **Split HTTP write (red herring):** `send_backend_request()` was changed to split headers and
   body into two `co_await bundle.out.write()` calls. Seastar's output_stream buffers until
   `flush()`, so these likely coalesced anyway. Reverted for safety but unlikely to be the cause.

**Fix:** 08ba984 (#321) restored per-chunk flush and merged the split writes back.

**Impact on benchmarks:** The regression inflated round-robin baselines by ~3.3x (P50: 890ms →
2,900ms). Since prefix-aware routing also uses the same SSE proxy path, its latency was also
inflated but proportionally less (P50: 740ms → 2,700ms). This made prefix-aware improvements
appear smaller than they are when measured as relative percentages against the inflated baseline.

**Commits affected:** All benchmarks on commits between c70f0c1 and 08ba984 (exclusive) have
invalid TTFT/P99/throughput numbers. This includes bb20555 (Instance 5) and c219fbd (Instance 6).

