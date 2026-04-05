# Changelog

All notable changes to Ranvier Core will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

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
