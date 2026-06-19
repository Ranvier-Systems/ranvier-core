# Cache-Aware Autoscaler Telemetry

**Status:** Largely implemented (P0–P2 shipped & merged 2026-06-19); see Status
Snapshot. This document remains the design rationale of record — the §6 code
sketches are illustrative API shape, not a line-for-line match to the merged code.
**Date:** 2026-06-17 (proposal); status updated 2026-06-19
**Author:** Generated exploration

## Status Snapshot

| Piece | Status | Notes |
|---|---|---|
| Per-replica KV %, running/queued, load score, throughput | ✅ already exported | `backend_vllm_*{backend_id}` on `:9180` (metrics_service.hpp) |
| Per-replica / fleet effective cache capacity | ✅ already exported | `backend_effective_cache_*{backend_id}`, `fleet_effective_cache_*_total` (health_service.cpp) |
| Per-replica prefix hit rate | ✅ shipped (P0) | `BackendMetrics` hit/attempt gauges (§6.A/B) |
| Per-replica resident working set | ✅ shipped (P1) | per-backend resident-route gauge in RouterService (§6.C) |
| Hot-prefix top-K + concentration | ✅ shipped (P1) | bounded `StreamSummary` + shard-0 merge (§6.D/E/F) |
| `GET /v1/cache/topology` snapshot | ✅ shipped (P1) | bounded JSON on `:9180`, auth-gated (§6.G) |
| Cluster-wide sole-holder index | ✅ shipped (P2) | `HOT_PREFIX_DIGEST` (0x07) + shard-0 `CacheTopologyIndex`; `sole_held`/`holders` in the JSON + `ranvier_sole_held_hot_prefixes` / `hot_prefix_sole_held_request_share` gauges. Gated by `enable_cache_topology` (default off) + DEGRADED-quorum freeze. **Operator-observability only** until the P3 residency gate — see below |
| Residency-verified holders (accuracy gate) | ⏭ proposed (P3) — **scoped**, see [residency-verification design](cache-topology-residency-verification.md) | intersect digest with native-KV `prefix_hash_index` before any automated reaping (Phase 5; the asymmetry below) |

### P2 prerequisites — shipped (2026-06-19)

Both prerequisites that gate when `sole_held` can be trusted have landed:

- **`enable_cache_topology` dark-launch flag (default off)** — `ClusterConfig`
  flag (env `RANVIER_CLUSTER_ENABLE_CACHE_TOPOLOGY`, YAML
  `cluster.enable_cache_topology`). When off the cluster sole-holder path is dark:
  no `HOT_PREFIX_DIGEST` gossip is emitted, received digests are ignored, and
  `/v1/cache/topology` + the sole-held gauges omit the holders/sole_held/quorum
  surface (the endpoint reverts to its P1 shape — it never emits a misleading
  `sole_held=false` from an empty index). Change requires restart.
- **DEGRADED-quorum freeze** — while `GossipConsensus` quorum is `DEGRADED`,
  `sole_held` never reads `false`: every hot prefix is treated as sole-held and
  the JSON carries `"quorum":"DEGRADED"`. This is the fail-safe response to the
  split-brain FN (a stale-but-unevicted peer masking a true sole-holder).

**Still operator-observability only.** Even with both gates, `sole_held` is *not*
yet safe for **automated reaping** — that waits on the P3 residency accuracy gate
below (route-membership alone can false-negative; see the asymmetry analysis).

## Problem Statement

Ranvier is an L7 LLM traffic controller that routes inference requests by token
prefix (ART + consistent-hash fallback) to keep backend KV-caches warm. A
serverless GPU platform scales replicas up/down, but today's autoscalers are
cache-**blind**: they scale on GPU utilization / queue depth and treat replicas
as fungible. Ranvier already knows the prefix→replica mapping and per-prefix hit
behavior — exactly the signal a cache-aware scaler needs to (a) drain the
**coldest** replica rather than a hot one, and (b) pre-warm / pin replicas for
hot prefixes.

This document does **not** propose building the scaler. It inventories the
telemetry Ranvier already has, identifies what's missing, and proposes a bounded
export surface to feed an external scaler — respecting the Hard Rules
(lock-free metrics #1, bounded containers #4, cross-shard memory #14, reactor
stalls #17, metrics-lambda teardown #6).

## What the scaler asks vs. what Ranvier knows

| Signal | Already exists? | Source | Gap |
|---|---|---|---|
| Per-replica KV-cache usage % | ✅ exported | `metrics_service.hpp` `backend_vllm_cache_usage{backend_id}`; scraped `health_service.cpp` `vllm:gpu_cache_usage_perc` | none |
| Per-replica running / queued | ✅ exported | `backend_vllm_requests_running/waiting{backend_id}` | none |
| Per-replica composite load score | ✅ exported | `backend_vllm_load_score{backend_id}`; `vllm_metrics.hpp::load_score()` | none |
| Per-replica prefill/decode throughput | ✅ exported | `backend_vllm_prompt/generation_throughput{backend_id}` | warmth proxy only |
| Per-replica effective cache capacity/usage | ✅ exported | `backend_effective_cache_*{backend_id}` (compression-aware) | none |
| Fleet effective cache capacity/usage | ✅ exported | `fleet_effective_cache_*_total` (health_service.cpp) | none |
| Global cache hit/miss + ratio | ✅ exported | `cache_hit_ratio`, `cache_hits/misses_total` | **global only**, not per-replica |
| Per-replica cache hit rate | ⚠️ partial | hits/misses global (`router_service.cpp`); route→backend known per request | needs `{backend_id}` hit/attempt pair |
| ART residency / route-table size / memory | ✅ exported | `routes_total`, `radix_tree_bytes`; `radix_tree.hpp::route_count()/estimate_memory_bytes()` | per-shard count, not per-replica tokens |
| Per-prefix request rate (hotness) | ❌ missing | `radix_tree.hpp` leaf has `last_accessed` (LRU) but **no access counter** | Gap #1 |
| Hot-prefix top-K set | ❌ missing | — | Gap #1 |
| Per-replica working-set size (tokens) vs KV capacity | ⚠️ partial | `routes_total` (count); `backend_effective_cache_capacity` (bytes) | not joined |
| Which replicas hold prefix P | ⚠️ partial, wrong scope | `router_service.cpp` `prefix_hash_index: hash→set<BackendId>` | per-shard, **eviction-cleanup only**; never aggregated |
| Sole-holder of a hot prefix (unsafe to reap) | ❌ missing | gossip `CACHE_STATE` / `CACHE_EVICTION` carry per-backend usage + (hash,backend), never assemble prefix→{nodes} | Gap #2 |
| Per-node DRAINING state | ✅ gossiped | `gossip_protocol.hpp` `NodeStatePacket` | mode only, no load/capacity |

**Summary:** replica-level telemetry is largely solved. The missing half is
*prefix-level* intelligence — hotness and holder-cardinality — which is exactly
what separates a cache-aware scaler from a load-aware one.

## Export surface — two surfaces, one port

The brief prefers extending Prometheus on `:9180` over a new endpoint. That holds
for everything **replica- or scalar-keyed**: low, *stable* cardinality
(`backend_id`, bounded by `MAX_TRACKED_BACKENDS`), and the scaler wants these as
time series for trend/derivative (warmth accumulation, drain-safety over time).

It does **not** hold for "hot prefix P lives on replica R." Prefix identity as a
Prometheus label is unbounded **and churning** — the top-K set rotates every few
scrape windows, producing series explosion and staleness that Prometheus handles
badly, and it contradicts the Rule #4 discipline applied everywhere else. A
scaler making pin/drain decisions wants a **point-in-time bounded snapshot**, not
thousands of churning gauges. Serve that as a small bounded JSON pull on the same
`:9180` listener.

### Prometheus additions (cardinality-safe)

```
# Per-replica prefix cache effectiveness (label: backend_id, bounded by MAX_TRACKED_BACKENDS)
ranvier_backend_prefix_hits_total{backend_id}        counter
ranvier_backend_prefix_attempts_total{backend_id}    counter   # ratio = warmth proxy

# Per-replica resident working set
ranvier_backend_resident_routes{backend_id}          gauge     # routes pinned to this backend
# working_set_ratio derived PromQL-side:
#   ranvier_backend_resident_routes / ranvier_backend_effective_cache_capacity

# Hot-prefix concentration (label-free → zero cardinality risk)
ranvier_hot_prefix_top1_request_share                gauge
ranvier_hot_prefix_topk_request_share                gauge
ranvier_hot_prefix_distinct_estimate                 gauge

# Drain-safety summary (label-free)
ranvier_sole_held_hot_prefixes                       gauge
ranvier_topk_snapshot_age_seconds                    gauge     # cf. router_gpu_load_cache_age_seconds
```

### Bounded JSON snapshot — `GET /v1/cache/topology`

Returns the merged top-K (length ≤ K, hard cap), auth-gated by the existing
`metrics_auth_handler`. `prefix_fp` is the existing `prefix_hash` truncated —
never token text. `sole_held: true` is the single bit that makes a replica
unsafe to reap.

```jsonc
{
  "snapshot_age_ms": 1873,
  "k": 128,
  "hot_prefixes": [
    { "prefix_fp": "a93f…", "req_rate_est": 412, "holders": [3],     "sole_held": true  },
    { "prefix_fp": "0c11…", "req_rate_est": 388, "holders": [1,4,7], "sole_held": false }
  ]
}
```

### Capping the unbounded signal to top-K (Rule #4)

Per-prefix request rate is unbounded. Use a **Space-Saving / Stream-Summary**
counter, per shard, with fixed `K` (propose 128):

- Fixed array of K `(prefix_hash, count, error)` slots — **bounded by
  construction** (no `push_back`, no MAX_SIZE check needed).
- Hot-path update is O(1) with the Stream-Summary linked-bucket variant
  (increment if tracked; else evict current-min, inherit its count as error).
  **Reuse the `prefix_hash` already computed for routing** — zero extra hashing.
- Approximate, with bounded over-estimation — correct semantics for "which
  prefixes are hot" (the scaler needs the set, not exact counts).
- A naive O(K) min-scan on every miss runs on the per-request path and must be
  avoided (Rule #17); use the O(1) Stream-Summary structure.

## Aggregation path (per-shard → node) and hot-path cost

> **Implementation note (2026-06-18 spike):** the per-shard→shard-0 aggregation
> described below is **already built** as `TelemetryService` (`telemetry_service.{hpp,cpp}`,
> `seastar::sharded<>`, off by default): bounded per-shard buckets + `_overflow`
> sentinel (#4), `foreign_ptr<ShardSnapshot>` gather (#14), gate + shard-0
> window-emitter timer (#5), forward-compat append-only schema, pluggable sink.
> Prefer **extending it** — add the hot-prefix top-K as a window-level aggregate
> (a sibling of `window_eviction_churn` on `ShardSnapshot`/`WindowReport`) — over
> standing up the standalone aggregator the steps below imply. Per-backend hit
> rate stays in `metrics_service` (its buckets are content-free, not per-`BackendId`).
> See BACKLOG §21 "Spike findings".

Per-shard ART + Space-Saving summaries are shard-local (Rule #8). The scaler
wants a node view. **Do not aggregate on scrape** — a gauge lambda fanning out to
N shards would stall the reactor on every scrape (Rule #1/#17). Use the
established **refresh-into-cached-snapshot-on-a-timer** pattern — which is exactly
what `TelemetryService`'s shard-0 window emitter already does (and which
`shard_load_balancer.hpp::refresh_all_snapshots()` / the `gpu_load_cache` /
`*_age_seconds` gauges also follow).

1. **Hot path (per request):** one O(1) increment into the shard-local
   Space-Saving summary, keyed by the already-computed `prefix_hash`. No locks,
   no atomics (shard-local, Rule #1). This is the only hot-path cost.
2. **Background timer (every 2–5s), shard 0:** `smp::submit_to` each shard to
   fetch its K-slot summary. **Rule #14:** the shard wraps its copy in a
   `foreign_ptr` (or returns a trivially-copyable fixed array by value, as
   `ShardLoadSnapshot` does); shard 0 re-allocates locally before merging. Merge
   K-summaries → still bounded at K. Yield over any route loop (Rule #17).
3. **Cluster merge (shard 0, gossip):** to compute `sole_held`, join the
   node-local hot set against peers'. Gossip today carries per-backend
   `CACHE_STATE` and per-(hash,backend) `CACHE_EVICTION` but never assembles
   prefix→{nodes}. Minimum viable: gossip only the top-K prefix hashes each node
   holds (bounded by K, fits Rule #4 and the existing `MAX_TOKENS`/dedup wire
   discipline); shard 0 builds a transient `hash → set<node>` of size ≤ K·N.
4. **Scrape / JSON read:** lock-free read of the shard-0 cached snapshot.
   `ranvier_topk_snapshot_age_seconds` exposes staleness.

| Stage | Cost | Reactor risk |
|---|---|---|
| Per-request top-K increment | O(1), reuses existing hash | none (shard-local, lock-free) |
| Per-shard gather (timer) | O(shards · K), off hot path | bounded; foreign_ptr + local copy (#14); yield (#17) |
| Cluster gossip of top-K hashes | O(K) per node per round, bounded wire | uses existing bounded/dedup gossip path |
| Prometheus scrape | O(1) read of cached snapshot | none — never triggers the gather |

## Biggest risk / open question

**Sole-holder detection requires a cluster-wide prefix→node index that does not
exist, and the naive way to build it breaks the Hard Rules.**

Draining the *coldest* replica is essentially solved (per-replica cache %, load,
effective capacity all exist). The hard part is *not reaping the last warm copy
of a hot prefix* — which needs prefix→{nodes} cardinality. Today that is:

- per-shard, not even node-level (`prefix_hash_index` is eviction-cleanup only), and
- never assembled across the cluster — gossip moves per-backend cache *usage* and
  individual *evictions*, but no node learns "I am the only holder of P."

The naive fix — gossip full per-prefix residency — is an unbounded, high-churn
firehose that violates Rule #4 and stalls on Rule #17. The proposed mitigation
(gossip only each node's top-K hashes, bounded by K) makes it tractable, but
makes **sole-holder itself approximate**: a prefix hot on node A but
cold-and-resident on node B is reported `sole_held` if B didn't rank it in its
top-K. **Open question: is an approximate, top-K-bounded sole-holder signal safe
enough to gate replica reaping on, or does drain-safety demand an exact (and far
more expensive) residency exchange?** That trade-off is the core design
decision — resolved in "Sole-holder index — scoping" below.

**Secondary, concrete trap:** the existing `backend_vllm_*` gauge lambdas call
`_health_service->get_vllm_metrics(backend_id)`, reading HealthService's
**shard-0-only** map. The v2.0.0 hard-rules audit notes this is "safe ONLY if
MetricsService runs on shard 0." Any new per-backend or merged gauge added here
inherits that constraint — register/read the cached snapshot on the owning shard
only, or it becomes a Rule #14 cross-shard read race.

## Sole-holder index — scoping

Scoping the open question above changes its risk assessment. The key insight is
that **the error is asymmetric**, which is what makes an approximate, bounded
index acceptable to build.

### The asymmetry

`sole_held = true` means "unsafe to reap this node." The two error directions are
not equally bad:

- **False positive** (report sole-held when ≥2 nodes hold P warm): scaler is
  *over*-conservative → keeps a replica it could have reaped → costs scaling
  efficiency, **not** correctness or latency.
- **False negative** (report *not* sole-held when exactly one node holds P warm):
  scaler reaps the last warm copy → cold-start prefill penalty (the 5–15s the
  system exists to avoid).

Top-K truncation and dropped unreliable broadcasts both tend to *omit* holders,
which inflates apparent sole-ownership → false positives → the safe direction.
**Design rule: the index must err toward omitting holders, never inventing
them.** The one construction that violates this is stale route-membership (below).

### Holder semantics — the Phase-0 decision

| Definition | Cost | Accuracy |
|---|---|---|
| (a) Announced/route membership — N has a `P→N` route | cheap; data already gossiped | routes are LRU-sticky; a node can be a "holder" of a prefix it already evicted (the staleness `push-cache-eviction-notifications.md` fights) |
| (b) Residency-verified — N has P warm *now* via `residency_weight` / native `BlockStored` | more moving parts | accurate |

(a) alone breaks the asymmetry: a stale route makes a truly-sole prefix look
*multiply*-held (the evicted node still appears) → **false negative → unsafe**.
Therefore: **(a) is fine for operator-facing observability; gating *automated*
reaping must wait for (b).** That gate is the load-bearing conclusion of this
scoping.

### Phased plan

| Phase | Deliverable | Files | Hard-Rule traps | Size |
|---|---|---|---|---|
| **0** | Decide holder semantics + freshness SLA (recommendation above) | — | — | design |
| **1** | Per-node digest source = `_topk_snapshot` hash set | reuse §E (this doc) | none new | S |
| **2** | `HOT_PREFIX_DIGEST = 0x07` packet: serialize/deserialize, `is_known_packet_type`, broadcast + handler callback | `gossip_protocol.{hpp,cpp}`, `byte_order.hpp` | #4 (bound payload to K), big-endian + append-only enum + forward-compat tail, #9 (log deserialize failures) | M |
| **3** | Shard-0 cluster index `hash → NodeSet`: per-node replacement, TTL, peer-death eviction, bounded + overflow counter | new `cache_topology_index.hpp` + wire into `gossip_service`/consensus | #4 (cap + overflow metric), #17 (yield merging K·N), #14 (shard-0-only; foreign_ptr if distributed), #5 (broadcast timer gate) | M |
| **4** | Expose: `sole_held`/`holders` in `/v1/cache/topology` JSON + `ranvier_sole_held_hot_prefixes` gauge (stubbed in §F) | aggregator service | #6 (deregister gauge first), shard-0 read trap | S |
| **5** | **Residency-verified holders** — intersect digest membership with native-KV `prefix_hash_index`; required before automated reap-gating. **Scoped: [residency-verification design](cache-topology-residency-verification.md)** (Q1 sidecar/`self_backend_id`, Q2 native-KV-only, Q3 trust fallback; sub-phases 5a–5d) | `telemetry_service`, `router_service`, `gossip_protocol`, `cache_topology_index` | M–L |

MVP for **observability** = Phases 1–4. MVP for **gating a scaler's reaping** = +Phase 5.

### New wire format — `HOT_PREFIX_DIGEST` (0x07)

Periodic, idempotent, latest-value-wins → **unreliable broadcast, no ACK/seq_num**,
like CACHE_STATE (`gossip_protocol.hpp:164`). Append at enum ordinal `0x07`; add
to `is_known_packet_type()` (`gossip_protocol.hpp:59`).

```
[type:1=0x07][version:1][backend_id:4][count:2][ hash:8 × count ]   big-endian
```

- `count <= K`, enforced on serialize *and* deserialize (Rule #4 at the wire
  boundary, like `RouteAnnouncementPacket::MAX_TOKENS`).
- Per-node **set replacement**: a node's latest digest wholly replaces its prior
  contribution (no append → no unbounded growth).
- **MTU ceiling:** K=128 × 8B = 1024B + header ≈ 1032B. Under a 1500B MTU before
  DTLS/IP/UDP overhead — this caps gossiped K at ~128 independent of a larger
  local top-K. Beyond that: truncate hashes to 6B (collision risk — quantify) or
  chunk across packets using a chunk index in the version-reserved tail.
- **Forward-compat tail:** copy CACHE_STATE's `len >= PACKET_SIZE` contract
  (`gossip_protocol.hpp:156`) so a later version can append a per-hash weight byte
  (the Phase-5 residency signal) without a breaking change.

### Cluster index (shard 0)

```cpp
// Lives on shard 0 only (gossip home). Bounded — Rule #4.
class CacheTopologyIndex {
    absl::flat_hash_map<uint64_t, absl::flat_hash_set<BackendId>> _holders;  // hash -> nodes
    absl::flat_hash_map<BackendId, std::vector<uint64_t>> _by_node;          // O(1) per-node replace
    static constexpr size_t MAX_HOT_PREFIXES = 16384;   // ~ K · max_nodes ceiling
    uint64_t _overflow = 0;                             // ranvier_cache_topology_overflow_total
};
```

- **On digest receipt:** replace `_by_node[backend]`; diff old vs new set to
  update `_holders`. Yield every `kYieldInterval` (Rule #17) — diff is O(K).
- **On peer death:** hook the existing `RoutePruneCallback`
  (`gossip_consensus.hpp:82`) to drop the dead node's contribution. This is the
  freshness backbone; without it dead nodes inflate holder counts → the
  dangerous false-negative.
- **TTL:** age out a node whose digest hasn't refreshed within N intervals
  (belt-and-suspenders for the silent-peer case).
- **Bound:** at `MAX_HOT_PREFIXES`, reject new keys + bump overflow counter.
  Never evict a *live* holder to make room — that manufactures false sole-ownership.

### Failure-mode / accuracy budget

| Scenario | Index says | Reality | Direction | Mitigation |
|---|---|---|---|---|
| Hot only on A, in A's top-K | sole_held | sole | ✅ correct | — |
| Held by A+B, both report | not sole | not sole | ✅ correct | — |
| Held by A+B, B's digest dropped | sole_held | not sole | conservative (FP) | next broadcast corrects |
| Held by A+B, B evicted but route stale | not sole | sole | **dangerous (FN)** | **Phase 5 residency verification** |
| Held only by A, not in A's top-K | absent | sole but cold-ish | low stakes (not hot) | larger K; watch `hot_prefix_topk_request_share` |
| DEGRADED quorum | unreliable | unknown | both | don't assert `sole_held=false` while `QuorumState::DEGRADED` |

The one row that breaks safety (stale-route FN) is exactly what Phase 5 closes.

### Open decisions for the implementer

1. **Gossiped K vs MTU** — cap at ~128 with 8-byte hashes, or chunk for larger? (Recommend cap.)
2. **Reap-gating semantics** — is `sole_held` a hard veto or a weighted cost? Hard veto + conservative-FP bias can pin capacity (hot-but-singly-held replicas become un-reapable); weighted cost degrades gracefully. (Lean weighted.)
3. **DEGRADED-quorum behavior** — fail safe (freeze reaping) vs fail stale (serve last-known + staleness flag)? **Resolved: fail-safe, shipped 2026-06-19** — `sole_held` never reads `false` while quorum is `DEGRADED`; JSON carries `"quorum":"DEGRADED"`.
4. **Phase-5 residency threshold** — what `residency_weight` counts as "warm enough"? Same calibration as cache-headroom routing.

## Registration sketch (illustrative — not shipped)

> These show API shape and Rule compliance. Names, arg order, and Seastar
> overloads must be verified against the pinned Seastar at implementation time.

### A. `metrics_helpers.hpp` — extend `BackendMetrics`

```cpp
struct BackendMetrics {
    MetricHistogram latency;
    MetricHistogram first_byte_latency;

    // Cache-aware scaler signals (shard-local, lock-free — Rule #1).
    // hits/attempts ratio = per-replica prefix "warmth".
    uint64_t prefix_hits = 0;       // prefix-routed requests that matched a warm prefix on this backend
    uint64_t prefix_attempts = 0;   // prefix-routed requests dispatched to this backend

    bool registered = false;

    BackendMetrics()
        : latency(backend_latency_buckets())
        , first_byte_latency(backend_latency_buckets()) {}
};
```

### B. `metrics_service.hpp` — append to per-backend `add_group` block

```cpp
            seastar::metrics::make_counter("backend_prefix_hits_total",
                seastar::metrics::description("Prefix-affinity cache hits routed to this backend. ratio with attempts = per-replica warmth."),
                {{"backend_id", backend_id_str}},
                [&metrics] { return metrics.prefix_hits; }),

            seastar::metrics::make_counter("backend_prefix_attempts_total",
                seastar::metrics::description("Prefix-affinity routing attempts dispatched to this backend."),
                {{"backend_id", backend_id_str}},
                [&metrics] { return metrics.prefix_attempts; })
```

If the labeled `make_counter(name, desc, labels, fn)` overload is unavailable,
fall back to `make_gauge` exactly as `prefix_hits_by_compression_tier` does — the
codebase already documents that workaround. No Rule #6 change needed:
`_backend_metrics.clear()` already deregisters these before the structs are
destroyed.

Hot-path write (same site that feeds `_cache_hits`):

```cpp
// Rule #1: shard-local, lock-free.
if (auto* bm = metrics().get_or_create_backend_metrics(route.backend_id); bm) {
    ++bm->prefix_attempts;
    if (route.cache_hit) ++bm->prefix_hits;
}
```

### C. `RouterService` — per-backend resident-route gauge + hot-prefix counter

```cpp
// ShardLocalState additions (per-shard, lock-free — Rule #1, Rule #8).
struct ShardLocalState {
    // ... existing members ...
    absl::flat_hash_map<BackendId, uint64_t> resident_routes;   // ++ on insert, -- on evict/expire
    static constexpr size_t MAX_TRACKED_BACKENDS = 10000;       // Rule #4

    StreamSummary hot_prefixes{/*k=*/128};                      // bounded top-K (see §D)
};
```

```cpp
// Alongside routes_total. Per-shard; Seastar sums → cluster-total per backend.
for (auto backend_id : _registry.known_backend_ids()) {          // bounded set
    std::string id_str = std::to_string(backend_id);
    _metrics.add_group("ranvier", {
        seastar::metrics::make_gauge("backend_resident_routes",
            seastar::metrics::description("Routes resident in this node's ART pinned to this backend. working-set proxy."),
            {{"backend_id", id_str}},
            [this, backend_id] {
                auto& m = shard_local().resident_routes;
                auto it = m.find(backend_id);
                return it == m.end() ? 0.0 : static_cast<double>(it->second);
            })
    });
}
```

```cpp
// route_request(), prefix path, after a successful ART lookup.
// O(1) — Rule #17 safe (no scan). Reuses prefix_hash already computed for prefix_hash_index.
shard_local().hot_prefixes.touch(prefix_hash);
```

### D. `stream_summary.hpp` (new) — bounded top-K, reactor-free

```cpp
// Approximate top-K by frequency (Metwally et al., "Space-Saving").
// Fixed K slots → bounded memory, no growth site (Rule #4 by construction).
class StreamSummary {
public:
    explicit StreamSummary(size_t k) : _k(k) { _slots.reserve(k); }

    void touch(uint64_t key);              // O(1): inc if tracked, else evict min, inherit count as error
    uint64_t total() const { return _total; }

    struct Entry { uint64_t key; uint64_t count; uint64_t error; };
    std::vector<Entry> snapshot() const;   // sorted desc by count, size ≤ K

private:
    size_t _k;
    uint64_t _total = 0;
    std::vector<Entry> _slots;             // size ≤ _k, invariant enforced in touch()
    // (production: intrusive min-bucket list for O(1) eviction)
};
```

No Seastar deps → unit-testable per the `tests/unit/<name>_test.cpp` convention.

### E. Shard-0 merge timer (Rule #5 / #14 / #17)

```cpp
seastar::future<> Aggregator::refresh_topk_snapshot() {
    auto holder = _gate.hold();                                   // Rule #5: spans the whole chain

    StreamSummary merged{_k};
    for (unsigned s = 0; s < seastar::smp::count; ++s) {
        // Rule #14: shard s allocates, wraps in foreign_ptr; we copy LOCAL before use.
        auto fp = co_await seastar::smp::submit_to(s, [] {
            auto v = std::make_unique<std::vector<StreamSummary::Entry>>(
                RouterService::shard_local().hot_prefixes.snapshot());
            return seastar::make_foreign(std::move(v));
        });
        std::vector<StreamSummary::Entry> local(fp->begin(), fp->end());  // local alloc
        for (size_t i = 0; i < local.size(); ++i) {
            merged.merge(local[i]);
            if ((i & (kYieldInterval - 1)) == 0) co_await seastar::coroutine::maybe_yield();  // Rule #17
        }
    }

    _topk_snapshot = build_topology(std::move(merged), _peer_topk);  // sole_held join (§5 open risk)
    _topk_snapshot_built_at = std::chrono::steady_clock::now();
}
```

`stop()` closes `_gate` *before* cancelling the timer (Rule #5).

### F. Shard-0 aggregator gauges (label-free — `fleet_*` precedent)

Registered in the shard-0 service group (same site as `fleet_effective_cache_*_total`).
Label-free + shard-0-only ⇒ no per-shard double-count.

```cpp
_metrics.add_group("ranvier", {
    seastar::metrics::make_gauge("hot_prefix_top1_request_share",
        seastar::metrics::description("Fraction of routes hitting the single hottest prefix (0..1)."),
        [this] { return _topk_snapshot.top1_share; }),
    seastar::metrics::make_gauge("hot_prefix_topk_request_share",
        seastar::metrics::description("Fraction of routes hitting the tracked top-K prefixes (0..1)."),
        [this] { return _topk_snapshot.topk_share; }),
    seastar::metrics::make_gauge("hot_prefix_distinct_estimate",
        seastar::metrics::description("Distinct hot prefixes currently tracked (<= K)."),
        [this] { return static_cast<double>(_topk_snapshot.entries.size()); }),
    seastar::metrics::make_gauge("sole_held_hot_prefixes",
        seastar::metrics::description("Top-K hot prefixes resident on exactly one node — unsafe to reap that node."),
        [this] { return static_cast<double>(_topk_snapshot.sole_held_count); }),
    seastar::metrics::make_gauge("topk_snapshot_age_seconds",
        seastar::metrics::description("Staleness of the merged hot-prefix snapshot."),
        [this] {
            return std::chrono::duration<double>(
                std::chrono::steady_clock::now() - _topk_snapshot_built_at).count();
        })
});
```

Rule #6: `clear()` this group as the first action in the service's `stop()`,
before tearing down `_topk_snapshot` / peer index.

### G. `GET /v1/cache/topology` — bounded JSON on `:9180`

```cpp
// application.cpp, after prometheus::start(*_metrics_server, ...).
seastar::httpd::function_handler* topo =
    new seastar::httpd::function_handler([this](auto req, auto rep) -> seastar::future<...> {
        rep->_content = _aggregator.topology_json();   // cached snapshot only, <= K entries
        rep->done("json");
        return seastar::make_ready_future<...>(std::move(rep));
    });
_metrics_server->_routes.put(seastar::httpd::GET, "/v1/cache/topology",
    _metrics_auth.wrap(topo));   // same Bearer + IP allowlist as /metrics
```

`topology_json()` emits ≤ K objects (RapidJSON), reading the pre-built snapshot —
no fan-out on request, so it cannot stall the reactor (Rule #1/#17).

## Cardinality recap (Rule #4 closure)

| New series | Cardinality | Bound source |
|---|---|---|
| `backend_prefix_hits_total` / `_attempts_total` | ≤ `MAX_TRACKED_BACKENDS` per shard | existing `_per_backend_metrics` cap |
| `backend_resident_routes` | ≤ `MAX_TRACKED_BACKENDS` per shard | bounded `resident_routes` map |
| `hot_prefix_*`, `sole_held_*`, `topk_snapshot_age_seconds` | 1 each (label-free, shard-0) | no labels |
| `/v1/cache/topology` payload | ≤ K objects | fixed-K `StreamSummary` |

The only churning, high-cardinality data (prefix identity) never becomes a
Prometheus label — it stays in the K-bounded JSON snapshot.

## Implementation notes (as-built, 2026-06-19)

Extracted from the BACKLOG §21 working entry on closeout, so the backlog stays
focused on active work while the decisions that shaped the merged code live here.

**Decision spike (P0, 2026-06-18).**
- **`StreamSummary` K = 128**, shared as the gossip-digest cap so the local top-K
  and the wire digest use one constant. Per-shard memory is negligible.
- **Per-backend hit/attempt are gauges, not counters.** The reactor-free test stub
  only provides labeled `make_gauge`/`make_histogram` overloads (`make_counter`
  with a `{{"backend_id",…}}` label list does not deduce). Mirrors
  `prefix_hits_by_compression_tier` — `make_gauge` + lambda over a `BackendMetrics`
  field, named without `_total`.
- **Hit-attribution call-site** is the existing per-backend recording site
  (`record_backend_latency_by_id`, `http_controller.cpp`); `route_result.cache_hit`
  was already captured, so a `record_prefix_outcome_by_id(backend, cache_hit)`
  wrapper sits alongside it.

**Course-correction — reused `TelemetryService` for P1 aggregation** instead of a
new shard-0 aggregator. The §20 `TelemetryService` (`seastar::sharded<>`, off by
default) already implements the per-shard→shard-0 path: bounded per-shard buckets +
`_overflow` sentinel (#4), `foreign_ptr<ShardSnapshot>` gather (#14), gate + shard-0
window-emitter timer (#5), append-only schema, pluggable sink. The hot-prefix top-K
became a window-level aggregate on `ShardSnapshot`/`WindowReport` (sibling of
`window_eviction_churn`); per-backend hit rate stayed in `metrics_service` (the
telemetry buckets are deliberately content-free, not per-`BackendId`). Touch points:
`telemetry_service.{hpp,cpp}`, `telemetry_schema.hpp`, the seastar metrics stub.

**P2 sole-holder, as shipped.** Gossip `HOT_PREFIX_DIGEST` (0x07) → shard-0
`CacheTopologyIndex` (prefix→nodes, peer-death eviction + TTL age-out, bounded by
`MAX_HOT_PREFIXES`/`MAX_NODES` with overflow counters). Each telemetry window the
emitter self-applies this node's merged top-K (so we count as a holder of our own
hot prefixes — sole-holder math is wrong otherwise) and gossips it. The endpoint
lists *this node's* top-K, so `holders ≥ 1` and `sole_held` means "only we hold it"
— the per-node reaping signal a scaler unions across nodes. Surfaced as
`holders`/`sole_held` per JSON entry + `ranvier_sole_held_hot_prefixes` and
`hot_prefix_sole_held_request_share` gauges. Both P2 prerequisites (the
`enable_cache_topology` gate + the DEGRADED-quorum freeze) shipped — see "P2
prerequisites — shipped" at the top.

**Hard-Rule watch (held across the work).** Lock-free shard-local counters (#1);
bounded by construction — fixed-K `StreamSummary`, `CacheTopologyIndex` caps +
overflow metric (#4); broadcast/merge timer gate (#5); deregister gauges first in
`stop()` (#6); shard-0 reads of cross-shard/peer state, never from another shard's
lambda (#14); yield when merging K·N entries (#17).

**Overhead.** Microbench (`make bench-hot-prefix`, reactor-free): `touch()` 2.3 ns
warm / 72 ns evict at K=128; `hash_prefix` ~0.4 µs at 128 tokens, ~2 µs at 512 —
the hit-path hash is byte-bound. See
`docs/benchmarks/cache-topology-telemetry-overhead.md`. The end-to-end A/B release
gate (telemetry on vs off; routing-latency p50/p99 < 1% delta, no p99 spike at the
shard-0 window cadence) is **not yet run** — tracked in the backlog.

**On bounding the fingerprint hash (reclassified 2026-06-19).** The §21 follow-up
"cap the fingerprint to ~64 tokens" was scoped Low but is correctness-sensitive,
not a quick win. The fingerprint is already bounded on the fallback path
(`prefix_len = min(len, prefix_token_length)`, default 128). The only uncapped cost
is the `prefix_boundary` branch, which hashes the full declared/detected prefix *by
design* — so requests sharing a system message co-locate for cache reuse. A cap
there would degrade routing precision (distinct prompts sharing the first N tokens
collide) and must respect the KV-event ledger's **boundary-identity invariant**
(`kv_event_ledger.hpp`): the ledger reconstructs Ranvier's exact `prefix_hash` by
continuing an FNV accumulator over the full token sequence at each block boundary,
so any cap must be a `block_alignment` multiple applied consistently to lookup, both
learn paths, and the `X-Ranvier-Prefix-Hash` header, or native-KV route-learning
stops matching routing lookups. If pursued, do it as an opt-in
`prefix_hash_max_tokens` (default = current/unbounded), never a default change.

## Related

- `docs/architecture/push-cache-eviction-notifications.md` — the `prefix_hash`
  reverse index and `CACHE_EVICTION` gossip this proposal reuses.
- `docs/architecture/kv-cache-compression-integration.md` — the
  compression-aware effective-capacity signals already exported.
- `docs/internals/shard-load-balancing.md` — the snapshot-refresh pattern §4
  copies for cross-shard aggregation.
