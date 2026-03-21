# Push-Based Cache Eviction Notifications

**Status:** Proposal (Design Exploration)
**Date:** 2026-03-20
**Author:** Generated exploration

## Problem Statement

Ranvier infers GPU backend KV cache state from routing history: when a request is routed to backend B with token prefix P, Ranvier records `P → B` in the RadixTree and assumes B holds prefix P in its KV cache. This works well under moderate cache pressure.

With 1M-token context windows becoming standard, GPU backends evict KV cache entries more aggressively (vLLM uses PagedAttention with block-level eviction; SGLang uses RadixAttention with LRU eviction). The **staleness window** between actual backend eviction and Ranvier's awareness grows, causing:

1. **Misrouted requests**: Ranvier sends request to backend B believing prefix P is cached, but B evicted P. The backend must re-prefill from scratch.
2. **Wasted prefill compute**: A full prefill on 128K tokens costs 5-15 seconds of GPU time. At scale, stale routing can waste 20-40% of prefill budget.
3. **TTL is a blunt instrument**: Ranvier's current TTL-based cleanup (`ttl_seconds`, default 1 hour) doesn't correlate with actual backend eviction timing, which depends on memory pressure, not wall-clock time.

## Design Goals

| Priority | Goal |
|----------|------|
| P0 | Remain engine-agnostic (OpenAI-compatible backends, no vLLM/SGLang coupling) |
| P0 | Push support is optional; graceful fallback to inferred routing |
| P0 | Integrate with existing gossip protocol (same distributed state machine) |
| P1 | Minimal contract (trivial to implement for engine authors) |
| P1 | Sub-second eviction propagation to Ranvier routing tables |
| P2 | Upstream adoptability (easy enough that engine teams would implement it) |

## Event Schema

### Design Principle: Minimal Lifecycle Events

The contract asks backends to report two events:

1. **Prefix evicted**: "I no longer have prefix X in my KV cache"
2. **Prefix loaded**: "I now have prefix X in my KV cache" (optional, for cold-start sync)

We do NOT ask backends to expose:
- Internal block/page structure
- Eviction policy details
- Memory utilization metrics
- Per-layer cache state

### Proposed Event Format

```json
{
  "type": "cache_event",
  "version": 1,
  "events": [
    {
      "event": "evicted",
      "prefix_hash": "a1b2c3d4e5f6",
      "prefix_token_count": 128,
      "timestamp_ms": 1711000000000
    },
    {
      "event": "loaded",
      "prefix_hash": "f6e5d4c3b2a1",
      "prefix_token_count": 256,
      "timestamp_ms": 1711000000001
    }
  ]
}
```

**Field descriptions:**

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | Always `"cache_event"` |
| `version` | int | Schema version (start at 1) |
| `events` | array | Batch of 1+ events (amortizes HTTP overhead) |
| `events[].event` | string | `"evicted"` or `"loaded"` |
| `events[].prefix_hash` | string | Hex-encoded hash of the token prefix (see below) |
| `events[].prefix_token_count` | int | Number of tokens in the prefix |
| `events[].timestamp_ms` | int64 | Unix timestamp (milliseconds) when the event occurred |

### Prefix Identification

Rather than transmitting full token sequences (which can be thousands of tokens), events use a **prefix hash**. Ranvier already computes prefix hashes for consistent-hash routing — the same hash function is reused here.

The hash must be computed identically by Ranvier and the backend. Options:

1. **Ranvier tells the backend**: On each proxied request, Ranvier includes a `X-Ranvier-Prefix-Hash` header with the hex hash of the routed prefix. The backend stores this alongside its KV cache entry and echoes it back on eviction. **This is the recommended approach** — it requires zero hash implementation on the backend side.

2. **Shared hash function**: Document a canonical hash (e.g., xxHash64 of the first N token IDs as little-endian int32 bytes). Both sides compute independently. More complex for backends to implement.

**Recommendation:** Option 1. The backend just needs to store and echo an opaque string. Minimal implementation burden.

## Transport Options

### Option A: HTTP POST Callback (Recommended)

Ranvier exposes a new endpoint:

```
POST /v1/cache/events
Content-Type: application/json
Authorization: Bearer <optional-api-key>
```

**Pros:**
- Every inference engine already has an HTTP stack
- Works through firewalls and load balancers
- Easy to test with curl
- Batching is natural (array of events per request)
- TLS for free (reuse existing cert infrastructure)

**Cons:**
- Higher latency than Unix socket (~1-5ms vs ~0.1ms)
- Connection overhead (mitigated by HTTP keep-alive)

**Integration with Seastar:** The existing `HttpController` handles this on the sharded HTTP server. Events are dispatched to shard 0 (where gossip runs) via `smp::submit_to`. This follows the same pattern as admin API endpoints.

### Option B: Unix Domain Socket

A local UDS for colocated backends (same pod/host).

**Pros:**
- Lowest latency (~0.1ms)
- No network overhead

**Cons:**
- Only works for colocated deployments
- Requires Seastar POSIX socket setup (non-trivial)
- Doesn't work in disaggregated prefill/decode architectures

### Option C: gRPC Stream

Server-streaming RPC from backend to Ranvier.

**Pros:**
- Bidirectional, low-latency streaming
- Schema enforcement via protobuf

**Cons:**
- Heavy dependency (gRPC library, protobuf codegen)
- Most inference engines don't ship a gRPC client
- Seastar + gRPC integration is painful (different event loops)

### Option D: UDP Push (Reuse Gossip Port)

Backends send eviction events as UDP packets to Ranvier's gossip port.

**Pros:**
- Lowest overhead, fire-and-forget
- Reuses existing UDP infrastructure

**Cons:**
- Unreliable (no ACK for eviction events)
- Requires new packet type in gossip protocol
- Mixing backend-to-Ranvier traffic with peer-to-peer gossip complicates security

### Recommendation

**HTTP POST callback (Option A)** as the primary transport, with an optional **Unix socket (Option B)** for latency-sensitive colocated deployments. HTTP is universally available, and the 1-5ms overhead is acceptable — even a 5ms notification delay is orders of magnitude better than the current minutes-to-hours staleness window.

## Integration with Gossip Protocol

### Push Events as First-Class Gossip Input

Cache eviction notifications enter the same state machine as inferred routes:

```
                    ┌─────────────────────────┐
                    │    RadixTree (per-shard) │
                    │   Token Prefix → Backend │
                    └────────▲──────▲──────▲───┘
                             │      │      │
                    ┌────────┘      │      └────────┐
                    │               │               │
            ┌───────┴──────┐ ┌─────┴──────┐ ┌──────┴───────┐
            │ Local Learn  │ │   Gossip   │ │  Push Evict  │
            │ (proxy resp) │ │ (peer ann) │ │ (backend CB) │
            └──────────────┘ └────────────┘ └──────────────┘
```

When Ranvier receives an eviction event for prefix hash H from backend B:

1. **Shard 0 receives** the HTTP POST on `HttpController`
2. **Validate** the event (known backend, valid hash, reasonable timestamp)
3. **Forward to RouterService** via `smp::submit_to(0, ...)` (gossip runs on shard 0)
4. **RouterService scans RadixTree** for routes matching backend B
5. For each matching route, check if the prefix hash matches H
6. **Remove matching routes** from the local RadixTree
7. **Broadcast eviction** to all shards via the existing batched shard broadcast
8. **Broadcast eviction** to cluster peers via gossip (new `CACHE_EVICTION` packet type)

### New Gossip Packet Type

```
GossipPacketType::CACHE_EVICTION = 0x05
```

Wire format:

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     Type      |    Version    |          Sequence Number      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      Sequence Number (cont)   |          Backend ID           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|        Backend ID (cont)      |    Prefix Hash (8 bytes)  ... |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                   ... Prefix Hash (continued)                 |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

Total: 18 bytes fixed. Uses the same reliable delivery (ACK + retry) as `ROUTE_ANNOUNCEMENT`.

### "Loaded" Events and Route Bootstrapping

`loaded` events are optional and serve two purposes:

1. **Cold-start sync**: When Ranvier starts with an empty RadixTree, backends can push their current cache contents so Ranvier doesn't need to "warm up" through inference traffic.
2. **Partition recovery**: After a network partition heals, backends can re-announce their cache state.

`loaded` events are treated as route learns with `RouteOrigin::PUSH` (new origin type, eviction priority between LOCAL and REMOTE).

## Consistency Model

### Push Takes Precedence Over Inferred State

| Scenario | Resolution |
|----------|------------|
| Push says evicted, inferred says cached | **Evict.** Backend is the source of truth for its own cache. |
| Push says loaded, no inferred route | **Learn.** Add route with PUSH origin. |
| Push says loaded, inferred says different backend | **Keep inferred.** The most recent local observation wins for conflicting backends. |
| No push support, inferred only | **Current behavior.** TTL-based expiry. |
| Push timestamp older than last route learn | **Ignore.** Stale event (reordering or delayed delivery). |

### Conflict Resolution Rules

1. **Eviction is authoritative**: If a backend says it evicted a prefix, that route is removed regardless of inferred state. A backend cannot be wrong about what it doesn't have.
2. **Load events are advisory**: A `loaded` event doesn't override a more recent local observation. If Ranvier just routed a request to backend B for prefix P (and got a successful response), that's a stronger signal than a `loaded` event from backend A for the same prefix.
3. **Timestamp ordering**: Events include millisecond timestamps. Ranvier discards events with timestamps older than the last state change for that prefix+backend pair.
4. **Distributed consistency**: Push events propagate to cluster peers via gossip. A peer receiving a gossip-relayed eviction applies the same precedence rules.

### Staleness Budget

With push notifications, the staleness budget becomes:

```
Total staleness = backend_eviction_delay + network_latency + shard_broadcast_delay
                ≈ 0ms                    + 1-5ms           + 20ms (batch interval)
                ≈ 21-25ms
```

vs. current approach:

```
Total staleness = min(TTL, time_until_next_request_reveals_miss)
                ≈ minutes to hours
```

A ~1000x improvement in staleness.

## Sidecar Pattern

### When Direct Integration Isn't Feasible

For inference engines that can't implement the HTTP callback directly, a thin **sidecar** process can bridge engine-specific internals to the universal event format:

```
┌─────────────────────────────┐     ┌────────────────────┐
│     Inference Engine        │     │   Cache Sidecar    │
│  (vLLM / SGLang / TRT-LLM)  │────▶│                    │────▶ Ranvier
│                             │     │  Polls engine API  │     POST /v1/cache/events
│  /metrics or /cache/status  │     │  Translates to     │
│  (engine-specific)          │     │  universal events  │
└─────────────────────────────┘     └────────────────────┘
```

**Sidecar responsibilities:**

1. **Poll engine-specific API** for cache state changes (e.g., vLLM's `/v1/cache/status`, SGLang's RadixAttention metrics)
2. **Diff against previous state** to detect evictions and loads
3. **Translate to universal event format** and POST to Ranvier

**Sidecar advantages:**
- Engine teams don't need to modify their codebase
- Sidecar can be a simple Python/Go script (50-100 LOC)
- One sidecar per engine flavor, maintained by the Ranvier community
- Runs as a second container in the same K8s pod

**Sidecar disadvantages:**
- Additional polling latency (sidecar poll interval, typically 1-5s)
- Extra process to deploy and monitor
- Depends on engine exposing some cache visibility API

### Recommended Sidecar Poll Targets

| Engine | API / Metric | Notes |
|--------|-------------|-------|
| vLLM | `/metrics` (`vllm:num_cached_tokens`, block utilization) | Prometheus metrics available by default |
| SGLang | `/v1/cache/stats` (if exposed) or `/metrics` | RadixAttention tree stats |
| TRT-LLM | Triton metrics or gRPC health endpoint | Less cache visibility currently |

The sidecar approach adds 1-5s latency (poll interval) vs. direct push (sub-25ms). Still a major improvement over the current hours-scale staleness.

## Ranvier Internal Changes

### New Components

| Component | Location | Description |
|-----------|----------|-------------|
| Cache event endpoint | `http_controller.cpp` | `POST /v1/cache/events` handler |
| `CacheEvictionPacket` | `gossip_protocol.hpp` | New packet type `0x05` |
| Prefix hash index | `router_service.cpp` | Reverse index: hash → route(s) for O(1) eviction lookup |
| `X-Ranvier-Prefix-Hash` | `http_controller.cpp` | Header injection on proxied requests |

### Configuration

```yaml
# New section in ranvier.yaml
cache_events:
  enabled: false                    # Opt-in (default: disabled)
  auth_token: ""                    # Optional Bearer token for event endpoint
  max_events_per_request: 100       # Bound batch size (Rule #4)
  max_event_age_seconds: 60         # Reject events older than this
  propagate_via_gossip: true        # Forward evictions to cluster peers
```

Environment variable overrides:
```
RANVIER_CACHE_EVENTS_ENABLED=true
RANVIER_CACHE_EVENTS_AUTH_TOKEN=secret
```

### Metrics

| Metric | Type | Description |
|--------|------|-------------|
| `ranvier_cache_events_received_total` | counter | Total events received via push |
| `ranvier_cache_events_evictions_applied` | counter | Evictions that matched and removed a route |
| `ranvier_cache_events_evictions_stale` | counter | Evictions ignored (timestamp too old) |
| `ranvier_cache_events_evictions_unknown` | counter | Evictions for unknown prefix hashes |
| `ranvier_cache_events_loads_applied` | counter | Load events that created new routes |
| `ranvier_cache_events_gossip_propagated` | counter | Evictions forwarded to cluster peers |

### Hard Rules Compliance

| Rule | How This Design Complies |
|------|-------------------------|
| #4 (Bounded containers) | `max_events_per_request` bounds batch size; prefix hash index bounded by `max_routes` |
| #5 (Timer gate guards) | No new timers — events are request-driven |
| #6 (Metrics deregistration) | New metrics deregistered in `HttpController::stop()` |
| #7 (No business logic in persistence) | Eviction logic stays in RouterService |
| #14 (Cross-shard data) | Events received on HTTP shard, forwarded to shard 0 via `foreign_ptr` |
| #16 (Lambda coroutine) | Event handler uses coroutine, not `.then()` chains |

## Upstream Adoption Path

### What Makes This Adoptable

1. **Opaque hash echoing**: Backends just store and echo back a header value. Zero hash computation.
2. **Standard HTTP**: No new protocols, no gRPC, no custom sockets.
3. **Batched**: Engines can buffer evictions and POST once per second.
4. **Optional**: Engines that don't implement it lose nothing.

### Proposed OpenAI-Compatible Extension

This could be proposed as a lightweight extension to the OpenAI-compatible API ecosystem:

```
# New optional endpoint (backends implement this)
POST /v1/cache/events/subscribe
{
  "callback_url": "http://ranvier:8080/v1/cache/events",
  "events": ["evicted", "loaded"],
  "prefix_hash_header": "X-Ranvier-Prefix-Hash"
}

# Response
{
  "subscription_id": "sub_abc123",
  "status": "active"
}
```

Alternatively, a simpler approach: Ranvier sends the `X-Ranvier-Prefix-Hash` header on every proxied request. Backends that understand it echo eviction events; backends that don't ignore it. No subscription needed.

### Engagement Strategy

1. **Start with a sidecar** for vLLM (most popular engine, best metrics exposure)
2. **Publish benchmark results** showing prefill savings with push vs. inferred-only
3. **Propose to vLLM/SGLang** as a 50-line feature: "Store this header, POST it back on eviction"
4. **Submit as RFC** to the emerging OpenAI-compatible API ecosystem

## Implementation Phases

### Phase 1: Foundation (MVP)

- [ ] Add `POST /v1/cache/events` endpoint to `HttpController`
- [ ] Add `X-Ranvier-Prefix-Hash` header injection on proxied requests
- [ ] Implement prefix hash reverse index in `RouterService`
- [ ] Handle `evicted` events (remove routes from RadixTree)
- [ ] Add `cache_events` config section
- [ ] Add Prometheus metrics
- [ ] Unit tests for event parsing, hash matching, route removal

### Phase 2: Cluster Propagation

- [ ] Add `CACHE_EVICTION` gossip packet type (`0x05`)
- [ ] Propagate eviction events to cluster peers
- [ ] Integrate with batched shard broadcast (existing `flush_route_batch`)
- [ ] Integration tests with multi-node cluster

### Phase 3: Load Events + Sidecar

- [ ] Handle `loaded` events (create routes with `RouteOrigin::PUSH`)
- [ ] Add `RouteOrigin::PUSH` to RadixTree (eviction priority between LOCAL and REMOTE)
- [ ] Build vLLM sidecar (Python, polls `/metrics`)
- [ ] Build SGLang sidecar
- [ ] Publish sidecar container images

### Phase 4: Upstream Engagement

- [ ] Benchmark: prefill savings with push notifications vs. inferred-only
- [ ] Write blog post / RFC for OpenAI-compatible extension
- [ ] Submit PRs to vLLM/SGLang for native push support

## Open Questions

1. **Hash collision handling**: With xxHash64, collision probability is ~1/2^64. Should we still handle it? (Probably not — a spurious eviction just causes one extra cache miss.)

2. **Bulk eviction storms**: When a backend restarts, it evicts everything. Should Ranvier rate-limit eviction processing to avoid RadixTree churn? Or is it better to immediately clear stale state?

3. **Prefix granularity**: Should the hash cover the full routed prefix, or just the first N tokens (matching `prefix_token_length`)? Using the full prefix is more precise; using the routing key is simpler.

4. **Security**: The cache event endpoint accepts state-changing requests from backends. Should it require mTLS (matching gossip DTLS), or is a Bearer token sufficient?

5. **Subscription lifecycle**: If Ranvier restarts, backends don't know to re-subscribe. The "always send header, echo on eviction" approach avoids this entirely — no subscription state to manage.
