# Ranvier Core: Context & Constraints

## Project Essence

Ranvier Core is a high-performance **Layer 7+ LLM traffic controller** built in **C++20** on the **Seastar** framework. It reduces GPU KV-cache thrashing by routing inference requests based on token prefixes rather than connection availability, achieving ~48% faster Time-To-First-Token for RAG, multi-turn chat, and few-shot workloads.

**License:** Apache 2.0

## Build Constraints

- **Static analysis only.** Do not attempt to run `cmake`, `make`, or build. Seastar dependencies are too heavy for the sandbox.
- **API verification:** Verify syntax against Seastar documentation logic.
- **Manual verification:** The developer builds in their Docker container and provides logs if it fails.
- **Do NOT read** the full `/docs` or `/assets` folders (large token-heavy files).

## Pre-Code Checklist

Before writing any code, verify:
- [ ] Have I read the relevant file(s) I'm about to modify?
- [ ] Does this touch cross-shard communication? Use `seastar::smp::submit_to`
- [ ] Is there a MAX_SIZE for any new container?
- [ ] Any new timer/callback? Needs gate guard scoped to full async lifetime
- [ ] Any new metrics lambda capturing `this`? Deregister in `stop()`
- [ ] Any C API string returns? Null-guard required
- [ ] Any lambda coroutine passed to `.then()`? Wrap with `seastar::coroutine::lambda()`
- [ ] Any loop >100 iterations? Insert `maybe_yield()` preemption point
- [ ] Any returned future not awaited? Must be `co_await`ed or explicitly handled
- [ ] Any semaphore usage? Must use `with_semaphore()`/`get_units()`, never raw `wait()`/`signal()`
- [ ] Any `do_with` lambda? Parameters must use `auto&` (with `&`)
- [ ] Any coroutine parameters? Take by value, not by reference
- [ ] Any function that might throw before returning a future? Wrap with `futurize_invoke()` or use coroutine
- [ ] Any `temporary_buffer::share()` stored beyond current request? Must `clone()` instead

## Architecture Reference

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
  -> HttpController (sharded, one per core; ingress concurrency cap)
  -> RateLimiter (token-bucket; per-agent/IP)
  -> AgentRegistry (User-Agent -> priority tier; pause/resume)
  -> RequestScheduler (per-agent fair queue, CRITICAL/HIGH/NORMAL/LOW)
  -> IntentClassifier (AUTOCOMPLETE / EDIT / CHAT)
  -> ChatTemplate (llama3 / chatml / mistral / none) -> formatted prompt
  -> TextValidator (UTF-8, null-byte, length)
  -> TokenizerService
       Layer 1: LRU cache (shard-local)
       Layer 2: cross-shard P2C dispatch (ShardLoadBalancer)
       Layer 3: TokenizerThreadPool (dedicated OS threads, 5-13ms FFI)
  -> BoundaryDetector (per-message split for multi-depth prefix lookup)
  -> RequestRewriter (inject prompt_token_ids into JSON body)
  -> RouterService
       PREFIX: RadixTree (ART, O(k) lookup) -> consistent-hash fallback
       HASH:   consistent hash on token prefix
       RANDOM: uniform random (skips tokenization)
  -> CircuitBreaker check (CLOSED / OPEN / HALF_OPEN)
  -> ConnectionPool -> GPU Backend (vLLM / SGLang / TensorRT-LLM / Ollama / LM Studio)
  -> StreamParser (SSE / chunked) -> client
  -> [Async] Learn route -> AsyncPersistence (MPSC ring -> SQLite worker)
                         -> GossipService (shard 0; ROUTE_ANNOUNCEMENT broadcast)
```

### Source Code Layout

```
src/
  main.cpp                    # Seastar reactor entry point (startup-only blocking I/O OK)
  application.{hpp,cpp}       # Service lifecycle (CREATED -> STARTING -> RUNNING ->
                              #                    DRAINING -> STOPPING -> STOPPED)

  # Routing Core
  router_service.{hpp,cpp}    # Unified routing interface (PREFIX/HASH/RANDOM modes);
                              #   route batching via pending buffer; gossip broadcast
  radix_tree.hpp              # Adaptive Radix Tree (Node4/16/48/256, byte-keyed, LRU-bounded)
  node_slab.{hpp,cpp}         # O(1) slab allocator for ART nodes (thread-local free-list)

  # HTTP Layer
  http_controller.{hpp,cpp}   # Sharded request handler, proxy, SSE streaming, backpressure
  connection_pool.hpp         # LRU + TTL connection management (templated on ClosePolicy)
  circuit_breaker.hpp         # Per-backend state machine (CLOSED/OPEN/HALF_OPEN; templated Clock)
  proxy_retry_policy.hpp      # Pure retry / backoff / fallback logic (no Seastar deps)
  rate_limiter.hpp            # Seastar wrapper: cleanup timer + gate + Prometheus
  rate_limiter_core.hpp       # Pure token-bucket algorithm (no Seastar deps)
  request_scheduler.hpp       # Per-agent fair scheduling, priority tiers
  stream_parser.{hpp,cpp}     # Zero-copy chunked + SSE parser
  request_rewriter.hpp        # Inject prompt_token_ids into request body (vLLM schema)

  # Request Processing / Text
  intent_classifier.{hpp,cpp} # AUTOCOMPLETE (FIM) / EDIT / CHAT classification (pure)
  agent_registry.{hpp,cpp}    # User-Agent -> priority tier; pause/resume; shard-local
  chat_template.hpp           # Pre-compiled templates (llama3, chatml, mistral, none)
  boundary_detector.hpp       # Marker-scan + proportional estimation for message boundaries
  text_boundary_info.hpp      # Boundary metadata used for multi-depth prefix extraction
  text_validator.hpp          # UTF-8 / null-byte / length validation (pre-tokenizer)
  cache_event_parser.hpp      # POST /v1/cache/events JSON parser (eviction/load events)

  # Tokenization
  tokenizer_service.{hpp,cpp} # 3-layer: LRU cache -> cross-shard P2C -> thread pool
  tokenizer_thread_pool.{hpp,cpp} # Dedicated OS workers for FFI (5-13ms BPE calls)

  # Clustering / Gossip
  gossip_service.{hpp,cpp}    # Cluster state sync orchestrator (shard 0 only)
  gossip_protocol.{hpp,cpp}   # Wire format, reliable delivery (ACKs), DNS discovery;
                              #   ROUTE_ANNOUNCEMENT, HEARTBEAT, ROUTE_ACK,
                              #   NODE_STATE, CACHE_EVICTION (big-endian)
  gossip_transport.{hpp,cpp}  # UDP channel, DTLS encryption
  gossip_consensus.{hpp,cpp}  # Peer table, quorum, split-brain detection
  crypto_offloader.{hpp,cpp}  # Adaptive DTLS crypto offload (symmetric on-shard,
                              #   asymmetric to worker thread); stall tracking
  dtls_context.{hpp,cpp}      # OpenSSL DTLS session management (mTLS support)
  byte_order.hpp              # Endian helpers for wire format

  # Configuration
  config.hpp                  # Facade header (includes schema + loader)
  config_infra.hpp            # Infrastructure config structs (ServerConfig, PoolConfig, etc.)
  config_schema.hpp           # Ranvier-specific configs (RoutingConfig, AssetsConfig, RanvierConfig)
  config_loader.{hpp,cpp}     # YAML parsing + env var overrides
  config_loader_async.{hpp,cpp} # Async config reload (no reactor stalls; SIGHUP path)
  sharded_config.hpp          # Lock-free per-shard config distribution

  # Persistence
  persistence.hpp             # Interface definition
  sqlite_persistence.{hpp,cpp} # SQLite WAL-mode backend
  async_persistence.{hpp,cpp} # Fire-and-forget MPSC ring -> dedicated OS worker (alien::run_on)
  mpsc_ring_buffer.hpp        # Lock-free bounded MPSC queue (Vyukov; power-of-two)

  # Infrastructure
  backend_registry.hpp        # Abstract backend lifecycle interface (decouples discovery + health)
  health_service.{hpp,cpp}    # Backend liveness loop + vLLM /metrics scrape; ACTIVE/DRAINING
  k8s_discovery_service.{hpp,cpp} # K8s EndpointSlice watcher; ranvier.io/* annotations
  local_discovery.{hpp,cpp}   # Probe local ports (Ollama 11434, vLLM 8080, LM Studio 1234, ...)
  metrics_service.hpp         # Ranvier Prometheus metrics (:9180); thread-local counters/gauges
  metrics_helpers.hpp         # Reusable histogram/bucket infra (MetricHistogram, BackendMetrics)
  metrics_auth_handler.hpp    # Auth / access control for metrics endpoint
  prometheus_parser.hpp       # Parse Prometheus text exposition (for vLLM scrape)
  vllm_metrics.hpp            # Per-backend vLLM stats struct (running/queued, KV-cache %, tps)
  tracing_service.{hpp,cpp}   # OpenTelemetry (optional WITH_TELEMETRY=ON; Zipkin/OTLP)
  shard_load_balancer.hpp     # Power-of-Two-Choices cross-shard dispatch (snapshot-cached)
  shard_load_metrics.hpp      # Per-shard load tracking (active + queued requests)
  cross_shard_request.hpp     # Safe foreign_ptr unwrap for cross-shard data

  # Utilities
  types.hpp                   # Core aliases (TokenId, BackendId), kYieldInterval
  logging.hpp                 # Structured logging + request-ID generation
  parse_utils.hpp             # Safe string-to-number, hex decoding

  dashboard/                  # Embedded web UI assets

tests/
  unit/                       # Google Test (reactor-free where possible)
  integration/                # Python tests + Locust load testing

deploy/helm/ranvier/          # Kubernetes Helm chart

docs/internals/               # Authoritative deep-dives (see Documentation Map below)
.dev-context/                 # Workflow prompts, audit findings, cheatsheet
```

### Key Types

```cpp
// src/types.hpp
using TokenId   = int32_t;            // BPE token identifier
using BackendId = int32_t;            // GPU pool identifier
inline constexpr size_t kYieldInterval = 128;  // co_await maybe_yield() cadence

struct RouteResult {
    std::optional<BackendId> backend_id;
    std::string routing_mode;  // "prefix" | "hash" | "random"
    bool cache_hit = false;
    std::string error_message;
};

// Routing / proxy
enum class RoutingMode         { PREFIX, HASH, RANDOM };
enum class CircuitState        { CLOSED, OPEN, HALF_OPEN };
enum class ConnectionErrorType { NONE, BROKEN_PIPE, CONNECTION_RESET };

// Request classification / scheduling
enum class RequestIntent { AUTOCOMPLETE, CHAT, EDIT };
enum class PriorityLevel { CRITICAL, HIGH, NORMAL, LOW };

// Templates / crypto
enum class ChatTemplateFormat { none, llama3, chatml, mistral };
enum class CryptoOpType {
    SYMMETRIC_ENCRYPT, SYMMETRIC_DECRYPT,
    HANDSHAKE_INITIATE, HANDSHAKE_CONTINUE, UNKNOWN
};

// Application lifecycle (application.hpp)
enum class ApplicationState {
    CREATED, STARTING, RUNNING, DRAINING, STOPPING, STOPPED
};
```

Service start order: `TokenizerService` -> `RouterService` -> `HttpController` -> `HealthService` -> `AsyncPersistenceManager` -> `K8sDiscoveryService` -> `GossipService`. Shutdown reverses this.

## Critical Constraints for Coding

1. **NO LOCKS:** This is a Seastar project. Never use `std::mutex` or atomics. Use `seastar::smp::submit_to` for cross-core communication.
2. **Async Only:** All I/O and cross-shard calls must return `seastar::future<>`.
3. **Prefix Logic:** Routing is based on the Adaptive Radix Tree (ART) lookup of token sequences.
4. **Visualize the FutureChain:** Since Seastar relies heavily on Future/Promise chains, "Visualize the Future Chain" in ASCII or Mermaid diagrams before writing the code to prevent "Future Leaks" or blocking the reactor.

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

### Reactor-Free Unit Testing (Seastar Components)

Seastar future continuations (`.then()`, `.finally()`, `.handle_exception()`) call `need_preempt()`, which dereferences the thread-local reactor pointer. **This segfaults without a running reactor — even on ready futures.** `output_stream::close()` chains `.finally()` internally, so even closing a default-constructed stream with a null data sink crashes.

Any code path that touches Seastar futures must be injectable if you want unit tests without a reactor. If it calls `.then()`, it needs a policy seam.

**Pattern 1 — Clock injection:** Template on `Clock` (default `steady_clock`). Tests use `TestClock` (`tests/unit/test_clock.hpp`) for deterministic timing. Backward-compatible alias: `using TypeName = BasicTypeName<>;`.
```
Examples: circuit_breaker.hpp, rate_limiter_core.hpp, connection_pool.hpp
```

**Pattern 2 — ClosePolicy injection:** Template on `ClosePolicy` for any component that closes Seastar streams. Production uses `AsyncClosePolicy` (heap + `.finally()`). Tests use `SyncClosePolicy` (mark invalid, drop).
```
Example: connection_pool.hpp
```

**Test file conventions:** `tests/unit/<name>_test.cpp`, Google Test, `using namespace ranvier;`, `TEST_F` with fixtures. Seastar-dependent tests go under `if(Seastar_FOUND)` in `CMakeLists.txt`.

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

## Build System

**CMake 3.24+** with C++20 standard. Key CMake options:

- `CMAKE_BUILD_TYPE=Release` (default) or `Debug`
- `WITH_TELEMETRY=ON/OFF` - Enable OpenTelemetry tracing
- `CMAKE_EXPORT_COMPILE_COMMANDS=ON` - Always on, for IDE support and clang-tidy

The full server binary (`ranvier_server`) requires Seastar installed on the system. Unit tests can be built and run without Seastar -- they test individual components in isolation.

## Documentation Map

- **API:** Admin on `:8080`, Data on `:8080`, Metrics on `:9180`.
- **Persistence:** SQLite tracks backends and routes.
- **Configuration:** YAML (default: `ranvier.yaml`) with env var overrides (e.g., `RANVIER_ROUTING_MAX_ROUTES=50000`). Hot-reload via SIGHUP (rate-limited to once per 10 seconds, atomic across all shards). See `ranvier.yaml.example`.

### Authoritative deep-dives (`docs/internals/`)

| File | Covers |
|------|--------|
| `radix-tree.md` | ART internals: byte-keyed Node4/16/48/256, slab allocator, path compression, LRU eviction. |
| `prefix-affinity-routing.md` | Hybrid ART + consistent-hash fallback; backend KV-cache prerequisites (vLLM APC, SGLang RadixAttention); ~81% vs 49% cache hit. |
| `tokenization.md` | 3-layer tokenization: LRU cache (80-90% hit) -> cross-shard P2C -> dedicated thread pool; FFI safety. |
| `request-lifecycle.md` | End-to-end request path (ingress -> rate-limit -> tokenize -> boundary -> route -> circuit -> connect -> stream -> learn). Source of truth for tracing. |
| `request-lifecycle-perf-analysis.md` | Perf mitigations: route batching (no per-request gossip), tokenization fallback, stale-connection retries, load-aware overrides. |
| `shard-load-balancing.md` | P2C algorithm + snapshot cache; currently advisory (HTTP hot path not yet dispatching). |
| `gossip-protocol.md` | Wire format, packet types (ROUTE_ANNOUNCEMENT / HEARTBEAT / ROUTE_ACK / NODE_STATE / CACHE_EVICTION), big-endian, ACK-based reliability, DTLS. |
| `async-persistence.md` | Lock-free MPSC ring -> dedicated OS worker -> SQLite WAL; fire-and-forget API + backpressure; `process_batch` accumulator. |

When changing behavior in any of these areas, update the corresponding doc in the same PR.

## CI/CD

GitHub Actions workflows in `.github/workflows/`:

| Workflow | Purpose |
|----------|---------|
| `docker-publish.yml` | Multi-arch Docker images (amd64/arm64) to GHCR on push to main/tags |
| `docker-base.yml` | Base image with Seastar + build tools (manual trigger, ~30min) |
| `benchmark.yml` | Locust load test regression on PRs and pushes to main |

## Deployment

- **Docker:** `ghcr.io/ranvier-systems/ranvier:latest` (requires `--cap-add=IPC_LOCK`)
- **Kubernetes:** Helm chart in `deploy/helm/ranvier/` (StatefulSet, HPA, service discovery)
- **Local development:** Docker Compose via `docker-compose.test.yml` (3 nodes + 2 backends)

## Workflow Prompts

The `.dev-context/` directory contains specialized prompt templates for different workflows:

| Prompt | Use When |
|--------|----------|
| `claude-prompt.md` | Starting any general task |
| `claude-impl-prompt.md` | Implementing a feature (staged: plan, code, optimize) |
| `claude-review-prompt.md` | Self-reviewing code against the Hard Rules |
| `claude-debug-prompt.md` | Triaging a build/runtime failure |
| `claude-doc-prompt.md` | Writing tests and updating docs post-implementation |
| `claude-planning-prompt.md` | Decomposing a feature into atomic steps |
| `claude-refactor-prompt.md` | Refactoring without behavioral changes |
| `claude-audit-prompt.md` | Holistic system audit for architecture drift |
| `claude-adversarial-audit-prompt.md` | Security-focused adversarial audit |
| `claude-perf-prompt.md` | Performance investigation and optimization |
| `claude-incident-prompt.md` | Incident response and root cause analysis |
| `claude-pattern-extractor-prompt.md` | Formalizing new anti-patterns into Hard Rules |
| `claude-strategic-alignment-prompt.md` | Strategic project assessment |

---

## Hard Rules

The following anti-patterns have been identified through security audits, production incident analysis, and study of ScyllaDB's Seastar experience. Each represents a "seemingly reasonable" approach that violates our Seastar/shared-nothing architecture or creates latent bugs. **Every rule applies to all new and modified code.**

For additional Seastar pitfalls not yet elevated to Hard Rules, see `.dev-context/seastar-pitfalls-reference.md`.

---

#### 0. The Cross-Core shared_ptr Anti-Pattern

**THE PATTERN:** Using `std::shared_ptr<T>` for objects that live within a Seastar service, assuming "shared ownership" is always safe.

**THE CONSEQUENCE:** `std::shared_ptr` uses atomic reference counting. When the last reference is released on a different CPU core than where the object was allocated, the destructor runs on the "wrong" shard--violating Seastar's shared-nothing model. This causes subtle data races, memory corruption, or crashes when the destructor accesses shard-local state.

**THE LESSON:** *Hard Rule: For shard-local objects, prefer `std::unique_ptr`.* When shared ownership is truly needed within a single shard, use `seastar::lw_shared_ptr` (lightweight, non-atomic). Only use `seastar::shared_ptr` (with atomic refcount) when the pointer genuinely crosses shard boundaries via `seastar::foreign_ptr`.

**PROMPT GUARD:** "Never use std::shared_ptr in Seastar code--use unique_ptr for single ownership, seastar::lw_shared_ptr for shard-local sharing, or foreign_ptr<shared_ptr<T>> for cross-shard transfer."

---

#### 1. The Lock-in-Metrics Anti-Pattern

**THE PATTERN:** Using `std::lock_guard<std::mutex>` to provide thread-safe read access in a metrics/query method (e.g., `queue_depth()`).

**THE CONSEQUENCE:** Metrics collection runs on the Seastar reactor thread. A mutex lock--even briefly--blocks the entire event loop. With Prometheus scraping every 15s across 64 shards, you get 64 stalls per scrape cycle. Under load, this causes cascading latency spikes.

**THE LESSON:** *Hard Rule: Metrics accessors must be lock-free.* Use `std::atomic<T>` with relaxed memory ordering for counters/gauges. Maintain atomic shadow variables updated alongside the protected data structure.

**PROMPT GUARD:** "Never use std::mutex in any method that could be called from the reactor thread--especially metrics, health checks, or status queries."

---

#### 2. The Sequential-Await-Loop Anti-Pattern

**THE PATTERN:** Writing `for (auto& item : items) { co_await process(item); }` to iterate over a collection of async operations.

**THE CONSEQUENCE:** O(n) latency for *this request* instead of O(1). If processing 100 K8s endpoints takes 10ms each, the request takes 1000ms end-to-end. The reactor itself is **not** stalled — each `co_await` suspends the fiber and the shard happily serves other requests in the meantime. The bug is per-request tail latency (and timeouts further down the chain), not throughput collapse.

**THE LESSON:** *Hard Rule: Replace sequential awaits with `seastar::parallel_for_each` or `max_concurrent_for_each`.* Batch concurrent operations with a semaphore (e.g., 16 in-flight) to bound parallelism without serializing. Only keep serial ordering when each iteration genuinely depends on the previous result.

**PROMPT GUARD:** "Never co_await inside a loop over external resources; use parallel_for_each with a concurrency limit."

*Corrected 2026-05-05: previous wording said the loop "blocks the reactor" and that "Seastar's event loop cannot multiplex." That was wrong — the reactor multiplexes across fibers; only this fiber's progress is serialized. The real symptom is per-request latency.*

---

#### 3. The Null-Dereference-from-C-API Anti-Pattern

**THE PATTERN:** Directly casting C library return values: `record.ip = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));`

**THE CONSEQUENCE:** `sqlite3_column_text()` returns NULL for SQL NULL values. Constructing `std::string` from a null pointer is undefined behavior--typically a segfault. This only surfaces when real data has NULLs, often weeks after deployment.

**THE LESSON:** *Hard Rule: Wrap all C string returns in a null-guard helper.* Create `safe_column_text()` that returns empty string for NULL. Skip records with NULL in required fields.

**PROMPT GUARD:** "Never cast sqlite3_column_text (or any C function returning char*) without an explicit null check."

---

#### 4. The Unbounded-Buffer Anti-Pattern

**THE PATTERN:** Using `buffer.push_back(item)` without size validation, trusting that normal traffic patterns will keep the buffer bounded.

**THE CONSEQUENCE:** Malicious or buggy peers can flood the buffer (e.g., gossip messages). Without a cap, memory grows until OOM kills the process. In a 12k LOC codebase, these grow sites are easy to miss during review.

**THE LESSON:** *Hard Rule: Every growing container must have an explicit MAX_SIZE.* Implement a drop strategy (oldest-first batch drop, rejection with backpressure, or ring buffer). Add an overflow counter metric.

**PROMPT GUARD:** "Every push_back, emplace_back, or append must have a corresponding size check or the container must be bounded by design."

---

#### 5. The Timer-Captures-This Anti-Pattern

**THE PATTERN:** Scheduling a timer callback with `[this] { this->on_timer(); }` and cancelling in the destructor.

**THE CONSEQUENCE:** Race condition: (1) Timer fires, callback queued on reactor. (2) `stop()` cancels timer (but callback already queued). (3) `stop()` returns, destructor runs. (4) Queued callback executes with dangling `this` -> use-after-free.

**THE LESSON:** *Hard Rule: Timer callbacks require RAII gate guards scoped to the full async lifetime.* Add a `seastar::gate _timer_gate`. Callbacks acquire `_timer_gate.hold()` at entry--fails if closed. **Critical: the holder must live for the entire async operation.** For synchronous callbacks, function-scope is sufficient. For async chains, move the holder into `.finally()` or the outermost lambda capture--a holder scoped only to a `try` block drops before the async work completes, leaving the remainder unprotected. `stop()` closes the gate *first* (waiting for in-flight callbacks), *then* cancels timers.

**PROMPT GUARD:** "Any lambda capturing 'this' for a timer or async callback must acquire a gate holder at entry, the holder must be scoped to the entire async operation (move into .finally() for async chains), and the owning class must close that gate before cancelling the timer."

---

#### 6. The Metrics-Lambda-Outlives-Service Anti-Pattern

**THE PATTERN:** Registering metrics with lambdas that capture `this`: `metrics::make_gauge("foo", [this] { return _count; })`.

**THE CONSEQUENCE:** Prometheus scrapes continue after service shutdown begins. If the lambda executes after `this` is destroyed, you get use-after-free. Metrics libraries don't automatically deregister on object destruction.

**THE LESSON:** *Hard Rule: Deregister metrics in `stop()` before any member destruction.* Call `_metrics.clear()` or equivalent as the *first* action in `stop()`, ensuring no lambda can fire during teardown.

**PROMPT GUARD:** "Any metrics lambda capturing 'this' must be deregistered in stop() before any other cleanup."

---

#### 7. The Business-Logic-in-Persistence Anti-Pattern

**THE PATTERN:** Placing validation rules (e.g., "max token count", "valid port range") inside the persistence/storage layer because "it has access to the data."

**THE CONSEQUENCE:** Architecture drift--the persistence layer accumulates business rules. Testing requires database setup. Changes to validation require touching storage code. The "layered architecture" becomes a lie.

**THE LESSON:** *Hard Rule: Persistence layers only transform and store.* Validation belongs in the service/business layer. Persistence accepts already-validated data and returns raw data for the service layer to interpret.

**PROMPT GUARD:** "Never add validation, transformation, or business rules to persistence/storage code--only the service layer validates."

---

#### 8. The Scattered-ThreadLocal Anti-Pattern

**THE PATTERN:** Declaring 10+ `thread_local` variables at file scope for per-shard state: `thread_local RadixTree* g_tree; thread_local Stats g_stats; ...`

**THE CONSEQUENCE:** State management becomes fragile--no clear initialization order, no single point for reset/cleanup, difficult to test. In a 12k LOC codebase, scattered thread_locals are invisible coupling.

**THE LESSON:** *Hard Rule: Consolidate per-shard state into a single `ShardLocalState` struct.* Provides explicit lifecycle (`init()`, `reset()`), single point of truth, and `reset_for_testing()` capability.

**PROMPT GUARD:** "Never declare standalone thread_local variables--group all per-shard state into a single ShardLocalState struct with explicit lifecycle methods."

---

#### 9. The Silent-Catch-All Anti-Pattern

**THE PATTERN:** `catch (...) {}` or `catch (const std::exception&) {}` with no logging, treating exceptions as "expected noise."

**THE CONSEQUENCE:** Configuration errors, network failures, and actual bugs are silently swallowed. The system appears to work but operates in a degraded state. Debugging requires adding logging and redeploying.

**THE LESSON:** *Hard Rule: Every catch block must log at warn level minimum.* Include the exception message, context (what operation failed), and any relevant identifiers. Consider adding a counter metric for exception rate.

**PROMPT GUARD:** "Never write an empty catch block--always log the exception at warn level with context about what operation failed."

---

#### 10. The Unchecked-String-Conversion Anti-Pattern

**THE PATTERN:** Using `std::stoi()`/`std::stol()` on external input without try-catch or validation.

**THE CONSEQUENCE:** Non-numeric strings throw `std::invalid_argument`. Out-of-range values throw `std::out_of_range`. Network input or config typos crash the process.

**THE LESSON:** *Hard Rule: Use `std::from_chars` or wrap in a validating helper.* Create `parse_port()`, `parse_int()` helpers that return `std::optional<T>` or `expected<T, Error>`. Validate ranges explicitly.

**PROMPT GUARD:** "Never use std::stoi/stol/stof on external input--use std::from_chars with explicit error handling and range validation."

---

#### 11. The Global-Static-Init-Race Anti-Pattern

**THE PATTERN:** Using global statics for shared state: `static Tracer* g_tracer; static bool g_enabled;`

**THE CONSEQUENCE:** Concurrent initialization and shutdown from different threads (or Seastar shards) causes data races. The "singleton" pattern without synchronization is undefined behavior in C++.

**THE LESSON:** *Hard Rule: Use `std::call_once` for one-time initialization.* For boolean flags, use `std::atomic<bool>`. For complex state, either make it per-shard (thread_local) or protect with proper synchronization.

**PROMPT GUARD:** "Never use bare global statics for runtime state--use std::call_once for init, std::atomic for flags, or thread_local for per-shard state."

---

#### 12. The Blocking-ifstream-in-Coroutine Anti-Pattern

**THE PATTERN:** Using `std::ifstream`, `std::ofstream`, raw `::read`/`::write` on fds, blocking SQL drivers, synchronous `getaddrinfo`/curl, or any other blocking syscall on the reactor thread. Also includes the common false-fix: wrapping the blocking call in `seastar::async(...)` or `seastar::thread(...)` and assuming that "offloads to a thread pool."

**THE CONSEQUENCE:** Blocking I/O on the reactor thread stalls every fiber on that shard — network I/O, timer callbacks, request processing. A 10ms disk read becomes 10ms of zero throughput. **Critically: `seastar::async`/`seastar::thread` do NOT fix this.** A `seastar::thread` is a stackful coroutine that runs *on the reactor*; the blocking call still blocks the reactor exactly as if it had been called directly. The wrapper only helps if the work being wrapped is itself non-blocking (e.g., uses `.get()` on Seastar futures).

**THE LESSON:** *Hard Rule: Use Seastar file I/O APIs for files. For genuinely blocking C libraries (SQLite, blocking DNS, etc.), run them on a dedicated OS thread and signal back via `seastar::alien::run_on()`.*

- Files: `seastar::open_file_dma()` + `seastar::make_file_input_stream()`.
- Blocking C libraries: dedicated `std::thread` worker pulling from an MPSC queue, results posted back to the originating shard with `seastar::alien::run_on`. See `src/tokenizer_thread_pool.cpp` for the canonical implementation.
- Startup-only blocking I/O (before the reactor starts) is fine — `main.cpp` dry-run and initial config load qualify. Document it explicitly.

**PROMPT GUARD:** "Never use std::ifstream/ofstream/blocking syscalls in Seastar code. `seastar::async` does NOT offload blocking work to a thread pool — it runs on the reactor. For files use seastar::open_file_dma; for blocking libraries use a dedicated OS thread + seastar::alien."

*Corrected 2026-05-05: previous wording only addressed file I/O and offered no guidance for blocking C libraries. The codebase's `async_persistence` and config-reload paths wrapped blocking SQLite/ifstream in `seastar::async` under the false assumption that it offloads to a thread pool — see BACKLOG follow-ups.*

---

#### 13. The Thread-Local-Raw-New Anti-Pattern

**THE PATTERN:** Using `thread_local T* g_ptr = nullptr;` with `g_ptr = new T();` for per-shard state, with no `delete`.

**THE CONSEQUENCE:** Memory leaks at process exit. But the more dangerous variant is the **subtle false fix**: switching to `thread_local std::unique_ptr<T>` only solves the problem if `T`'s destructor is trivial. The unique_ptr is destroyed at *thread-exit time*, which on Seastar shards happens **after** the reactor has already torn down. Any destructor that touches reactor primitives (metric registrations, gates, foreign_ptrs, futures) will run against a dead reactor — typically manifests as use-after-free or hangs at shutdown rather than a clean leak report.

**THE LESSON:** *Hard Rule: Per-shard state needs an explicit destroy function called during the shard's `stop()`, BEFORE the reactor tears down.* Acceptable patterns:

1. **Preferred:** `seastar::sharded<T>` — handles lifecycle ordering correctly.
2. Raw `thread_local T*` paired with an explicit `destroy_X()` invoked from the service's `stop()` (and verify it's actually wired up — `cleanup_*` functions that exist but are never called are worse than none).
3. `thread_local std::unique_ptr<T>` is acceptable **only** when `~T()` is trivial / pure-memory and touches no reactor state. Even then, prefer (1) or (2) for clarity.

**PROMPT GUARD:** "thread_local std::unique_ptr is NOT a fix on its own — its destructor runs after reactor teardown. Every per-shard thread_local needs an explicit destroy hook called from stop(), or use seastar::sharded<T>."

*Corrected 2026-05-05: previous wording recommended `thread_local std::unique_ptr<T>` as an equally-good alternative to an explicit destroy hook. That's only true for trivially-destructible state. For anything touching reactor primitives, the unique_ptr destruction happens too late.*

---

#### 14. The Cross-Shard Heap Memory Anti-Pattern

There are THREE distinct cross-shard memory bugs:

**BUG #1 - Cross-Shard FREE:** Moving heap-owning types across shards causes wrong-shard deallocation:
```cpp
auto tokens = get_tokens();  // Heap allocated on shard 0
smp::submit_to(shard_1, [tokens = std::move(tokens)] {
    // tokens' heap memory is still on shard 0!
    // When destructor runs here on shard 1, SIGSEGV or "free(): invalid pointer"
});
```

**BUG #2 - Cross-Shard READ:** Capturing references to shard-0 memory and reading from shard N is a race:
```cpp
do_with(std::move(data), [](auto& shared_data) {
    return parallel_for_each(shards, [&shared_data](unsigned shard_id) {
        return smp::submit_to(shard_id, [&shared_data] {  // BUG: &shared_data captured!
            // Reading shared_data from shard N while it lives on shard 0
            // This is a cross-shard memory access race condition
            auto copy = std::vector<T>(shared_data.begin(), shared_data.end());
        });
    });
});
```

**BUG #3 - Cross-Shard Callback State Access:** A callback captures `this` from shard X, then is broadcast to run on shard Y, accessing shard X's member variables:
```cpp
// On shard 0: set up callback that captures 'this'
service->set_callback([this](Data data) {
    _member_vector.push_back(data);  // Accesses shard 0's member!
});

// Later, in some broadcast code:
parallel_for_each(shards, [callback](unsigned shard_id) {
    return smp::submit_to(shard_id, [callback, data] {
        callback(data);  // BUG: Running on shard N, but callback accesses shard 0's state!
    });
});
```
This is particularly insidious because: (1) the callback looks innocent, (2) the broadcast looks correct, but (3) the combination causes multiple shards to simultaneously access shard 0's state without synchronization--a data race.

**THE CONSEQUENCE:** Bug #1 causes allocator corruption and delayed crashes. Bug #2 causes data corruption, null pointers, or SIGSEGV during reads. Bug #3 causes data races with corrupted containers, lost updates, or crashes. All three can manifest long after the bug occurs, making debugging extremely difficult.

**THE CORRECT PATTERN:** Use `foreign_ptr` to safely pass data across shards:
```cpp
do_with(std::move(data), [](auto& data) {
    return parallel_for_each(shards, [&data](unsigned shard_id) {
        // 1. Wrap copy in foreign_ptr (allocated on shard 0)
        auto ptr = std::make_unique<std::vector<T>>(data);
        auto foreign = seastar::make_foreign(std::move(ptr));

        return smp::submit_to(shard_id, [foreign = std::move(foreign)]() mutable {
            // 2. Read from foreign_ptr and create LOCAL allocation
            std::vector<T> local(foreign->begin(), foreign->end());
            // 3. Use local copy (properly allocated on THIS shard)
            process(local);
            // 4. foreign_ptr destructor returns to shard 0 for cleanup
        });
    });
});
```

**WHY `foreign_ptr` WORKS:**
- Wraps a `unique_ptr` for safe cross-shard transfer
- Destructor runs on the HOME shard (where it was created), not current shard
- Safe to READ from on any shard (data is guaranteed alive)
- Forces you to explicitly create local allocations

**RED FLAGS TO AUDIT:**
- `submit_to` lambdas capturing `&reference` to outer scope data
- `submit_to` lambdas capturing `vector`, `string`, `unique_ptr` by value (wrong-shard free)
- `parallel_for_each` broadcasting heap-owning data to all shards
- Raw pointers passed to `submit_to` lambdas (e.g., `[ptr = &data]`)
- `foreign_ptr` with `std::move(*foreign)` extracting data without local copy
- **Callbacks that capture `this` being broadcast to multiple shards** (Bug #3)
- `parallel_for_each` + `submit_to` patterns where the callback accesses member variables

**PROMPT GUARD:** "For cross-shard data: (1) Never capture references to outer-scope data in submit_to lambdas. (2) Never move heap-owning types directly--use foreign_ptr and create local allocations on the target shard. (3) Never broadcast a callback that captures `this` to multiple shards--either run the callback only on the owning shard, or ensure it only accesses shard-local state."

---

#### 15. The FFI Cross-Boundary Memory Anti-Pattern

**THE PATTERN:** Passing heap-allocated data (strings, vectors) across shard or thread boundaries to FFI code (Rust, C libraries):
```cpp
// Cross-shard FFI call
smp::submit_to(target_shard, [text = std::move(text)]() {
    rust_tokenizer->Encode(text);  // BUG: text's buffer allocated on calling shard
});

// Cross-thread FFI call (std::thread worker)
void worker_thread(Job& job) {
    rust_library->process(job.text);  // BUG: job.text allocated on reactor thread
}
```

**THE CONSEQUENCE:** While Seastar's `do_foreign_free` mechanism can handle cross-shard C++ allocations, **FFI allocators don't participate in Seastar's memory tracking**:
- Rust uses jemalloc (or system allocator) independently
- C libraries call `malloc`/`free` directly
- When FFI code operates on foreign-shard memory, it may reallocate, cache pointers, or interact with the buffer in ways that corrupt Seastar's per-shard allocator state

Real-world symptom: Under stress testing (100 users, 30 minutes), SIGSEGV crashes with corrupted pointers (`si_addr=0x5`) in the Rust tokenizer FFI.

**THE LESSON:** *Hard Rule: Always reallocate locally when data crosses shard/thread boundaries with FFI involved.*

This is **bidirectional**:
1. **INPUT to FFI:** Reallocate before passing to FFI functions
2. **OUTPUT from workers:** Reallocate when returning data to reactor threads

```cpp
// Cross-shard INPUT: reallocate at start of submit_to lambda
smp::submit_to(target_shard, [text_copy = std::move(text)]() {
    std::string local_text(text_copy.data(), text_copy.size());  // Local allocation
    rust_tokenizer->Encode(local_text);  // FFI sees locally-allocated memory
});

// Cross-thread INPUT: reallocate before FFI call
void worker_thread(Job& job) {
    std::string local_text(job.text.data(), job.text.size());  // Local allocation
    auto tokens = rust_library->process(local_text);

    // Cross-thread OUTPUT: reallocate in reactor callback
    alien::run_on(reactor, shard, [tokens = std::move(tokens)]() {
        // tokens was allocated by worker (foreign_malloc) - reallocate on reactor
        std::vector<int32_t> local_tokens(tokens.begin(), tokens.end());
        use(local_tokens);  // Reactor uses locally-allocated copy
    });
}
```

**IMPORTANT:** Seastar replaces `malloc` globally. Worker threads (std::thread) DO use Seastar's allocator - their allocations are tracked as `foreign_mallocs`. When the reactor frees worker-allocated memory, `do_foreign_free` is triggered, which can cause corruption with FFI code.

**THIS APPLIES TO:**
- Rust libraries via C FFI (tokenizers, regex, simd-json, etc.)
- C libraries that may reallocate or cache buffers (SQLite, zlib, etc.)
- Any `std::thread` workers processing data from Seastar reactors
- Data returned FROM worker threads TO reactor threads (bidirectional!)
- Any external code outside Seastar's allocator control

**RED FLAGS TO AUDIT:**
- `smp::submit_to` lambdas that pass strings/vectors directly to FFI functions
- `seastar::alien::run_on` callbacks passing data to/from worker threads
- Worker threads (std::thread) calling FFI with reactor-allocated data
- **Worker threads returning heap data to reactors via alien::run_on** (bidirectional!)
- Any FFI call in code that could run on a different shard than allocation

**PROMPT GUARD:** "When data crosses shard/thread boundaries with FFI involved, reallocate in BOTH directions: (1) reallocate input before FFI calls, (2) reallocate output when returning from worker threads to reactors. Seastar's global malloc means worker allocations are foreign_mallocs that cause corruption on reactor free."

---

#### 16. The Lambda-Coroutine-Fiasco Anti-Pattern

**THE PATTERN:** Passing a lambda coroutine as a continuation to Seastar APIs like `.then()`, `parallel_for_each()`, or `with_semaphore()`:
```cpp
seastar::future<> f() {
    return seastar::yield().then([captured_state] () -> seastar::future<> {
        co_await seastar::sleep(1ms);
        use(captured_state);  // BUG: use-after-free!
    });
}
```

**THE CONSEQUENCE:** When `.then()` receives the lambda, it moves/copies it into an internal memory area. When `operator()` returns (at the first `co_await`), `.then()` frees that memory area. But the coroutine is still suspended and will later resume, accessing the now-freed captures. This is a **use-after-free** that only manifests when the coroutine actually suspends--ready futures hide the bug during testing.

This is so well-known in the Seastar community that it has its own name: the ["Lambda Coroutine Fiasco"](https://github.com/scylladb/seastar/blob/master/doc/lambda-coroutine-fiasco.md).

**THE LESSON:** *Hard Rule: Wrap lambda coroutines passed to continuation-based APIs with `seastar::coroutine::lambda()`.* This extends the lambda's lifetime until the returned future resolves. In C++23, use `this auto` as the first parameter to copy captures into the coroutine frame.

```cpp
// CORRECT: wrap with seastar::coroutine::lambda()
co_await seastar::yield().then(seastar::coroutine::lambda([captured_state] () -> seastar::future<> {
    co_await seastar::sleep(1ms);
    use(captured_state);  // Safe: lambda lifetime extended
}));

// CORRECT (C++23): deducing this copies captures to coroutine frame
co_await seastar::yield().then([captured_state] (this auto) -> seastar::future<> {
    co_await seastar::sleep(1ms);
    use(captured_state);  // Safe: captures copied into frame
});
```

**SAFE APIs:** Some Seastar primitives (e.g., coroutine-aware versions) are already safe for lambda coroutines. Check the Seastar docs for each API. When in doubt, always wrap.

**PROMPT GUARD:** "Never pass a lambda coroutine to .then(), parallel_for_each(), or other continuation-based APIs without wrapping it in seastar::coroutine::lambda(). Lambda captures are freed at the first co_await otherwise."

---

#### 17. The Reactor-Stall Anti-Pattern

**THE PATTERN:** Running CPU-intensive work without inserting preemption points:
```cpp
// Pattern A: CPU-bound loop
for (auto& entry : large_map) {
    process(entry);  // No yield, no co_await
}

// Pattern B: Recursive .then() chains without preemption
seastar::future<> drain_loop() {
    return queue.pop().then([this](auto item) {
        process(item);
        return drain_loop();  // No preemption check between .then() continuations
    });
}
```

**THE CONSEQUENCE:** Seastar's cooperative scheduler cannot preempt a running task. If a task runs for more than the task quota (~500μs), it causes a **reactor stall**--all network I/O, timer callbacks, and other request processing on that shard freeze. A 10ms stall on a shard handling Raft heartbeats can trigger leadership elections. At scale, anything above O(log N) complexity per continuation risks stalls.

**THE LESSON:** *Hard Rule: Insert preemption points in any loop that may iterate more than ~100 times or run for >100μs.* Use `seastar::coroutine::maybe_yield` in coroutines, `seastar::maybe_yield()` in future chains, or Seastar's built-in looping constructs (`repeat`, `do_until`) which have built-in preemption checks.

```cpp
// CORRECT: coroutine with yield points
for (auto& entry : large_map) {
    process(entry);
    co_await seastar::coroutine::maybe_yield();
}

// CORRECT: use Seastar looping primitives
return seastar::do_for_each(large_map, [](auto& entry) {
    process(entry);  // Built-in preemption check between iterations
});
```

**NOTE:** This is distinct from Rule #2 (sequential co_await in loops over external resources). Rule #2 is about latency from serialized I/O. This rule is about CPU-bound work starving the reactor even with no I/O at all.

**PROMPT GUARD:** "Any loop that may iterate more than ~100 times must include a preemption point (maybe_yield, co_await, or use a Seastar looping primitive). Recursive .then() chains have NO built-in preemption--use repeat/do_until instead."

---

#### 18. The Discarded-Future Anti-Pattern

**THE PATTERN:** Calling an async function without awaiting or storing its returned future:
```cpp
void on_request(Request req) {
    log_request_async(req);  // Future returned but discarded!
    process(req);
}

// Or intentionally ignoring:
seastar::future<> cleanup() {
    auto f = remove_temp_files();  // f destroyed without being awaited
    return seastar::make_ready_future<>();
}
```

**THE CONSEQUENCE:** A discarded future silently drops any exception it carries. If the async operation fails, the error vanishes. Seastar detects this and logs a warning ("Exceptional future ignored"), but the damage is already done--the error is lost, and the application may be in an inconsistent state. In debug builds, Seastar may abort on discarded exceptional futures.

**THE LESSON:** *Hard Rule: Every future must be either `co_await`ed, returned, or explicitly acknowledged with `.discard_result()` / `.ignore_ready_future()`.* If you intentionally want fire-and-forget, use a gate-guarded background fiber with its own error handling.

```cpp
// CORRECT: await it
co_await log_request_async(req);

// CORRECT: fire-and-forget with gate + error handling
(void)seastar::with_gate(_gate, [this, req] {
    return log_request_async(req).handle_exception([](auto ep) {
        logger.warn("Background log failed: {}", ep);
    });
});
```

**PROMPT GUARD:** "Never discard a Seastar future. Every future must be co_awaited, returned, or explicitly handled. Fire-and-forget operations need gate guards and their own exception handling."

---

#### 19. The Semaphore-Units-Leak Anti-Pattern

**THE PATTERN:** Using separate `semaphore::wait()` and `semaphore::signal()` calls for concurrency control:
```cpp
return _sem.wait(1).then([this] {
    return slow_operation();  // What if this throws synchronously?
}).finally([this] {
    _sem.signal(1);
});
```

**THE CONSEQUENCE:** If `slow_operation()` throws an exception (not returns a failed future, but *throws*), the `.finally()` continuation is never attached. The semaphore unit is permanently lost. Over time, the available units monotonically decrease until the semaphore is exhausted and all waiters deadlock. This is especially insidious because it only manifests under error conditions, potentially weeks after deployment.

Additionally, requesting more units than the semaphore's total capacity creates a future that can **never** resolve--a permanent deadlock.

**THE LESSON:** *Hard Rule: Never use raw `wait()`/`signal()` pairs. Use `with_semaphore()` or `get_units()` for exception-safe RAII semantics.* Validate that requested units never exceed total capacity.

```cpp
// CORRECT: with_semaphore wraps in futurize_invoke (catches sync throws)
return seastar::with_semaphore(_sem, 1, [this] {
    return slow_operation();
});

// CORRECT: RAII units returned automatically on destruction
auto units = co_await seastar::get_units(_sem, 1);
co_await slow_operation();
// units auto-returned when scope exits (even on exception)
```

**PROMPT GUARD:** "Never use raw semaphore wait()/signal()--use with_semaphore() or get_units() for exception-safe RAII. Validate that requested units never exceed semaphore capacity."

---

#### 20. The do_with-Missing-Reference Anti-Pattern

**THE PATTERN:** Forgetting the `&` in `do_with` lambda parameters, or passing objects by value to async functions called within `do_with`:
```cpp
// Bug 1: Missing & -- creates a copy that dies when lambda returns
return seastar::do_with(std::move(buffer), [](auto obj) {  // BUG: should be auto&
    return slow_op(obj);
});

// Bug 2: slow_op takes by value, creating a copy
seastar::future<> slow_op(std::string text);  // BUG: should be const std::string&
return seastar::do_with(std::move(text), [](auto& text) {
    return slow_op(text);  // Implicit copy destroyed before future resolves
});
```

**THE CONSEQUENCE:** `do_with` allocates the object on the heap and passes a reference to the lambda. If the lambda parameter isn't a reference (`auto` without `&`), C++ creates a copy that is destroyed when the lambda returns--before the future resolves. This is a use-after-free that the compiler will **not** warn about. Symptoms include intermittent crashes under load, often in completely unrelated code due to heap corruption.

**THE LESSON:** *Hard Rule: In new code, prefer C++20 coroutines and let the coroutine frame own the variable — `do_with` is rarely needed.* When `do_with` is unavoidable (e.g., pre-coroutine code paths, or anchoring a `foreign_ptr` for an `invoke_on_all` chain that doesn't return a future to the caller), always use `auto&` or explicit `Type&` in the lambda parameters.

Two structural defenses that prevent this bug entirely:
1. **Coroutines:** `co_await slow_op(buffer)` keeps `buffer` on the coroutine frame for the full duration. No do_with, no missing-reference bug possible.
2. **Move-only types:** if the captured object is move-only (no copy ctor), the buggy `[](auto obj){...}` form fails to compile. Marking heavy types `noncopyable` (e.g., `std::unique_ptr<T>`, types with deleted copy) turns this from a silent runtime use-after-free into a compile error.

```cpp
// CORRECT: reference parameter
return seastar::do_with(std::move(buffer), [](auto& obj) {
    return slow_op(obj);  // obj is the do_with-managed heap object
});

// BETTER: coroutine (no do_with needed)
seastar::future<> handle(Buffer buffer) {
    co_await slow_op(buffer);  // buffer lives on coroutine frame
}
```

**PROMPT GUARD:** "Prefer coroutines over do_with for new code — they eliminate this class of lifetime bugs entirely. When do_with is unavoidable, always use auto& in the lambda. Move-only types turn missing-& into a compile error rather than runtime UAF."

*Corrected 2026-05-05: previous wording presented `do_with` and coroutines as roughly co-equal options. C++20 coroutines remove the need for `do_with` in nearly all cases. Also added the move-only-type defense, which converts silent runtime UAF into a compile error.*

---

#### 21. The Coroutine-Reference-Parameter Anti-Pattern

**THE PATTERN:** Passing parameters by `const&` or `&` to coroutines, following the normal C++ convention of avoiding copies:
```cpp
seastar::future<> process(const std::vector<int>& tokens) {
    // ... some work ...
    co_await seastar::sleep(10ms);
    use(tokens);  // BUG: caller may have destroyed tokens by now!
}

// Caller:
auto t = get_tokens();
process(t);  // Returns future, doesn't wait
// t destroyed here -- process() has dangling reference
```

**THE CONSEQUENCE:** A coroutine may suspend and resume long after the caller returns. Any reference parameter becomes dangling if the caller doesn't co_await the returned future before the referenced object's scope ends. Even if the caller does await today, a later refactor may change that. Redpanda's engineering team learned this the hard way and eventually **forbade passing references into coroutines** entirely.

**THE LESSON:** *Hard Rule: Coroutines should take parameters by value (using `std::move` for large objects), not by reference.* Move semantics make this cheap. The only exception is when the coroutine is `co_await`ed in the same expression and the referenced object provably outlives it.

```cpp
// CORRECT: take by value, move in
seastar::future<> process(std::vector<int> tokens) {  // By value
    co_await seastar::sleep(10ms);
    use(tokens);  // Safe: tokens owned by coroutine frame
}

// Caller:
auto t = get_tokens();
co_await process(std::move(t));  // Moved in, no copy
```

**PROMPT GUARD:** "Never pass reference parameters to coroutines--take by value and std::move. References become dangling when the coroutine suspends past the caller's scope."

---

#### 22. The Exception-Before-Future Anti-Pattern

**THE PATTERN:** A function that may throw a C++ exception before it returns a future:
```cpp
seastar::future<> handle(Request req) {
    validate(req);  // Throws std::invalid_argument -- never returns a future
    return do_work(req).finally([] {
        cleanup();  // Never reached if validate() threw!
    });
}

// Caller expects to handle errors via future:
handle(req).handle_exception([](auto ep) {
    log_error(ep);  // Never called! Exception propagated as C++ throw, not failed future.
});
```

**THE CONSEQUENCE:** Seastar continuations (`.then()`, `.finally()`, `.handle_exception()`) only handle **exceptional futures**, not C++ exceptions thrown before a future is returned. If a function throws before returning any future, the `.finally()` and `.handle_exception()` continuations are never attached. The exception propagates up the call stack as a regular C++ throw, bypassing all future-based error handling. This causes resource leaks (semaphore units, gate holders) and unhandled crashes.

**THE LESSON:** *Hard Rule: Functions returning `seastar::future<>` must not throw--convert all potential throws to failed futures.* Use `seastar::futurize_invoke()` to wrap calls that might throw, or catch and convert in coroutines.

```cpp
// CORRECT: coroutine naturally converts throws to failed futures
seastar::future<> handle(Request req) {
    validate(req);  // If this throws, coroutine machinery wraps it as failed future
    co_await do_work(req);
    cleanup();
}

// CORRECT: futurize_invoke for continuation-style code
return seastar::futurize_invoke([&] {
    validate(req);  // Throw caught and converted to failed future
    return do_work(req);
}).finally([] {
    cleanup();  // Always runs now
});
```

**PROMPT GUARD:** "Functions returning seastar::future must not throw C++ exceptions--use coroutines (which auto-convert throws) or wrap with futurize_invoke(). Throws before future-return bypass all .then()/.finally()/.handle_exception() chains."

---

#### 23. The Shared-temporary_buffer-Pin Anti-Pattern

**THE PATTERN:** Holding a `share()`d view of a `seastar::temporary_buffer` long-term, after the original request is done:
```cpp
seastar::future<> handle_request(temporary_buffer<char> buf) {
    // Slice out a 32-byte header from a 64KB network buffer
    auto header = buf.share(0, 32);  // Zero-copy view

    // Store for later use (e.g., in a cache or lookup table)
    _header_cache[key] = std::move(header);  // BUG: pins entire 64KB allocation!
}
```

**THE CONSEQUENCE:** `temporary_buffer::share()` creates a zero-copy view backed by the same underlying allocation. The entire original buffer stays alive as long as *any* shared view exists. A 32-byte cached header silently pins a 64KB network buffer in memory. Under traffic, this causes unexplained memory growth that doesn't correlate with logical data sizes.

**THE LESSON:** *Hard Rule: Never hold `share()`d `temporary_buffer` views beyond the current request.* For long-lived data, `clone()` or copy the bytes into an independent allocation.

```cpp
// CORRECT: clone for long-term storage
auto header = buf.share(0, 32);
_header_cache[key] = header.clone();  // Independent 32-byte allocation

// CORRECT: copy to string for storage
std::string header_str(buf.get(), 32);
_header_cache[key] = std::move(header_str);
```

**PROMPT GUARD:** "Never store shared temporary_buffer views beyond the current request--they pin the entire underlying allocation. Use clone() or copy for long-lived data."

---

### Quick Reference: Hard Rules

| # | Rule | Violation |
|---|------|-----------|
| 0 | Prefer `unique_ptr`; use `lw_shared_ptr` for shard-local sharing | `std::shared_ptr` in Seastar code |
| 1 | Metrics accessors must be lock-free | `std::mutex` in query method |
| 2 | No `co_await` inside loops over external resources | Sequential await in for-loop |
| 3 | Null-guard all C string returns | Direct cast of `sqlite3_column_text` |
| 4 | Every growing container needs MAX_SIZE | Unbounded `push_back` |
| 5 | Timer callbacks need gate guards scoped to full async lifetime | Lambda captures `this` without gate; holder drops before async completes |
| 6 | Deregister metrics first in `stop()` | Lambda outlives service |
| 7 | Persistence only stores, never validates | Business logic in storage layer |
| 8 | Single `ShardLocalState` struct for per-shard state | Scattered `thread_local` variables |
| 9 | Every catch block logs at warn level | Silent `catch (...)` |
| 10 | Validating helpers for string-to-number | Bare `std::stoi()` on input |
| 11 | `std::call_once` or `std::atomic` for global state | Unprotected global statics |
| 12 | Use Seastar file I/O APIs | `std::ifstream/ofstream` in Seastar code |
| 13 | Thread-local raw new needs destroy function | `thread_local T* = new T()` without cleanup |
| 14 | Force local allocation for cross-shard data | Moving `vector`/`string` via `submit_to` |
| 15 | Reallocate locally before FFI across boundaries | Passing shard-allocated data to Rust/C FFI |
| 16 | Wrap lambda coroutines with `seastar::coroutine::lambda()` | Lambda coroutine passed to `.then()` without wrapper |
| 17 | Insert preemption points in loops (>100 iterations) | CPU-bound loop without `maybe_yield()` or Seastar loop primitive |
| 18 | Every future must be awaited or explicitly handled | Discarded future silently drops errors |
| 19 | Use `with_semaphore()`/`get_units()`, never raw `wait()`/`signal()` | Semaphore units leak on synchronous throw |
| 20 | Always use `auto&` in `do_with` lambdas; prefer coroutines | Missing `&` creates copy destroyed before future resolves |
| 21 | Coroutines take parameters by value, not reference | `const&` parameter dangles when coroutine suspends |
| 22 | Wrap throws in `futurize_invoke()`; prefer coroutines | Exception thrown before future returned bypasses `.finally()` |
| 23 | Clone `temporary_buffer` for long-lived data, don't `share()` | Shared view pins entire underlying allocation |
