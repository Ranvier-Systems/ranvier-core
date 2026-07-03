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
- **T3** ◻ The intrusive LRU list contains exactly the leaf nodes, ordered by recency.
  Every clear-leaf path must `lru_remove` first; node grow/shrink/split must splice the
  replacement node into the list (`transfer_node_metadata`, inline splices in
  `split_node`/`split_long_prefix`).
- **T4** ◻ Node48 `index[]` ↔ `keys`/`children` parallel-vector consistency: `index[k]`
  is either `EMPTY_MARKER` or the slot of key `k`. `remove_child` (Node48 branch) must
  reindex after erase; the generic `remove_child_keyed` must never be applied to Node48.
- **T5** ◻ Leaves exist only at TokenId-aligned byte depths (asserted in
  `for_each_leaf_recursive`). Holds because all inserts enter via the TokenId-span API
  with block-aligned truncation.
- **T6** ◻ No node prefix exceeds `MAX_PREFIX_LENGTH` (`split_long_prefix` chains).
- **T7** ⚠️ Trust ladder LOCAL > PUSH > REMOTE: a higher-trust route is never displaced
  by a lower-trust write, and eviction prefers REMOTE, then PUSH, then LOCAL.
  Enforced by `insert_if_trusted` (PUSH materialization) and `evict_lowest_trust`, but
  **plain `insert()` bypasses it** — the gossip REMOTE apply path overwrites LOCAL
  routes (finding I-5, invariant-audit-2026-07-03).

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
  moved. (⚠️ `headroom_redirects` currently counts data-presence, not influence —
  finding I-7.)
- **R10** ◻ Batching accounting: `local_routes_batched = applied + deduplicated +
  dropped_overflow` over any window (per shard); overflow drops are counted at the
  drop site, never silently.
- **R11** ◻ Verified-residency semantics: "verified resident/evicted" verdicts are
  issued only while the backend's native stream is fresh
  (`native_residency_fresh`); MATERIALIZE alone must never refresh freshness
  (comment at the MATERIALIZE case explains the divert-loop this prevents).
- **R12** ◻ Fail-open is a cluster-wide state: when quorum loss triggers fail-open,
  every shard's routing should observe it. (⚠️ currently only shard 0 does —
  finding I-6.)

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
