# Cache-Residency-Aware Routing

**Status:** implemented. **Scope:** routing capability + its gossip plumbing.

## Problem

The router's Adaptive Radix Tree (ART) maps a token prefix to the backend that
last *served* it. It has no signal for whether that backend still *holds* the
prefix in its KV cache. Under cache pressure a "warm" route can be stale — the
backend evicted the prefix — so honoring the route pays a cache miss while we
believe we scored a hit. The symptom is that a sustained TTFT win degrades back
toward baseline as cache pressure climbs.

The fix: route by where a prefix still **resides**, not just where it was last
served. Each node broadcasts its backends' cache state over the existing gossip
layer; the router discounts an ART hit whose owning backend is likely to have
evicted the prefix.

## Gossip payload diff

One new packet type — `CACHE_STATE` (`0x06`). One packet describes one backend.

```
[type:1][version:1][backend_id:4][cache_usage:2][residency_weight:2]  = 10 bytes
```

- `cache_usage` ∈ [0,1] — KV-cache fullness (`vllm:gpu_cache_usage_perc`).
- `residency_weight` ∈ [0,1] — estimated prefix retention.

Both 0..1 signals are quantized to `uint16` fixed-point (`value * 65535`).

**Source.** `HealthService` (shard 0) already scrapes vLLM `/metrics` each health
cycle. We reuse that exact path: `residency_weight = 1 - effective_cache_pressure`
(`VLLMMetrics::estimated_prefix_retention()`, compression-adjusted). No new
scrape, no new collection mechanism.

**Extensibility without speculative build.** `deserialize()` accepts
`len >= PACKET_SIZE` (not `==`) and reads only the fields it knows. A future
version may append additional per-node aggregate signals (bumping `version`)
and older nodes will transparently skip the tail. We add exactly one type and
two fields today — no generalized dispatch, no reserved fields for hypothetical
uses.

**Rolling-upgrade safety contract.** `handle_packet()` rejects a type tag it
doesn't recognize *before* attempting to parse it: peer liveness is still
updated (peer stays healthy and counted in `cluster_peers_alive`), the packet is
dropped, and `cluster_unknown_packet_types` is incremented. This is distinct
from `router_cluster_sync_invalid` (malformed packets of a *known* type). So an
old node that predates `CACHE_STATE` ignores it cleanly rather than crashing or
quarantining the emitting peer.

**Reliability.** `CACHE_STATE` is broadcast unreliably (no seq_num, no ACK):
periodic, idempotent, latest-value-wins state. A dropped update self-heals on
the next scrape cycle, so it never touches the pending-ACK table.

## Decision logic

In `get_backend_for_prefix()` (the composite-load path), after the ART lookup
returns a live backend and *before* accepting it as a hit:

```
residency = get_cached_residency(art_backend)        // -1 if no signal / stale
if residency >= 0 and residency < cache_residency_threshold and >1 live backend:
    record residency downgrade (counts as a cache miss)
    fall through to the load-based hash strategy,
        excluding the cache-cold backend from the candidate set
else:
    honor the ART hit (existing behavior)
```

Properties:

- **No signal ⇒ no change.** Backends with no residency reported (peers on an
  older build, non-vLLM backends, or a stale entry past the cache TTL) are
  never downgraded — the route is honored exactly as before.
- **High residency ⇒ keep cache affinity.** The whole point of prefix routing
  is preserved when the backend still holds the prefix.
- **Low residency ⇒ treat as a likely miss** and divert to the configured
  load-based strategy (`BOUNDED_LOAD`/`P2C`/…), *skipping* the cold backend so
  we don't immediately re-pick it.
- **Single backend ⇒ keep the route.** With nowhere to divert, we still route
  to the only backend (downgrade requires an alternative).

The residency value is stored per-shard with **per-entry** freshness (not the
map-global timestamp the GPU/headroom caches use) because residency is fed by
multiple independent sources — this node's own scrape and any number of
gossiping peers — each touching a different subset of backends. Per-entry upsert
prevents one source from clobbering another's backends.

## Threshold: default and rationale

`routing.cache_residency_threshold` — **default `0.2`**. Configurable via YAML
(`routing.cache_residency_threshold`) or `RANVIER_CACHE_RESIDENCY_THRESHOLD`;
validated to `[0.0, 1.0]`.

Because `residency_weight = 1 - effective_cache_pressure`, the threshold reads as
"minimum estimated probability the prefix still resides." `0.2` means we only
downgrade when the backend's effective cache is roughly **>80% full** — a
strong, conservative signal that the learned prefix was likely evicted. This
biases toward *keeping* cache affinity (the feature's whole value) and only
abandons a route when eviction is very likely. Setting the threshold to `0.0`
disables downgrades entirely (pre-feature behavior).

## Payload / reactor budget

- **Wire:** with `B` locally-scraped backends, one node adds `B` × 10-byte
  CACHE_STATE payloads per peer per health-scrape cycle (default 5 s). For a
  typical node (`B ≤ 16`) that's ≤ 160 bytes/peer/cycle before DTLS framing —
  negligible against existing heartbeat + route-announcement traffic. Tracked by
  the integration payload-budget test.
- **Reactor:** gossip + residency aggregation run on the control-plane shard
  (shard 0). The receive path does one `smp::invoke_on_all` upsert per received
  packet (bounded, low rate). The hot routing read is a single shard-local hash
  lookup with no allocation and no cross-shard access.

## Observability

| Metric | Meaning |
|--------|---------|
| `ranvier_gossip_cache_states_sent_total` | CACHE_STATE packets broadcast |
| `ranvier_gossip_cache_states_received_total` | CACHE_STATE packets received |
| `ranvier_cluster_unknown_packet_types` | Packets ignored for an unknown type tag |
| `ranvier_router_residency_route_downgrades_total` | ART hits downgraded for low residency |
| `ranvier_router_residency_cache_size` | Backends tracked in the per-shard residency cache |

## Persistence

Residency is transient and never persisted — it is a live, seconds-fresh signal
with its own staleness TTL.

## Benchmarking

The before/after methodology (residency on vs off under real cache pressure),
the `churn` load-test workload that makes eviction actually happen, and the
orchestration script live in
[docs/benchmarks/cache-residency-ab-benchmark.md](../benchmarks/cache-residency-ab-benchmark.md)
/ `scripts/bench-residency-ab.sh`. The A/B toggle is
`--cache-residency-threshold` (0.0 = off). A run is only meaningful when
`ranvier_router_residency_route_downgrades_total > 0` — the wrapper's probe
step gates on that.
