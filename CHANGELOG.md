# Changelog

All notable changes to Ranvier Core will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- **Local Backend Discovery (VISION 3.1)** — Auto-discovers local LLM servers (Ollama, vLLM,
  LM Studio, llama.cpp, LocalAI, Text Generation WebUI) using semantic liveness checks.
  Probes configured ports with real HTTP GET `/v1/models` requests to solve the zombie port
  problem (TCP connect succeeds but server is unresponsive). Backends are hot-added/removed
  from the router with 3-miss hysteresis. New config fields: `discovery_scan_interval_seconds`,
  `discovery_probe_timeout_ms`, `discovery_connect_timeout_ms`.

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
