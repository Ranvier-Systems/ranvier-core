# Elastic Backend Platform Integration

**Status:** Design — integration contract. Synthesizes the shipped
cache-aware telemetry (§21) with the still-open replica-addressability work into a
single producer/consumer contract. No code change proposed here; this is the
boundary spec the discovery and scaler-facing pieces implement against.
**Date:** 2026-06-20
**Author:** Generated exploration
**Parent:** [`cache-aware-autoscaler-telemetry.md`](cache-aware-autoscaler-telemetry.md)
(the telemetry surface this contract consumes)

## Purpose & scope

Define a contract for integrating Ranvier with an *Elastic Backend Platform* —
any system that dynamically creates and destroys GPU inference replicas. The
framing is deliberately abstract: Kubernetes with HPA/KEDA, spot / preemptible
GPU fleets, serverless GPU platforms, and bespoke schedulers are each one point
on an elasticity spectrum.

Ranvier's core value — prefix-affinity routing (ART + consistent-hash fallback)
that keeps backend KV-caches warm — both **depends on** such a platform (it must
expose addressable replicas with stable identity) and **informs** it (it exports
the cache-topology signals a cache-aware scaler needs). This document specifies
that two-way boundary.

**Non-goal — the scope boundary from §21 holds:** Ranvier *exports* telemetry and
*consumes* a replica registry. The autoscaler itself — reap/pin policy, replica
lifecycle, warm-pool sizing — lives in the platform. Reap-gating semantics in
particular are the platform's decision, not Ranvier's.

## The abstraction

```
   ┌──────────────────────────── Elastic Backend Platform ─────────────────────────────┐
   │  scheduler / autoscaler        logical replicas            GPU instances          │
   │       (reap/pin policy)   ┌── slot A ──┐ ┌── slot B ──┐   (vLLM / SGLang / …)     │
   └───────────▲───────────────│            │ │            │──────────▲────────────────┘
               │               └─────┬──────┘ └─────┬──────┘          │
   Leg 2: telemetry              Leg 1: register      Leg 1: register   Leg 3: native-KV
   (Ranvier → platform)          (platform → Ranvier) stable identity   residency events
               │                       │                  │            (backend → Ranvier)
   ┌───────────┴───────────────────────┴──────────────────┴──────────────────────────────┐
   │                                  R a n v i e r                                      │
   │   BackendRegistry → RadixTree (ART) routing → TelemetryService / CacheTopologyIndex │
   │   exports: Prometheus :9180  +  GET /v1/cache/topology                              │
   └─────────────────────────────────────────────────────────────────────────────────────┘
```

A **logical replica** (a *slot*) is a stable routing identity the platform binds
to a physical GPU instance and *rebinds* across instance churn. The slot — not the
instance — is Ranvier's `BackendId`. This decoupling is what lets prefix affinity,
per-replica time-series, and the sole-holder index survive autoscaling (see
Leg 1).

## Elasticity is a spectrum

The value Ranvier delivers, and the depth of integration that pays off, scale with
how elastic the platform is:

| Platform shape | Churn rate | Hardest problem | What Ranvier adds |
|---|---|---|---|
| Static reserved fleet | ~none | none | load-aware routing only |
| K8s + HPA/KEDA | per scale event / rollout | learned routes orphaned on rollout | affinity + cache-aware scale signals |
| Spot / preemptible | unpredictable | warm replica vanishes mid-flight | + reap-safety telemetry |
| Serverless GPU | per traffic burst (scale-to-zero) | replicas hidden, identity ephemeral | + verified reap-gating (needs all three legs) |

The legs and tiers below let a platform self-select how far right it sits.

## Three integration legs

| Leg | Direction | Purpose | Status in Ranvier today |
|---|---|---|---|
| **1. Replica registration & stable identity** | platform → Ranvier | expose addressable replicas with churn-stable `BackendId` | **partial** — discovery assumes durable backends; identity not churn-stable |
| **2. Telemetry consumption** | Ranvier → platform | feed the cache-aware scaler | **shipped** (§21 P0–P3) |
| **3. Native-KV residency** | backend → Ranvier | upgrade sole-holder to verified (automated reap-gate) | **shipped wire; opt-in** (`kv-events-port`, §21 Phase 5) |

The integration's center of gravity is **Leg 1** — Legs 2 and 3 are largely built;
Leg 1 is the prerequisite that makes their `{backend_id}`-keyed signals meaningful
on an elastic fleet.

## Capability tiers

A platform self-selects its tier by which legs it satisfies.

| Tier | Platform provides | Ranvier capability unlocked | Legs |
|---|---|---|---|
| **0** | addressable, ~durable replicas | load-aware routing | partial 1 |
| **1** | + **stable slot identity** across instance churn | prefix affinity **survives autoscaling** | 1 |
| **2** | + scrapes Prometheus `:9180` | drain-the-**coldest** replica (cache-aware scale-down) | 1, 2a |
| **3** | + reads `GET /v1/cache/topology` | sole-holder **reap-safety** (operator-observability) | 1, 2 |
| **4** | + forwards native-KV residency events | **verified** automated reap-gating | 1, 2, 3 |

Reference points (illustrative, not normative — a platform's tier is whatever
its legs satisfy, not what its category implies): K8s + KEDA reaches ~Tier 2 with
today's discovery; a serverless platform that exposes per-container slots and
KV-block events can reach Tier 4.

---

## Leg 1 — Replica registration & stable identity

**Today.** `BackendRegistry` (`src/backend_registry.hpp`) is fed by two discovery
sources — `K8sDiscoveryService` (EndpointSlice watcher) and `LocalDiscoveryService`
(port probe), both shard-0-only. A backend is identified by a `BackendId` derived
from a durable handle: `K8sEndpoint::to_backend_id()` hashes the pod UID (FNV-1a).
A backend's address is a `seastar::socket_address`. This works because pods and
local servers are relatively durable.

**Required of the platform** (to reach Tier 1+):

1. **Stable slot identity.** The platform supplies a `BackendId` (or a stable
   key Ranvier hashes to one) bound to a *logical* replica, which the platform
   rebinds to fresh instances across scale/restart/preemption. Without this, an
   instance replacement mints a new `BackendId`, which:
   - orphans every ART route learned for the old id (affinity cold-spell, acute
     during rolling deploys), and
   - breaks Prometheus time-series continuity — the scaler reads *derivatives*
     (warmth accumulation, drain-safety trend), which a churning series destroys, and
   - causes `CacheTopologyIndex` TTL / peer-death eviction to thrash.

   *Cheap conformance:* a static/HPA fleet can satisfy this with a
   `ranvier.io/slot-id`-style annotation (or StatefulSet ordinal) instead of the
   pod-UID hash. *Full conformance:* a serverless platform maintains an explicit
   slot→instance binding table. (Companion design: stable logical slots — to be
   written; tracked separately.)

2. **Endpoint descriptor.** Tier 0/1 platforms exposing replicas as URLs with TLS
   and token auth (rather than cluster-internal `host:port`) require generalizing
   the registry's `socket_address` to a richer endpoint descriptor (scheme, host,
   port, SNI, auth header). *This is a real Ranvier interface change, not yet made*
   — `BackendRegistry::get_backend_address()` and the register signature are
   `socket_address`-typed today. K8s-internal and local fleets do **not** need it.

3. **Lifecycle signals.** `ready` / `draining` / `dead` per replica, so
   `CacheTopologyIndex` peer-death eviction (hooked to `RoutePruneCallback`) drops
   a dead slot's contribution promptly — the freshness backbone that prevents the
   dangerous sole-holder false-negative.

**Contract shape.** A new discovery source (sibling to K8s/local) that drives the
existing `BackendRegisterCallback` / drain-callback pattern, supplying the
platform's stable slot id as the `BackendId`. The platform's scheduler *is* the
registry — no probing — so health comes from its lifecycle signals, not active
liveness checks (active probes against scale-to-zero replicas cause cold starts).

**Hard-Rule notes for the implementer:** shard-0-only like existing discovery
(Rule #14 cross-shard read trap on any per-backend map); bound the registry
(Rule #4, cf. `K8S_MAX_ENDPOINTS`); endpoint churn must not allocate unboundedly.

## Leg 2 — Telemetry consumption

Fully specified in the parent doc; summarized here as the platform-facing contract.
**Two surfaces, one port (`:9180`):**

- **Prometheus (replica-keyed, stable cardinality)** → *drain-the-coldest*:
  `backend_vllm_cache_usage{backend_id}`, `backend_vllm_load_score{backend_id}`,
  `backend_effective_cache_*{backend_id}`, `ranvier_backend_prefix_hits/attempts_total{backend_id}`,
  `ranvier_backend_resident_routes{backend_id}`. These are time-series; the scaler
  reads trends/derivatives.
- **`GET /v1/cache/topology` (bounded JSON snapshot, auth-gated)** → *reap-safety*:
  per-hot-prefix `holders` / `sole_held` (membership, observability) and
  `verified_holders` / `verified_sole_held` (residency-verified, Leg 3), plus
  `quorum`. `prefix_fp` is a truncated hash, never token text; node identities are
  never exported (`holders` is a scalar count).

**The contract the platform must honor** (the safety asymmetry is load-bearing):

- A **false positive** (`sole_held` when it isn't) is *safe* — the scaler keeps a
  replica it could have reaped → wasted capacity, self-correcting.
- A **false negative** (not `sole_held` when it is the last warm copy) is
  *dangerous* — the scaler reaps it → 5–15 s cold-start prefill, the latency the
  system exists to avoid.
- Therefore the platform must treat `sole_held: true` and `quorum: "DEGRADED"`
  **conservatively** (do not reap). Ranvier already biases this way (top-K omits
  rather than invents holders; DEGRADED freezes `sole_held` to `true`).
- **`sole_held` (membership) is observability-only.** Gate *automated* reaping on
  `verified_sole_held` (Leg 3) — membership alone false-negatives on stale routes.
- **Reap policy is the platform's.** Recommended (per §21 open decision #2): treat
  the signal as a **weighted reap-cost**, not a hard veto — a hard veto on
  conservatively-biased FPs can pin hot-but-singly-held capacity un-reapably.

Minimal scaler loop:

```
for replica in fleet:
    if scale_down_pressure and replica is coldest by cache_usage/load_score:
        topo = GET /v1/cache/topology
        if any hot prefix on `replica` has verified_sole_held and quorum == HEALTHY:
            raise reap-cost(replica)        # weighted, not veto
        else:
            reap(replica)
```

## Leg 3 — Native-KV residency

Required only for Tier 4 (verified automated reap-gating). Replicas emit native
KV-block residency events (e.g. vLLM `BlockStored` / SGLang equivalents) into
Ranvier's residency mirror, surfaced per the existing `ranvier.io/kv-events-port`
annotation (and optional `kv-events-replay-port`). Ranvier intersects route
membership with verified residency to populate `verified_sole_held` — the signal
that closes the stale-route false-negative. Full design and threshold calibration:
[`cache-topology-residency-verification.md`](cache-topology-residency-verification.md).

## Conformance checklist

- [ ] **Tier 1:** every registered replica carries a platform-stable `BackendId`
  that survives instance replacement (slot, not instance handle).
- [ ] **Tier 1:** replica lifecycle (`ready`/`draining`/`dead`) is pushed to
  discovery; no reliance on Ranvier active-probing.
- [ ] **Tier 1 (URL fleets only):** endpoint descriptor carries scheme/TLS/auth
  (requires the registry generalization above).
- [ ] **Tier 2:** scaler scrapes the replica-keyed Prometheus series and scales
  down by coldest, not round-robin.
- [ ] **Tier 3:** scaler reads `/v1/cache/topology`; treats `sole_held` and
  `quorum:DEGRADED` conservatively; uses a weighted reap-cost.
- [ ] **Tier 4:** replicas forward native-KV residency; scaler gates *automated*
  reaping on `verified_sole_held`, not membership `sole_held`.

## Open questions & non-goals

- **Endpoint-descriptor generalization** (Leg 1.2) is a genuine `BackendRegistry`
  change with blast radius into the connection path; unneeded for cluster-internal
  fleets. Scope it only when a URL-endpoint platform is the target.
- **Slot-identity minting** is platform-owned. Ranvier requires only determinism
  and stability across instance churn; *how* slots are minted/agreed (and held
  consistent across a Ranvier fleet via gossip) is the platform's design.
- **Multi-tenancy.** A multi-tenant platform must partition routing state per
  tenant — separate ART/top-K and `HOT_PREFIX_DIGEST` gossip scoping — so prefixes
  (and the prompts they fingerprint) never cross a tenant boundary. Ranvier is
  single-fleet today; per-tenant isolation is out of scope here and a prerequisite
  for any shared-tenant deployment.
- **Warmth vs. addressing.** Slots preserve the *routing decision* across churn;
  they do **not** preserve *cache warmth* — a rebound slot on a cold instance still
  pays prefill. Warmth preservation is the scaler's job (keep a hot slot pinned),
  fed by Leg 2. The two interlock; neither substitutes for the other.

## Related

- [`cache-aware-autoscaler-telemetry.md`](cache-aware-autoscaler-telemetry.md) —
  the telemetry surface (Leg 2) and the sole-holder asymmetry, in full.
- [`cache-topology-residency-verification.md`](cache-topology-residency-verification.md) —
  verified residency (Leg 3 / Tier 4).
- [`push-cache-eviction-notifications.md`](push-cache-eviction-notifications.md) —
  the `prefix_hash` reverse index and `CACHE_EVICTION` gossip the topology index reuses.
- [`routing-direction-2026.md`](routing-direction-2026.md) — strategic routing direction.
- `docs/internals/prefix-affinity-routing.md` — per-`BackendType` applicability
  (which backends learn / scrape / forward token IDs).
