# Hybrid Fleets

This guide walks operators through running Ranvier in front of a mixed fleet: some backends are GPU-based (vLLM, SGLang, TensorRT-LLM) with KV caches that benefit from prefix-affinity routing, and some are remote-API backends (Cerebras, OpenAI-compatible) that don't. Ranvier handles both in the same configuration — the same admin API, the same metrics, the same circuit breakers — but applies the prefix-affinity optimisations only where they earn anything.

## When you want this

- You have a primary GPU fleet (e.g. 8× A100 running vLLM) and want to spill overflow traffic to a remote-API backend for burst capacity.
- You have a heterogeneous fleet split by priority tier: GPU for interactive traffic, remote API for batch or low-priority traffic.
- You want a single ingress with one admin API, one Prometheus endpoint, one rate-limiter, one routing layer — regardless of how requests are ultimately served.

## What a "non-cacheable" backend means here

[`prefix-affinity-routing.md`](../internals/prefix-affinity-routing.md#backend-type-applicability) has the full truth table. Practically, two things are different about a non-cacheable backend in a Ranvier fleet:

1. **No route is learned for it.** When Ranvier proxies a request to a backend with no KV cache, it skips the post-success "remember that prefix P went to backend B" step. Cache-hit-rate counters and TTFT-improvement numbers don't accrue against this traffic.
2. **No `/metrics` is scraped from it.** Ranvier's vLLM-shaped Prometheus scrape is skipped. The backend's load score stays at `0.0`, so it looks idle to the load-aware router and tends to attract more traffic. Usually fine — non-cacheable backends are typically remote APIs whose pitch is "no queueing" — but it's worth understanding.

Everything else — rate limiting per agent, circuit breaking per backend, fair scheduling across priority tiers, request retries, agent-priority overrides, API-key forwarding — works identically across both classes.

## A working configuration

The fastest path to a hybrid fleet is the static-config `backends:` block in `ranvier.yaml`. Pair it with the existing K8s or local discovery for your GPU fleet (or use the admin API for ad-hoc registration).

```yaml
# ranvier.yaml

routing:
  routing_mode: prefix          # ART + hash fallback
  load_aware_routing: true      # diversion on top of the routing decision

backends:
  # Primary GPU fleet — registered statically here, but could equally
  # come from K8s discovery or the POST /admin/backends API.
  - id: 1
    host: 10.0.0.1
    port: 8000
    type: vllm
    weight: 100
    compression_ratio: 1.0

  - id: 2
    host: 10.0.0.2
    port: 8000
    type: vllm
    weight: 100

  # Overflow / burst capacity. Cerebras has no KV cache and no Prometheus
  # endpoint — ART learning and metrics scraping both stay off.
  # Lower weight nudges hash routing to land here less often.
  - id: 101
    host: api.cerebras.ai
    port: 443
    type: cerebras
    weight: 50
    priority: 1                  # Falls back to this group only when priority=0 is down
    api_key_env: CEREBRAS_API_KEY
```

Run it:

```bash
export CEREBRAS_API_KEY="sk-..."
ranvier --config ranvier.yaml
```

At startup Ranvier emits (one line per non-VLLM backend under load-aware routing):

```
INFO  ranvier.router - Backend 101 (cerebras): metrics scraping disabled; \
                       load-aware routing will treat this backend as zero-load
```

This is informational — not a warning. It exists because the zero-load assumption changes routing behaviour and we want it visible without scanning code.

## API key handling

If a backend entry has `api_key_env: <NAME>`:

- The environment variable is read **once at startup**, on shard 0.
- The resolved value is broadcast to every shard's in-memory side-map.
- The value is **never** written to disk (no SQLite row), **never** logged, and not copied into any cluster-gossip messages.
- The proxy hot path injects `Authorization: Bearer <key>` per request to that backend, sanitised against CR/LF.

If the env var is unset or empty at startup, the backend is **skipped** with a warning rather than registered in a state where every request would return 401. To rotate a key in production, set the new env var and restart (or use the K8s rolling-restart pattern); there is no live-reload of credentials yet.

Backends without `api_key_env` register normally and receive no auth header — which is correct for vLLM on a trusted network.

## Performance expectations

Ranvier's headline benchmark numbers (e.g. README's "44% faster TTFT" on Llama-3.1-70B, the "~49% → 81%" cache-hit rate in [`prefix-affinity-routing.md`](../internals/prefix-affinity-routing.md)) measure the prefix-affinity routing benefit on **cacheable** backends. They do not credit Ranvier for anything happening on a Cerebras / OpenAI-compatible backend in a hybrid fleet — the latency on that traffic is dominated by the remote service, not by Ranvier's routing layer.

When measuring a hybrid fleet:

- Compare apples to apples. Filter your benchmark to traffic that landed on the cacheable backends, and compare that to a baseline of the same workload through random routing on the same backends. Don't dilute the cache-hit rate by including remote-API traffic in the denominator.
- Use the `X-Backend-ID` response header to attribute each request to a specific backend in your post-processing.
- The `router_cache_hits` and `router_cache_misses` counters are per-shard and per-backend; aggregate them with the `backend` label intact for per-class breakdowns.

A reasonable summary for an external audience: *"Ranvier gives you ~50% → ~80% cache hit rate on your vLLM fleet; the Cerebras endpoint behind the same ingress benefits from rate-limiting, circuit-breaking, and fair scheduling but not from prefix-affinity routing — by design."*

## Observability for hybrid fleets

| Metric | Where it comes from | What it tells you |
|---|---|---|
| `ranvier_router_cache_hits` / `router_cache_misses` | Shard-aggregate counter (no per-backend label) | Fleet-wide cache hit rate. For a hybrid fleet, "misses" here means ART misses *plus* every routing decision that landed on a non-cacheable backend (those never get learned, so the next request through the same prefix misses again). Use `X-Backend-ID` for per-backend attribution. |
| `ranvier_router_prefix_hit_by_compression_tier` | Counter, bucketed by backend compression ratio | Cache hits broken down by tier. Lets you compare vLLM-uncompressed vs vLLM-compressed in the same fleet. |
| `ranvier_health_vllm_scrapes_total` | Counter | Number of scrape attempts. Should equal `live_vllm_backends × scrape_cycles`. |
| `ranvier_health_vllm_scrapes_suppressed` | Counter | Number of scrapes skipped — proactive (type ≠ vLLM) or adaptive (consecutive failures past threshold). Most of this is the type-based skip for hybrid fleets. |
| `ranvier_backend_active_requests` | Per-backend gauge | In-flight count per backend. Non-cacheable backends still report inflight load here — only the *load score* (derived from the vLLM `/metrics` scrape) is zero. |
| `X-Backend-ID` response header | Per-request | Which backend served the request. The single most useful field for per-backend post-hoc attribution. |

## What doesn't work yet

- **Per-deployment ART-learning opt-out.** Today, ART learning is a type-level decision (`cerebras` opts out, everything else opts in). If you have an OpenAI-compatible shim that doesn't cache, you can't yet flag it as "no-cache" without modifying the type — this is on the roadmap.
- **Multiple credentials per backend.** Each backend can have one `api_key_env`; we don't support rotating two keys for blue/green or signing requests with two headers.
- **Live key reload.** Changing the env var after Ranvier is running has no effect. Restart to pick up new credentials.
- **K8s Secret references.** `api_key_env` resolves to a process environment variable. K8s Secret-backed env vars work transparently (the kubelet does the substitution before Ranvier starts), but there's no first-class `api_key_secret_ref` schema field — yet.

## See also

- [Static-config `backends:` schema](../../ranvier.yaml.example) (search for "Static-Config Backends" in the file)
- [Prefix-affinity routing internals](../internals/prefix-affinity-routing.md) — algorithm details and the per-`BackendType` applicability table
- [Request lifecycle](../internals/request-lifecycle.md) — the end-to-end request path showing where the routing decision and auth injection fit
- [Cloud deployment](./cloud-deployment.md) — production deployment patterns
