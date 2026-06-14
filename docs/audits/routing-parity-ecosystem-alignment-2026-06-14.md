# §20 Routing Parity & Ecosystem Alignment — Retrospective (2026-06-14)

Closeout record for BACKLOG §20, the routing-parity-and-ecosystem-alignment
arc scoped in [`routing-direction-2026.md`](../architecture/routing-direction-2026.md).
Goal: bring Ranvier's routing core to parity with the 2024–2026 state of the art
(engine KV-events + a single weighted score + disaggregated awareness) and let it
slot into the standard LLM stack (GIE, OpenTelemetry GenAI, pluggable metering)
**without giving up the standalone inline data plane**, which remains the primary,
lower-latency path.

**Status: CLOSED (2026-06-14).** All actionable items (§20.1 P0.x, §20.2 P1.x)
shipped and merged. §20.3 is P2 / out of scope.

## What shipped

- **P0.2 — Unified weighted route scorer.** Replaced the sequential
  ART→load→cost override chain with one composable weighted score
  (`src/route_scorer.hpp`, a pure reactor-free decision core) applied in
  `get_backend_for_prefix`. Placement terms (prefix affinity + hardware price)
  pick the learn target; transient terms (load hinge over the strategy
  allowance, cost-budget hinge) pick the dispatch target. **Neutral default
  weights reproduce the previous pipeline's decisions bit-for-bit** — the
  consolidation is invisible until an operator tunes `routing.scoring.*`.

- **P0.3 — Disaggregated prefill/decode pool roles.** A `pool_role`
  (`unified`/`prefill`/`decode`) dimension threaded through registration and
  applied as a hard candidate *filter* on the scoring pass (not a weight):
  misses and load/cost diverts target prefill/unified only; decode pools are
  affinity-only. An availability valve waives the filter when only decode is
  live. Unlabeled fleets route identically to before.

- **P0.1 — Native vLLM KV-event mode (two PRs).** An opt-in in-core ZMQ
  subscriber on one dedicated OS thread consumes vLLM's `KVEventBatch`
  (msgpack) and feeds a per-shard `prefix_hash_index` that mirrors each
  backend's cache. ART hits on stream-fresh backends become **verified**
  (present = resident, absent = evicted-with-certainty), with the probabilistic
  gossip signal as fallback. Part 2 added route *materialization* (PUSH routes
  at covered block boundaries, trust-ordered insertion/eviction) and *replay*
  (sequence-gap recovery + connect-backfill). vLLM block hashes are bridged to
  Ranvier prefix hashes by incremental FNV accumulation — no shared hash
  function. `src/kv_event_{subscriber,decoder,ledger}.hpp`.

- **P1.6 — OpenTelemetry GenAI semantic conventions.** The `ranvier.request`
  span now carries `gen_ai.*` semconv attributes (operation, provider=engine
  class, request model/max_tokens, exact input tokens, server address, error
  type) so Datadog/GCP/Azure map it natively. Span name kept; response-side
  usage deliberately omitted (the span ends at dispatch handoff). Entirely at
  the `http_controller.cpp` call sites — `tracing_service.{hpp,cpp}` were
  already sufficient.

- **P1.5 — Usage-ledger sink seam.** A pluggable per-request `UsageLedgerSink`
  (interface + `Noop` default + process-wide factory) so external
  metering/billing backends consume per-API-key usage without core depending on
  any implementation. Per-request, per-shard (unlike the windowed telemetry
  sink); fed from the existing attribution data. `src/usage_ledger_{sink,schema}.hpp`.

- **P1.4 — GIE Endpoint-Picker (EPP) ext_proc mode (two PRs).** A gRPC
  `envoy.service.ext_proc.v3.ExternalProcessor` server (`src/gie_epp_server.{hpp,cpp}`)
  that exposes the routing core as a GIE-conformant endpoint picker, returning
  the chosen backend via the `x-gateway-destination-endpoint` header +
  `dynamic_metadata`. Part 1: the gRPC↔Seastar bridge + scaffolding. Part 2:
  prefix-aware routing — tokenize the request body with the inline path's chat
  template so tokens align with the KV-event residency index. See
  [`gie-epp.md`](../internals/gie-epp.md).

## Cross-cutting decisions & lessons

- **Behaviour-preserving by construction.** Every item is inert until opted
  into: neutral scorer weights, unlabeled pool roles, off-by-default
  KV-events runtime / sink / EPP. This let each land on main with "stock
  behaviour unchanged" as a checkable invariant.

- **Build-flag gating tracks the strategy, not the convenience.**
  `WITH_KV_EVENTS` defaults **ON** (it is *the* differentiator; libzmq is
  light); `WITH_GIE_EPP` defaults **OFF** (a compatibility mode, and gRPC is
  heavy). "Support a standard well" (clean code, conformance, docs, a supported
  flag, a CI lane) was kept distinct from "compile it into every binary."

- **One OS-thread + alien bridge pattern, both directions.** Heavy/foreign
  event loops (ZMQ, gRPC) live on dedicated OS threads and cross into the
  reactor only via `seastar::alien` — fire-and-forget `run_on` for the KV
  subscriber, request/response `submit_to` for the EPP. Decisions cross back as
  trivially-copyable PODs so no shard heap is freed off-reactor (Rules #14/#15);
  bodies are copied reactor-side from a `string_view`; named coroutines (not
  lambdas) carry the by-value payload across suspension (Rules #16/#21).

- **Minimal wire-compatible vendored protos beat dependency trees.** Both the
  msgpack KV decoder and the ext_proc proto were hand-trimmed to the exact
  fields used, with upstream field numbers verified — interoperable without
  dragging in msgpack-c or the full Envoy/xds/protobuf-well-known tree.

- **Pluggable sink seams compose.** The P1.5 usage-ledger sink mirrors the
  pre-existing telemetry sink (abstract interface + `Noop` + process-wide
  factory + forward-compat schema), keeping core free of concrete
  metering/export implementations.

- **Tokenization alignment is load-bearing for residency.** The EPP must
  tokenize with the same chat template the engine uses, because the KV-event
  residency index is keyed on the engine's token hashes — a template-less
  extraction silently misses. Prefix-aware EPP routing therefore *composes
  with and depends on* P0.1 in a pure-EPP deployment.

- **Static-analysis-only here; the Docker build is load-bearing verification.**
  Per CLAUDE.md the sandbox never builds (Seastar + now gRPC are too heavy), so
  the developer's container is where compile/link/cross-feature issues surface.
  Examples this arc: the libzmq-in-derived-images Docker fix, the MATERIALIZE
  freshness bug, and the gRPC/zeromq toolchain not being in a stale dev
  container. High-complexity items (P0.1, P1.4) were split into two PRs each
  specifically so this verification loop stayed tractable.

## Deferred / out of scope

- **P1.4 follow-ups (own tickets):** 429 request-shedding (needs a real
  overload signal, not a blunt load threshold); the published inline-vs-sidecar
  ext_proc overhead benchmark (needs cluster/hardware); per-model chat-template
  selection for heterogeneous fleets; `mode_override` for body acquisition;
  multi-endpoint fallback lists.
- **§20.3 (P2 / out of scope):** semantic (embedding) cache; KV-offload
  awareness (LMCache/Mooncake) beyond compatibility; guardrails (hook only);
  multi-provider shims and cost/quality model routing.

## Provenance

Written 2026-06-14 once §20.1/§20.2 closed, as standalone audit-doc history so
the per-item narrative doesn't crowd the active backlog. BACKLOG §20 carries the
per-item `[x]` completion notes and remains the live index; the planning
rationale lives in [`routing-direction-2026.md`](../architecture/routing-direction-2026.md).
