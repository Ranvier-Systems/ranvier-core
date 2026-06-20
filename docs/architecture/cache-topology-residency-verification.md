# Cache-Topology Residency Verification (§21 Phase 5)

**Status:** Design — scoped, not yet implemented. Open decisions Q1–Q3 resolved
(below). Prerequisite for consuming `sole_held` to gate **automated** replica
reaping; P2 (membership-based) ships today as operator-observability only.
**Date:** 2026-06-19
**Parent:** [`cache-aware-autoscaler-telemetry.md`](cache-aware-autoscaler-telemetry.md)
(§"Sole-holder index — scoping" → Phase 5)

## Problem — membership ≠ residency

P2 reports a backend as a *holder* of prefix `P` when `P` is in that backend's
hot-prefix digest — i.e. it **routes `P` often**. "Routes often" is not "warm in
KV cache now": a backend under memory pressure can evict `P` while still routing
it. The dangerous error (per the parent doc's asymmetry analysis) is the **false
negative** — `P` looks multiply-held because an evicted backend still appears as a
holder, so a scaler reaps the true last warm copy → cold-start prefill cliff (the
5–15 s the system exists to avoid).

Phase 5 closes this by counting a backend as a holder only when `P` is
**verified-resident** in its KV cache, not merely frequently routed.

## The asymmetry (safety foundation)

`sole_held = true` means "unsafe to reap." The error directions are not equal:

- **False positive** (report sole-held when ≥2 hold `P` warm): scaler is
  over-conservative → keeps a reapable replica → costs efficiency, not correctness.
- **False negative** (report not-sole when exactly one holds `P` warm): scaler
  reaps the last warm copy → the cold-start cliff.

**Design rule (unchanged from P2): err toward *omitting* holders, never inventing
them.** Residency verification only ever *removes* holders (evicted or
un-attested backends drop out), which inflates apparent sole-ownership → the safe
(false-positive) direction.

## Infrastructure reused (no new residency subsystem)

Phase 5 is mostly wiring existing signals into the sole-holder index:

- **`prefix_hash_index`** (`router_service`) — *"an exact per-`(hash, backend)`
  residency mirror"* maintained by native-KV `UPSERT`/`REMOVE` (vLLM
  `BlockStored`→present, `BlockRemoved`→absent). This is the per-prefix residency
  oracle.
- **Verified-residency trust** (per backend) — native-KV `ALIVE` heartbeats keep a
  backend's residency "trusted"; `RESET` (sequence gap / decode fault) drops trust
  so routing falls back to the probabilistic gossip signal. The same routing
  fallback gates Phase 5's verified-vs-membership choice.
- **`residency_weight`** (`residency_cache`) — coarse per-backend gossip signal.
  Used by routing; **not** used by Phase 5's verified tier (see Q2).

## Resolved design decisions

### Q1 — Holder/attestation model: **sidecar / `self_backend_id`**

A Ranvier node routes to many backends (1:N), but its cluster *identity* is a
single `self_backend_id` (the only thing DRAINING, the gossip local-id, and the
P2 self-apply use). Phase 5 adopts the **sidecar mesh**: each node attests exactly
its own backend (`self_backend_id`) from its exact `prefix_hash_index`. The digest
stays per-node = per-backend — **the wire shape and the P2 emit path are
unchanged**, only residency-filtered.

- *Configurability:* no new knob. Which backends get *verified* (vs membership)
  attestation is already determined by the existing **per-backend `kv_events`
  opt-in**. A backend without native KV events falls back to membership.
- *Multi-backend (router-shard) topologies* (one node fronting N backends) are a
  **safe degradation**: the N−1 non-self backends are un-attested → omitted from
  the cluster index → over-conservative (false-positive direction), never
  dangerous. Full per-subscribed-backend attestation is a clean **additive
  follow-up** if a real router-shard deployment needs it (see "Deferred").

### Q2 — `residency_weight` role: **native-KV-only for the verified tier**

With attestation scoped to self-backend via the exact `prefix_hash_index`, the
coarse per-backend `residency_weight` adds no per-prefix precision. Keep it for
routing; keep the verified tier **exact** (native KV only). Simpler, and avoids a
muddy "partially-verified" state.

### Q3 — Trust-suspended transitions: **reuse the routing fallback**

On `RESET` (stream fault) a backend's verified residency is dropped. The emit path
reads the same per-backend native-KV trust freshness routing already consults; while
trust is suspended the digest reverts to **membership (unverified)** mid-stream
until `ALIVE`/`UPSERT` re-establishes trust. No new state.

## Confidence tiers

Two holder counts, exposed side by side:

- **`holders`** — membership (verified + unverified). P2 behavior, unchanged;
  drives the existing `sole_held` for **operator observability**.
- **`verified_holders`** — native-KV-confirmed-resident only. Drives a new
  `verified_sole_held`, the signal an autoscaler gates **automated reaping** on.

`verified_sole_held` is only meaningful on native-KV-enabled fleets; on mixed or
non-native fleets it stays conservative (more apparently-sole). This is a
documented limitation, not a bug — the safe direction.

## Wire format — `residency_verified` flag

`HOT_PREFIX_DIGEST (0x07)` gains a **single per-digest flag** via the forward-compat
tail the packet already carries (parent doc §"New wire format"). One bit suffices
because a *verified* digest is already filtered to resident hashes — every hash in
it is verified-resident on the sending backend.

- **Set:** sender's `self_backend_id` had fresh native-KV trust and the hash set was
  residency-filtered.
- **Clear / absent (old peers, non-native backends):** membership — treated as
  unverified (back-compat; an old peer's digest simply never counts toward
  `verified_holders`).

## Index change

`CacheTopologyIndex` records a **per-holder verified bit** alongside membership:

- `apply_digest(node, hashes, verified, now)` — store whether this node's
  contribution is residency-verified.
- `verified_holder_count(hash)` — count of holders whose contribution is verified.
- `holder_count(hash)` unchanged (membership) for the observability tier.

Same bounds/eviction/age-out as today (the verified bit is per-node, so it follows
the existing per-node set-replace and peer-death/TTL paths).

## Exposure

- `/v1/cache/topology`: each entry gains `verified_holders` and `verified_sole_held`
  (alongside the existing `holders`/`sole_held`).
- New gauge `ranvier_sole_held_verified_hot_prefixes` (count of this node's hot
  top-K that are verified-sole-held) — the automated-reaping signal. Mirrors the
  existing `ranvier_sole_held_hot_prefixes` plumbing.
- The DEGRADED-quorum freeze applies to the verified tier too (never assert
  `verified_sole_held=false` during split-brain).

## Phased breakdown

| Sub-phase | Deliverable | Files | Size |
|---|---|---|---|
| **5a** | Emit-time residency filter: if `self_backend_id` has fresh native-KV trust, intersect the hot top-K with `prefix_hash_index[hash].contains(self_backend_id)`; else membership. Add a shard-0 `RouterService` residency-query seam. | `telemetry_service.cpp`, `router_service.{hpp,cpp}` | S–M |
| **5b** | `residency_verified` flag on `HOT_PREFIX_DIGEST` (forward-compat tail); set on send, read on receive; `GossipService`/protocol plumbing. | `gossip_protocol.{hpp,cpp}`, `gossip_service.*` | S |
| **5c** | Per-holder verified bit in `CacheTopologyIndex` + `verified_holder_count`; reactor-free tests. | `cache_topology_index.hpp`, `tests/unit/cache_topology_index_test.cpp` | M |
| **5d** | `verified_holders`/`verified_sole_held` in the JSON + `ranvier_sole_held_verified_hot_prefixes` gauge; DEGRADED freeze applies. | `telemetry_schema.hpp`, `telemetry_service.cpp`, `tests/unit/telemetry_sink_test.cpp` | S |

MVP for gating automated reaping = 5a–5d. Each sub-phase is independently
mergeable; 5a+5b are inert until 5c/5d expose the tier.

## Hard-Rule considerations

- **#1 / #14:** the residency query (5a) reads `prefix_hash_index` on shard 0 only
  (the emit home) — never another shard's map. The seam must be a shard-0 read,
  like the existing `cache_topology_quorum_degraded()`.
- **#4:** no new unbounded state — the verified bit rides the existing per-node
  bounded sets; the wire flag is one bit.
- **#17:** the emit-time filter is O(K) over the top-K (already bounded), no new
  yield point.
- **#9 / forward-compat:** an old peer's flag-less digest must deserialize cleanly
  as unverified (len-tolerant, like `CACHE_STATE`).

## Deferred / out of scope

- **Per-subscribed-backend attestation** (Q1 Option 2): a node emitting verified
  digests for *every* natively-resident backend, for router-shard topologies. Adds
  a per-backend emit loop and a "who attests a shared backend" coordination
  question. Additive on top of this design; pursue only if a real multi-backend
  deployment needs full (non-degraded) attestation.
- **Hash-truncation / per-hash residency weights:** the parent doc's optional
  finer-grained residency (a weight byte per hash) is unnecessary under the
  per-digest-flag model and is not planned.
