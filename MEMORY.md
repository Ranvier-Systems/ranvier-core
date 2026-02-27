# MEMORY.md — Ranvier Core

Persistent context for ongoing work across sessions.

## SSE Flush Regression (Feb 2026)

### Root Cause

Commit c70f0c1 (#280) changed `stream_backend_response()` in `http_controller.cpp` to flush
SSE chunks only on first/last chunk instead of every chunk. Without per-chunk flush, intermediate
tokens buffered in Seastar's `output_stream` (8KB default), inflating TTFT by ~3.3x.

**Fix:** 08ba984 (#321) — restored per-chunk flush. Merged to main Feb 25, 2026.

### Fix Validation (same Lambda Labs instance)

| Metric | Before Fix | After Fix |
|--------|-----------|-----------|
| Baseline P50 | 2,900ms | 890ms |
| Baseline P99 | 6,000ms | 4,300ms |
| Prefix P50 | 2,700ms | 740ms |
| Prefix P99 | 9,700ms | 1,200ms |

### Invalid Benchmark Data

All TTFT/P99/throughput numbers from these instances are invalid:
- **Instance 5** (bb20555, Feb 21) — full suite with speculative load + batched routes
- **Instance 6** (c219fbd, Feb 22) — flush interval sweep + cross-shard sync evaluation
- **All 667a4e3 (main) runs** — any runs on main between c70f0c1 and 08ba984

**What's still valid from those runs:**
- Cache hit rates (routing decisions unaffected by SSE flushing)
- Hot-spotting patterns (directionally valid)
- 0% incomplete rate observations
- Tokenization overhead measurements

**Valid benchmark data (pre-regression):**
- Instance 1-2 (Jan-Feb 2026, pre-c70f0c1) — had stale connection issues but TTFT valid
- Instance 3 (Feb 10-14, post stale-connection fix, 08c4915) — current reference dataset

### Re-Run Plan

Full re-run suite planned on 08ba984+ (post-fix). Priorities:

1. **Core comparison** (~2.5h): 13B 20u 10m (2x), 13B 10u 10m, 13B 30u 30m, 8B 20u 10m, 8B 30u 30m
2. **Flush interval** (~2h): 2ms, 5ms, 20ms (2x), 50ms — confirm 10ms default is still optimal
3. **Cross-shard sync** (~1.5h): sync ON/OFF at 10ms and 20ms flush — re-test the 86.8% cache hit result
4. **Variants** (~2h): prefix ratio 0.7/0.5, client-tokenize, 8B 64u stress

Total: ~8 hours on 8xA100 instance. Details in `docs/benchmarks/benchmark-guide-8xA100.md`
under "Post-Fix Re-Run Plan".

### Expected Outcomes

- 13B P99 improvements should be **stronger** than bb20555's -24% to -51% since baselines
  won't be inflated. Expect closer to Instance 3 (-79% to -85%), though batched route
  learning may reduce cache affinity vs per-request SMP broadcast.
- Cache hit rates should be similar to bb20555 (87-89% at 0.9 ratio) — routing is unaffected.
- Flush interval conclusion (10ms optimal) likely holds directionally but needs re-measurement.
- Cross-shard sync benefit (86.8% cache hits) needs re-validation — was the strongest result
  but measured against inflated baseline.

## Post-Fix Benchmark Results (Feb 27, 2026)

### 13B 20u 10m — Primary Comparison (b63c165, Instance 7)

| Metric | Round-Robin | Prefix-Aware | Change |
|--------|-------------|--------------|--------|
| P99 TTFT | 4,700ms | 960ms | **-79.6%** |
| P50 TTFT | 730ms | 610ms | **-16.4%** |
| Throughput | 30.8 req/s | 37.6 req/s | **+22.1%** |
| Cache Hit Rate | 12.4% | 81.2% | +68.8% |
| Incompletes | 0 | 0 | 0% |
| Tokenization P50 | — | 16.4ms | Higher than expected |

**Matches Instance 3** (-78.7% P99). Throughput +22.1% is the best recorded.

### 13B 20u 10m — Run 2 (b63c165, Instance 7, HOT-SPOTTING)

| Metric | Round-Robin | Prefix-Aware | Change |
|--------|-------------|--------------|--------|
| P99 TTFT | 3,500ms | 4,700ms | **+34.3% WORSE** |
| P50 TTFT | 700ms | 730ms | +4.3% |
| Throughput | 34.5 req/s | 33.8 req/s | -2.2% |
| Cache Hit Rate | 12.8% | 78.6% | +65.7% |
| Tokenization P50 | — | 16.7ms | Consistent with run 1 |

**Hot-spotting on back-to-back run.** Prefix P99 spiked to 4,700ms while RR baseline was
actually healthier than run 1a (3,500ms vs 4,700ms). Same pattern as c219fbd Instance 6.
Batched route learning creates variance windows. 3218554 (per-request SMP) had no such issue.

**Key issues identified:**
1. **Run-to-run variance**: P99 -79.6% vs +34.3% back-to-back on same instance
2. **Tokenization 2x regression**: 16.5ms on main vs 8.0ms on 3218554, consistent across runs
3. **Hot-spotting from batched routes**: 10ms flush interval allows cross-shard routing divergence

### Pre-Regression Baseline Comparison (3218554 vs b63c165, same instance)

| Metric | 3218554 (old) | b63c165 (main) |
|--------|---------------|----------------|
| P99 TTFT change | -88.3% | -79.6% |
| Throughput change | +20.9% | +22.1% |
| Cache Hit Rate | 97.4% | 81.2% |
| Prefix P99 | 890ms | 960ms |
| RR P99 | 7,600ms | 4,700ms |

**No hidden regressions.** Prefix path performs identically (~890-960ms P99). The relative
improvement difference (-88% vs -80%) comes from main's healthier round-robin baseline.
Tokenization P50 (16.4ms vs 8.0ms) worth monitoring.

### Quick Validation: 13B 20u 1m (b63c165, Instance 7)

1-minute quick run showed P99 -67.6%. Converged to -79.6% on the full 10-minute run.

## Architecture Notes

### Key Commits (Benchmark-Relevant)

| Commit | What | Benchmark Impact |
|--------|------|-----------------|
| 08ba984 | Fix SSE flush regression | Restores correct baselines |
| c70f0c1 | SSE flush regression introduced | 3.3x P50 inflation |
| bb20555 | Speculative load + batched routes | Architecture under test |
| c219fbd | Cross-shard sync fix branch | Flush interval + sync config |
| ea86bc8 | Fix cross-shard sync SMP storm | Reduced tokenization overhead |
| 575dd78 | Cross-shard speculative load sync | Feature under evaluation |
| b2f8cac | Batched route learning | Replaced per-request SMP broadcast |

### Valid Reference Data (Instance 3, Feb 10-14)

| Model | Config | P99 TTFT | Throughput | Cache Hits |
|-------|--------|----------|------------|------------|
| 13B | 10u 10m | -79.1% | +14.6% | 96.8% |
| 13B | 20u 10m | -78.7% | +13.7% | 97.6% |
| 13B | 30u 30m | -85.3% | +22.3% | 97.5% |
| 8B | 20u 10m | -13.8% | -1.2% | 98.1% |
| 8B | 30u 30m | +6.5% | -1.1% | 98.0% |
| 70B (TP=2) | 16u 30m | flat | flat | 97.8% |

These are pre-batched-route-learning (per-request SMP broadcast). The post-fix re-run will
show the batched route learning architecture's true performance.
