# Routing Architecture — Direction & Ecosystem Alignment (2026-06)

> Generated: 2026-06-07. Companion to [VISION.md](VISION.md) (product roadmap) and
> [BACKLOG.md](../../BACKLOG.md) (actionable items). VISION described the path from a prefix
> router to an "intelligence layer"; this document records where ranvier-core's routing core
> stands against the state of the art and the near-term engineering direction.

---

## 1. Bottom line

KV-cache/prefix-aware routing is the highest-ROI lever in LLM serving — established by
peer-reviewed work (Preble, ICLR 2025; Mooncake, USENIX FAST 2025 best paper) and now standard
in production inference stacks. Over the last ~18 months the ecosystem converged on two things
ranvier-core's routing core should align with:

1. **Signal freshness** — route on *live, block-level KV-cache events* emitted by the engine
   (vLLM's native KV-event stream), not learned history alone.
2. **Topology awareness** — be *disaggregation-aware* (separate prefill/decode pools).

A Kubernetes-native standard — the **Gateway API Inference Extension (GIE)** with its
**Endpoint Picker (EPP)** protocol — has emerged as the interop surface for inference routing.

ranvier-core's distinct, durable edge is **architectural**: a C++/Seastar inline data plane
that makes routing decisions in microseconds with no GIL and **no per-request ext_proc sidecar
hop**, deployable as a **single binary without Kubernetes**, and **engine-agnostic** (vLLM,
SGLang, TensorRT-LLM, Ollama, LM Studio). The direction is not "add every feature" — it is
**stay the fast, simple, standalone router, close the routing-quality gaps below, and make the
core pluggable into the standard** (§4).

## 2. Where ranvier-core stands (grounded in the code)

### Strengths

- **Routing-decision latency.** Inline C++/Seastar routing (ART lookup <50µs) avoids a
  per-request gRPC round-trip to a separate endpoint-picker sidecar. Quantifying that overhead
  delta is an open benchmark to publish (§4).
- **Deployment simplicity & reach.** Single binary — no Kubernetes, CRDs, or sidecars required.
  Non-K8s clusters, edge, sovereign on-prem, and developer-local are first-class.
- **Engine-agnostic.** We route on raw token prefixes without engine cooperation, so we work
  across vLLM + SGLang + TensorRT-LLM + Ollama + LM Studio (`BackendType`, `src/types.hpp`).
- **Breadth already shipped.** Rate limiting, priority scheduling, gossip multi-node route
  sharing, intent classification, chat templating, K8s + local discovery, API-key auth +
  per-key attribution, `prompt_token_ids` injection.
- **Eviction-aware today.** Push-based cache eviction (`POST /v1/cache/events`,
  `evict_by_prefix_hash_global`, `prefix_hash_index`; BACKLOG §2.1,
  `docs/architecture/push-cache-eviction-notifications.md`); probabilistic residency weighting
  (`VLLMMetrics::estimated_prefix_retention`, `cache_residency_threshold`) across shards and
  gossiped to peers; integrated load- and cost-aware routing (`get_composite_backend_load`,
  `CostBudgetGuard`, `RoutingConfig::load_aware_routing`, `cost_routing`).

### Gaps we're closing

- **Freshness protocol.** Today's eviction signal is a ranvier-defined HTTP protocol; the
  engine-native path is vLLM's **KV-event stream** (block-hash granularity). `prefix_hash_index`
  is the right substrate — feeding it from the native stream makes residency *exact* rather than
  probabilistic (P0.1).
- **Disaggregation (P/D) awareness.** `BackendType` is engine class, not pool role; in a
  disaggregated deployment we can misroute. Adding a `pool_role` dimension is table-stakes for
  frontier-model serving (P0.3).
- **Standard conformance.** Not yet a GIE-conformant endpoint picker; can't be dropped into a
  GIE gateway or consume `InferencePool`/`InferenceObjective` CRDs (P1.4).
- **Unified scoring.** We layer ART → load redirect → cost redirect as sequential *steps*
  (`PrefixRouteResult.was_load_redirect`, `was_cost_redirect`); folding them into one tunable
  weighted score is a refactor of the decision core — the inputs already exist (P0.2).

## 3. Ecosystem context

The inference-router space has split into two overlapping shapes: KV-cache/inference-aware
routers (route across replicas of one model to maximize cache reuse + balance GPU load) and
broader AI gateways (provider breadth, semantic cache, guardrails — generally not KV-aware).
The two are converging via the GIE EPP, which gateways delegate to for the inference-routing
decision. ranvier-core's position is deliberate: do KV-aware routing **natively in the data
plane**, and offer an EPP-compatibility mode so it can *also* slot into the standard. The
relevant lessons from the state of the art:

- The router is a **stateful, KV-topology-aware scheduler**, not a load balancer.
- Mature designs combine a **live per-worker block index** (engine KV-events) with a **single
  weighted score** blending prefix overlap + queue depth + KV utilization (published heuristics
  exist, e.g. a `prefix:queue:kv` weighting).
- KV transfer / offload (NIXL, LMCache, Mooncake) is a *separate* layer — ranvier-core should
  be compatible with it, not own it.

(The Adaptive Radix Tree at the routing layer is a ranvier-core implementation detail, not a
differentiator — lead with inline C++/Seastar + engine-agnostic + no-K8s.)

## 4. Direction

The market split is an opportunity. The GIE/EPP standard owns K8s-native, multi-vendor,
vLLM-centric routing — at the cost of K8s lock-in and a per-request sidecar hop. That leaves a
real wedge for a fast, standalone, engine-agnostic router.

**Identity: the best standalone router for the un-served** — non-K8s / edge / sovereign-on-prem
/ mixed-engine deployments / latency-critical inline paths. **Mode: also conform** — ship an
EPP-compatibility mode (P1.4) so the core can plug into GIE-conformant gateways without
abandoning the standalone identity. Do the P0 routing-quality work so we're not behind on the
decision itself.

**Proof point:** publish a head-to-head **benchmark** of inline routing vs. a sidecar ext_proc
endpoint-picker hop, measuring the absolute routing-overhead delta — a number not currently
published anywhere.

**Out of scope:** leading with "ART" (an implementation detail); chasing multi-provider breadth
(owned by general-purpose gateways); building guardrail classifiers (expose a hook; integrate
providers, don't build).

## 5. Roadmap

### P0 — table-stakes routing quality

- **P0.1 — Precise, native KV-event mode.** Feed `prefix_hash_index` from vLLM's native
  KV-event stream (block-hash granularity) so residency is exact; keep the ART history path as
  the engine-agnostic fallback. _Anchors:_ `router_service.hpp`, `vllm_metrics.hpp`,
  `health_service.cpp`. _Complexity:_ High.
- **P0.2 — Unified weighted scorer.** Replace the sequential ART→load→cost override pipeline
  with one composable, tunable score (prefix depth + composite load + residency + cost + future
  SLO). _Anchors:_ `router_service.cpp`, `RoutingConfig`. _Complexity:_ Medium.
- **P0.3 — Disaggregated prefill/decode awareness.** Add a `pool_role` dimension
  (`prefill`/`decode`/`unified`) to backend registration + routing. _Anchors:_
  `backend_registry.hpp`, `register_backend_global`, `types.hpp`, `config_schema.hpp`.
  _Complexity:_ Medium–High.

### P1 — ecosystem parity

- **P1.4 — GIE Endpoint-Picker (EPP) compatibility mode.** Expose the routing core as an
  ext_proc gRPC endpoint picker so any GIE-conformant gateway can delegate to it. Pairs with the
  benchmark proof point. _Complexity:_ High.
- **P1.6 — OpenTelemetry GenAI semantic conventions.** Emit `gen_ai.*` attributes from the
  existing tracing path. _Complexity:_ Low.

### P2 — selective / out of scope

- **Semantic (embedding) cache** — optional add, not core; app-layer dedup orthogonal to the KV
  differentiator, and it drags in an embedding dependency.
- **KV-offload awareness (LMCache/Mooncake)** — be compatible now; later factor shared-store
  hits into the P0.2 score.
- **Guardrails (PII/prompt-injection)** — expose a hook; integrate providers, don't build
  classifiers.
- **Multi-provider shims + cost/quality model routing** — out of scope; general-purpose gateways
  own provider breadth, and research shows no model-router dominates all domains.

## 6. Sources & prior art (dated; confidence noted)

High confidence (multi-source + primary):
- Kubernetes blog, *Introducing Gateway API Inference Extension*, 2025-06-05; GIE reached GA
  (v1.x) in 2026 — `kubernetes-sigs/gateway-api-inference-extension`.
- Mooncake, USENIX FAST 2025 best paper (arXiv:2407.00079); Preble, ICLR 2025 (arXiv:2407.00023).
- vLLM native KV-event stream (block-hash granularity) documentation.
- Google Cloud, *GKE Inference Gateway GA*, 2025-09-11 + latency blog (source of the published
  `prefix:queue:kv` weighting heuristic).
- OpenTelemetry GenAI semantic conventions (experimental).

Context (directional): multiple production inference stacks (2024–2026) independently converged
on engine KV-events + a single weighted score; their self-reported speedups are
workload-specific and should be treated as directional, not guaranteed.
