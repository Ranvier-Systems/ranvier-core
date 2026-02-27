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

### Quick Validation: 13B 20u 1m (b63c165, Instance 7)

| Metric | Round-Robin | Prefix-Aware | Change |
|--------|-------------|--------------|--------|
| P99 TTFT | 3,700ms | 1,200ms | **-67.6%** |
| Throughput | 23.9 req/s | 26.9 req/s | **+12.6%** |
| Cache Hit Rate | 11.0% | 85.2% | +74.2% |
| Incompletes | 0 | 0 | 0% |

**Prediction confirmed:** P99 -67.6% is between Instance 3 (-79%) and the invalidated bb20555
(-48%), exactly as predicted. The gap vs Instance 3 is from batched route learning (85% vs 98%
cache hits). 1-minute run only — 10-minute runs will narrow confidence intervals.

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
