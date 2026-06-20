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

### Q4 — Membership-tier preservation: **B-as-superset** (resolved 2026-06-20)

An earlier draft proposed a *single per-digest flag* with the verified digest
**filtered** to resident hashes only. That conflicts with the stated invariant
"`holders` — P2 behavior, unchanged": on a native-KV fleet a filtered digest drops
*hot-but-evicted* prefixes from membership, shifting the shipped `sole_held` /
`ranvier_sole_held_hot_prefixes` gauge and collapsing the useful
"hot-and-routed-but-nobody-resident" signal to `holders=0` (indistinguishable from
"not hot").

**Resolved: the wire carries the full membership set as superset, plus an optional
verified-resident subset.** This:

- preserves P2 `sole_held` byte-for-byte everywhere (membership = full hot top-K);
- adds strictly more signal — *sole-held & resident*, *sole-held but evicted*, and
  *multiply-routed, singly-resident* are all distinguishable;
- keeps the A/B choice a **runtime detail, not a one-way door**: filtering the
  membership digest before broadcast (the old "A") is expressible as a future
  per-node config knob (`cluster.residency_filter_membership`, default off) *without
  a wire change*, because the full set is always transmitted. The reverse (A's
  1-bit wire → B) would require a wire extension.

The verified subset is bounded by the membership set (≤ K), so Rule #4 holds.

## Wire format — membership array + verified-resident subset

`HOT_PREFIX_DIGEST (0x07)` keeps its existing membership hash array unchanged
(P2 wire, full hot top-K) and **appends an optional verified-resident subset** via
the forward-compat tail the packet already carries (parent doc §"New wire format"):

- **Verified subset present:** sender's `self_backend_id` had fresh native-KV trust;
  the subset lists those membership hashes confirmed resident in
  `prefix_hash_index`. Every hash in the subset is also in the membership array
  (subset ⊆ membership).
- **Absent (old peers, non-native backends, trust-suspended):** membership only —
  the digest contributes to `holders` but never to `verified_holders` (back-compat;
  an old peer's tail-less digest deserializes cleanly as "no verified subset").

Encoding (resolved in 5b): the subset rides the tail as a **bitmap over the
membership array** — `ceil(count/8)` bytes, bit `i` (LSB-first) set ⇒
`prefix_hashes[i]` is verified. Chosen over an index-list because it needs no
per-element validation (bits past `count` are ignored), is most compact in the
common "mostly-resident" case (`count/8` bytes regardless of how many are set),
and is bounded by `ceil(MAX_HASHES/8) = 16` bytes. The bitmap is emitted only when
the verified subset is non-empty, so a membership-only window stays byte-identical
to an old v1 (tail-less) peer. `PROTOCOL_VERSION` bumped to 2; version is recorded
but never a rejection boundary (capability is length-detected), so v1 readers
ignore the tail and v1 senders simply omit it. A partial/truncated tail is ignored
(membership stays authoritative) rather than rejecting the digest.

## Index change

`CacheTopologyIndex` records, per `(hash, holder)`, a **two-state contribution**
— *member* (always) and *verified* (resident subset) — verified ⊆ member:

- `apply_digest(node, membership_hashes, verified_hashes, now)` — `verified_hashes`
  ⊆ `membership_hashes`; empty when the holder sent no verified subset.
- `holder_count(hash)` — unchanged (membership) for the observability tier.
- `verified_holder_count(hash)` — holders whose contribution marked `hash` verified.

Same bounds/eviction/age-out as today: both states are per-node, so they follow the
existing per-node set-replace and peer-death/TTL paths (the verified set is stored
alongside the membership set per holder and dropped together).

## Exposure

- `/v1/cache/topology`: each entry gains `verified_holders` and `verified_sole_held`
  (alongside the existing `holders`/`sole_held`).
- New gauge `ranvier_sole_held_verified_hot_prefixes` (count of this node's hot
  top-K that are verified-sole-held) — the automated-reaping signal. Mirrors the
  existing `ranvier_sole_held_hot_prefixes` plumbing.
- The DEGRADED-quorum freeze applies to the verified tier too (never assert
  `verified_sole_held=false` during split-brain).

## Phased breakdown

**Status: 5a–5c landed on `main` (PRs #587–#589); 5d completes the tier.** Once 5d
merges the residency-verified holder tier is wired end to end — seam → wire → index
→ exposure — gated behind the existing P2 `cache_topology_enabled` dark-launch flag.

| Sub-phase | Deliverable | Files | Size |
|---|---|---|---|
| **5a** | Shard-0 `RouterService` residency-query seam — `verified_resident_subset(self_id, hashes)`: when `self_id` has fresh native-KV trust, return the subset of hashes present in `prefix_hash_index`; empty otherwise. Inject as a getter into `start_emitter`; each window compute the subset and expose a **local** `ranvier_hot_prefix_verified_resident` gauge. **Membership digest and `sole_held` untouched** (B-as-superset). | `telemetry_service.{hpp,cpp}`, `router_service.{hpp,cpp}`, `application.cpp` | S–M |
| **5b** | Carry the verified-resident subset on `HOT_PREFIX_DIGEST` as a bitmap tail (v2); thread it through the **send** path (telemetry → router → gossip_service → protocol); `serialize`/`deserialize` round-trip. Receive-side `deserialize` populates the field; the live dispatch still forwards membership only (5c wires receive→index). Old/v1 peers decode as "no subset." | `gossip_protocol.{hpp,cpp}`, `gossip_service.*`, `router_service.{hpp,cpp}`, `telemetry_service.*`, `application.cpp`, `tests/unit/gossip_hot_prefix_digest_test.cpp` | S–M |
| **5c** | Widen the receive callback + `apply_peer_digest` to forward `verified_hashes`; two-state per-`(hash, holder)` (`member` + `verified`) in `CacheTopologyIndex`; `apply_digest(node, membership, verified, now)` + `verified_holder_count`; reactor-free tests. | `gossip_protocol.hpp`, `router_service.cpp`, `telemetry_service.*`, `cache_topology_index.hpp`, `tests/unit/cache_topology_index_test.cpp` | M |
| **5d** | `verified_holders`/`verified_sole_held` in the JSON + `ranvier_sole_held_verified_hot_prefixes` gauge; DEGRADED freeze applies. | `telemetry_schema.hpp`, `telemetry_service.cpp`, `tests/unit/telemetry_sink_test.cpp` | S |

Optional follow-up (not in 5a–5d): `cluster.residency_filter_membership` knob to
filter the membership digest before broadcast (the old "A"); default off. No wire
change — the full set is already transmitted.

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
