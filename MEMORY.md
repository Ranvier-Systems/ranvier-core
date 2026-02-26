# MEMORY.md — Benchmark State & Key Findings

Last updated: 2026-02-26

## Critical: SSE Flush Regression (c70f0c1)

**Status:** Fixed in `08ba984` (#321), merged Feb 25, 2026.

**Root cause:** `stream_backend_response()` was changed to only flush on first/last SSE chunk
instead of every chunk. Without per-chunk flush, intermediate tokens sat in Seastar's
output_stream buffer, inflating TTFT by ~3.3x.

**Second change reverted (likely red herring):** `send_backend_request()` split headers and body
into two `co_await bundle.out.write()` calls. Seastar's output_stream buffers until `flush()`,
so these likely coalesced anyway.

### What's Invalid

All benchmarks on commits after c70f0c1 (introduced after 3218554) have invalid TTFT/P99/throughput:

| Instance | Commit | Date | Invalid Metrics | Valid Metrics |
|----------|--------|------|-----------------|---------------|
| Instance 5 | bb20555 | Feb 21 | TTFT, P99, throughput | Cache hit rates |
| Instance 6 | c219fbd | Feb 22 | TTFT, P99, throughput (flush sweep, cross-shard sync) | Cache hit rates, directional hot-spotting |
| Any | 667a4e3 (main) | — | All TTFT/P99/throughput | — |

### What's Valid

| Instance | Commit | Date | Notes |
|----------|--------|------|-------|
| Instance 1-2 | 08c4915 and earlier | Jan-Feb 2026 | Pre-regression. Stale connection issue (30-37% incomplete) |
| Instance 3 | 08c4915 | Feb 10-14 | Post stale-connection fix. Per-request SMP broadcast architecture |
| Instance 4 | 08c4915 | Feb 2026 | 70B runs (80GB A100s, TP=2) |

### Fix Validation (Same Instance)

| Metric | Before Fix | After Fix | Improvement |
|--------|-----------|-----------|-------------|
| Baseline P50 | 2,900ms | 890ms | 3.3x faster |
| Baseline P99 | 6,000ms | 4,300ms | 1.4x faster |
| Prefix P50 | 2,700ms | 740ms | 3.6x faster |
| Prefix P99 | 9,700ms | 1,200ms | 8.1x faster |

---

## Validated Performance Numbers (Instance 3, pre-batched-routes)

These are the only valid TTFT/P99/throughput numbers. Uses per-request SMP broadcast
(higher cache affinity, higher SMP overhead than batched route learning).

### CodeLlama-13b (default config, prefix-ratio 0.9)

| Config | Cache Hits | P99 TTFT | Throughput | XLarge Improvement |
|--------|-----------|----------|------------|-------------------|
| 10u 10m | 96.8% | **-79.1%** | +14.6% | 39.4% |
| 20u 10m | 97.6% | **-78.7%** | +13.7% | 35.9% |
| 30u 30m | 97.5% | **-85.3%** | **+22.3%** | 32.8% |

### Llama-3.1-8B (default config, prefix-ratio 0.9)

| Config | Cache Hits | P99 TTFT | Throughput | XLarge Improvement |
|--------|-----------|----------|------------|-------------------|
| 20u 10m | 98.1% | -13.8% | -1.2% | 31.6% |
| 30u 30m | 98.0% | +6.5% | -1.1% | 40.5% |
| 64u 15m | 98.0% | — | — | 29.0% |

### Llama-3.1-70B (TP=2, 80GB, 4 backends)

| Config | Cache Hits | P99 TTFT | Throughput | XLarge Improvement |
|--------|-----------|----------|------------|-------------------|
| 16u 30m | 97.8% | flat | flat | 43.8% |
| 16u 15m | 96.9% | -33.3% | -1.4% | 48.0% |

---

## Key Architectural Findings (Validated)

1. **Cache hit rates always improve**: 12% baseline → 87-98% with prefix routing
2. **13B is the sweet spot**: P99 -79% to -85%, throughput +14% to +22%
3. **8B is routing-neutral**: Inference too fast (~410-430ms) for cache savings to show in aggregate
4. **70B benefit is per-request**: XLarge 44-49% improvement, but aggregate P99/throughput flat
5. **0% incomplete rate**: Post stale-connection fix (Feb 9)
6. **XLarge improvement** is consistent: 30-49% across all models (valid runs)

## Findings Pending Re-Validation (on 08ba984+)

These findings were from invalidated runs (bb20555/c219fbd). Cache hit rates are directionally
valid but TTFT/P99 numbers need fresh data.

1. **Batched route learning cache affinity**: bb20555 showed 87-89% cache hits (vs 97-98% on Instance 3).
   This is likely real (architectural change) but P99/throughput impact unknown
2. **Flush interval 10ms optimal**: Directional finding (2ms = hot-spotting, 50ms = no benefit) likely valid.
   Absolute P99 differences between 10ms/20ms/50ms need re-measurement
3. **Cross-shard sync**: Best result was 86.8% cache hits / -52.2% P99 at 10ms flush.
   The 86.8% cache hit rate is likely valid; the -52.2% P99 was measured against inflated baseline
4. **Hot-spotting at prefix ratio <0.9**: bb20555 showed hot-spotting at 0.7/0.5 ratios.
   Need to determine if this persists with correct baselines or was amplified by regression
5. **Client-tokenize hot-spotting**: +18.3% P99 on bb20555. May be different with valid baselines

---

## Re-Run Plan

Full re-run plan with commands is documented in:
`docs/benchmarks/benchmark-guide-8xA100.md` → [Post-Fix Re-Run Plan](#post-fix-re-run-plan-feb-2026)

### Priority Order

1. **13B 20u 10m** (x2 runs) — primary comparison, variance check
2. **13B 30u 30m** — sustained load, strongest previous result
3. **13B 10u 10m** — low concurrency baseline
4. **Flush interval sweep** (5ms, 10ms, 20ms) — confirm 10ms optimal
5. **Cross-shard sync** (enabled vs disabled @ 10ms) — determine true benefit
6. **8B 20u 10m, prefix ratio variants, client-tokenize** — if time permits

### Expected Commit for Re-Runs

All re-runs must be on **main at 08ba984 or later**. Verify with:
```bash
git log --oneline -1
# Must show 08ba984 or descendant
```

---

## Instance Log

| Instance | GPU | Date | Commit | Runs | Status |
|----------|-----|------|--------|------|--------|
| 1 | 8xA100 40GB | Jan 2026 | pre-08c4915 | Pre-fix baseline | Valid (30-37% incomplete) |
| 2 | 8xA100 40GB | Jan-Feb 2026 | pre-08c4915 | Pre-fix full suite | Valid (30-37% incomplete) |
| 3 | 8xA100 40GB | Feb 10-14 | 08c4915 | Post-fix full suite | Valid |
| 4 | 8xA100 80GB | Feb 2026 | 08c4915 | 70B runs | Valid |
| 5 | 8xA100 40GB | Feb 21 | bb20555 | Full suite + variants | INVALIDATED (SSE regression) |
| 6 | 8xA100 40GB | Feb 22 | c219fbd | Flush sweep + sync | INVALIDATED (SSE regression) |
| 7 | TBD | TBD | 08ba984+ | Post-fix re-runs | PENDING |
