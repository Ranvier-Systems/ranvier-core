# Source Code Map

This guide maps source files to logical modules, helping contributors navigate the ~35,790 LOC codebase.

## Core Application

| File | Description |
|------|-------------|
| `main.cpp` | Entry point, Seastar reactor loop, signal setup |
| `application.*` | Service lifecycle management, startup/shutdown orchestration |
| `config.hpp` | Backward-compatible facade (includes schema + loader) |
| `config_schema.hpp` | Ranvier-specific configuration data structures (all `*Config` structs) |
| `config_infra.hpp` | Generic infrastructure config structs (server, pool, TLS, auth, DB, telemetry, etc.) |
| `config_loader.*` | YAML loading, validation, environment variable overrides |
| `sharded_config.hpp` | Per-shard configuration for lock-free access |
| `types.hpp` | Common type definitions (TokenId, BackendId) |
| `logging.hpp` | Structured logging utilities |
| `byte_order.hpp` | Big-endian read/write helpers for byte buffers (gossip, radix tree) |
| `mpsc_ring_buffer.hpp` | Lock-free bounded MPSC ring buffer primitive (used by async persistence) |

## Routing Engine (The "Brain")

| File | Description |
|------|-------------|
| `router_service.*` | Main routing logic, route learning/propagation |
| `radix_tree.hpp` | Adaptive Radix Tree (ART) for prefix matching |
| `node_slab.*` | Per-shard memory pool for RadixTree nodes |
| `shard_load_balancer.hpp` | P2C algorithm for cross-core request distribution |
| `shard_load_metrics.hpp` | Per-shard load tracking (active requests, queue depth) |
| `cross_shard_request.hpp` | Safe cross-shard request context transfer |
| `request_scheduler.hpp` | Priority-aware request queueing with per-agent fair scheduling |
| `intent_classifier.*` | Classifies requests as AUTOCOMPLETE / EDIT / CHAT for routing hints |
| `backend_registry.hpp` | Abstract backend lifecycle interface (decouples health/discovery from router) |
| `cache_event_parser.hpp` | Pure parser for `POST /v1/cache/events` JSON payloads |
| `tokenizer_service.*` | HuggingFace tokenizer integration (GPT-2) |
| `tokenizer_thread_pool.*` | Dedicated worker pool for blocking tokenizer FFI calls |
| `request_rewriter.hpp` | Token injection for pre-tokenized forwarding |
| `chat_template.hpp` | Pre-compiled chat templates (llama3, chatml, mistral) |
| `boundary_detector.hpp` | Pure strategies for detecting message boundaries in token sequences |
| `text_boundary_info.hpp` | Lightweight boundary metadata struct (separated from request_rewriter) |

## Networking Layer

| File | Description |
|------|-------------|
| `http_controller.*` | HTTP/1.1 handling, SSE streaming, admin API |
| `stream_parser.*` | Zero-copy chunked transfer parsing |
| `connection_pool.hpp` | Upstream connection management with TTL |
| `rate_limiter_core.hpp` | Pure token bucket algorithm (no Seastar deps, testable) |
| `rate_limiter.hpp` | Seastar service wrapper with timer, gate, and metrics |
| `proxy_retry_policy.hpp` | Retry/backoff/fallback decisions for backend proxying |
| `text_validator.hpp` | UTF-8 and null-byte input validation |
| `parse_utils.hpp` | Safe integer parsing via `std::from_chars` |
| `agent_registry.*` | Identifies AI coding agents (Cursor, Claude Code, Cline, Aider) and exposes pause/resume + priority controls |
| `dashboard/` | Embedded admin dashboard (`index.html` + CMake-generated `dashboard_resource.hpp`) |

## Cluster State (Gossip)

The gossip subsystem is split into focused modules:

| File | Description |
|------|-------------|
| `gossip_service.*` | Thin orchestrator, metrics registration (~350 LOC) |
| `gossip_consensus.*` | Peer table, quorum, split-brain detection (~430 LOC) |
| `gossip_protocol.*` | Message handling, reliable delivery, DNS discovery (~870 LOC) |
| `gossip_transport.*` | UDP channel, DTLS encryption (~540 LOC) |
| `dtls_context.*` | OpenSSL DTLS session management |
| `crypto_offloader.*` | Adaptive crypto offloading to prevent reactor stalls |

## Discovery & Health

| File | Description |
|------|-------------|
| `k8s_discovery_service.*` | Kubernetes API integration (endpoints, services) |
| `local_discovery.*` | Auto-discovery of local LLM servers (Ollama, vLLM, LM Studio, llama.cpp) via semantic liveness probes |
| `health_service.*` | Backend health checking |
| `circuit_breaker.hpp` | Circuit breaker pattern for failing backends |
| `vllm_metrics.hpp` | Per-backend vLLM metrics struct (queue depth, KV cache, GPU memory) |
| `prometheus_parser.hpp` | Lightweight parser for vLLM `/metrics` Prometheus text output |

## Persistence

| File | Description |
|------|-------------|
| `async_persistence.*` | Non-blocking write queue, owns SQLite store |
| `sqlite_persistence.*` | SQLite WAL-mode backend/route storage |
| `persistence.hpp` | Persistence interface definitions |

## Observability

| File | Description |
|------|-------------|
| `metrics_service.hpp` | Prometheus metrics (counters, histograms, gauges) |
| `metrics_helpers.hpp` | Reusable metrics infrastructure (histogram buckets, bounded per-backend maps) |
| `metrics_auth_handler.hpp` | Optional Bearer token + IP allowlist auth for the `/metrics` endpoint |
| `tracing_service.*` | OpenTelemetry span propagation |

## Architecture Notes

- **Shared-Nothing**: Each CPU core has its own shard. No `std::mutex` in reactor-callable code.
- **Async Only**: All I/O returns `seastar::future<>`. Never use blocking syscalls (e.g., `std::sleep`, `fs::read`).
- **Memory**: No `new`/`delete`. Use `seastar::lw_shared_ptr` or `std::unique_ptr`.
- **Exceptions**: Use `seastar::make_exception_future` path, not `throw`, for runtime errors.
- **Hot Path**: `HttpController` -> `TokenizerService` -> `RouterService` -> `RadixTree` -> `ConnectionPool`

## Load-Bearing Files (High Blast Radius)

| Rank | File | Impact |
|------|------|--------|
| 1 | `radix_tree.hpp` | Core prefix matching - breaks all routing |
| 2 | `router_service.cpp` | Route learning - breaks cache affinity |
| 3 | `gossip_service.cpp` | Cluster coordination - breaks multi-node |
| 4 | `http_controller.cpp` | All traffic flows through here |
| 5 | `connection_pool.hpp` | Backend connectivity |

See `BACKLOG.md` Section 8.3 for full blast radius analysis.
