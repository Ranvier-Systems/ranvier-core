# CLAUDE.md - Ranvier Core

## Project Overview

Ranvier Core is a high-performance **Layer 7+ LLM traffic controller** built in **C++20** on the **Seastar** framework. It reduces GPU KV-cache thrashing by routing inference requests based on token prefixes rather than connection availability, achieving ~48% faster Time-To-First-Token for RAG, multi-turn chat, and few-shot workloads.

**License:** Apache 2.0

## Quick Reference

```bash
# Build
make build             # Release build (-O3)
make build-debug       # Debug build with symbols
make clean             # Remove build directory

# Test
make test              # Unit tests (builds first)
cd build && ctest --output-on-failure   # Run tests directly

# Integration tests (requires Docker)
make test-integration  # 3-node cluster + 2 mock backends

# Code quality
make format            # clang-format on src/ and tests/
make lint              # clang-tidy (requires build first)

# Benchmarks
make benchmark         # Locust load test with mock backends
make bench             # Lambda Labs GPU benchmark (auto-detects GPUs)
```

## Architecture

### Tech Stack

| Component | Technology |
|-----------|-----------|
| Language | C++20 (ISO standard) |
| Async Framework | Seastar (shared-nothing, thread-per-core) |
| Build System | CMake 3.24+ with FetchContent |
| Persistence | SQLite (WAL mode) |
| Testing | Google Test (unit), Python + Docker Compose (integration), Locust (load) |
| Tokenization | HuggingFace tokenizers via Rust FFI (tokenizers-cpp) |
| Cluster Sync | UDP gossip protocol with DTLS encryption |

### Data Flow

```
HTTP Request (:8080)
  -> HttpController (sharded, one per core)
  -> TokenizerService (BPE encoding with LRU cache)
  -> RouterService
     -> RadixTree (Adaptive Radix Tree - O(k) prefix lookup)
     -> Fallback: consistent hash or weighted random
  -> CircuitBreaker check
  -> ConnectionPool -> GPU Backend
  -> [Async] Learn route + persist + gossip broadcast
```

### Source Code Layout

```
src/
  main.cpp                    # Seastar reactor entry point
  application.{hpp,cpp}       # Service lifecycle orchestration (CREATED->RUNNING->DRAINING->STOPPED)

  # Routing Core
  router_service.{hpp,cpp}    # Unified routing interface (prefix/hash/random modes)
  radix_tree.hpp              # Adaptive Radix Tree (Node4/16/48/256, slab-allocated)
  node_slab.{hpp,cpp}         # O(1) slab allocator for ART nodes

  # HTTP Layer
  http_controller.{hpp,cpp}   # Request handling, proxy, SSE streaming
  connection_pool.hpp         # LRU + TTL connection management
  circuit_breaker.hpp         # Backend health state machine (CLOSED/OPEN/HALF_OPEN)
  rate_limiter.hpp            # Request throttling
  stream_parser.{hpp,cpp}     # SSE stream parsing
  request_rewriter.hpp        # Token ID injection into requests

  # Tokenization
  tokenizer_service.{hpp,cpp} # HuggingFace BPE via Rust FFI (sharded, one per core)
  tokenizer_thread_pool.{hpp,cpp} # Thread pool for FFI calls (5-13ms each)

  # Clustering
  gossip_service.{hpp,cpp}    # Cluster state sync coordinator (shard 0 only)
  gossip_protocol.{hpp,cpp}   # Wire format, reliable delivery, DNS discovery
  gossip_transport.{hpp,cpp}  # UDP channel, DTLS encryption
  gossip_consensus.{hpp,cpp}  # Peer table, quorum, split-brain detection
  crypto_offloader.{hpp,cpp}  # DTLS crypto on worker threads
  dtls_context.{hpp,cpp}      # OpenSSL DTLS session management

  # Configuration
  config.hpp                  # Facade header (includes schema + loader)
  config_schema.hpp           # All config structs (RanvierConfig, RoutingConfig, etc.)
  config_loader.{hpp,cpp}     # YAML parsing + env var overrides
  sharded_config.hpp          # Lock-free per-shard config distribution

  # Persistence
  persistence.hpp             # Interface definition
  sqlite_persistence.{hpp,cpp} # SQLite WAL-mode backend
  async_persistence.{hpp,cpp} # Fire-and-forget queue with batch writes

  # Infrastructure
  health_service.{hpp,cpp}    # Backend liveness monitoring
  k8s_discovery_service.{hpp,cpp} # Kubernetes EndpointSlice watching
  metrics_service.hpp         # Prometheus metrics (:9180)
  tracing_service.{hpp,cpp}   # OpenTelemetry (optional, Zipkin exporter)
  shard_load_balancer.hpp     # Power-of-Two-Choices cross-shard dispatch
  shard_load_metrics.hpp      # Per-shard load tracking
  cross_shard_request.hpp     # Safe cross-shard pointer passing

  # Utilities
  types.hpp                   # Core type aliases (TokenId, BackendId)
  logging.hpp                 # Structured logging
  parse_utils.hpp             # Safe string-to-number conversion
  text_validator.hpp          # Input validation

tests/
  unit/                       # 24+ Google Test files
  integration/                # Python tests + Locust load testing

docs/
  architecture/               # VISION.md, system-design.md
  internals/                  # radix-tree.md, gossip-protocol.md
  api/                        # reference.md
  deployment/                 # kubernetes.md, performance.md

deploy/helm/ranvier/          # Kubernetes Helm chart

.dev-context/                 # Extended AI assistant guides (16 hard rules, audit findings)
```

### Key Types

```cpp
using TokenId = int32_t;     // BPE token identifier
using BackendId = int32_t;   // GPU pool identifier

struct RouteResult {
    std::optional<BackendId> backend_id;
    std::string routing_mode;  // "prefix" | "hash" | "random"
    bool cache_hit = false;
    std::string error_message;
};

enum class RoutingMode { PREFIX, HASH, RANDOM };
enum class CircuitState { CLOSED, OPEN, HALF_OPEN };
```

## Dependencies

| Library | Version | Purpose |
|---------|---------|---------|
| Seastar | system | Async I/O framework (shared-nothing) |
| GTest | 1.14.0 | Unit testing |
| yaml-cpp | 0.8.0 | YAML config parsing |
| Abseil | 20240116.1 | `flat_hash_map`/`flat_hash_set` |
| RapidJSON | 1.1.0 | JSON parsing (header-only) |
| SQLite3 | 3.45.0 | Persistent state (built from source if needed) |
| tokenizers-cpp | main | Rust FFI for BPE tokenization |
| OpenTelemetry | 1.14.2 | Distributed tracing (optional: `WITH_TELEMETRY=ON`) |
| OpenSSL | system | DTLS encryption for gossip |

Dependencies are resolved via system packages first, with CMake FetchContent as fallback.

## Coding Conventions

### Naming

- **Private members:** `_snake_case` prefix (`_config`, `_router`)
- **Functions:** `snake_case` (`route_request()`, `learn_route_global()`)
- **Enums:** `PascalCase` (`RoutingMode`, `CircuitState`)
- **Type aliases:** `PascalCase` (`TokenId`, `BackendId`, `NodePtr`)
- **Constants:** `kPascalCase` or `SCREAMING_SNAKE_CASE`
- **Namespace:** `ranvier`

### File Organization

- `foo.hpp` - Public interface with leading comments
- `foo.cpp` - Implementation
- `*_schema.hpp` - Data structures only
- `*_loader.hpp` - Loading/parsing logic
- `*_service.{hpp,cpp}` - Service orchestration

### Error Handling

- No exceptions on hot paths
- Return `std::optional<T>` for missing data
- Return structs with error_message fields for routing failures
- Seastar futures propagate exceptions for async failures
- Every catch block must log at warn level minimum (Rule #9)

### Metrics

```cpp
namespace sm = seastar::metrics;
_metrics.add_group("ranvier_router", {
    sm::make_counter("routes_learned", _routes_learned),
    sm::make_gauge("cache_size", [this] { return _tree.size(); }),
});
```

Deregister metrics first in `stop()` before any other cleanup (Rule #6).

## The 16 Hard Rules

These rules are **mandatory** for all code changes. Violations cause crashes, data corruption, or security issues in production. See `.dev-context/claude-context.md` for full explanations with code examples.

| # | Rule | What NOT to Do |
|---|------|----------------|
| 0 | Use `unique_ptr`; `lw_shared_ptr` for shard-local sharing | `std::shared_ptr` in Seastar code |
| 1 | Metrics accessors must be lock-free (`std::atomic`) | `std::mutex` in any reactor-thread method |
| 2 | No `co_await` inside loops over external resources | `for (...) { co_await process(item); }` |
| 3 | Null-guard all C string returns | Direct cast of `sqlite3_column_text` without null check |
| 4 | Every growing container needs `MAX_SIZE` | Unbounded `push_back`/`emplace_back` |
| 5 | Timer callbacks need gate guards | Lambda captures `this` without `_gate.hold()` |
| 6 | Deregister metrics first in `stop()` | Metrics lambda outlives service |
| 7 | Persistence only stores, never validates | Business logic in storage layer |
| 8 | Single `ShardLocalState` struct for per-shard state | Scattered `thread_local` variables |
| 9 | Every catch block logs at warn level | Silent `catch (...) {}` |
| 10 | Validating helpers for string-to-number | Bare `std::stoi()` on external input |
| 11 | `std::call_once` or `std::atomic` for global state | Unprotected global statics |
| 12 | Use Seastar file I/O APIs | `std::ifstream`/`ofstream` in Seastar code |
| 13 | Thread-local raw new needs destroy function | `thread_local T* = new T()` without cleanup |
| 14 | Force local allocation for cross-shard data | Moving `vector`/`string` via `submit_to` |
| 15 | Reallocate locally before FFI across boundaries | Passing shard-allocated data to Rust/C FFI |

### Critical Seastar Patterns

**Cross-shard communication:** Always use `seastar::smp::submit_to()`. Never locks or atomics.

**Cross-shard data transfer:** Use `foreign_ptr` + local reallocation:
```cpp
auto ptr = std::make_unique<std::vector<T>>(data);
auto foreign = seastar::make_foreign(std::move(ptr));
smp::submit_to(target, [foreign = std::move(foreign)]() mutable {
    std::vector<T> local(foreign->begin(), foreign->end());  // Local alloc
    process(local);
});
```

**FFI safety:** Reallocate in BOTH directions when crossing shard/thread boundaries with Rust/C FFI. Seastar replaces `malloc` globally; FFI allocators don't participate in Seastar's per-shard memory tracking.

**Async-only I/O:** All file I/O must use `seastar::open_file_dma()` + `seastar::make_file_input_stream()`. Never `std::ifstream`.

**Bounded containers:** Every `push_back` must have a size check or the container must be bounded by design.

## Build System Details

**CMake 3.24+** with C++20 standard. Key CMake options:

- `CMAKE_BUILD_TYPE=Release` (default) or `Debug`
- `WITH_TELEMETRY=ON/OFF` - Enable OpenTelemetry tracing
- `CMAKE_EXPORT_COMPILE_COMMANDS=ON` - Always on, for IDE support and clang-tidy

The build produces test binaries under `build/` that are run via `ctest`.

**Note:** The full server binary (`ranvier_server`) requires Seastar installed on the system. Unit tests can be built and run without Seastar - they test individual components in isolation.

## CI/CD

GitHub Actions workflows in `.github/workflows/`:

| Workflow | Purpose |
|----------|---------|
| `docker-publish.yml` | Multi-arch Docker images (amd64/arm64) to GHCR on push to main/tags |
| `docker-base.yml` | Base image with Seastar + build tools (manual trigger, ~30min) |
| `benchmark.yml` | Locust load test regression on PRs and pushes to main |

## Configuration

Configuration is loaded from YAML (default: `ranvier.yaml`) with environment variable overrides (e.g., `RANVIER_ROUTING_MAX_ROUTES=50000`). Hot-reload via SIGHUP is supported (rate-limited to once per 10 seconds, atomic across all shards).

See `ranvier.yaml.example` for the full configuration template.

## Deployment

- **Docker:** `ghcr.io/ranvier-systems/ranvier:latest` (requires `--cap-add=IPC_LOCK`)
- **Kubernetes:** Helm chart in `deploy/helm/ranvier/` (StatefulSet, HPA, service discovery)
- **Local development:** Docker Compose via `docker-compose.test.yml` (3 nodes + 2 backends)

## Extended Context

For detailed anti-pattern explanations with code examples, security audit findings, and implementation patterns, see:

- `.dev-context/claude-context.md` - 16 Hard Rules with full rationale
- `.dev-context/cheatsheet.md` - Quick-reference patterns and troubleshooting
- `docs/architecture/` - System design and vision documents
- `docs/internals/` - Radix tree, gossip protocol, tokenization internals
