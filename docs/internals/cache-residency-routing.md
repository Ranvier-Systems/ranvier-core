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

> **vLLM v1 semantics caveats (measured 2026-06-11, vLLM 0.15.1):**
>
> 1. **Metric name.** vLLM v1 renamed the gauge (`vllm:gpu_cache_usage_perc`
>    → `vllm:kv_cache_usage_perc`), and against 0.15.1 the old name never
>    matched: an independent sampler found zero occurrences while the same
>    endpoints served `/health`. An unmatched name leaves the parsed value at
>    its 0.0 default, so `residency_weight` broadcast a constant 1.0 and the
>    downgrade was structurally unable to fire in any benchmark to date. The
>    scrape now accepts **both** names (`health_service.cpp`).
> 2. **Gauge semantics.** The v1 gauge counts only blocks held by *running*
>    requests; freed-but-cached prefix blocks are reclaimable and report as
>    free (the engine logs `Running: 0 reqs … usage: 0.0%` alongside a 91%
>    prefix-cache hit rate). The downgrade therefore fires on **active-demand
>    saturation**, not on "prefix cache is populated" — much later than this
>    doc's prose implies. Usage >80% still entails rampant prefix eviction,
>    so the signal is directionally sound, but a per-prefix source
>    (prefix-cache hit counters, or the push eviction-events path) would fire
>    on the intended condition. Open question for a human decision; benchmark
>    + details:
>    [cache-residency-ab-benchmark.md](../benchmarks/cache-residency-ab-benchmark.md).

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

## Verified residency (native KV events)

### Requirement: Ranvier must tokenize as the backend does

The stream mirrors the backend's cache in the backend's token ids: the ledger hashes vLLM's
stored ids at every 16-token block boundary into `prefix_hash_index`, and an ART hit looks up
`hash_prefix` of Ranvier's **own** ids of the request in that same index. The two meet only when
both sides produce the same ids for the same request, which takes two settings:

- **Tokenizer** (`assets.tokenizer_path`, `RANVIER_TOKENIZER_PATH`): the served model's own
  `tokenizer.json`. The bundled `assets/gpt2.json` matches no production model.
- **Chat template** (`assets.chat_template_format`, `RANVIER_CHAT_TEMPLATE_FORMAT`): the format
  that renders the backend's template byte for byte. vLLM's `/v1/chat/completions` applies the
  model's template, so `none` never matches it. CodeLlama-Instruct and Llama-2-chat are
  `llama2` (alias `codellama`); see the template table in `prefix-affinity-routing.md`.

Either one wrong makes every stream key disjoint from every routing key, and nothing fails: the
index fills, `kv_ops` climbs, and the only entries a lookup can find are the router's own
learned routes, so "verified" hits read healthy while the stream contributes nothing.

**Self-checks.**

- **The alignment probe** (BACKLOG §30, `kv_events.alignment_probe`, default `quarantine`). Shard 0
  asks each backend with a live stream, from the health loop, whether it tokenizes like Ranvier:
  it renders a fixed probe conversation through the routing path (RequestRewriter, the configured
  template, the shard's tokenizer) and POSTs the same messages to the backend's `/tokenize`. The
  verdict compares the two id sequences up to the routing prefix: the longest prefix routing could
  hash for the probe (the system-prefix boundary or `prefix_token_length`, whichever is larger),
  capped at `max_indexed_token_depth` and aligned down to the block size. A difference after it (a
  generation prompt the template does not render) still reads **aligned**; one inside it reads
  **misaligned**, logged at warn with the first differing token; no `/tokenize` reads
  **unsupported**. It re-runs whenever the stream is marked complete again (an engine restart, an
  AllBlocksCleared). Under `quarantine`, a misaligned backend's stream stops vouching for
  anything on every shard: it is not fresh for routing, never a verified holder or a replica, and
  its absence is never an eviction. That is exactly stream-off routing for that backend, and it
  matters because a misaligned stream is otherwise *worse* than none: no routing key is ever
  present in its index, so every ART hit on it reads verified-cold and skips the probabilistic
  gate. A later aligned or unsupported verdict lifts it. `warn` probes and logs only; `off` skips.
- At startup, with `kv_events` on, a warning when the template is `none` or the tokenizer path is
  the bundled `gpt2.json`.
- `router_native_routing_key_matches_total` counts stream UPSERTs whose hash equals a routing
  key the router recently computed (a 64-entry ring per shard, keys within
  `max_indexed_token_depth`). It is the direct signal that the two key spaces meet: it should
  climb on any workload with repeated prefixes. Shard 0 logs a warning once if it is still 0
  after 10,000 UPSERTs.
- `tests/tokenizer_parity/` checks Ranvier's `llama2` rendering and tokenization against ids
  captured from vLLM `/tokenize` (gated test, `RANVIER_BUILD_LLAMA2_PARITY_TEST`).

For `llama2` the routing boundary is the tokenized system prefix (boundary Strategy 4), never the
character-ratio estimate, so the boundary depth lands on a token the backend has at that
position. `bench.sh --kv-events` takes the model's tokenizer from the host's Hugging Face cache
and `--chat-template` sets the format; the compare header names both and warns when unaligned.

**Every GPU benchmark through 2026-10-16 ran unaligned** (GPT-2 tokenizer, template `none`), so
the stream measurements quoted in this section before that date are not results (BACKLOG §29).
**Aligned, 2026-10-17/18** (CodeLlama-13B, 8×A100, vs least-loaded): the stream moved the fitted
20u row from −6.4% P99 (aligned keys, stream off) to −18.1% (3/3), and the 50-prefix 20u row
from +13.5% to −1.7% (no reliable effect), with `holder_redirects` 100–221 per arm. On the
fitted row it raises KV hit; on the eviction row its gain is load spread alone
(`benchmark-results-current.md`, "Aligned native KV events and replication").

### How verified residency works

When a backend opts into the native vLLM KV-event stream (`kv_events:`
section + per-backend port; build flag `WITH_KV_EVENTS`), residency for that
backend graduates from the probabilistic estimate above to a VERIFIED,
block-exact signal: `BlockStored`/`BlockRemoved` events maintain
`prefix_hash_index` as a live mirror of the backend's cache, and an ART hit
checks the request's prefix hash against it directly — present means
resident (the probabilistic gate is skipped entirely); absent means the home
has evicted it, and the request goes to another backend the index shows
holding the same prefix (a verified hit; that backend becomes the learned
home) or, with no other holder, stays on the home to re-prefill. A verified-cold
hit is never downgraded to miss placement: measured on 8×A100 (2026-10-10/11,
13B 20 users, 16 prefixes of 2,000–4,000 tokens) that re-placed and re-learned
the prefix on every evicted block, about 110 home migrations per 10-minute
run, which took the KV hit rate from 68% to 22–27% and P99 TTFT from −12% to
+77…+188% against a least-loaded router. The lookup is also skipped at routing
depths beyond `max_indexed_token_depth`, where the index cannot contain the
hash. Freshness is per backend
(`freshness_ttl_seconds`, kept alive by events and subscriber heartbeats);
when the stream goes stale, routing falls back to the probabilistic path
above with no operator action needed.

Trust is asymmetric. A fresh stream makes *presence* in the index a verified
hit, because a stored-block event is exact whenever it arrives. *Absence* is a
verified eviction only while the stream is also **complete**: seen in full
since a known-empty cache. The subscriber marks a stream complete when its
connect-backfill begins at the publisher's sequence 0 or a live frame carries
sequence 0 (the engine started or restarted empty); an `AllBlocksCleared`
event does the same. A stream fault (a sequence gap replay cannot fill, a
decode failure) resets the backend and revokes completeness, and so does
starting from a backfill whose older batches the publisher has already
trimmed. Heartbeats keep such a stream fresh, so its presence entries still
count, but absence falls back to the probabilistic gate
(`router_native_absence_untrusted_total`) until a clear or a sequence-0 frame:
otherwise a Ranvier restart against a warm fleet, or any reset, would read
every block the index no longer lists as evicted and redirect requests off
warm homes. A redirect target must also have a fresh stream of its own, since
learned routes write the same index and a backend without one may appear in it
on a route claim alone. See
`src/kv_event_ledger.hpp` for how vLLM block hashes are bridged to Ranvier
prefix hashes without sharing a hash function.

`prefix_hash_index` has two producers with one hash identity: both learn
paths publish `(hash, backend)` at the route's stored effective-boundary
depth — the same depth routing lookups and push evictions hash at — and
native `UPSERT`/`REMOVE` publish at the ledger's block boundaries. Its bound
is enforced by lifecycle, not a MAX_SIZE: every TTL cycle,
`ttl_cleanup_on_shard` rebuilds the learned portion from the live RadixTree
(so routes removed by TTL expiry, LRU eviction, peer-death pruning, or
backend unregistration cannot leave stale entries behind), while entries of
backends with a fresh native stream are preserved across the rebuild — their
lifecycle is owned exclusively by the stream's `UPSERT`/`REMOVE`/`CLEAR`/
`RESET` ops, and the per-backend native entry counters are recomputed from
what was preserved. Index growth is observable per shard via the
`router_prefix_hash_index_size` gauge.

Two extensions complete the native mode. **Route materialization**
(`kv_events.materialize_routes`, default **off** since 2026-10-13): `BlockStored` token chains
insert `RouteOrigin::PUSH` routes at every covered block boundary, so
prefixes computed without Ranvier's involvement — engine warm-up, other
frontends, replay backfill — become routable without waiting for traffic to
learn them. It is off by default because of what that does on a restart in
front of warm backends: the replayed history materializes a route for every
prefix head at whichever backend last stored it before placement has seen a
single request, those routes capture the first requests, and the homes for the
whole run are set by replay order. On 8×A100 (13B, 20 users, 16 prefixes vs a
least-loaded router) four such starts regressed P99 by 12–73 points; with
materialization off and the same replay still applied, three repeats read
−13.2 / −10.1 / −12.7% against a stream-off control of −12.5%. Turn it on
where other frontends compute prefixes that nothing else will learn. The trust order is enforced at insertion (locally-learned routes
are never overwritten; PUSH outranks gossip) and at eviction
(`evict_lowest_trust()`: REMOTE, then PUSH, then LOCAL). **Replay**
(per-backend `kv_events_replay_port`): forward sequence gaps recover the
missed batches from the publisher's buffered window instead of resetting,
and `replay_on_connect` backfills that window on subscribe — which is what
lets a restarted Ranvier recover warm backends' recent block state (PUB/SUB
itself has no snapshot; coverage is bounded by the publisher's buffer).

### Replication by diversion

`routing.replication.max_holders` (env `RANVIER_REPLICATION_MAX_HOLDERS`,
default **0 = off**, range 0–64, hot-reloadable) is the first routing decision
the native stream changes beyond the verified-cold redirect. It acts only on an
ART hit whose home has forfeited its BOUNDED_LOAD allowance, the case the load
divert used to send to the least-loaded *stranger*. On the 50-prefix 13B
20-user row a quarter of requests took that path (row 18, +23.1 / +14.7 /
+18.9% P99 against least-loaded with the stream on). Every stranger prefilled
the prefix and stored it, so the diverts were already creating replicas. But
one prefix's overflow was sprayed over seven backends, each saw it once or
twice, and in a fleet whose cache cannot hold the set a block touched once is
the first one LRU drops: `holder_redirects` read 0 in 376 verified-cold
decisions. Replication by diversion makes the divert **holder-aware and
sticky**, so one prefix's overflow goes to one replica that both uses it and
keeps it warm:

1. **Verified-holder dispatch** (`max_holders >= 1`). An under-allowance
   candidate that `prefix_hash_index` shows holding the request's prefix, on a
   fresh stream of its own, outranks every stranger. The divert is a hit once a
   replica exists. The holder lookup is the same shard-local helper the
   verified-cold redirect uses (`ShardLocalState::verified_holders_of`).
2. **Deterministic designation** (`max_holders >= 2`). With no holder under
   allowance and fewer holders than the cap (anchor included, holders counted
   at any load), the target is the first candidate in jump-probe order
   (`jump_consistent_hash(prefix_hash + k, n)`, k ascending) that is under
   allowance, not the anchor, not a holder, and on a fresh stream, instead of
   the coldest one. The hash and the sorted live set are the same on every
   shard of every node, so all of them send this prefix's overflow to the same
   backend with no shared state. The stream then confirms that backend as a
   holder and rule 1 takes over. This is not the pre-2026-10-02 first-under-cap
   rule that stranded the coldest backend: that chose the target of every
   divert of every prefix, while this chooses only where a prefix's *first*
   replica is born, and only while none exists. A shard-local "replica hint"
   map was rejected because three nodes × N shards would designate up to 3N
   different replicas. The fresh-stream requirement is what lets the replica
   be confirmed and counted toward the cap.
3. **Holder cap.** At the cap with every holder over allowance, the dispatch
   set narrows to the holders and the least-loaded one serves the hit, over
   allowance or not. At saturation a queued hit on a warm backend beat a cold
   prefill on a cold one on every ε of the saturation suite (BACKLOG §27, 2026-10-09).
   So the cap turns the divert off for a hot prefix once its replicas exist,
   instead of minting another copy.

`max_holders = 1` enables rule 1 alone; the anchor is the one holder and
nothing is ever designated or narrowed.

**Learning never moves.** Every rule is a *dispatch* tie key
(`ScoreCandidate::holder_rank`: 0 verified holder, 1 designated replica,
2 none), ranked after the clean-placement-winner preference and before cost
pressure and load (see [prefix-affinity-routing.md](prefix-affinity-routing.md)
§ Unified Route Scoring). Placement score and placement tie order are untouched,
so `original_selected` stays the home and route learning (eager and first-byte)
pins the home, never the replica. The home migrates only through the
verified-cold path above (home evicted, the least-loaded other fresh holder
takes the route), which with replicas present finally has somewhere to go.

**What it needs.** The stream: with `kv_events` off (TTL 0), at routing depths
beyond `max_indexed_token_depth`, or on candidates whose streams are stale,
every rule is inert and dispatch is byte-for-byte the least-loaded spillover.
The hit path's depth-skip counter has already counted a too-deep request, and
the spillover does not count it again. **What it risks:** a replica is a
second copy of a 2,000–4,000-token prefix in a fleet whose cache already cannot
hold one copy of the set, so it trades KV capacity for queueing and can lose
that trade. Read the KV hit rate before P99. The GPU verdict (BACKLOG §28
Step 8, `bench-runner.sh --suite kvrepl`) decides the default. Its 2026-10-16 run was
unaligned and inconclusive. The aligned run (2026-10-18, cap 2): the fitted 20u row improves
from −18.1% to −23.1% P99 vs least-loaded (3/3, KV 70–74% → 80–82%, replicas read more often
than minted); the 50-prefix 20u eviction row reads MIXED at −6.4% (KV unchanged, 2.5–4.5
replicas minted per replica read). The pre-registered rule needs both rows, so the default
stays 0; setting 2 is reasonable where the hot set fits in the fleet's cache.

Cost on the hot path: one prefix hash per forfeiting hit (already paid by the
spillover), a scan of the hash's holder set against the live set into a fixed
64-slot buffer, and at most one probe-rank pass over the dispatch candidates.
All reads are shard-local, with no allocation, timer or cross-shard call.

Beyond the hard gate, the unified route score can blend residency continuously:
`routing.scoring.residency_weight > 0` discounts the ART anchor's affinity bonus
by reported cache-cooling (`affinity *= 1 - w*(1-residency)`), so a cooling-but-
above-threshold backend gradually loses stickiness instead of holding it in full
until the gate trips. Default `0.0` keeps the gate as the only residency effect.
See [prefix-affinity-routing.md](prefix-affinity-routing.md) § Unified Route
Scoring.

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
| `ranvier_router_replica_dispatches_total` | Over-allowance ART hits served by a verified holder other than the home (replication by diversion) |
| `ranvier_router_replica_designations_total` | Over-allowance ART hits sent to the probe-order designated replica |
| `ranvier_router_replica_holders_saturated_total` | Over-allowance ART hits at the holder cap with every holder over allowance, kept on the least-loaded holder |

## Persistence

Residency is transient and never persisted — it is a live, seconds-fresh signal
with its own staleness TTL.

## Benchmarking

**Measured (2026-06-11, 8x A100-40GB, Llama-3.1-8B, vLLM 0.15.1):** under
sustained KV saturation (the regime where the v1 usage signal fires),
residency ON vs OFF cut overall TTFT P99 by **57%** (2800 → 1200 ms) at equal
P50/throughput/error rates, with a 0.2% downgrade rate. Full results, the
composition caveats on hit/miss splits, and a newly surfaced cross-node
route-flapping issue triggered by divert re-learns live in
[docs/benchmarks/cache-residency-ab-benchmark.md](../benchmarks/cache-residency-ab-benchmark.md).

The before/after methodology, the `churn` load-test workload that makes
eviction actually happen, and the orchestration script live in the same doc /
`scripts/bench-residency-ab.sh`. The A/B toggle is
`--cache-residency-threshold` (0.0 = off). A run is only meaningful when
`ranvier_router_residency_route_downgrades_total > 0` — the wrapper's probe
step gates on that.
