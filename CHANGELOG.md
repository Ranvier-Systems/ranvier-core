# Changelog

All notable changes to Ranvier Core will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- **GIE EPP integration test + overhead microbenchmark** (BACKLOG §20.2 P1.4
  follow-up) — A gRPC `ext_proc` client (`tests/integration/epp_client.py`,
  stubs generated at runtime from `proto/ext_proc_min.proto`) that drives a real
  running EPP and is shared by both a new integration test and a microbenchmark.
  `tests/integration/test_gie_epp.py` (+ `docker-compose.epp-test.yml`, a
  standalone `WITH_GIE_EPP=ON` node from the `Dockerfile.gie-epp` builder stage +
  a mock backend) asserts the end-to-end picker behaviour the unit/CI coverage
  couldn't: `ImmediateResponse` 503 with no backend, the
  `x-gateway-destination-endpoint` header (+ matching `dynamic_metadata`) once a
  backend is registered, and bodyless (headers-EOS) routing. The
  `epp-overhead` microbenchmark (`scripts/bench-epp-overhead.sh` →
  `tests/integration/epp_microbench.py`, methodology in
  `docs/benchmarks/epp-overhead-microbenchmark.md`) reports the per-request
  ext_proc routing-decision + bridge latency. New `make test-epp` / `make
  bench-epp`; Python gRPC deps in `tests/integration/requirements.txt` (the
  suite skips the EPP test when they're absent). No core/runtime change.

- **GIE Endpoint-Picker (EPP) ext_proc mode — part 2: prefix-aware routing**
  (BACKLOG §20.2 P1.4, completing the item) — The picker now routes on the
  actual prompt instead of part 1's header-level load/hash. It captures the
  ext_proc `request_body` phase, and on a dedicated reactor coroutine
  (`route_on_reactor`, bridged via `alien::submit_to`) extracts the prompt and
  tokenizes it with **the same chat template the inline path uses**
  (`assets.chat_template_format`) before calling `route_request` — so the EPP's
  tokens align with the backend's and with the KV-event-fed residency index
  (P0.1), which is what makes prefix-/residency-aware selection actually hit.
  Bodyless requests (headers `end_of_stream`) and an unloaded/empty prompt fall
  back to load/hash — no regression. The chosen endpoint is also surfaced in
  `dynamic_metadata` (`envoy.lb` namespace) alongside the header, via a minimal
  inlined `google.protobuf.Struct` (wire-compatible, no `struct.proto` import).
  The body is copied reactor-side from a `string_view` (no cross-thread free)
  and the named-coroutine bridge avoids the Rule #16 lambda-lifetime trap.
  429 request-shedding and the inline-vs-sidecar benchmark remain follow-ups.
  Still build-gated `WITH_GIE_EPP=OFF`; stock builds unaffected.

- **GIE Endpoint-Picker (EPP) ext_proc mode — part 1: bridge + server**
  (BACKLOG §20.2 P1.4) — Optional gRPC `envoy.service.ext_proc.v3.ExternalProcessor`
  server that exposes Ranvier's routing core as a Gateway API Inference Extension
  Endpoint-Picker, so a GIE-conformant gateway can delegate endpoint selection to
  it (returning the chosen backend via the `x-gateway-destination-endpoint`
  header, or `ImmediateResponse` 503 when none is ready). A compatibility mode
  alongside — not a replacement for — the standalone inline data plane.
  Build-gated behind `WITH_GIE_EPP` (**default OFF**: gRPC + protobuf are heavy
  and the inline path is primary); when ON, the ext_proc stubs are generated at
  build time from `proto/ext_proc_min.proto` — a minimal, wire-compatible subset
  of the Envoy proto (field numbers verified against upstream) rather than the
  full proto tree. Runtime opt-in via `gie_epp.enabled` / `RANVIER_GIE_EPP_*`.
  gRPC runs on its own threads; handlers bridge into the reactor with
  `seastar::alien::submit_to` (the request/response inverse of the KV
  subscriber's fire-and-forget bridge) and reuse `route_request` +
  `get_backend_address`, with the decision crossing back as a trivially-copyable
  POD so no shard heap is freed off-reactor. This part routes at the header level
  (empty-token load/hash selection); prefix-aware routing over the tokenized
  request body, `dynamic_metadata`, 429 shedding, and the inline-vs-sidecar
  benchmark are the follow-up. Stock builds (`WITH_GIE_EPP=OFF`) are unaffected.
  CI coverage without changing the default: the pure decision-helper test runs
  in the default unit-test lane (always built), and a dedicated `WITH_GIE_EPP=ON`
  lane (`Dockerfile.gie-epp` + `.github/workflows/gie-epp-tests.yml`) compiles +
  links the gRPC server and runs ctest. The gRPC toolchain is in
  `Dockerfile.base`; `make gie-epp-test` is the local one-liner.

- **Usage-ledger sink seam** (BACKLOG §20.2 P1.5) — A pluggable, per-request
  usage-event sink for external metering / attribution / billing backends,
  layered on the existing per-API-key attribution without core depending on any
  concrete implementation. `src/usage_ledger_sink.hpp` defines the
  `UsageLedgerSink` interface, the `NoopUsageLedgerSink` default, and a
  process-wide factory seam (`set_/get_usage_ledger_sink_factory`);
  `src/usage_ledger_schema.hpp` defines the `UsageEvent` with the same
  forward-compatibility contract as the telemetry schema. Unlike the telemetry
  sink (aggregate, content-free, shard-0 windowed), the usage sink is
  per-request and per-shard: each shard installs its own instance and the
  completion path calls a synchronous, non-blocking `record(UsageEvent)` next to
  the existing `request_attribution` enqueue (both fed from one computed set of
  outcome values, independently gated; skipped entirely when neither is active).
  The event carries the attribution identifiers (`api_key_id`, `request_id`),
  `model`, and estimated token/cost figures — **no** prompt/response content and
  **no** token IDs. Token/cost are the same pre-flight estimates the attribution
  rows record (the upstream response `usage` is not parsed). Independent of
  SQLite persistence. Gated by `usage_ledger.enabled` (default false;
  `RANVIER_USAGE_LEDGER_ENABLED`); toggling requires a restart. The stock build
  wires the Noop sink, so the completion path is a single null check when
  disabled.

- **OpenTelemetry GenAI semantic conventions** (BACKLOG §20.2 P1.6) — The
  `ranvier.request` root span now carries the OpenTelemetry GenAI semconv
  (`gen_ai.*`) attributes, so Ranvier slots into the standard
  LLM-observability stack (Datadog/GCP/Azure map the semconv natively):
  `gen_ai.operation.name` (`chat`/`text_completion`), `gen_ai.provider.name`
  (backend engine class — `vllm`/`sglang`/…; semconv sanctions custom values,
  more honest than labeling a self-hosted backend `openai`),
  `gen_ai.request.model`, `gen_ai.request.max_tokens`,
  `gen_ai.usage.input_tokens` (the exact tokenized count — omitted under
  partial or skipped tokenization rather than reporting a fraction),
  `server.address`/`server.port` (the upstream behind the proxy), and
  `error.type` plus span-status Error on the routing-failure paths. The span
  name stays `ranvier.request` (dashboards key on attributes). Response-side
  usage (output tokens, response model) is intentionally not emitted — the
  request span ends at dispatch handoff, before the response streams back.
  Gated by `telemetry.genai_semconv` (default true;
  `RANVIER_TELEMETRY_GENAI_SEMCONV`); with the flag off behavior is
  byte-identical to before. The `model` field is read during the existing
  single request-body parse, adding no JSON work to the hot path.

- **Native KV-event mode, part 1: verified residency** (BACKLOG §20.1 P0.1) —
  Opt-in subscriber for vLLM's native KV-cache event stream (ZMQ PUB,
  msgpack `KVEventBatch`): `BlockStored`/`BlockRemoved` events maintain
  `prefix_hash_index` as a block-exact, ~ms-fresh mirror of each opted-in
  backend's cache. ART hits on stream-fresh backends are then VERIFIED —
  present = resident (probabilistic gate skipped), absent = evicted
  (downgrade fires with certainty); stream faults (sequence gaps, decode
  errors) reset trust and fall back to the probabilistic gossip signal.
  vLLM block hashes are bridged to Ranvier prefix hashes by incremental
  FNV accumulation along block parent chains (`src/kv_event_ledger.hpp`) —
  no shared hash function needed. Per-block evictions deliberately leave the
  RadixTree untouched (the verified check neutralizes stale routes);
  `AllBlocksCleared` purges. Build option `WITH_KV_EVENTS` (libzmq, default
  ON; `-DWITH_KV_EVENTS=OFF` for a zmq-free server build — unit-test-only
  builds never need libzmq); config `kv_events:` /
  `RANVIER_KV_EVENTS_*`; per-backend opt-in via static `kv_events_port` or
  the `ranvier.io/kv-events-port` annotation. Metrics:
  `router_native_kv_ops_total`, `router_native_verified_hits_total`,
  `router_native_verified_evictions_total`, `router_native_stream_resets_total`,
  `router_native_index_overflow_total`.

- **Native KV-event mode, part 2: route materialization + replay**
  (BACKLOG §20.1 P0.1, completing the item) — `BlockStored` token chains now
  insert `RouteOrigin::PUSH` routes at every covered block boundary
  (`kv_events.materialize_routes`, default on; `max_materialize_tokens`
  bounds per-chain retention), so prefixes computed without Ranvier's
  involvement become routable. Trust order enforced at insertion
  (`RadixTree::insert_if_trusted`: locally-learned routes are never
  overwritten, PUSH outranks gossip; same-backend confirmations LRU-refresh)
  and at eviction (`evict_lowest_trust()`: REMOTE → PUSH → LOCAL, replacing
  `evict_oldest_remote()` and making the documented precedence literal).
  Forward sequence gaps recover via vLLM's replay ROUTER socket (per-backend
  `kv_events_replay_port` / `ranvier.io/kv-events-replay-port`; DEALER
  client, 8-byte big-endian start seq, sentinel-terminated, contiguity
  verified) instead of resetting; `replay_on_connect` backfills the
  publisher's buffered window on subscribe — restart cold-start, bounded by
  that buffer. Shipments are chunked by op weight so token-carrying ops
  can't oversize a reactor application. Metrics:
  `router_native_routes_materialized_total`,
  `router_native_materialize_trust_skips_total`.

- **Disaggregated prefill/decode pool roles** (BACKLOG §20.1 P0.3) — Backends
  carry a `pool_role` (`unified` default / `prefill` / `decode`) through
  registration, acting as a hard eligibility filter on the unified route
  score: fresh cache misses and load/cost/price diverts target only
  prefill/unified backends; decode pools are affinity-only (reached via a
  learned/ART route), preserving decode affinity while new turns go to
  prefill. KV transfer between pools stays with the serving stack
  (NIXL/LMCache). If only decode pools are live the filter is waived
  (availability first) and `router_pool_role_fallbacks_total` counts it.
  Unlabeled fleets route identically to before. Labeling:
  `ranvier.io/pool-role` EndpointSlice annotation, static-backend YAML
  `pool_role:`, admin `POST /admin/backends?...&pool_role=`; persisted in
  SQLite (`pool_role` column, additive migration, default `unified`) and
  surfaced in `GET /admin/backends`.

### Changed

- **Unified weighted route scorer** (BACKLOG §20.1 P0.2) — The post-anchor
  routing decision is now one weighted ranking over the live candidates
  (`src/route_scorer.hpp`) instead of the former sequential override chain
  (residency downgrade → load redirect → cost redirect → hardware-cost
  preference). Stable terms (prefix affinity, hardware price) pick the
  placement / learn target; transient terms (load hinge over the strategy
  allowance, cost-budget hinge) pick the dispatch target. Per-signal weights
  in `routing.scoring.*` (env `RANVIER_SCORING_*`): `prefix_weight`,
  `load_weight`, `residency_weight`, `cost_weight`, `price_weight`, plus a
  reserved `slo_weight` seam. **Neutral defaults reproduce the previous
  pipeline's decisions exactly** — the existing strategy/threshold keys keep
  their authority until weights are tuned. Counter-semantics note:
  `router_load_aware_fallbacks_total` now counts every load-driven ART-hit
  divert under `bounded_load` (the former re-probe skipped diverts landing on
  the primary hash bucket); with cost routing enabled, the budget/fast-lane
  triggers blend continuously instead of overriding sequentially, and the 4b
  divert target is deterministic (least budget pressure) rather than
  random-two-choices.

## [2.1.0] - 2026-04-11

Performance release. Introduces partial tokenization for routing — truncates
input text to a byte budget before tokenizing, since the ART lookup only
needs the first `prefix_token_length` tokens (default 128). Full tokenization
is deferred and only performed when token forwarding to `/v1/completions`
backends is enabled.

### Added

- **Partial Tokenization for Routing** — Two-phase tokenization: a truncated
  input (default 768 bytes, ~128 tokens) is tokenized for routing, and full
  tokenization is deferred to the forwarding path only when needed. Disabled
  automatically when multi-depth routing is enabled or token forwarding
  requires the full token vector. Config: `routing.enable_partial_tokenization`
  (default true), `routing.partial_tokenize_byte_budget` (default 768),
  `routing.partial_tokenize_bytes_per_token` (default 6).
  Env: `RANVIER_PARTIAL_TOKENIZATION`, `RANVIER_PARTIAL_TOKENIZE_BUDGET`.
- **TokenizerService::truncate_for_routing()** — UTF-8-safe byte budget
  truncation utility. Returns a `string_view` into the original text
  (zero-copy).
- **Metrics**: `ranvier_tokenization_partial_total`,
  `ranvier_tokenization_partial_bytes_saved`,
  `ranvier_tokenization_deferred_full_total`.

### Performance

CI benchmark (100 users, 60s, docker-compose mock backends) vs v2.0.0 baseline:

| Metric        | v2.0.0  | v2.1.0  | Delta  |
|---------------|---------|---------|--------|
| P50 latency   | 49ms    | 46ms    | -6%    |
| P90 latency   | 66ms    | 52ms    | -21%   |
| P99 latency   | 85ms    | 59ms    | -30%   |
| Throughput    | 502 rps | 513 rps | +2%    |
| Failure rate  | 0%      | 0%      | —      |

The P99 improvement reflects reduced thread pool queue contention and
context-switch overhead — tokenization still runs off-reactor, but the
smaller input produces tokens faster, freeing thread pool capacity.
Real-world impact on GPU-backed deployments (where tokenization is the
dominant per-request cost) is expected to be even more significant;
re-validation on 8x A100 pending GPU availability.

## [2.0.0] - 2026-04-05

Intelligence Layer release. Transforms Ranvier from a "smart router" into a full
Intelligence Layer for Inference Infrastructure, completing the entire VISION.md
roadmap (Phases 1-4, all 🔓 Core/Open Source items).

### Foundation (Phase 1)

- **Request Cost Estimation (VISION 1.1)** — Heuristic token count and cost derivation
  from request body. Populates estimated_input_tokens, estimated_output_tokens, and
  estimated_cost_units in ProxyContext before routing.
- **Priority Tiers (VISION 1.2)** — Four-tier priority classification (CRITICAL, HIGH,
  NORMAL, LOW) via X-Ranvier-Priority header, User-Agent pattern matching, or cost-based
  defaults. Per-priority metrics.
- **Priority Queue (VISION 1.2 integration)** — RequestScheduler with per-tier bounded
  deques, fair scheduling by agent (oldest-last-served wins), queue-jumping for CRITICAL,
  and per-agent pause-aware dequeue. Replaces direct semaphore acquire when enabled.
- **Intent Classification (VISION 1.4)** — Wire-format inspection classifies requests as
  AUTOCOMPLETE (FIM fields), EDIT (system prompt keywords/tags), or CHAT (default).
  Advisory routing hint for downstream phases.

### Cloud Intelligence (Phase 2)

- **BackendRegistry Interface** — Abstract interface decoupling HealthService and
  LocalDiscoveryService from RouterService. Enables independent testing and clean
  extension for vLLM metrics.
- **vLLM Metrics Ingestion (VISION 2.1)** — Periodic scraping of vLLM Prometheus
  `/metrics` endpoint. Extracts GPU request queue depth, KV cache usage, memory,
  and throughput. Composite load_score() (0.0–1.0) for routing decisions.
  Prometheus text parser included. Graceful degradation for non-vLLM backends.
- **GPU-Aware Load Routing (VISION 2.2)** — Per-shard GPU load cache broadcast from
  shard 0. get_composite_backend_load() blends shard-local in-flight counts with
  vLLM GPU metrics. Integrated into P2C, bounded-load, and median-based routing
  strategies. Configurable gpu_load_weight and load_redirect_threshold.
- **Cost-Based Routing (VISION 2.3)** — Per-backend cost budget tracking. Small-request
  fast lane routes cheap requests to least-cost-loaded backends. Large requests check
  budget headroom before routing. Reserve on route, release on completion.

### Ranvier Local (Phase 3)

- **Local Mode Config (VISION 1.3)** — `local_mode.enabled` flag disables clustering,
  gossip, and persistence. RANVIER_LOCAL_MODE=true environment variable support.
  Auto-enables backend discovery.
- **Local Backend Discovery (VISION 3.1)** — Auto-discovers Ollama, vLLM, LM Studio,
  llama.cpp, LocalAI, and Text Generation WebUI using semantic liveness checks (HTTP
  GET /v1/models with 50ms timeout). Solves the zombie port problem. Hot-add/remove
  with 3-miss hysteresis.
- **Agent-Aware Request Handling (VISION 3.2)** — AgentRegistry identifies agents from
  User-Agent headers and X-Ranvier-Agent custom header. Built-in patterns for Cursor,
  Claude Code, Cline, Aider. Pause/resume via admin API. Per-agent metrics.
- **Request Queuing with Pause/Resume (VISION 3.3)** — Paused agents' requests are held
  in queue (not rejected) and skipped during dequeue. Resume signals the condition
  variable for immediate drain. Per-agent queue depth limits prevent starvation.

### Polish & Release (Phase 4)

- **Single-Binary Local Distribution (VISION 4.1)** — `ranvier --local` CLI starts with
  sensible defaults, no config file needed. Tokenizer auto-search (./assets, ~/.ranvier,
  /usr/local/share/ranvier). Startup banner with discovery info. CMake install targets.
  Homebrew formula skeleton. GitHub release workflow skeleton.
- **Local Dashboard UI (VISION 4.2)** — Vanilla JS dashboard at localhost:9180/dashboard.
  Shows discovered backends, request queue depths, active agents with pause/resume
  controls, and throughput stats. Embedded in binary at compile time. Dark theme,
  5-second auto-refresh, no external dependencies.
- **Documentation & Examples (VISION 4.3)** — Getting Started with Ranvier Local,
  Cloud Deployment Guide, IDE Integration Guide (Cursor, Claude Code, Cline, Aider),
  and Benchmark Reproduction Guide.
- **Re-benchmark** — Full intelligence layer validated under CI load. See Performance below.

### Performance

- **Intelligence Layer Overhead**: All §15 features enabled on mock backend CI benchmark
  (100 users, 60s, docker-compose):
  - P50 latency: 49ms (v1.0: 61ms, -20%)
  - P99 latency: 85ms (v1.0: 140ms, -39%)
  - Throughput: 502 rps (v1.0: 473 rps, +6%)
  - Priority queue scheduler wait: ~1.88ms average
  - Zero failures, zero sync errors
- v1.0 benchmark results on 8x A100 GPUs remain valid for prefix-affinity routing.
  Intelligence layer features add advisory signals; core routing path unchanged.

## [1.0.0] - 2026-03-16

Initial public release. Ranvier Core is a high-performance Layer 7+ LLM traffic controller
that reduces GPU KV-cache thrashing by routing inference requests based on token prefixes,
achieving 33-44% faster Time-To-First-Token for prefix-heavy workloads.

### Core Features

- **Prefix-Affinity Routing** — Adaptive Radix Tree (ART) maps token prefixes to GPU backends,
  steering requests to the GPU that already holds the relevant KV cache.
- **Passive Route Learning** — Routes are learned automatically from backend responses;
  no manual prefix configuration required.
- **Streaming Proxy** — Full SSE (Server-Sent Events) pass-through with zero-copy
  `string_view` parsing and read-position tracking.
- **Multi-Node Clustering** — Gossip protocol (v2) with CRDT-based route synchronization
  across cluster nodes. DTLS-encrypted transport.
- **Backend Discovery** — Static YAML configuration, Kubernetes EndpointSlice watch,
  and DNS-based discovery.
- **Load-Aware Routing** — Shard load metrics with cross-shard speculative load
  synchronization to prevent burst hot-spotting.
- **Circuit Breaker** — Per-backend circuit breaker with configurable thresholds,
  half-open probing, and automatic recovery.
- **API Key Authentication** — Multi-key support with metadata (name, creation date,
  expiry), constant-time comparison, and hot-reload via SIGHUP.
- **Rate Limiting** — Token bucket rate limiter with per-key and global limits.
- **Request Rewriting** — Chat template application and tokenized prompt rewriting
  for vLLM-aligned requests.
- **Configuration Hot-Reload** — SIGHUP-triggered config and API key reload
  without downtime.

### Performance

- **Tokenizer Thread Pool** — Dedicated per-shard worker threads with lock-free
  SPSC queues offload HuggingFace tokenizer FFI calls off the Seastar reactor.
- **Cross-Shard Tokenization Dispatch** — On cache miss, tokenization is dispatched
  to another shard via `smp::submit_to`, keeping the calling reactor responsive.
- **Slab Allocator** — Per-shard node pooling for Radix Tree allocations with
  size-classed pools (Node4/16/48/256) and O(1) free-list recycling.
- **Tree Compaction** — Post-order traversal removes tombstoned nodes and downsizes
  oversized internal nodes to reclaim slab memory.
- **Async Persistence** — Fire-and-forget queue with batched SQLite writes via
  `seastar::async`, decoupled from the request hot path.
- **Batched Route Broadcasting** — Locally-learned routes are batched (configurable
  flush interval, default 20ms) to eliminate per-request SMP storms.
- **Zero-Copy SSE Parsing** — Read-position offset parsing with buffer compaction
  at 50% consumption; no `substr()` copies.
- **Jemalloc Isolation** — Rust tokenizer FFI uses statically-linked jemalloc,
  eliminating memory corruption from Seastar allocator interaction.

### Observability

- **Prometheus Metrics** — Radix tree stats (hits/misses, node counts, slab utilization),
  connection pool metrics, routing decisions, tokenization latency, and queue depths.
- **OpenTelemetry Tracing** — Distributed tracing with Zipkin and OTLP exporters
  (compile-time gated via `WITH_TELEMETRY`).
- **Route Table Metrics** — Route count, estimated memory usage, and per-shard
  tree statistics exposed via admin API.

### Deployment

- **Docker Images** — Multi-stage production builds (`Dockerfile.production`) and
  fast incremental builds (`Dockerfile.production.fast`) for linux/amd64 and linux/arm64.
- **Helm Chart** — Kubernetes StatefulSet with HPA, ServiceMonitor, Ingress,
  and configurable gossip/DTLS settings.
- **GitHub Actions CI** — Automated Docker image publishing and benchmark pipelines.

### Testing

- 40 C++ unit tests (GTest) covering all major subsystems.
- 11 Python integration tests including multi-node cluster, prefix routing,
  graceful shutdown, and negative path validation.
- Locust-based load testing with LMSYS benchmark data.
- Benchmark suite validated on 8x A100 GPUs (30-minute runs).

### Benchmark Results (8x A100, February 2026)

| Model | Cache Hit Rate | TTFT Improvement | P99 Latency |
|-------|----------------|------------------|-------------|
| Llama-3.1-70B (TP=2, 4 backends) | 25% → 98% | 44% faster | ~same |
| CodeLlama-13b (8 backends) | 12% → 58-98% | 33% faster | -60% to -85% |
| Llama-3.1-8B (8 backends) | 12% → 68-98% | 40% faster | flat |
