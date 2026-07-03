# Hot-Path Invariant Catalog

A living reference (like `seastar-pitfalls-reference.md`): the semantic invariants the
hot path must maintain — things that, when broken, produce *wrong answers* rather than
crashes. `/invariant-audit` reads this catalog, checks code against it, and appends new
invariants discovered during a run. Update an entry when the code's contract changes;
add an entry when an audit or incident reveals an invariant nothing had written down.

Each entry: **ID**, statement, where it's maintained, and how it can break.
Status markers: ✅ verified by unit test · ⚠️ known violation (see linked finding) · ◻ unverified.

Seeded from the 2026-07-03 invariant audit (`invariant-audit-2026-07-03.md`).

---

## RadixTree (`src/radix_tree.hpp`)

- **T1** ◻ `route_count_` equals the number of nodes with `leaf_value` set.
  Maintained by `insert_recursive`'s `is_new_route` return and the decrement-with-clamp
  in every removal path. The clamp (`count > removed ? count - removed : 0`) masks
  violations — if it ever saturates, T1 was already broken upstream.
- **T2** ◻ `sum(routes_by_backend_) == route_count_` while under `kMaxTrackedBackends`.
  `note_route_added/removed` must be called at every leaf add/remove/overwrite-to-
  different-backend, before `leaf_value` is reassigned.
- **T3** ◻ The intrusive LRU lists together contain exactly the leaf nodes, each list
  ordered by recency. Every clear-leaf path must `lru_remove` first; node
  grow/shrink/split must splice the replacement node into the source node's list
  (`transfer_node_metadata`, inline splices in `split_node`/`split_long_prefix`).
  Which list a leaf belongs in is T8's contract.
- **T4** ◻ Node48 `index[]` ↔ `keys`/`children` parallel-vector consistency: `index[k]`
  is either `EMPTY_MARKER` or the slot of key `k`. `remove_child` (Node48 branch) must
  reindex after erase; the generic `remove_child_keyed` must never be applied to Node48.
- **T5** ◻ Leaves exist only at TokenId-aligned byte depths (asserted in
  `for_each_leaf_recursive`). Holds because all inserts enter via the TokenId-span API
  with block-aligned truncation.
- **T6** ◻ No node prefix exceeds `MAX_PREFIX_LENGTH` (`split_long_prefix` chains).
- **T7** ✅ Trust ladder LOCAL > PUSH > REMOTE: a higher-trust route is never displaced
  by a lower-trust write, and eviction prefers REMOTE, then PUSH, then LOCAL.
  Enforced by `insert_if_trusted` on both lower-trust write paths — PUSH
  materialization (native KV) and the gossip REMOTE batch apply
  (`apply_route_batch_to_local_tree`) — plus `evict_lowest_trust`. A REMOTE
  announcement for a DIFFERENT backend is REFUSED (route untouched, counted as
  `remote_routes_trust_refused`); same-backend re-announcement is TOUCHED_SAME
  (LRU refresh, origin unchanged); REMOTE-over-REMOTE is OVERWROTE. Finding I-5
  (invariant-audit-2026-07-03) closed: operator chose the ladder, not latest-wins.
  Pinned by `RemoteBatchRefusedByLocalRoute` / `RemoteBatchSameBackendKeepsLocalRoute`
  / `RemoteBatchOverwritesExistingRemoteRoute` (RouterService path) and
  `TrustedSameBackendRefreshesLruKeepingOrigin` (RadixTree LRU) in
  `tests/unit/cache_eviction_test.cpp`.
- **T8** ✅ Per-origin LRU membership/ordering: each leaf is linked in exactly the
  intrusive list matching its `origin` (`lru_head_`/`lru_tail_` arrays indexed by
  origin), each list is ordered by recency, and list lengths sum to `route_count_`.
  This is what makes `evict_lowest_trust`/`evict_oldest` O(1) tail pops (Rule #17 at
  the synchronous batch-apply call sites). Because list selection derives from
  `node->origin`, any path that changes a *linked* node's origin must `lru_remove`
  BEFORE reassigning `origin`, then `lru_push_front` (the overwrite branch of
  `insert_recursive` does exactly this). Asserted by `validate_lru_lists()` in debug
  builds; verified by the `PerOriginLruTest` cases in
  `tests/unit/cache_eviction_test.cpp` and per-op validation with eviction ops in
  `tests/fuzz/radix_tree_fuzz.cpp` (fixed 2026-07-03, finding I-3).

## Router service (`src/router_service.cpp`)

- **R1** ✅ `prefix_hash_index` membership ⊆ live routing state: an entry
  `(hash, backend)` exists only while either a tree route or a native-KV block backs
  it. Maintained by `ttl_cleanup_on_shard` phase 3: every TTL cycle the index is
  rebuilt from the live tree (`for_each_leaf`), preserving entries of backends with
  fresh native streams (their lifecycle belongs to UPSERT/REMOVE/CLEAR/RESET) and
  recomputing the per-backend native entry counters. Point removals (push evictions,
  native REMOVE/CLEAR/RESET) prune between cycles; membership converges within one
  cycle of any removal path (TTL expiry, LRU eviction, peer prune, unregister).
  Verified by `tests/unit/prefix_hash_index_lifecycle_test.cpp` (finding I-1, fixed
  2026-07-03).
- **R2** ✅ Hash-depth identity: every producer and consumer of a prefix hash for the
  same request must hash the same token count — routing lookup, BOTH learn paths'
  index inserts, the `X-Ranvier-Prefix-Hash` header, and the ledger's boundary hashes.
  (The warning block above the `prefix_len` computation in `get_backend_for_prefix`
  states this.) Both `apply_*_batch` learn paths now hash at `route.tokens.size()`
  (the effective boundary the tokens were truncated to), matching the routing-side
  uncapped `prefix_boundary` hash. Verified by
  `tests/unit/prefix_hash_index_lifecycle_test.cpp` (finding I-2, fixed 2026-07-03).
- **R3** ◻ Route learning pins the PLACEMENT winner (`original_selected` /
  `learn_target_backend`), never the transient dispatch target. Load/cost diverts are
  per-request; the learned route must follow the stable choice
  (route_scorer.hpp contract; honored at http_controller's learn site).
- **R4** ◻ `active_requests` guard symmetry: every `BackendRequestGuard` increment has
  exactly one decrement on every exit path; move transfers ownership exactly once.
  Wholesale `BackendInfo` replacement on re-registration resets the counter with guards
  in flight — the destructor's floor-at-zero absorbs it (accepted drift, not silent
  growth).
- **R5** ◻ Cost-budget symmetry: `cost_reserved_total - cost_released_total` equals the
  number of in-flight reservations; `current_cost_budget` returns to 0 when idle
  (float drift clamped at release).
- **R6** ◻ Cross-shard load bookkeeping: `cross_shard_load[b]` equals the sum over
  source shards of the latest snapshot values for `b`. `apply_load_snapshot` must
  subtract the exact prior snapshot before adding the new one.
- **R7** ◻ GPU-seconds accounting: `backend_gpu_seconds_total` is monotonic per
  registration epoch, scrapes never advance stored state, and every liveness or
  gpu_count transition folds the in-progress segment exactly once (all transition
  sites stamp one shared `now` and reset `gpu_seconds_segment_start`).
- **R8** ◻ Scorer parity: with default `ScoringWeights`, placement == the pre-scorer
  anchor and dispatch reproduces the former override chain (load hinge opens strictly
  above the strategy allowance; probe ranks only for under-allowance candidates;
  `load_diverted`/`cost_diverted` set only when the corresponding weighted penalty
  was > 0 and dispatch ≠ placement).
- **R9** ◻ Divert counters mean what they say: `load_aware_fallbacks`,
  `cost_redirects`, `hardware_cost_diverts` increment only when the decision actually
  moved. `headroom_redirects` counts headroom-influenced diverts: headroom pressure
  present on a BOUNDED_LOAD/P2C decision that moved off the primary hash bucket
  (documented approximation — a moved decision with pressure present may still be
  load-driven; exact attribution would need a second selection pass, deliberately
  not paid). Pinned by `CapacityAwareHashTest.HeadroomRedirectNotCountedWhenSelectionCannotMove`
  and `...CountedWhenPressurePushesPrimaryOverCap` in `tests/unit/router_service_test.cpp`.
- **R10** ◻ Batching accounting: `local_routes_batched = applied + deduplicated +
  dropped_overflow` over any window (per shard); overflow drops are counted at the
  drop site, never silently.
- **R11** ◻ Verified-residency semantics: "verified resident/evicted" verdicts are
  issued only while the backend's native stream is fresh
  (`native_residency_fresh`); MATERIALIZE alone must never refresh freshness
  (comment at the MATERIALIZE case explains the divert-loop this prevents).
- **R12** ✅ Fail-open is a cluster-wide state: when quorum loss triggers fail-open,
  every shard's routing observes it. `GossipConsensus::check_quorum` fires a
  transition seam on both enter and exit; `RouterService::broadcast_fail_open`
  mirrors the posture into each shard's `ShardLocalState::fail_open_active` via
  `smp::invoke_on_all`, and `route_request` reads it shard-locally (no cross-shard
  read on the hot path). Pinned by `RouterServiceTest.FailOpenFlagForcesRandomRoutingOverArtHit`
  (`tests/unit/router_service_test.cpp`) and the `QuorumTest.FailOpenSeam_*` cases
  (`tests/unit/quorum_test.cpp`) covering enter/exit, disabled-config, and
  initially-degraded convergence (finding I-6).

## Connection pool (`src/connection_pool.hpp`)

- **P1** ✅(debug) `_total_idle_connections == Σ pool deque sizes`
  (`debug_validate_idle_count` asserts in debug builds).
- **P2** ◻ Every bundle removed from a pool is either returned to a caller or closed
  via `ClosePolicy` — no silent drops (leaked sockets) on any liveness-check path.
- **P3** ◻ `_pools` entry count ≤ `max_backends` after any `put()`; empty entries are
  reaped by `cleanup_expired()`.

## KV-event ledger (`src/kv_event_ledger.hpp`)

- **L1** ◻ Boundary identity: at every emitted UPSERT/REMOVE, the raw FNV accumulator
  equals `hash_prefix(tokens, cumulative_count, block_alignment)`. Requires
  `block_size % block_alignment == 0` (rejected otherwise) — this is what makes native
  index entries findable by routing lookups.
- **L2** ◻ Materialize-cap monotonicity: `own_tokens` is retained iff
  `token_count <= max_materialize_tokens`, so every in-cap block's ancestor chain is
  fully walkable (chain breaks are defensive-counted, never silently mis-assembled).
- **L3** ◻ Wire-order application: CLEAR/RESET must be applied at their position in
  the shipment relative to surrounding UPSERTs (the segment-walk in
  `apply_native_kv_ops` exists solely for this).

## Accounting (pure headers)

- **A1** ◻ `gpu_seconds_total` / `gpu_seconds_with_in_progress` never subtract:
  non-positive gpu_count or seconds contribute nothing (monotonic counter contract).
- **A2** ◻ `UsageEvent.tokens_estimated` honesty: false only when the engine's
  response `usage` was actually snooped; estimates must never masquerade as actuals.
