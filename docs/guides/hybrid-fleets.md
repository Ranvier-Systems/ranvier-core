# Hybrid Fleets

This guide walks operators through running Ranvier in front of a mixed fleet: some backends are self-hosted instances Ranvier can address directly (vLLM, SGLang, TensorRT-LLM) and benefit from prefix-affinity routing, and some are managed-API endpoints (Cerebras, OpenAI-compatible) where every request goes to a single URL that the provider's own scheduler load-balances internally. Ranvier handles both in the same configuration — the same admin API, the same metrics, the same circuit breakers — but applies prefix-affinity routing only to the backends whose physical instances it can actually pin requests to.

A common misconception worth dispelling upfront: managed-API backends like Cerebras *do* have KV caches (every autoregressive transformer does; on Cerebras the cache lives in on-wafer SRAM rather than GPU HBM). The reason prefix-affinity earns nothing on them isn't the absence of a cache — it's that the endpoint is opaque, so Ranvier can't influence which physical instance handles a given request. Cache locality, where possible, is the provider's internal concern.

## When you want this

- You have a primary GPU fleet (e.g. 8× A100 running vLLM) and want to spill overflow traffic to a remote-API backend for burst capacity.
- You have a heterogeneous fleet split by priority tier: GPU for interactive traffic, remote API for batch or low-priority traffic.
- You want a single ingress with one admin API, one Prometheus endpoint, one rate-limiter, one routing layer — regardless of how requests are ultimately served.

## What an opaque-endpoint backend means here

[`prefix-affinity-routing.md`](../internals/prefix-affinity-routing.md#backend-type-applicability) has the full truth table. Practically, two things are different about a managed-API backend in a Ranvier fleet:

1. **No route is learned for it.** When Ranvier proxies a request to a backend the provider load-balances internally, it skips the post-success "remember that prefix P went to backend B" step. An ART entry pointing at `api.cerebras.ai` would just mean "send this prefix to the same opaque URL we already send everything to" — which earns nothing. Cache-hit-rate counters and TTFT-improvement numbers don't accrue against this traffic, but the provider's internal scheduler may still be doing cache reuse on its side — it's just not observable to us.
2. **No `/metrics` is scraped from it.** Ranvier's vLLM-shaped Prometheus scrape is skipped (the schema is vLLM-specific). The backend's load score stays at `0.0`, so it looks idle to the load-aware router and tends to attract more traffic. Usually acceptable — managed-API providers absorb the implied skew via their own internal scheduling and capacity — but worth understanding.

Everything else — rate limiting per agent, circuit breaking per backend, fair scheduling across priority tiers, request retries, agent-priority overrides, API-key forwarding — works identically across both classes. This is the strategic value: one control plane, one set of operational concerns, regardless of which physical backend ultimately runs the inference.

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

  # Overflow / burst capacity. Cerebras is a managed-API endpoint —
  # ART learning is off (every request hits the same opaque URL,
  # so prefix-affinity earns nothing here even though Cerebras's
  # internal scheduler still does cache reuse) and the vLLM-shaped
  # /metrics scrape is off (different exposition format).
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

Ranvier's headline benchmark numbers (e.g. README's "44% faster TTFT" on Llama-3.1-70B, the "~49% → 81%" cache-hit rate in [`prefix-affinity-routing.md`](../internals/prefix-affinity-routing.md)) measure what prefix-affinity routing earns when Ranvier can actually steer requests to specific physical backends. They do not credit Ranvier for anything happening on a managed-API backend in a hybrid fleet, because Ranvier doesn't choose which Cerebras (or OpenAI, or other managed) instance handles a request — the provider's internal scheduler does, and any cache reuse there is invisible to us. Reporting the prefix-affinity headline for traffic that landed on a managed backend would be miscredited.

When measuring a hybrid fleet:

- Compare apples to apples. Filter your benchmark to traffic that landed on the cacheable backends, and compare that to a baseline of the same workload through random routing on the same backends. Don't dilute the cache-hit rate by including remote-API traffic in the denominator.
- Use the `X-Backend-ID` response header to attribute each request to a specific backend in your post-processing.
- The `router_cache_hits` and `router_cache_misses` counters are per-shard and per-backend; aggregate them with the `backend` label intact for per-class breakdowns.

A reasonable summary for an external audience: *"Ranvier delivers ~50% → ~80% cache hit rate on the self-hosted GPU pool, where it controls per-instance routing. The Cerebras endpoint behind the same ingress shares the rate-limiting, circuit-breaking, agent priorities, observability, and unified control plane, but not the prefix-affinity benefit — that requires per-instance addressability, which a managed API doesn't expose."*

## Observability for hybrid fleets

| Metric | Where it comes from | What it tells you |
|---|---|---|
| `ranvier_router_cache_hits` / `router_cache_misses` | Shard-aggregate counter (no per-backend label) | Fleet-wide cache hit rate. For a hybrid fleet, "misses" here means ART misses *plus* every routing decision that landed on a non-cacheable backend (those never get learned, so the next request through the same prefix misses again). Use `X-Backend-ID` for per-backend attribution. |
| `ranvier_router_prefix_hit_by_compression_tier` | Counter, bucketed by backend compression ratio | Cache hits broken down by tier. Lets you compare vLLM-uncompressed vs vLLM-compressed in the same fleet. |
| `ranvier_health_vllm_scrapes_total` | Counter | Number of scrape attempts. Should equal `live_vllm_backends × scrape_cycles`. |
| `ranvier_health_vllm_scrapes_suppressed` | Counter | Number of scrapes skipped — proactive (type ≠ vLLM) or adaptive (consecutive failures past threshold). Most of this is the type-based skip for hybrid fleets. |
| `ranvier_backend_active_requests` | Per-backend gauge | In-flight count per backend. Non-cacheable backends still report inflight load here — only the *load score* (derived from the vLLM `/metrics` scrape) is zero. |
| `X-Backend-ID` response header | Per-request | Which backend served the request. The single most useful field for per-backend post-hoc attribution. |

## K8s-native equivalent

If you're discovering your fleet via Kubernetes EndpointSlices (the `k8s_discovery` path) rather than static YAML, you don't need a `backends:` block at all. Tag the EndpointSlice with two annotations and Ranvier picks up both the type and the credential:

```yaml
apiVersion: discovery.k8s.io/v1
kind: EndpointSlice
metadata:
  name: cerebras-overflow
  annotations:
    ranvier.io/backend-type: cerebras
    ranvier.io/api-key-secret-ref: cerebras-prod-key
    ranvier.io/weight: "50"
    ranvier.io/priority: "1"
  ...
```

Provision the Secret with the conventional `api-key` field:

```bash
kubectl create secret generic cerebras-prod-key \
  --from-literal=api-key="$CEREBRAS_API_KEY"
```

When Ranvier processes the EndpointSlice, it fetches the Secret via the K8s API, decodes the `api-key` field, and pushes it to the same per-shard side-map the static-YAML path writes to. The auth-header injection at the proxy hot path is identical regardless of how the key arrived.

**RBAC requirement.** The default Helm chart now grants `secrets: [get]` (no `list`/`watch`) to the discovery Role. If you provision RBAC yourself, add this rule to the existing Role; if no backend in your fleet uses the annotation, the rule can be omitted (Ranvier will start fine, but those backends will be skipped with a warn).

**Credential rotation.** There's no Secret watcher — Ranvier reads the Secret once per backend registration. To rotate a key without restarting:
1. `kubectl edit secret <name>` to update the `api-key` field.
2. Touch any annotation on the EndpointSlice (e.g. `kubectl annotate endpointslice <name> ranvier.io/rotate="$(date +%s)" --overwrite`) to force a `MODIFIED` event.
3. Ranvier's `handle_endpoint_modified` re-fetches the Secret and pushes the new value to the side-map.

A Pod restart (rolling deployment, readiness flip) also re-triggers the registration path. Both work; choose whichever fits your existing rotation tooling.

## What doesn't work yet

- **Per-deployment ART-learning opt-out.** Today, ART learning is a type-level decision (`cerebras` opts out, everything else opts in). If you front a managed OpenAI-compatible API where Ranvier can't influence per-instance routing, you currently can't tag it as "no-learn" without classifying it as `cerebras`. A per-deployment opt-out flag is on the roadmap.
- **Multiple credentials per backend.** Each backend can have one `api_key_env` / `api-key-secret-ref`; we don't support rotating two keys for blue/green or signing requests with two headers.
- **Live key reload for static-YAML backends.** Changing the env var after Ranvier is running has no effect on backends registered via the `backends:` YAML block. Restart to pick up new credentials. K8s-discovered backends *do* support rotation via the annotation-touch pattern above.
- **Custom Secret field names.** The `api-key-secret-ref` annotation always reads the conventional `api-key` field from the Secret. If you need to point at a different field in an existing multi-field Secret, that's a future enhancement.

## See also

- [Static-config `backends:` schema](../../ranvier.yaml.example) (search for "Static-Config Backends" in the file)
- [Prefix-affinity routing internals](../internals/prefix-affinity-routing.md) — algorithm details and the per-`BackendType` applicability table
- [Request lifecycle](../internals/request-lifecycle.md) — the end-to-end request path showing where the routing decision and auth injection fit
- [Cloud deployment](./cloud-deployment.md) — production deployment patterns
