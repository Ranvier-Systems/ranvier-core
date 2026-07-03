# Invariant Audit — 2026-07-03

**Type:** Semantic-correctness audit (Pass B: "wrong answers, not crashes").
**Scope:** Hot-path routing core — `radix_tree.hpp`, `route_scorer.hpp`,
`router_service.{hpp,cpp}`, `connection_pool.hpp`, `kv_event_ledger.hpp`,
`gpu_seconds_accounting.hpp`, `usage_ledger_schema.hpp` — read in full, plus targeted
verification greps across `src/` and the learn site in `http_controller.cpp`.
**Method:** Enumerate the invariants each component must maintain (now the living
catalog `.dev-context/invariants.md`), then hunt code paths that violate them —
error/early-return paths, cross-component counter symmetry, and hash/identity
consistency in particular.
**Static analysis only** — nothing below was executed; Deferred Gates at the end.

## Criticality Score: 5/10

No crash or security exposure found; the tree and pool are internally consistent and
defensively coded. But two confirmed findings make the routing state *quietly wrong or
unbounded over time* (I-1, I-2), one is a latent reactor-stall at capacity (I-3), and
one is a documented-contract violation whose intent needs a decision (I-5). Fix I-1/I-2
before the next release; the rest before v1.0.

---

## Findings

| # | Severity | File:Line | Invariant | Issue | Recommendation |
|---|----------|-----------|-----------|-------|----------------|
| I-1 | HIGH | `src/router_service.cpp:544,1336,3719` | R1 | `prefix_hash_index` is populated on every learned route but never pruned when routes TTL-expire (`ttl_cleanup_on_shard`), are LRU-evicted at capacity, are pruned on peer death (`remove_routes_for_backend`), or when a backend unregisters. Its documented bound ("Bounded by max_routes… No separate MAX_SIZE needed", line 543) is false: under prefix churn the map grows without bound (Rule #4 by broken invariant), and stale `(hash, backend)` entries feed wrong `loads_applied` results and the I-2 junk population. | Prune the index on every route-removal path, or rebuild it from `for_each_leaf` during the 60s TTL/compaction cycle, or give it a real MAX_SIZE + staleness eviction. |
| I-2 | HIGH | `src/router_service.cpp:1333,3716` vs `:2616-2621,2699` | R2 | Hash-depth mismatch: the learn paths insert index entries hashed at `min(tokens.size(), prefix_token_length)`, but routing hashes at the **uncapped** `prefix_boundary`. For any request with `prefix_boundary > prefix_token_length` (system prompts longer than 128 tokens — the common RAG case), the learned index entry's hash matches nothing routing, push-eviction, or verified-residency will ever compute. This violates the explicit consistency contract in the NOTE at lines 2607-2615. Learned entries for such prefixes are permanent junk (compounding I-1); push evictions for them count `evictions_unknown`. | In both `apply_*_batch_to_local_tree`, hash over `route.tokens.size()` (the tokens are already truncated to the effective boundary by `learn_route_global`) instead of re-capping at `prefix_token_length`. |
| I-3 | MEDIUM | `src/radix_tree.hpp:678-693`; call sites `src/router_service.cpp:1319-1326,5264-5272` | Rule #17 | `evict_lowest_trust()` walks the entire LRU list up to twice per call, and the remote-batch / MATERIALIZE apply loops call it once **per insert** while at capacity, synchronously. Worst case (tree of 100k mostly-LOCAL routes, 100-route remote batch): ~20M pointer dereferences in one reactor task — a multi-ms stall on every batch flush, forever, once the tree fills. The local-batch path uses O(1) `evict_oldest()` and is fine. | Maintain per-origin LRU tails (REMOTE/PUSH/LOCAL) for O(1) lowest-trust eviction, or bound the scan and fall back to `evict_oldest()`. |
| I-4 | MEDIUM | `src/router_service.cpp:2199-2201` | Rule #14 | `run_ttl_cleanup` copies `backend_cutoffs` (an `absl::flat_hash_map`, heap-owning) on shard 0 and moves it into the `submit_to` lambda; the lambda is destroyed on the target shard → cross-shard free. Every neighboring broadcast uses foreign_ptr; this one (compression-aware TTL) doesn't. Latent: only bites when a backend has `compression_ratio > 1.0` (map otherwise empty, no heap). | Ship as `foreign_ptr<unique_ptr<vector<pair<BackendId, time_point>>>>` and rebuild locally, matching the load-snapshot broadcast pattern. |
| I-5 | MEDIUM | `src/router_service.cpp:1329`; contract `src/radix_tree.hpp:89-102` | T7 | Gossip-learned routes use plain `insert(..., REMOTE)`, which overwrites an existing LOCAL (or PUSH) route for the same key unconditionally — the documented trust ladder (LOCAL wins over PUSH wins over REMOTE) is enforced only for PUSH materialization. Two peers serving the same prefix can flap the route between backends: A learns LOCAL→1, B announces →2, A's ART flips to REMOTE→2, A re-learns LOCAL→1, re-gossips… each flip is a KV-cache miss. | Decide the intent. If the trust ladder is the contract, use `insert_if_trusted(..., REMOTE)` in `apply_route_batch_to_local_tree`. If latest-wins convergence is intended, document it at the RouteOrigin comment and delete the ladder claim. |
| I-6 | LOW-MEDIUM | `src/router_service.cpp:2338-2356` | R12 | Fail-open mode only ever activates on shard 0 (`_gossip->is_fail_open_mode()` returns false elsewhere, per the comment). During split-brain, 1/N of traffic gets availability-first random routing and (N-1)/N keeps normal routing — an inconsistent cluster posture that also makes the feature hard to observe. | Broadcast the fail-open flag into `ShardLocalState` on quorum transitions (same shape as `broadcast_gpu_load`), and read it shard-locally. |
| I-7 | LOW | `src/router_service.cpp:2839-2848` | R9 | `headroom_redirects` increments whenever the selected backend's capacity-adjusted load differs from its base load — i.e., whenever headroom data exists and pressure > 0 — regardless of whether the selection actually changed. The metric reads as "redirects" but measures "headroom data was present". | Either compare the selection with-vs-without the headroom term, or rename the metric/description to match what it counts. |
| I-8 | LOW | `src/router_service.cpp:2707-2746` | R11 | With exactly one live backend, a verified-cold ART hit increments `native_verified_downgrades` *and* then takes the hit branch (`cache_hits++`, `art_hit=true`) because the downgrade requires `live_backends.size() > 1`. Routing is correct (nowhere else to go) but the counters tell contradictory stories for the same decision, and `cache_hit=true` propagates to telemetry for a verified miss. | Count a distinct `verified_cold_honored` (or suppress the downgrade counter) on the single-backend path so hits and downgrades stay disjoint. |

### What was checked and found sound

Recording these so future runs don't re-derive them:

- **RadixTree internal consistency (T1-T6):** all leaf-clearing paths `lru_remove`
  before tombstoning; grow/shrink/split splice LRU correctly; Node48 removal reindexes
  (the generic `remove_child_keyed` is never dispatched to Node48); `routes_by_backend_`
  is updated on every leaf mutation including overwrite-to-different-backend, with a
  balanced at-cap posture. `shrink_node` bounds always match `should_shrink` so the
  truncating copy loops never drop children.
- **Scorer parity wiring (R8):** allowances match the pre-scorer triggers per strategy
  (BOUNDED_LOAD `cap-1` off-by-one is deliberate and documented); the load term is
  correctly suppressed for load-vetted BOUNDED_LOAD/P2C hash anchors; probe ranks are
  filled only for under-allowance candidates; learning pins `learn_target_backend`
  (placement), not dispatch (R3 holds at `http_controller.cpp:1102-1131`).
- **Guards (R4, R5):** `BackendRequestGuard`/`CostBudgetGuard` are symmetric on all
  exit paths incl. move-assign; underflow is floored; `reserve_cost_budget`
  deliberately reserves even over budget to keep release symmetric.
- **GPU-seconds (R7, A1):** every transition (register, status up/down, gpu_count
  change) folds the in-progress segment exactly once with a single shared timestamp;
  scrape reads are pure.
- **Cross-shard load sync (R6):** subtract-old/add-new is exact; ghost entries
  self-heal on the next snapshot.
- **Connection pool (P1-P3):** counter invariant is debug-asserted; every liveness
  rejection path closes via policy; map growth is bounded and reaped.
- **KV ledger (L1-L3):** boundary identity is enforced by rejecting misaligned
  block sizes; the materialize chain walk is cycle-guarded and length-verified;
  shipment application preserves wire order around CLEAR/RESET; MATERIALIZE
  deliberately does not refresh freshness (prevents a verified-evict divert loop).

---

## Structural Fixes (for BACKLOG.md)

```markdown
- [ ] [HIGH] Fix: prune prefix_hash_index on TTL expiry / LRU eviction / peer prune / unregister (invariant audit 2026-07-03, I-1)
- [ ] [HIGH] Fix: learn-path index inserts must hash at the effective boundary, not min(len, prefix_token_length) (I-2)
- [ ] [MEDIUM] Fix: O(1) per-origin LRU tails for evict_lowest_trust — remove O(n) scan per insert at capacity (I-3)
- [ ] [MEDIUM] Fix: foreign_ptr the backend_cutoffs map in run_ttl_cleanup (Rule #14) (I-4)
- [ ] [MEDIUM] Decide: REMOTE-overwrites-LOCAL — enforce trust ladder via insert_if_trusted or document latest-wins (I-5)
- [ ] [MEDIUM] Fix: broadcast fail-open state to all shards (I-6)
- [ ] [LOW] Fix: headroom_redirects counts data-presence, not redirects (I-7)
- [ ] [LOW] Fix: single-backend verified-cold hit double-counts hit + downgrade (I-8)
```

## Fix Prompt — I-1 + I-2 (one change-set; they touch the same two functions)

**PROBLEM:** `prefix_hash_index` (`router_service.cpp:544`) violates two invariants:
(a) entries are never removed when their backing route TTL-expires, is LRU-evicted,
is pruned on peer death, or when the backend unregisters — unbounded growth and stale
residency/load-event state; (b) the learn paths (`apply_route_batch_to_local_tree`
:1333, `apply_local_batch_to_tree` :3716) hash at `min(tokens.size(),
prefix_token_length)` while routing (`get_backend_for_prefix` :2616-2621) hashes at the
uncapped `prefix_boundary`, so boundary-learned entries are keyed at the wrong depth.

**CONSTRAINTS:** Shard-local only (Rule #1) — the index lives in `ShardLocalState`; no
cross-shard access on the hot path. Any sweep must be bounded or yielded (Rule #17).
Native-KV UPSERT/REMOVE provenance must keep working — the index is shared between
learned routes and native events by design. Do NOT cap the routing-side
`prefix_boundary` hash (see the NOTE at :2607-2615).

**REFERENCE IMPLEMENTATION in this codebase:** `native_purge_backend_local`
(:5309-5350) is the existing bounded two-pass sweep over the index;
`ttl_cleanup_on_shard` (:2116-2157) is the natural periodic hook (already yields
between phases).

**FIX APPROACH:** (1) In both apply-batch functions, compute
`prefix_len = route.tokens.size()` (tokens are pre-truncated to the effective boundary
by `learn_route_global`; keep the `> 0` guard). (2) Add a phase 3 to
`ttl_cleanup_on_shard`: rebuild the learn-derived portion of the index from
`tree->for_each_leaf` — collect `(hash_prefix(entry.tokens, entry.tokens.size(),
block_alignment), backend)` pairs, then swap-in, preserving entries whose backend has
fresh native state (their lifecycle belongs to UPSERT/REMOVE/CLEAR). Yield before the
rebuild (`maybe_yield`, matching the existing phase separation). (3) Update the bound
comment at :543 to describe the actual mechanism.

**ACCEPTANCE CRITERIA:** New unit tests below pass; `router_residency_cache_size`-style
gauge for the index (add `router_prefix_hash_index_size`) stays flat under a
learn/expire churn loop; existing `gossip_cache_eviction_test` and
`kv_event_translation_test` suites stay green.

---

## Proposed unit tests (Deferred Gates)

Static analysis only — the developer runs these in the Docker container:

```bash
# 1. New: tests/unit/prefix_hash_index_lifecycle_test.cpp
#    - learn route → TTL-expire it (TestClock / direct remove_expired) →
#      assert index no longer contains (hash, backend)          [captures I-1]
#    - learn with prefix_boundary=192, prefix_token_length=128 →
#      evict_by_prefix_hash_local(hash_prefix(tokens,192,16)) →
#      assert evictions_applied==1, not evictions_unknown       [captures I-2]
# 2. New case in tests/unit/cache_eviction_test.cpp:
#    - apply_route_batch (REMOTE) over an existing LOCAL route →
#      assert LOCAL preserved (or update test to codify latest-wins)  [I-5]
# 3. New case in tests/unit/gpu_load_routing_test.cpp:
#    - single backend, pressure>0, no divert → headroom_redirects unchanged [I-7]
cd build && ctest -R "prefix_hash_index|cache_eviction|gpu_load_routing" --output-on-failure

# Sanitizer pass over the touched suites (index rebuild is pointer-heavy):
cmake -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=address .. && make -j && ctest --output-on-failure
```

## Anti-Pattern Candidates

- "Reverse index populated on write, never pruned on the owner's delete paths" (I-1)
  is the same shape as E1 (route_count drift, 2026-02-12): a side-structure updated on
  add but not on every remove. If a third instance appears, promote via
  `/extract-pattern`: *every side-index must name the removal path that prunes it, per
  removal path of the primary structure.*
