# Investigation: Load-Aware Routing Regression After Consolidation (#289)

**Date:** 2026-02-15
**Investigator:** Claude (automated code analysis)
**Commits analyzed:** bc92993, 858e682, ea9f2b2, 887b9c4, a6d2a20

## Executive Summary

The regression (P99 13,000ms, -24% throughput) at 20 users on 13B is most likely
caused by **load-aware routing threshold flapping** at the specific concurrency
of 2.5 req/GPU. PR #289 itself is not the root cause — the consolidation is
behaviorally correct. However, multiple changes landed in the same window that
collectively altered hash distribution, and the threshold configuration is
poorly tuned for this mid-range concurrency.

**Root cause ranking:**

1. **(HIGH) Load-aware threshold flapping at 2.5 req/GPU** — Investigation Area #6
2. **(MEDIUM) Jump consistent hash redistribution** — Investigation Area #3
3. **(LOW) `cache_hit` semantic change** — Investigation Area #2
4. **(NEGLIGIBLE) Timing/sort/BackendInfo changes** — Areas #1, #4, #5

---

## Detailed Analysis by Investigation Area

### Area 1: Timing Difference (Post-#289 Fall-Through)

**Verdict: NOT A FACTOR**

Pre-#289, the ART path returned immediately:
```cpp
return {final_backend, true};  // Early return from ART branch
```

Post-#289, the ART path sets variables and falls through to:
1. `if (!art_hit)` check — branch not taken (~1 cycle)
2. `apply_load_aware_selection()` call (same as before)
3. Debug logging check (`if (!request_id.empty() && ...)`)
4. Return

The extra work is a single boolean check and a conditional debug log check.
This adds ~2-5ns per request. At 20 users generating ~100 req/s, this is
~500ns total per second — completely negligible compared to the 10,000ms P99
delta. The regression is measured in *milliseconds*, not nanoseconds.

### Area 2: `cache_hit` Semantic Change

**Verdict: NO DOWNSTREAM BEHAVIORAL IMPACT**

Commit bc92993 changed `route_request()` from:
```cpp
result.cache_hit = true;  // Prefix mode always has affinity
```
to:
```cpp
result.cache_hit = prefix_result.art_hit;  // Only true for ART hit
```

**Downstream usage in `http_controller.cpp`** (exhaustive search):

| Location | Usage | Behavioral? |
|----------|-------|-------------|
| Line 1119 | `route_span.set_attribute("ranvier.cache_hit", ...)` | Tracing attribute only |
| Line 1131-1132 | `log_proxy.debug(...)` | Debug log only |

**`cache_hit` is NOT used for any behavioral decisions.** It does not affect:
- Connection reuse
- Retry logic
- Route learning (which is gated by `_config.should_learn_routes()` and
  `ctx.tokens.size() >= _config.min_token_length`, not cache_hit)
- Backend selection
- Backpressure

The change correctly fixes misleading metrics but has zero effect on request routing
or handling behavior.

### Area 3: Jump Consistent Hash Redistribution

**Verdict: CONTRIBUTING FACTOR (MEDIUM)**

Commit 858e682 replaced modular hash with jump consistent hash:
```cpp
// Before:
size_t index = prefix_hash % live_backends.size();
// After:
int32_t index = jump_consistent_hash(prefix_hash, static_cast<int32_t>(live_backends.size()));
```

Jump consistent hash has better properties for topology changes (remaps ~1/n
keys vs all keys), but it **produces different bucket assignments** than modular
hash for the same key and bucket count. With 8 backends:

- `prefix_hash % 8` and `jump_consistent_hash(prefix_hash, 8)` may map the
  same key to different buckets
- The distribution is still uniform in both cases (provably so for jump hash)
- However, during the **transition period** (benchmark run), all ART entries
  were learned under the old modular hash mapping

**Impact theory:** If the benchmark started with a cold ART (empty tree), all
initial requests use hash fallback. Under modular hash, prefix P mapped to
backend X. Under jump hash, prefix P now maps to backend Y. The ART learns
the new mapping after the first success. This is a one-time transient, not
a sustained regression.

**However**, if the benchmark infrastructure used pre-warmed backends or
sequential test runs, the **GPU KV caches on each backend contain entries from
the modular-hash era**. Jump hash sends different prefixes to different backends
than before, causing KV-cache misses on the GPU side. This would manifest as
higher TTFT (time-to-first-token) because the GPU must recompute attention
from scratch. At 20 users on 13B, the GPU is under moderate pressure (2.5
req/GPU), and each KV miss is expensive (~1-2s for 13B). This could contribute
to the P99 spike but likely normalizes after the test's warm-up period.

**Key question for the developer:** Was the benchmark run against fresh backends
or backends that had prior state? If prior state, the hash change is a major
contributor.

### Area 4: `get_live_backends()` Sort Ordering

**Verdict: SORT IS IDENTICAL**

Pre-refactor (inline in `get_backend_for_prefix()`):
```cpp
std::vector<BackendId> live_backends;
for (BackendId id : state.backend_ids) {
    // ... filter dead/draining ...
    live_backends.push_back(id);
}
std::sort(live_backends.begin(), live_backends.end());
```

Post-refactor (`ShardLocalState::get_live_backends()`):
```cpp
std::vector<BackendId> result;
for (BackendId id : backend_ids) {
    // ... identical filter ...
    result.push_back(id);
}
std::sort(result.begin(), result.end());
return result;
```

Both iterate `backend_ids` (same source), apply the same dead/draining filter,
and sort by `BackendId` (int32_t, so `std::sort` uses `operator<`). The
ordering is **identical**.

The concern about `get_least_loaded_backend()` picking a different "first
zero-load" backend is valid *only if sort order changed*, which it did not.
The `if (load == 0) break` at line 415-417 returns the first zero-load
backend in candidate iteration order, which is the sorted `live_backends`
vector — same in both cases.

### Area 5: BackendInfo Copy/Move After Atomic Removal

**Verdict: NOT A FACTOR**

Commit 887b9c4 replaced `std::atomic<uint64_t> active_requests` with plain
`uint64_t`. The concern was about `absl::flat_hash_map` relocating entries
during rehash.

**Analysis:**

- `BackendInfo` lives in `absl::flat_hash_map<BackendId, BackendInfo>` inside
  `thread_local ShardLocalState`
- `absl::flat_hash_map` does relocate entries during rehash (it uses open
  addressing with flat storage)
- Pre-refactor: Copy/move constructors did `other.active_requests.load(relaxed)`
  — this was a valid read even during rehash because all access is single-threaded
- Post-refactor: Compiler-generated copy/move does plain `uint64_t` copy — also
  a valid single-threaded operation
- Both produce identical results: the `active_requests` value is faithfully
  copied during rehash

**No stale load values can persist** because:
1. All access is on a single reactor thread (no concurrent reads during rehash)
2. Rehash completes before any subsequent `active_requests` read
3. The compiler-generated copy is functionally identical to the explicit
   `load(relaxed)` copy when there's no concurrent access

### Area 6: 20-User Threshold Flapping (PRIMARY HYPOTHESIS)

**Verdict: MOST LIKELY ROOT CAUSE (HIGH)**

Configuration defaults (`src/config_schema.hpp:119-121`):
```cpp
bool load_aware_routing = true;
uint64_t queue_depth_threshold = 4;
uint64_t queue_diff_threshold = 2;
```

**Concurrency analysis by user count:**

| Users | Backends | Req/GPU | vs Threshold | Behavior |
|-------|----------|---------|--------------|----------|
| 10    | 8        | 1.25    | Well below 4 | Load-aware never triggers → stable prefix affinity |
| 20    | 8        | 2.50    | **Approaching 4** | **Flapping zone**: backends oscillate around threshold |
| 30    | 8        | 3.75    | Consistently near/above 4 | Load-aware triggers consistently → stabilizes to least-loaded |

**The flapping mechanism:**

At 2.5 req/GPU average, the actual per-backend queue depth at any instant is
stochastic. With Poisson arrival and 13B inference times (~3-5s per request),
the instantaneous queue depth per backend follows roughly:

- P(queue >= 4) ≈ 15-25% per backend at any given moment
- P(queue >= 4 for preferred AND queue <= 2 for alternative) ≈ 5-10%

This creates a pathological cycle:
1. Backend A has queue=4 for prefix P → load-aware diverts to backend B
2. Backend B now processes prefix P → no KV-cache hit → slow (full recompute)
3. Backend A's queue drops to 3 → next request for prefix P goes to A again
4. But A's KV-cache for P may have been evicted (LRU pressure from other prefixes)
5. Result: **both A and B experience cache misses for prefix P**

**This is the "affinity thrashing" failure mode:**
- Load-aware routing breaks prefix affinity without actually reducing queuing
- The diverted request takes LONGER on the alternative backend (KV-cache miss)
- This increases overall latency, which increases queue depths, which triggers
  more diversions → positive feedback loop

**Why it doesn't happen at 10 or 30 users:**
- At 10 users: `queue <= 2` almost always → load-aware never fires → stable affinity
- At 30 users: `queue >= 4` almost always → load-aware fires consistently, but
  the `queue_diff_threshold = 2` check prevents flapping because ALL backends are
  similarly loaded. Diversions only happen when there's a genuinely lighter backend.

**Why it's worse on 13B than 8B:**
- 8B inference is ~3x faster → queue drains faster → less time in the threshold zone
- 13B has longer inference time → queue depth fluctuates more slowly → longer dwell
  time in the flapping zone

**Critical data point needed:** Check the `load_aware_fallbacks` counter from the
20-user run vs other runs. If this hypothesis is correct, the 20-user run should
show an anomalously high fallback count (likely 20-40% of requests diverted) while
the 10-user run shows ~0% and the 30-user run shows a moderate, stable percentage.

---

## PR #289 Consolidation: Behavioral Change Assessment

The consolidation in PR #289 (commit a6d2a20) is **behaviorally equivalent**.

Pre-#289 had two call sites:
```cpp
// ART path:
BackendId final_backend = apply_load_aware_selection(art_backend, live_backends, request_id, "ART");
return {final_backend, true};

// Hash path:
BackendId final_backend = apply_load_aware_selection(selected, live_backends, request_id, "hash");
return {final_backend, false};
```

Post-#289 has one call site:
```cpp
BackendId final_backend = apply_load_aware_selection(selected, live_backends, request_id, source);
return {final_backend, art_hit};
```

The function receives the same `selected` backend, the same `live_backends`,
and the same `request_id` in both cases. The `source` parameter is only used
for debug logging. The return values are identical. **There is no behavioral
change from the consolidation itself.**

---

## Recommended Fixes

### Fix 1: Raise `queue_depth_threshold` for 13B workloads (Quick Win)

```yaml
routing:
  queue_depth_threshold: 8   # Was 4 — too aggressive for 13B latency
  queue_diff_threshold: 3    # Was 2 — require larger gap to justify cache miss
```

**Rationale:** With 13B inference, a queue depth of 4 is normal operating load,
not overload. Raising to 8 keeps load-aware routing from triggering during
normal operation while still providing protection against genuine hot-spotting.

### Fix 2: Add Hysteresis to Load-Aware Selection (Proper Fix)

The current implementation has no hysteresis — it makes independent decisions
per-request. Add a "sticky diversion" mechanism:

```cpp
// In apply_load_aware_selection():
// If we diverted this prefix recently (within last N requests), keep diverting
// to the same alternative to build cache affinity there.
// If we didn't divert recently, require a higher threshold to start diverting.
```

This prevents the oscillation pattern where requests for the same prefix
bounce between backends.

### Fix 3: Weighted Threshold Based on Model Size (Config Enhancement)

Allow per-backend or per-model threshold configuration:
```yaml
routing:
  load_aware_routing: true
  queue_depth_threshold: 4        # Default for 8B
  queue_depth_threshold_13b: 8    # Override for 13B
```

Or more generically, scale threshold by expected inference latency.

### Fix 4: Verify Benchmark Methodology

Ensure the benchmark runs against **fresh backends** (cold KV cache) to
eliminate the jump-consistent-hash redistribution confound. If the regression
disappears with a fresh start, the hash change contribution is confirmed and
can be addressed separately.

---

## Data Requests

To confirm/refute these hypotheses, the following data from the benchmark runs
would be valuable:

1. **`load_aware_fallbacks` counter** for all runs (especially 20-user vs 10/30-user)
2. **Per-backend request distribution** — are requests uniformly distributed or
   concentrated?
3. **ART cache hit rate by time** — does the 20-user run show declining hit rate
   over the test duration?
4. **GPU KV-cache hit rate** from vLLM metrics — distinct from Ranvier's ART cache
5. **Whether backends were restarted** between the modular→jump hash migration and
   the benchmark run

---

## Files Examined

| File | Lines | Purpose |
|------|-------|---------|
| `src/router_service.cpp:375-422` | `get_backend_load()`, `get_least_loaded_backend()` | Load helpers |
| `src/router_service.cpp:440-485` | `apply_load_aware_selection()` | Load-aware diversion logic |
| `src/router_service.cpp:517-536` | `jump_consistent_hash()` | Hash function |
| `src/router_service.cpp:990-1064` | `route_request()` | Unified routing entry point |
| `src/router_service.cpp:1145-1256` | `get_backend_for_prefix()` | Prefix routing (consolidated) |
| `src/router_service.cpp:249-260` | `ShardLocalState::get_live_backends()` | Live backend filtering |
| `src/router_service.cpp:60-78` | `struct BackendInfo` | Backend metadata |
| `src/router_service.hpp:70-74` | `PrefixRouteResult` | Return type struct |
| `src/http_controller.cpp:1116-1135` | Post-routing `cache_hit` usage | Tracing/logging only |
| `src/config_schema.hpp:119-121` | Load-aware defaults | threshold=4, diff=2 |

## Commits Analyzed

| Commit | Description | Relevant? |
|--------|-------------|-----------|
| bc92993 | Fix cache_hit=false for hash fallback | Yes — semantic change, no behavioral impact |
| 858e682 | Replace modular hash with jump consistent hash | Yes — changes backend selection for cache misses |
| ea9f2b2 | Extract live-backend filtering to helpers | Yes — verified identical sort ordering |
| 887b9c4 | Replace std::atomic with uint64_t | Yes — verified no stale load values |
| a6d2a20 | Consolidate load-aware selection (#289) | Yes — verified no behavioral change |
