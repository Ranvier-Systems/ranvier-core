# KV-Cache Compression-Aware Routing

**Status:** Proposal (Design Exploration)
**Date:** 2026-04-05
**Catalyst:** Google TurboQuant (arXiv:2504.19874, ICLR 2026)

## Context

### The KV-Cache Memory Problem

During LLM inference, the KV (key-value) cache grows linearly with sequence length and can consume more memory than model weights for long contexts. This is the same bottleneck Ranvier addresses from the routing side: Ranvier reduces GPU KV-cache **thrashing** by steering requests to backends that already hold relevant prefix data.

### What TurboQuant Does

TurboQuant is a two-stage KV-cache compression algorithm from Google Research:

1. **PolarQuant**: Random orthogonal rotation spreads each KV vector's energy uniformly, enabling near-optimal scalar quantization per coordinate.
2. **QJL Error Correction**: 1-bit residual correction using Quantized Johnson-Lindenstrauss projections eliminates quantization bias.

Key properties: zero calibration, zero training, 3-bit keys / 2-bit values, ~6x KV-cache memory reduction, up to 8x attention speedup on H100s, no measurable accuracy loss. Being presented at ICLR 2026; community implementations already exist for vLLM, llama.cpp, and PyTorch.

### Why This Matters for Ranvier

Ranvier and KV-cache compression are **complementary** — they attack the same bottleneck from opposite sides:

| | Ranvier (routing) | Compression (e.g., TurboQuant) |
|---|---|---|
| **Approach** | Route requests to where KV-cache already exists | Compress KV-cache so more fits in GPU memory |
| **Layer** | Network / L7 traffic control | GPU runtime / inference engine |
| **Effect** | Fewer cache misses | More cache capacity per backend |

The combination should be superlinear: compression expands effective capacity, and intelligent routing ensures that expanded capacity is used efficiently rather than randomly fragmented.

### Generality

Several opportunities below are useful **independent of TurboQuant** — they apply to any heterogeneous fleet where backends differ in effective KV-cache capacity (different GPU memory, different quantization settings, different eviction policies). TurboQuant is the most compelling near-term catalyst, but the design should be technology-agnostic.

## Opportunities

### P0: Compression-Aware Load Scoring ✅

**Status:** Implemented ([`538b795`](https://github.com/Ranvier-Systems/ranvier-core/commit/538b795), [`0ea30c7`](https://github.com/Ranvier-Systems/ranvier-core/commit/0ea30c7))

**Problem:** The current `VLLMMetrics::load_score()` formula (`0.7 * request_pressure + 0.3 * cache_pressure`) treats all backends identically. A `gpu_cache_usage_perc` of 0.5 means "half full" regardless of whether the backend has 24GB or 144GB of effective cache capacity (after compression).

**Solution:** Per-backend `compression_ratio` (default 1.0) adjusts cache pressure in the load score formula:

```
effective_cache_pressure = raw_cache_usage / compression_ratio
```

A TurboQuant backend at 50% raw usage with 6x compression has `effective_cache_pressure = 0.083` — it has enormous headroom. A non-TurboQuant backend at 50% has `effective_cache_pressure = 0.5`. This distinction now influences routing.

**Changed files:**
- `src/vllm_metrics.hpp` — `load_score(compression_ratio)` + `effective_cache_pressure()` method
- `src/router_service.cpp` — `BackendInfo::compression_ratio` field, plumbed through registration
- `src/router_service.hpp` — `BackendState::compression_ratio`, updated `register_backend_global` signature
- `src/backend_registry.hpp` — Updated virtual interface
- `src/health_service.hpp/cpp` — Per-backend compression ratio storage, passed to `load_score()` during broadcast
- `src/config_schema.hpp` — `RoutingConfig::default_compression_ratio` (env: `RANVIER_DEFAULT_COMPRESSION_RATIO`)
- `src/config_loader.cpp` — YAML + env var parsing, validation (>= 1.0)
- `src/http_controller.cpp` — Admin API `compression_ratio` parameter on `POST /admin/backends`
- `src/parse_utils.hpp` — `parse_double()` utility

**Tests:** 10 new tests in `tests/unit/vllm_metrics_test.cpp`, 5 new tests in `tests/unit/parse_utils_test.cpp`.

**Configuration:** Static config via admin API or YAML (`routing.default_compression_ratio`). Auto-detection from vLLM metrics deferred to when inference engines expose compression ratio natively.

**Complexity:** Low. Config + formula change using existing data flow. Zero new async boundaries on the hot path.

### P1: Effective Capacity in Cost-Based Routing ✅

**Status:** Implemented

**Problem:** `CostBasedRoutingConfig::max_cost_per_backend` is a flat cap. The cost budget approximates how much KV-cache a request will generate (in tokens). But the same token budget costs ~6x less actual memory on a compressed backend.

**Solution:** Scale the cost budget ceiling per backend:

```
effective_max_cost = max_cost_per_backend * compression_ratio
```

TurboQuant backends accept proportionally more concurrent cost budget. This naturally steers large/long-context requests toward backends with more effective headroom.

**Changed files:**
- `src/router_service.cpp` — `reserve_cost_budget()`: effective max scaled by `compression_ratio`
- `src/router_service.cpp` — `find_backend_with_budget()`: per-backend effective max in eligibility check
- `src/router_service.cpp` — Step 4 cost-aware override: budget threshold scaled by selected backend's `compression_ratio`

**Tests:** 5 new tests in `tests/unit/router_service_test.cpp` covering effective capacity scaling, redirect behavior with compressed/uncompressed backends, and budget reservation with compression ratios.

**Complexity:** Low. Multiplier on existing budget math. Zero new async boundaries.

**Independence from TurboQuant:** Yes. Any capacity asymmetry between backends benefits from this.

### P1: Fleet-Wide Cache Efficiency Metrics ✅

**Status:** Implemented

**Problem:** Operators can't currently see the aggregate cache efficiency picture across a heterogeneous fleet.

**Solution:** New Prometheus gauges:

| Metric | Type | Description |
|--------|------|-------------|
| `ranvier_backend_effective_cache_capacity` | gauge | Raw capacity * compression ratio, per backend |
| `ranvier_backend_effective_cache_usage` | gauge | Raw usage / compression ratio, per backend |
| `ranvier_fleet_effective_cache_capacity_total` | gauge | Sum across all backends |
| `ranvier_fleet_effective_cache_usage_total` | gauge | Sum of effective usage across all backends |
| `ranvier_prefix_hits_by_compression_tier` | counter | Cache hit rate bucketed by compression tier (none/moderate/high) |

**Changed files:**
- `src/health_service.hpp` — Fleet aggregate computation methods
- `src/health_service.cpp` — `compute_fleet_effective_cache_capacity()`, `compute_fleet_effective_cache_usage()`, per-backend effective capacity/usage accessors, fleet-wide Prometheus gauges
- `src/metrics_service.hpp` — Per-backend `backend_effective_cache_capacity`/`backend_effective_cache_usage` gauges, `prefix_hits_by_compression_tier` counter (3 tiers: none/moderate/high), `record_prefix_hit_by_compression_tier()` method
- `src/router_service.cpp` — Calls `record_prefix_hit_by_compression_tier()` at both cache hit sites

**Tests:** 15 new tests in `tests/unit/health_service_test.cpp` (fleet aggregates, per-backend effective metrics), 8 new tests in `tests/unit/metrics_service_test.cpp` (compression tier bucketing).

**Complexity:** Low. Derived metrics from existing data + `compression_ratio` field. Zero new async boundaries. All gauge lambdas are lock-free (Rule #1).

### P2: Capacity-Aware Hash Fallback Selection

**Problem:** When ART lookup misses (Step 1 in `get_backend_for_prefix()`), hash fallback (Step 2) selects backends based on hash consistency and load. It doesn't consider how much room a backend has for new KV-cache entries.

**Proposal:** For hash fallback, factor in effective remaining capacity — especially for large-context requests. A backend with 80% prefix match but exhausted cache is worse than 70% match with headroom. The `estimated_cost` field (already computed) provides request size; combine with effective remaining capacity:

```
effective_headroom = (1.0 - effective_cache_usage) * effective_capacity
candidate_score = f(hash_affinity, load, effective_headroom, estimated_cost)
```

**Where:**
- `src/router_service.cpp` — `bounded_load_selection()`, `p2c_selection()`
- Uses existing `gpu_cache_usage_perc` from `VLLMMetrics` + new `compression_ratio`

**Complexity:** Medium. Requires tuning the multi-signal scoring function.

**Independence from TurboQuant:** Yes. Useful any time backends have different remaining capacity.

### P2: Compression-Aware Route TTL

**Problem:** `ttl_seconds` (default 1 hour) governs how long Ranvier assumes a learned route is valid. But on a TurboQuant backend, the underlying KV-cache entry survives longer (6x more fits in memory, so less eviction pressure). The fixed TTL doesn't reflect this.

**Proposal:** Scale route TTL by the backend's compression ratio:

```
effective_ttl = base_ttl * min(compression_ratio, max_ttl_multiplier)
```

Routes to compressed backends persist longer in the ART. Routes to non-compressed backends expire at the base rate.

**Where:**
- `src/router_service.cpp` — TTL check in route expiry logic
- Bounded by a `max_ttl_multiplier` to prevent excessively stale routes

**Complexity:** Low.

**Note:** This interacts with [push-based cache eviction notifications](push-cache-eviction-notifications.md). If push notifications are implemented, TTL becomes a fallback rather than the primary expiry mechanism, and this optimization matters less.

### P3: Tiered Compression Signaling (Ranvier to Backend)

**Problem:** Ranvier knows which prefixes are hot (high ART traversal frequency) and which are cold. Backends applying TurboQuant don't have this fleet-wide view — they compress uniformly.

**Proposal:** Ranvier injects a cache priority hint into proxied requests:

```
X-Ranvier-Cache-Priority: high | normal | low | evictable
```

- **high**: Keep this prefix's KV-cache at full precision (no compression)
- **normal**: Standard compression (default)
- **low**: Aggressive compression (2-bit values) — infrequent prefix
- **evictable**: Safe to evict first under memory pressure

Priority is derived from ART traversal statistics: prefix frequency, recency, number of distinct clients.

**Where:**
- `src/request_rewriter.hpp` — header injection (extends existing `X-Ranvier-Prefix-Hash` pattern)
- `src/router_service.cpp` — prefix popularity tracking (new per-route counter)

**Complexity:** High. Requires backend-side support (vLLM plugin or custom inference server). The Ranvier-side injection is straightforward, but value depends on downstream adoption.

**Independence from TurboQuant:** Partially. The compression tier concept assumes backends can vary compression per prefix. Without that capability, this reduces to eviction priority hints — still useful but less impactful.

## Integration Surface

### New Backend Metadata

The core enabler is a `compression_ratio` field per backend. Three sourcing options:

| Source | Pros | Cons |
|--------|------|------|
| **Static config** (per-backend YAML) | Simple, no backend changes | Operator must keep in sync manually |
| **vLLM metrics scrape** (new metric) | Automatic, authoritative | Requires vLLM to expose compression ratio |
| **Auto-detect from cache behavior** | Zero config | Fragile heuristic |

**Recommendation:** Start with static config (Option 1). Add scrape-based detection (Option 2) when/if inference engines expose the metric. This mirrors how `supports_token_ids` is currently handled in `BackendInfo`.

**Current status:** Static config implemented (P0). Admin API and YAML both supported.

### Existing Structs That Change

| Struct | File | Change | Status |
|--------|------|--------|--------|
| `BackendInfo` | `src/router_service.cpp` | Add `double compression_ratio = 1.0` | ✅ P0 |
| `VLLMMetrics` | `src/vllm_metrics.hpp` | Add `effective_cache_pressure()` method, `load_score(compression_ratio)` | ✅ P0 |
| `RoutingConfig` | `src/config_schema.hpp` | Add `default_compression_ratio` | ✅ P0 |
| `BackendState` | `src/router_service.hpp` | Add `double compression_ratio` (admin API visibility) | ✅ P0 |
| `HealthService` | `src/health_service.hpp` | Add `_backend_compression_ratios` map + setter | ✅ P0 |
| `RouteResult` | `src/router_service.hpp` | Add `double effective_cache_headroom` (observability) | Deferred to P1 |

### Relationship to Other Proposals

- **[Push-Based Cache Eviction Notifications](push-cache-eviction-notifications.md):** Complementary. Push notifications tell Ranvier *what* was evicted; compression awareness tells Ranvier *how much room remains*. Compression-aware TTL (P2 above) becomes less critical if push notifications ship first.
- **[Intelligence Layer VISION](VISION.md):** The cost-based routing already shipped (Tier 2). Compression-aware cost budgets (P1 above) are a natural extension of that work.

## Open Questions

1. **Heterogeneous compression within a backend**: Can a single vLLM instance apply different compression levels to different prefixes? If so, the tiered signaling (P3) is directly actionable. If not, it's a future aspiration.

2. **Dynamic compression ratio**: TurboQuant uses fixed quantization levels (3-bit keys, 2-bit values). But a backend might switch between modes under memory pressure. Should `compression_ratio` be a static config or a scraped metric that changes over time?

3. **Interaction with PagedAttention block alignment**: Ranvier's `block_alignment` config (default 16) aligns prefix hashes to vLLM's PagedAttention block boundaries. Does TurboQuant change the effective block size or alignment? If so, `block_alignment` may need to be compression-aware.

4. **Benchmarking strategy**: How do we measure the combined benefit? Proposed approach: run the existing 8xA100 benchmark suite with TurboQuant-enabled vLLM backends, comparing Ranvier+TurboQuant vs. Ranvier-only vs. TurboQuant-only vs. baseline. Key metrics: P99 TTFT, cache hit rate, effective fleet utilization.

5. **Which community TurboQuant implementation to target first?** The `0xSero/turboquant` vLLM integration is closest to Ranvier's existing backend support. The llama.cpp fork is relevant for Ranvier Local mode.
