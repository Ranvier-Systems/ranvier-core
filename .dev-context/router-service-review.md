# Router Service Code Review

Deep-dive review of `router_service.hpp` and `router_service.cpp` with improvement
suggestions along correctness, maintainability, and performance dimensions.

## 1. Modular Hash Is Not Consistent Hash (Correctness — High)

**Location:** `src/router_service.cpp:1271` and `src/router_service.cpp:1337`

The hash fallback uses `prefix_hash % live_backends.size()` to select a backend.
The code and config comments call this "consistent hashing," but modular hashing is
**not** consistent hashing. When a backend is added or removed,
`live_backends.size()` changes and nearly *all* keys remap to different backends.
True consistent hashing (ring-based, jump hash, or rendezvous hashing) remaps only
~1/n keys on a topology change.

This matters directly for the project's core value proposition: KV-cache affinity.
When a backend goes down, every prefix that was using hash fallback gets remapped to
a random new backend, destroying cache locality for all of them instead of just ~1/n.

**Suggestion:** Replace `prefix_hash % live_backends.size()` with jump consistent
hash (`jump_consistent_hash(prefix_hash, live_backends.size())`), which is a single
function (~10 lines), stateless, and remaps only 1/n keys. Since `live_backends` is
already sorted for determinism, the index returned by jump hash is directly usable.

---

## 2. `route_request()` Reports `cache_hit = true` for Hash Fallbacks (Correctness — Medium)

**Location:** `src/router_service.cpp:1050`

```cpp
result.cache_hit = true;  // Prefix mode always has affinity (ART or hash)
```

When `get_backend_for_prefix()` returns a backend via the hash fallback path (ART
miss), `route_request()` unconditionally sets `cache_hit = true`. The
`RouteResult::cache_hit` field is documented as "True if route was found in cache
(ART or prefix affinity)," but semantically a hash fallback is a cache *miss* — the
route was not found in the ART, and the hash provides no guarantee the backend has
the prefix cached in its KV-cache.

This makes the `cache_hit` field unreliable for monitoring. External consumers
(dashboards, alerting) that check `cache_hit` will see inflated cache hit rates.

**Suggestion:** Have `get_backend_for_prefix()` return a richer result (e.g., a
small struct or an enum indicating ART-hit vs hash-fallback) so `route_request()`
can set `cache_hit` accurately. Alternatively, add a `bool art_hit` output
parameter.

---

## 3. Duplicated Live-Backend Collection Logic (Maintainability — Medium)

**Location:** `get_random_backend()` (line 1092), `get_backend_for_prefix()` (line
1184), `get_backend_by_hash()` (line 1307)

The same pattern is repeated three times:

```cpp
std::vector<BackendId> live_backends;
for (BackendId id : state.backend_ids) {
    if (state.dead_backends.contains(id)) continue;
    auto it = state.backends.find(id);
    if (it == state.backends.end()) continue;
    if (it->second.is_draining) continue;
    live_backends.push_back(id);
}
```

If the filtering criteria ever change (e.g., adding a weight=0 check), all three
sites must be updated in lockstep. This is a latent inconsistency risk.

**Suggestion:** Extract a `ShardLocalState::get_live_backends()` helper that returns
the filtered vector. All three callers use it. `get_random_backend()` would need a
slight variant that also collects weights/priorities, but the core filtering can
still be shared.

---

## 4. `std::map` Allocation on Hot Path in `get_random_backend()` (Performance — Medium)

**Location:** `src/router_service.cpp:1102`

```cpp
std::map<uint32_t, std::vector<std::pair<BackendId, uint32_t>>> priority_groups;
```

`std::map` performs a heap allocation per entry (red-black tree nodes). This is
constructed on every call to `get_random_backend()`, which runs on the hot path for
RANDOM routing mode and as a fallback in fail-open mode.

With typical deployments having 1-3 priority groups and <100 backends, a simpler
approach would avoid all heap allocations.

**Suggestion:** Since priority groups are typically very few (1-3), use a sorted
`std::vector<std::pair<uint32_t, ...>>` or `absl::flat_hash_map` (already in deps).
Better yet, since you only need the *highest priority* group (lowest number), a
single-pass scan that tracks the minimum priority and its backends would eliminate
the need for any grouping data structure entirely.

---

## 5. `handle_node_state_change` Hardcodes Weight on ACTIVE Restore (Correctness — Medium)

**Location:** `src/router_service.cpp:2172`

```cpp
it->second.weight = 100;  // Restore default weight
```

When a peer transitions back to ACTIVE after DRAINING, the weight is unconditionally
set to 100. If the backend was originally registered with a custom weight (e.g., 50
for a smaller GPU or 200 for a larger one), that weight is permanently lost. The
comment acknowledges this: "could be enhanced to store original weight."

**Suggestion:** Store the original weight before setting it to 0 during DRAINING
(e.g., add an `original_weight` field to `BackendInfo`). Alternatively, since the
draining check via `is_draining` already skips draining backends in
`get_random_backend()` and `get_backend_for_prefix()`, setting `weight = 0` during
DRAINING is redundant — the `is_draining` flag is sufficient. Removing the
`weight = 0` assignment eliminates this bug entirely.

---

## 6. `std::atomic` Unnecessary for Shard-Local Counter (Performance — Low)

**Location:** `src/router_service.cpp:73`

```cpp
std::atomic<uint64_t> active_requests{0};
```

`BackendInfo` lives inside `ShardLocalState`, which is `thread_local`. In Seastar's
cooperative threading model, `active_requests` is only ever accessed from a single
reactor thread. The `BackendRequestGuard` is documented as "Shard-local operation
only." Since there is no concurrent access, `std::atomic` with relaxed ordering is
functionally correct but architecturally misleading and adds unnecessary overhead:

- Forces atomic load/store instructions (on ARM these carry fencing implications).
- Requires ~40 lines of manual copy/move boilerplate (lines 87-131) because
  `std::atomic` isn't copyable/movable.
- Signals to readers that cross-thread access is expected, which is false.

**Suggestion:** Replace `std::atomic<uint64_t>` with plain `uint64_t`. This
eliminates the copy/move boilerplate (compiler-generated defaults work), removes
architectural confusion, and `BackendInfo` shrinks from 5 special member functions
to 1.

---

## 7. Fire-and-Forget `unregister_backend_global` in Draining Reaper (Correctness — Low-Medium)

**Location:** `src/router_service.cpp:2111`

```cpp
(void)unregister_backend_global(id);
```

When the draining reaper finds a backend whose drain timeout has expired, it calls
`unregister_backend_global()` and discards the future. If the unregistration fails
(e.g., a shard is overloaded and `submit_to` times out), the backend remains
registered but its drain timeout has passed. There's no retry, no error logging, and
no way to detect the failure.

This could leave ghost backends in the routing table — backends that are draining,
past their timeout, but never cleaned up.

**Suggestion:** Attach a `.handle_exception()` continuation that logs the failure
(per Rule #9). Ideally, the reaper should track which backends it has attempted to
remove and retry on the next tick if the future failed.

---

## Summary Table

| # | Dimension       | Severity  | Location              | Issue                                          |
|---|-----------------|-----------|-----------------------|------------------------------------------------|
| 1 | Correctness     | High      | `cpp:1271,1337`       | Modular hash != consistent hash                |
| 2 | Correctness     | Medium    | `cpp:1050`            | `cache_hit` misreported for hash fallbacks     |
| 3 | Maintainability | Medium    | `cpp:1092,1184,1307`  | Triplicated backend filtering logic            |
| 4 | Performance     | Medium    | `cpp:1102`            | `std::map` heap allocs on hot path             |
| 5 | Correctness     | Medium    | `cpp:2172`            | Weight=100 hardcoded on ACTIVE restore         |
| 6 | Performance     | Low       | `cpp:73`              | Unnecessary `std::atomic` for shard-local data |
| 7 | Correctness     | Low-Med   | `cpp:2111`            | Fire-and-forget unregister, no error handling  |
