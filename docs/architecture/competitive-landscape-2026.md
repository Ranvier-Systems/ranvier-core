# Competitive Landscape & Open-Core Strategy (2026-06)

> Generated: 2026-06-07. Companion to [VISION.md](VISION.md) (product roadmap) and
> [BACKLOG.md](../../BACKLOG.md) (actionable items). VISION described the path from
> prefix router to "intelligence layer"; that roadmap (BACKLOG §15) is complete.
> This document maps where Ranvier sits against the mid-2026 field of inference-aware
> routers and AI gateways, draws the open-core line between `ranvier-core` (Apache-2.0)
> and `ranvier-internal` (proprietary), and lists the parity work as a P0/P1/P2 roadmap.
> The actionable P0 checkboxes live in [BACKLOG §20](../../BACKLOG.md#20-competitive-parity-2026-06-07).

---

## 1. Bottom line

Ranvier is in the **right category at the right time**. KV-cache/prefix-aware routing is
now the highest-ROI lever in LLM serving, validated by peer-reviewed work (Preble,
ICLR 2025; Mooncake, USENIX FAST 2025 best paper) and by every major production system
(llm-d, GKE Inference Gateway, NVIDIA Dynamo, SGLang, AIBrix). But in the ~18 months since
the idea took off, the field **standardized and moved past where we are today** on two axes:

1. **Signal freshness** — leaders route on *live, block-level KV-cache events* from the
   engine, not learned history. We have a head-start here (push-based eviction, residency
   weighting) but on a *bespoke* protocol, not the ecosystem-standard vLLM KV-event stream.
2. **Topology** — leaders are *disaggregation-aware* (separate prefill/decode pools). We
   treat all backends as homogeneous.

Meanwhile a Kubernetes-native standard — the **Gateway API Inference Extension (GIE)** with
its **Endpoint Picker (EPP)** protocol — consolidated the ecosystem behind CNCF/NVIDIA/
Google/Red Hat/ByteDance. We are outside it.

Our genuine, defensible edge is **architectural**: a C++/Seastar inline data plane that makes
routing decisions in microseconds with no GIL and **no per-request ext_proc sidecar hop**,
deployable as a **single binary without Kubernetes**, and **engine-agnostic** (vLLM *and*
SGLang/TensorRT-LLM/Ollama/LM Studio). The strategic call is **not** "catch up on features"
— it is "stay the fast, simple, standalone alternative, and make the routing core pluggable
into the standard." See §5.

---

## 2. The landscape (mid-2026)

Two camps. We straddle them but are judged against **Camp A**.

### Camp A — KV-cache/inference-aware routers (our direct competitors)

Route across replicas of the *same* model to maximize KV-cache reuse + balance GPU load.

| System | Lang / arch | Routing signal | Live KV-state? | Disagg (P/D)? | Deploy | Backing |
|---|---|---|---|---|---|---|
| **Ranvier** | C++/Seastar, inline proxy | learned-history ART + hash; scrapes vLLM KV%/queue; residency weight; cost budget | ~ partial (own evict protocol + probabilistic residency) | ✗ | **single binary, no K8s** | solo |
| **llm-d** | Go EPP over Envoy | filter→score→select: prefix + KV-util + predicted-latency SLO | ✓ precise (vLLM KVEvents → global index) | ✓ (NIXL) | K8s + CRDs + sidecars | CNCF, RH/Google/IBM/NVIDIA/CoreWeave |
| **GIE / EPP** (standard) | Go, ext_proc spec | KV-util, queue, LoRA, prefix block-hash LRU | ✓ | via impl | K8s standard | SIG-Network (GA 2026) |
| **NVIDIA Dynamo** | Rust core, Python API | cost = `w·prefill_blocks + decode_blocks`; KVIndexer+RadixTree from KV events | ✓ | ✓ conditional | framework | NVIDIA (1.0, 2026) |
| **AIBrix** | Go + Envoy ext_proc | Preble + hash-prefix + VTC fairness; ZMQ KV events | ✓ | ✓ | K8s control plane | ByteDance |
| **vLLM production-stack router** | Python proxy | RR / session / prefix (longest-match, WIP) | partial | via stack | K8s/Helm | vLLM project |
| **SGLang router** | Rust | cache-aware (approx radix shadow of RadixAttention) + P2C | ✓ shadow | roadmap | sidecar/PyPI | LMSYS/SGLang |
| **Ray Serve PrefixCacheAffinityRouter** | Python/C++ | per-replica radix prefix affinity | shadow | — | Ray | Anyscale |

**The architectural lesson from Camp A:** the router is no longer a load balancer — it is a
*stateful, KV-topology-aware scheduler*. The leaders converged on (a) a live per-worker
block index fed by engine KV-events, and (b) a **single weighted score** that blends prefix
overlap + queue depth + KV utilization (GKE publishes `prefix:queue:kv = 3:3:2`; Dynamo uses
`w·prefill_blocks + decode_blocks`).

Honest note: **no surveyed competitor uses an *Adaptive* Radix Tree at the routing layer** —
they use plain radix/Patricia tries or block-hash tables. The data-structure choice is not
the moat; the *policy* and *signal freshness* are. Lead messaging with **C++/Seastar inline +
engine-agnostic + no-K8s**, not "ART."

### Camp B — broader AI gateways (overlap on *gateway* features, generally NOT KV-aware)

| System | Lang/arch | KV-aware? | Notable features |
|---|---|---|---|
| **LiteLLM** | Python/FastAPI | ✗ | 100+ providers, virtual keys, budgets, semantic cache, guardrail plugins (Presidio/PII) |
| **Kong AI Gateway** | OpenResty/Lua | ✗ | semantic routing + caching, prompt compression, token rate-limit, guardrails |
| **Portkey** | TS/Node (Hono), OSS Mar 2026 | ✗ | routing/fallback, semantic cache, 50+ guardrails, governance/budgets |
| **Higress** | Envoy/Istio + Wasm | ~ via managed GIE | multi-model proxy, semantic cache, token rate-limit; GIE-conformant |
| **Cloudflare AI Gateway** | managed edge | ✗ (exact-match only) | DLP, rate-limit, dynamic routing, unified billing |
| **Envoy AI Gateway** | Envoy (C++) + Go | ~ via GIE EPP (v0.3) | OpenAI-compat multi-provider, token limits, upstream auth |

The two camps are converging: Envoy AI Gateway and Higress reach KV-aware routing by
**delegating to the GIE EPP** — they bolt the inference brain on via the standard rather than
building it. Ranvier is the rare project that does KV-aware routing **natively in the data
plane**.

---

## 3. Where Ranvier stands today (grounded in the code)

### Real, defensible wins

- **Latency & efficiency of the routing decision.** Inline C++/Seastar routing (ART lookup
  <50µs) avoids the cost every Camp-A K8s system pays: a per-request gRPC ext_proc round-trip
  to a separate Go EPP sidecar. **None of the K8s projects publish the absolute ms overhead of
  that hop** — that is our open benchmark to land (see P1.4).
- **Deployment simplicity & reach.** Single binary, **no Kubernetes, no CRDs, no sidecars**.
  The entire Camp-A standard *requires* K8s. Non-K8s clusters, edge, sovereign on-prem, and
  developer-local are uncontested ground.
- **Engine-agnostic.** We route on raw token prefixes without engine cooperation, so we work
  across vLLM + SGLang + TensorRT-LLM + Ollama + LM Studio (`BackendType`, `src/types.hpp`).
  GIE precise-prefix routing effectively *requires vLLM* today; SGLang isn't even supported.
- **Already further along than most pure routers.** We ship rate limiting, priority
  scheduling, gossip multi-node route sharing, intent classification, chat templating, K8s +
  local discovery, API-key auth + per-key attribution, and `prompt_token_ids` injection.
- **We are not eviction-blind.** Unlike the "history-only" routers, we already have:
  - push-based cache eviction (`POST /v1/cache/events`, `evict_by_prefix_hash_global`, the
    `prefix_hash_index`; BACKLOG §2.1, `docs/architecture/push-cache-eviction-notifications.md`);
  - probabilistic residency weighting (`VLLMMetrics::estimated_prefix_retention`,
    `cache_residency_threshold`) broadcast across shards *and* gossiped to peers;
  - integrated load-aware + cost-aware routing (`get_composite_backend_load`, `CostBudgetGuard`,
    `RoutingConfig::load_aware_routing`, `cost_routing`), plus compression-aware scoring.

### Where we're behind

- **History-based prefix routing, on a bespoke freshness protocol.** Our eviction signal is a
  Ranvier-defined HTTP protocol; the ecosystem standardized on vLLM's **native KV-event stream**
  (block-hash granularity, ZMQ) consumed by Dynamo (`KVPublisher`), AIBrix (ZMQ), and llm-d
  (`kvcache.Index`). Our `prefix_hash_index` is the right substrate — it just needs to be fed
  from the native stream so residency is *exact*, not probabilistic, and so "loaded" events can
  create routes (today they can't — the wire format carries no tokens; see `load_route_global`).
- **No disaggregation (P/D) awareness.** `BackendType` is engine class, not pool role. In a
  disaggregated deployment we silently misroute and forfeit the gains disaggregation exists to
  deliver. This is *table-stakes* for frontier-model serving in 2026.
- **Outside the standard.** Not GIE-conformant: we can't be dropped into Istio/Envoy/GKE/
  agentgateway as the endpoint picker, and we don't consume `InferencePool`/`InferenceObjective`
  CRDs.
- **Sequential-override scoring, not a unified score.** We layer ART → load redirect → cost
  redirect as *steps* (`PrefixRouteResult.was_load_redirect`, `was_cost_redirect`). The field
  fused these into one tunable weighted score. We have every input already; we mostly need to
  fuse them (see P0.2).
- **Adoption & ecosystem.** A solo project vs. CNCF/NVIDIA/ByteDance/Google. Their KV
  connectors, NIXL, LMCache, and EPP libraries are Go/Rust/Python — **we must reimplement
  integrations they get for free.** That is the structural tax of the C++ differentiator.

---

## 4. The open-core boundary

**Decision (2026-06-07): "Ops & governance" line, with a proprietary `ranvier-internal`.**

> **The principle.** Anything that improves a *single request's* routing, latency, or quality
> stays in `ranvier-core` (Apache-2.0). Anything that helps an *organization* operate, govern,
> and account for a *fleet* of Ranvier nodes across many teams goes to `ranvier-internal`
> (proprietary). This keeps our differentiator free — adoption is the moat against llm-d — and
> sells to the org, not the engineer.

| Stays OSS — `ranvier-core` (Apache-2.0) | Goes proprietary — `ranvier-internal` |
|---|---|
| **All routing & performance**, incl. the P0 differentiators (precise KV-event mode, unified scorer, P/D awareness) | **Multi-tenancy**: tenant isolation, hierarchical quotas/budgets, chargeback/showback, spend ledgers |
| Clustering (gossip), health, circuit breaker, discovery (K8s + local) | **Governance**: SSO (OIDC/SAML), fine-grained RBAC, policy engine, approval workflows |
| Tokenization, intent classification, chat templates, priority scheduling | **Compliance**: tamper-evident audit log, data-residency controls, retention, SOC2/ISO evidence |
| Prometheus metrics, OTel tracing, local dashboard | **Fleet / multi-cluster**: centralized control plane, federation, cross-cluster config, fleet dashboards |
| Basic API-key auth + per-key attribution; per-IP / per-key rate limiting (DoS protection) | **Enterprise integrations**: external secrets (Vault), guardrail-provider connectors (PII/prompt-injection), SIEM export |
| mTLS to backends, transport security basics (trust requires these to be open) | **Support / SLA**, hardened images, LTS builds |
| **GIE/EPP compatibility mode** (must be OSS to be adopted as a standard endpoint picker) | Managed/hosted control plane (if a SaaS motion is added later) |

**Boundary subtleties worth pinning:**
- *Attribution vs. enforcement.* Per-API-key **attribution** (observability) is already OSS and
  stays OSS. Per-key/IP **rate limiting** (protection) stays OSS. Cross-tenant **budget/quota
  governance with chargeback** is enterprise.
- *Security basics vs. enterprise auth.* A single admin key + API-key auth + mTLS stay OSS.
  **SSO, RBAC roles, and audit logging** (BACKLOG §4.3 / §3.3) move to enterprise.
- *Guardrails.* Out of scope for core entirely; OSS exposes a filter hook, the enterprise repo
  ships the *connectors* to Presidio/Bedrock/Pillar etc. (industry pattern — everyone delegates).

**Build for the boundary, don't fork across it.** `ranvier-core` already has the right seams:
`TelemetrySinkConfig` ("pluggable telemetry sink," `config_schema.hpp`), the overridable sink
factory (commit #539), `LoadScoreCallback`, and `PoolCleanupCallback`. The enterprise layer
should attach through stable extension points (an auth/authz hook, a policy hook, an
audit-event emitter, a usage-ledger sink), **not** by patching core. Each new enterprise
capability should land in OSS as a *seam* (a no-op default + a registration point); the
proprietary implementation lives in `ranvier-internal`. Track the seam contracts in a
dedicated `docs/architecture/extension-points.md` when the first one ships.

---

## 5. Strategy: differentiate vs. conform

The market bifurcated, and that is an opportunity. The GIE/EPP standard owns *K8s-native,
multi-vendor, vLLM-centric* routing — at the cost of K8s lock-in and a per-request sidecar hop.
That leaves us a real, uncontested wedge.

- **Option 1 — "Fastest EPP" (conform to ride distribution).** Implement the EPP ext_proc
  protocol so our C++ core can drop into Istio/Envoy/GKE/agentgateway. Pro: rides a CNCF
  distribution channel. Con: we become a *component* competing head-on with
  `llm-d-inference-scheduler` on its home turf, where it has native KV-events + CNCF backing.
- **Option 2 — "Best standalone router for the un-served" (differentiate).** Own non-K8s /
  edge / sovereign-on-prem / mixed-engine fleets / latency-critical inline paths — exactly where
  the K8s camp is weak or absent.

**Recommendation: Option 2 as identity, Option 1 as a mode.** Keep Ranvier's soul as the fast,
single-binary, engine-agnostic inline router — the only place the C++/Seastar bet is genuinely
differentiated. Do the P0 work so we're not behind on routing *quality*, and ship the
**EPP-compatibility mode** (P1.4) so we can also plug into the standard without abandoning the
wedge. The sharpest near-term proof point is a **published head-to-head benchmark**: Ranvier
inline vs. Istio/Envoy + GIE EPP, measuring the absolute routing-overhead delta. Nobody in
Camp A has published that number — and it is the one place we almost certainly win.

**Stop doing / out of scope:** leading with "ART" (not the moat); chasing multi-provider
breadth (lost vs. LiteLLM/Portkey); building guardrail classifiers (integrate, don't build).

---

## 6. Prioritized roadmap

All P0/P1 routing items are **OSS** (per the §4 boundary). Enterprise items are tagged.

### P0 — table-stakes to stay a credible KV-aware router (OSS)

- **P0.1 — Precise, native KV-event mode.** Feed the existing `prefix_hash_index` from vLLM's
  *native* KV-event stream (block-hash granularity) so residency is exact, upgrading from
  today's probabilistic `estimated_prefix_retention` + bespoke `/v1/cache/events`. Realizes
  Phase 3 of the push-eviction design (BACKLOG §2.1) but aligned to the ecosystem wire format.
  Keep the ART history path as the engine-agnostic fallback for SGLang/Ollama/TRT.
  _Anchors:_ `router_service.hpp` (cache-event API), `vllm_metrics.hpp`, `health_service.cpp`,
  `docs/architecture/push-cache-eviction-notifications.md`. _Complexity:_ High.
- **P0.2 — Unified weighted scorer.** Replace the sequential ART→load→cost *override* pipeline
  with one composable, tunable score (prefix depth + composite load + residency + cost + future
  SLO), mirroring GKE's `prefix:queue:kv` weights and llm-d's scorer plugins. Inputs already
  exist; this is a refactor of the decision core. _Anchors:_ `router_service.cpp`
  (`get_backend_for_prefix` / `route_request`), `RoutingConfig`. _Complexity:_ Medium.
- **P0.3 — Disaggregated prefill/decode awareness.** Add a `pool_role` dimension
  (`prefill`/`decode`/`unified`) to backend registration + routing so we play in disaggregated
  deployments. We need not own KV transfer (NIXL/LMCache do) — we must stop being topology-blind.
  _Anchors:_ `backend_registry.hpp`, `register_backend_global`, `types.hpp`, `config_schema.hpp`.
  _Complexity:_ Medium–High.

### P1 — strategic parity / ecosystem

- **P1.4 — GIE Endpoint-Picker (EPP) compatibility mode (OSS).** Expose the routing core as an
  ext_proc gRPC endpoint picker so any GIE-conformant gateway can delegate to it. Pairs with the
  published-benchmark proof point. _Complexity:_ High.
- **P1.5 — Per-tenant token budgets, quotas & audit log (ENTERPRISE → `ranvier-internal`).**
  Build on OSS per-key attribution + SQLite; the enforcement/ledger/audit governance is the
  paid layer. The OSS side ships the usage-ledger *sink* seam. _Complexity:_ Medium.
- **P1.6 — OpenTelemetry GenAI semantic conventions (OSS).** Emit `gen_ai.*` attributes from
  the existing tracing path so we slot into the standard observability stack. _Complexity:_ Low.

### P2 — adopt selectively / deliberately out of scope

- **Semantic (embedding) cache** — optional OSS add, not core. Useful for RAG/support bots, but
  app-layer dedup orthogonal to our KV differentiator, and drags in an embedding dependency.
- **KV-offload awareness (LMCache/Mooncake)** — be compatible now; later factor shared-store
  hits into the P0.2 score.
- **Guardrails (PII/prompt-injection)** — OSS exposes a hook; connectors are enterprise. Never
  build classifiers.
- **Multi-provider shims + cost/quality model routing (RouteLLM-style)** — out of scope;
  Python/TS gateways own provider breadth, and research shows no model-router dominates all
  domains.

---

## 7. Sources (dated; confidence noted)

High confidence (multi-source + primary):
- Kubernetes blog, *Introducing Gateway API Inference Extension*, 2025-06-05; GIE reached GA
  (v1.x), 2026 — `kubernetes-sigs/gateway-api-inference-extension`.
- CNCF blog, *Welcome llm-d to the CNCF*, 2026-03-24; llm-d *KV-Cache Wins* / *v0.4–v0.5* blogs
  (2025–26); Red Hat Developer, *KV-cache-aware routing with llm-d*, 2025-10-07.
- NVIDIA Dynamo GA (2026) + KV-router/disaggregation docs; Baseten, *2× faster with Dynamo
  KV-aware routing* (independent corroboration of the 2× TTFT claim).
- AIBrix v0.3 (2025-05-21) & v0.4 (2025-08) blogs (Preble/VTC, ZMQ KV-event sync); arXiv:2504.03648.
- LMSYS, *SGLang v0.4 cache-aware load balancer*, 2024-12-04; `sglang-router` (Rust), PyPI.
- Google Cloud, *GKE Inference Gateway GA*, 2025-09-11 + latency blog (`prefix:queue:kv = 3:3:2`).
- Mooncake, USENIX FAST 2025 best paper (arXiv:2407.00079); Preble, ICLR 2025 (arXiv:2407.00023).
- Envoy AI Gateway v0.1 (2025-02-25) / v0.3 EPP integration; LiteLLM, Kong AI Gateway 3.8/3.11,
  Portkey, Cloudflare AI Gateway docs (2024–2026); OpenTelemetry GenAI semconv (experimental).

Hedged (vendor-reported, workload-specific): all "Nx faster" figures (llm-d up to ~57×, GKE
~35–96%, SGLang 1.9×, Dynamo 2× TTFT) are self-reported and workload-sensitive — treat as
directional, not guaranteed.

Excluded from this analysis (single-source / unverifiable at time of writing): several 2026
arXiv preprints on cross-region/cross-datacenter KV routing surfaced during research but could
not be corroborated; their specific numbers did not drive any recommendation here.
