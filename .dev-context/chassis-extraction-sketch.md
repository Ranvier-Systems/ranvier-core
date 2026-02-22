# Chassis Extraction Sketch: Seastar Service Toolkit

**Date:** 2026-02-22
**Status:** Design sketch (pre-implementation)
**Context:** Evaluating extraction of reusable infrastructure from Ranvier Core into a shared toolkit that can power an API Gateway, KV Store, or DNS Server without the generalization tax of a framework.

---

## 1. Guiding Principle: Toolkit, Not Framework

A **framework** forces everything through one abstraction (generic ART interface, pluggable protocol handlers). A **toolkit** provides building blocks that each product composes directly. Seastar itself is a toolkit — that's why it works.

**Concrete rule:** No shared virtual base class for "request handlers" or "storage engines." Each product wires its own hot path. The toolkit provides infrastructure around the hot path, not through it.

---

## 2. The Three Layers

```
┌─────────────────────────────────────────────────────────┐
│  PRODUCT LAYER (per-product, NOT shared)                │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │ Ranvier Core │  │ API Gateway  │  │   KV Store   │  │
│  │ (LLM router) │  │              │  │              │  │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  │
├─────────┼──────────────────┼──────────────────┼─────────┤
│  CHASSIS LAYER (shared library: libseastar-chassis)     │
│                                                         │
│  ┌─────────────────────────────────────────────────┐    │
│  │ Service Lifecycle   │ Sharded Config             │    │
│  │ Connection Pool     │ Async Persistence Queue    │    │
│  │ Circuit Breaker     │ Rate Limiter (core + svc)  │    │
│  │ Health Service      │ Metrics Helpers            │    │
│  │ Shard Load Balancer │ Gossip Cluster Layer       │    │
│  │ K8s Discovery       │ Tracing Service            │    │
│  │ Logging Framework   │ P2C Algorithm              │    │
│  │ Text Validator      │ Parse Utils                │    │
│  │ Stream Parser       │ Cross-Shard Request Utils  │    │
│  └─────────────────────────────────────────────────┘    │
├─────────────────────────────────────────────────────────┤
│  SEASTAR + SYSTEM LIBRARIES                             │
│  seastar, abseil, yaml-cpp, sqlite3, openssl, rapidjson │
└─────────────────────────────────────────────────────────┘
```

---

## 3. File-Level Extraction Map

### 3.1 Chassis Layer (moves to shared lib)

These files have **zero LLM/tokenization/ART coupling** and can be extracted as-is or with minimal namespace changes.

| File | Current Coupling | Extraction Notes |
|------|-----------------|------------------|
| `connection_pool.hpp` | `logging.hpp` only | Move as-is. Templatized on Clock + ClosePolicy already. |
| `circuit_breaker.hpp` | `types.hpp` (BackendId) | Generalize BackendId → template param or `int32_t` alias in chassis. |
| `rate_limiter_core.hpp` | None (pure algorithm) | Move as-is. Zero Seastar deps. |
| `rate_limiter.hpp` | `rate_limiter_core.hpp` | Move as-is. Seastar service wrapper. |
| `stream_parser.{hpp,cpp}` | `logging.hpp` only | Move as-is. Generic chunked/SSE parser. |
| `sharded_config.hpp` | `config.hpp` (RanvierConfig) | **Template on config type**: `ShardedConfig<T>` instead of hardcoded RanvierConfig. |
| `shard_load_balancer.hpp` | `shard_load_metrics.hpp` | Move as-is. Pure P2C algorithm. |
| `shard_load_metrics.hpp` | None | Move as-is. |
| `cross_shard_request.hpp` | None | Move as-is. |
| `persistence.hpp` | `types.hpp` (TokenId, BackendId) | **Generalize interface**: Replace route/backend-specific methods with generic key-value ops, or keep as trait/CRTP for products to specialize. |
| `async_persistence.{hpp,cpp}` | `persistence.hpp`, `types.hpp` | **Generalize Op variant**: Template on operation types, or extract the queue/timer/batch pattern as a generic `AsyncBatchQueue<Op>`. |
| `sqlite_persistence.{hpp,cpp}` | `persistence.hpp` | Stays coupled to PersistenceStore interface. Moves with it. |
| `health_service.{hpp,cpp}` | `router_service.hpp` | **Decouple**: Accept a generic `BackendRegistry` interface (list of addresses + status callback) instead of `RouterService&`. |
| `gossip_service.{hpp,cpp}` | `config.hpp`, `types.hpp` | **Decouple callbacks**: Already uses `RouteLearnCallback` / `NodeStateCallback` function objects. Generalize message payload (currently route-specific). |
| `gossip_consensus.{hpp,cpp}` | `config.hpp` | Move as-is. Peer table + quorum is generic. |
| `gossip_protocol.{hpp,cpp}` | `types.hpp` (TokenId) | **Generalize packet types**: Make payload extensible (product registers message handlers). |
| `gossip_transport.{hpp,cpp}` | None beyond Seastar + OpenSSL | Move as-is. Pure UDP/DTLS channel. |
| `crypto_offloader.{hpp,cpp}` | None | Move as-is. |
| `dtls_context.{hpp,cpp}` | None beyond OpenSSL | Move as-is. |
| `k8s_discovery_service.{hpp,cpp}` | `router_service.hpp` | **Decouple**: Accept a generic `EndpointSink` callback instead of `RouterService&`. |
| `logging.hpp` | `seastar/util/log.hh` | Move core utilities (request ID generation, traceparent extraction). Product-specific loggers (log_router, log_proxy) stay in product. |
| `parse_utils.hpp` | None | Move as-is. |
| `text_validator.hpp` | None | Move as-is. |
| `proxy_retry_policy.hpp` | None | Move as-is. Generic retry with backoff. |
| `types.hpp` | None | **Split**: Chassis defines `BackendId = int32_t`. Product adds `TokenId = int32_t`. |

### 3.2 Ranvier Product Layer (stays in ranvier-core)

These files are LLM-specific and stay in the product repo, consuming the chassis as a dependency.

| File | Why It Stays |
|------|-------------|
| `radix_tree.hpp` | ART is the LLM prefix routing core. Not generic infrastructure. |
| `node_slab.{hpp,cpp}` | Slab allocator purpose-built for ART node sizes. |
| `router_service.{hpp,cpp}` | Token-prefix routing, route batching, load-aware routing — all LLM-specific. |
| `tokenizer_service.{hpp,cpp}` | HuggingFace BPE via Rust FFI. |
| `tokenizer_thread_pool.{hpp,cpp}` | Thread pool specifically for tokenizer FFI offload. |
| `http_controller.{hpp,cpp}` | Ranvier's specific HTTP handling (tokenization, body rewriting, SSE proxying). |
| `request_rewriter.hpp` | Token ID injection into request bodies. |
| `chat_template.hpp` | vLLM chat template formatting. |
| `metrics_service.hpp` | **Split needed**: Generic histogram/counter helpers → chassis. Ranvier-specific metrics (ART, tokenizer, prefix boundary) → product. |
| `config_schema.hpp` | **Split needed**: Generic configs (ServerConfig, PoolConfig, HealthConfig, TlsConfig, AuthConfig, RateLimitConfig, RetryConfig, CircuitBreakerConfig, ShutdownConfig, BackpressureConfig, ClusterConfig, K8sDiscoveryConfig, TelemetryConfig, LoadBalancingConfig) → chassis. LLM-specific configs (RoutingConfig, AssetsConfig) → product. |
| `config_loader.{hpp,cpp}` | **Split needed**: YAML loading + env override machinery → chassis. Ranvier-specific field parsing → product. |
| `application.{hpp,cpp}` | **Template/pattern**: Extract the lifecycle state machine (CREATED→RUNNING→DRAINING→STOPPED) as a chassis pattern. Product implements concrete service wiring. |
| `main.cpp` | Product-specific CLI + reactor entry. |

### 3.3 Metrics Service Split (detailed)

The current `MetricsService` is ~620 lines with Ranvier-specific counters baked in. The split:

**Chassis (`chassis/metrics_helpers.hpp`):**
```
- MetricHistogram class (bucket-based accumulator)
- latency_buckets(), backend_latency_buckets(), etc. (configurable bucket sets)
- Per-backend metrics pattern (bounded map + dynamic registration)
- g_metrics thread-local pattern
- init_metrics() / stop_metrics() lifecycle
- to_seconds() helper
```

**Product (`ranvier/metrics_service.hpp`):**
```
- All Ranvier-specific counters (tokenization, cache hits, ART lookups, prefix boundary)
- Ranvier-specific histogram choices
- get_backend_load() integration
```

---

## 4. Dependency Graph After Extraction

```
                    ┌──────────────────┐
                    │   ranvier-core   │
                    │   (product)      │
                    └────────┬─────────┘
                             │ depends on
                    ┌────────▼─────────┐
                    │ seastar-chassis  │
                    │   (shared lib)   │
                    └────────┬─────────┘
                             │ depends on
              ┌──────────────┼──────────────┐
              │              │              │
        ┌─────▼────┐  ┌─────▼────┐  ┌─────▼────┐
        │ Seastar  │  │  Abseil  │  │ yaml-cpp │
        │          │  │          │  │ sqlite3  │
        └──────────┘  └──────────┘  │ openssl  │
                                    └──────────┘
```

Each new product (API Gateway, KV Store, DNS Server) depends on `seastar-chassis` but **not** on `ranvier-core`.

---

## 5. Key Interfaces to Generalize

### 5.1 BackendRegistry (replaces RouterService& in health/k8s)

```cpp
// chassis/backend_registry.hpp
namespace chassis {

using BackendId = int32_t;

struct BackendEndpoint {
    BackendId id;
    seastar::socket_address addr;
    uint32_t weight = 100;
    uint32_t priority = 0;
};

// Interface that products implement
class BackendRegistry {
public:
    virtual ~BackendRegistry() = default;
    virtual std::vector<BackendId> get_all_backend_ids() const = 0;
    virtual std::optional<seastar::socket_address> get_backend_address(BackendId) = 0;
    virtual seastar::future<> set_backend_status(BackendId, bool alive) = 0;
    virtual seastar::future<> register_backend(BackendEndpoint) = 0;
    virtual seastar::future<> unregister_backend(BackendId) = 0;
};

} // namespace chassis
```

This is the **one** virtual interface the chassis needs. HealthService, K8sDiscoveryService, and GossipService all interact with backends through this instead of `RouterService&` directly.

**Why a virtual interface here is acceptable:** This is control plane (ms-scale operations like health checks and K8s syncs), not the data plane hot path. The vtable call cost is noise.

### 5.2 AsyncBatchQueue (generalized from AsyncPersistenceManager)

```cpp
// chassis/async_batch_queue.hpp
namespace chassis {

template<typename Op>
class AsyncBatchQueue {
public:
    struct Config {
        std::chrono::milliseconds flush_interval{100};
        size_t max_batch_size = 1000;
        size_t max_queue_depth = 100000;
    };

    using BatchProcessor = std::function<void(std::vector<Op>)>;

    AsyncBatchQueue(Config config, BatchProcessor processor);
    bool try_enqueue(Op op);
    seastar::future<> start();
    seastar::future<> stop();
    size_t queue_depth() const;
    bool is_backpressured() const;

private:
    // Same timer/gate/semaphore/mutex pattern as AsyncPersistenceManager
};

} // namespace chassis
```

Products instantiate with their own op types:
- **Ranvier:** `AsyncBatchQueue<PersistenceOp>` with SQLite processor
- **API Gateway:** `AsyncBatchQueue<AccessLogEntry>` with file/syslog processor
- **KV Store:** `AsyncBatchQueue<WALEntry>` with WAL processor

### 5.3 Gossip Message Extensibility

Current gossip protocol has hardcoded packet types (ROUTE_ANNOUNCEMENT, HEARTBEAT, etc.). Generalize:

```cpp
// chassis/gossip_protocol.hpp
namespace chassis {

// Core message types (chassis-provided)
enum class GossipMessageType : uint8_t {
    HEARTBEAT = 1,
    HEARTBEAT_ACK = 2,
    NODE_STATE = 3,
    // 128+ reserved for product-specific messages
    PRODUCT_BASE = 128,
};

// Products register handlers for their message types
using MessageHandler = std::function<void(seastar::socket_address from,
                                          const uint8_t* payload,
                                          size_t len)>;

class GossipProtocol {
public:
    void register_message_type(uint8_t type, MessageHandler handler);
    seastar::future<> broadcast(uint8_t type, const uint8_t* payload, size_t len);
};

} // namespace chassis
```

- **Ranvier** registers type 128 = ROUTE_ANNOUNCEMENT, 129 = ROUTE_ACK
- **KV Store** registers type 128 = KEY_INVALIDATION, 129 = REPLICATION_CHUNK
- **DNS Server** registers type 128 = ZONE_UPDATE, 129 = ZONE_TRANSFER_REQUEST

### 5.4 ShardedConfig Generalization

```cpp
// chassis/sharded_config.hpp
namespace chassis {

template<typename ConfigType>
class ShardedConfig {
public:
    ShardedConfig() = default;
    explicit ShardedConfig(ConfigType config) noexcept : _config(std::move(config)) {}
    seastar::future<> stop() noexcept { return seastar::make_ready_future<>(); }
    const ConfigType& config() const noexcept { return _config; }
    void update(const ConfigType& c) { _config = c; }
    void update(ConfigType&& c) noexcept { _config = std::move(c); }
private:
    ConfigType _config;
};

} // namespace chassis
```

Trivial change. `seastar::sharded<chassis::ShardedConfig<RanvierConfig>>` in Ranvier, `seastar::sharded<chassis::ShardedConfig<GatewayConfig>>` in the API Gateway.

---

## 6. Application Lifecycle Pattern

The current `Application` class is 280 lines of service wiring. Extract the **pattern**, not the class:

```cpp
// chassis/application_base.hpp
namespace chassis {

enum class AppState { CREATED, STARTING, RUNNING, DRAINING, STOPPING, STOPPED };

// NOT a base class. A composition helper.
class ApplicationLifecycle {
public:
    AppState state() const;
    bool is_running() const;
    bool is_shutting_down() const;

    // Signal management
    void setup_signal_handlers(std::function<seastar::future<>()> reload_callback);
    void signal_shutdown();

    // Drain helpers
    seastar::future<> wait_for_drain(seastar::gate& request_gate,
                                      std::chrono::seconds timeout);

    // State transitions (products call these)
    void transition_to(AppState state);

private:
    AppState _state = AppState::CREATED;
    std::atomic<bool> _shutdown_initiated{false};
    std::atomic<int> _sigint_count{0};
    std::unique_ptr<seastar::promise<>> _stop_signal;
    std::chrono::steady_clock::time_point _last_reload_time;
};

} // namespace chassis
```

Products compose this into their own Application class and wire their specific services.

---

## 7. Config Schema Split

### Chassis configs (generic infrastructure):

```cpp
namespace chassis {
    struct ServerConfig { ... };           // bind_address, api_port, metrics_port
    struct PoolConfig { ... };             // connection pool settings
    struct HealthConfig { ... };           // check intervals, thresholds
    struct TlsConfig { ... };             // cert/key paths
    struct AuthConfig { ... };            // API keys, secure_compare
    struct RateLimitConfig { ... };       // token bucket settings
    struct RetryConfig { ... };           // backoff settings
    struct CircuitBreakerConfig { ... };  // failure/success thresholds
    struct ShutdownConfig { ... };        // drain timeouts
    struct BackpressureConfig { ... };    // concurrency limits
    struct ClusterConfig { ... };         // gossip, peers, quorum
    struct K8sDiscoveryConfig { ... };    // endpoint watching
    struct TelemetryConfig { ... };       // OTLP settings
    struct LoadBalancingConfig { ... };   // P2C settings
    struct DatabaseConfig { ... };        // SQLite path, journal mode
}
```

### Ranvier-specific configs (product layer):

```cpp
namespace ranvier {
    struct RoutingConfig { ... };   // ART, hash strategy, prefix routing, load-aware
    struct AssetsConfig { ... };    // tokenizer path, chat template, cache settings

    struct RanvierConfig {
        chassis::ServerConfig server;
        chassis::DatabaseConfig database;
        chassis::HealthConfig health;
        chassis::PoolConfig pool;
        RoutingConfig routing;            // Product-specific
        chassis::TimeoutConfig timeouts;
        AssetsConfig assets;              // Product-specific
        chassis::TlsConfig tls;
        chassis::AuthConfig auth;
        chassis::RateLimitConfig rate_limit;
        chassis::RetryConfig retry;
        chassis::CircuitBreakerConfig circuit_breaker;
        chassis::ShutdownConfig shutdown;
        chassis::BackpressureConfig backpressure;
        chassis::ClusterConfig cluster;
        chassis::K8sDiscoveryConfig k8s_discovery;
        chassis::TelemetryConfig telemetry;
        chassis::LoadBalancingConfig load_balancing;
    };
}
```

---

## 8. API Gateway: What's New

With the chassis extracted, the API Gateway needs these **new** components:

### 8.1 Route Matcher (replaces ART/tokenizer)

```
src/
  route_matcher.hpp         # Path/header/method matching (not token prefix)
  route_table.hpp           # Trie or flat map of route rules
  route_config.hpp          # Route definition schema (YAML)
```

Matching logic: `{method} + {path pattern} + {header predicates}` → `BackendId`.
This is the gateway's hot path. No virtual dispatch, no shared abstraction with Ranvier's ART.

### 8.2 Request/Response Transform Pipeline

```
src/
  transform_pipeline.hpp    # Ordered list of transforms applied to req/resp
  transforms/
    header_transform.hpp    # Add/remove/rewrite headers
    path_transform.hpp      # Path prefix stripping, rewriting
    auth_transform.hpp      # JWT validation, OAuth token exchange
    cors_transform.hpp      # CORS header injection
```

### 8.3 Gateway-Specific HTTP Controller

```
src/
  gateway_controller.hpp    # HTTP handler that uses route_matcher + transforms
  gateway_controller.cpp    # Wires chassis::ConnectionPool + circuit_breaker + rate_limiter
```

This replaces Ranvier's `HttpController`. It uses the **same** `ConnectionPool`, `CircuitBreaker`, and `RateLimiter` from the chassis, but with different routing logic and no tokenization.

### 8.4 Admin/Config API

```
src/
  admin_controller.hpp      # Route CRUD, backend management, health status
  gateway_config.hpp        # GatewayConfig (extends chassis configs)
```

### 8.5 New vs Reused (API Gateway Bill of Materials)

| Component | Source | LOC Estimate |
|-----------|--------|-------------|
| Route Matcher | **New** | ~400 |
| Route Table | **New** | ~300 |
| Transform Pipeline | **New** | ~600 |
| Gateway HTTP Controller | **New** | ~800 |
| Admin API | **New** | ~400 |
| Gateway Config | **New** | ~200 |
| Connection Pool | Chassis (reuse) | 0 |
| Circuit Breaker | Chassis (reuse) | 0 |
| Rate Limiter | Chassis (reuse) | 0 |
| Health Service | Chassis (reuse) | 0 |
| Gossip Cluster | Chassis (reuse) | 0 |
| K8s Discovery | Chassis (reuse) | 0 |
| Async Persistence | Chassis (reuse) | 0 |
| Metrics/Tracing | Chassis (reuse) | 0 |
| Config/Lifecycle | Chassis (reuse) | 0 |
| **Total new code** | | **~2,700** |
| **Reused from chassis** | | **~5,500** |

---

## 9. Implementation Phasing

### Phase 0: Preparatory Refactors in ranvier-core (no new repo yet)

These changes make extraction possible without breaking Ranvier. Each is a single PR.

1. **Template ShardedConfig on config type** — trivial, backward-compatible alias
2. **Extract BackendRegistry interface** — HealthService and K8sDiscoveryService accept interface instead of `RouterService&`
3. **Split config_schema.hpp** — infrastructure configs vs routing/assets configs into separate headers
4. **Split metrics_service.hpp** — generic histogram helpers vs Ranvier-specific counters
5. **Generalize gossip message types** — product message handler registration

Each PR passes existing tests. No behavior change.

### Phase 1: Create seastar-chassis repo

1. Copy extracted files into new repo structure
2. Set up CMake with `FetchContent` for dependencies
3. Build as a static library
4. Ranvier-core switches to depending on seastar-chassis
5. Verify all existing tests still pass

### Phase 2: Build API Gateway

1. New repo: `seastar-gateway`
2. Depends on `seastar-chassis`
3. Implement route matcher, transforms, gateway controller
4. Integration tests with Docker Compose

### Phase 3 (future): KV Store / DNS Server

Each is its own repo, depending on `seastar-chassis`, with purpose-built hot paths.

---

## 10. Risk Assessment

| Risk | Mitigation |
|------|-----------|
| Chassis extraction breaks Ranvier | Phase 0 refactors are backward-compatible. Run full test suite after each PR. |
| Premature generalization | Only generalize what's proven needed by 2+ products. API Gateway is the validation. |
| Chassis becomes a dumping ground | Strict rule: no product-specific code in chassis. Code review gate. |
| Performance regression from indirection | No virtual dispatch on hot paths. BackendRegistry is control-plane only. Template parameters resolve at compile time. |
| Maintenance burden of shared lib | One owner. Semantic versioning. Products pin to specific versions. |
| Gossip protocol backward-compat | Version field in packet header (already exists as protocol_version = 2). New products use version 3+ with extensible message types. |

---

## 11. Lines of Code Estimate

| Component | Current LOC | Chassis LOC | Stays in Ranvier |
|-----------|-------------|-------------|------------------|
| Connection pool | ~600 | ~600 | 0 |
| Circuit breaker | ~300 | ~300 | 0 |
| Rate limiter (core + svc) | ~300 | ~300 | 0 |
| Stream parser | ~400 | ~400 | 0 |
| Health service | ~200 | ~200 (generalized) | 0 |
| Gossip layer | ~2,100 | ~2,100 (generalized msgs) | 0 |
| Persistence layer | ~900 | ~900 (generalized queue) | 0 |
| Config infrastructure | ~600 | ~400 | ~200 (routing/assets) |
| Metrics helpers | ~600 | ~200 | ~400 (Ranvier-specific) |
| Load balancer | ~300 | ~300 | 0 |
| K8s discovery | ~400 | ~400 (generalized) | 0 |
| Tracing | ~300 | ~300 | 0 |
| Utilities | ~300 | ~250 | ~50 |
| **Subtotal** | **~7,300** | **~6,650** | **~650** |
| Router/ART/slab | ~2,500 | 0 | ~2,500 |
| Tokenizer | ~1,200 | 0 | ~1,200 |
| HTTP controller | ~1,500 | 0 | ~1,500 |
| Application/main | ~800 | ~200 (lifecycle) | ~600 |
| **Total** | **~13,300** | **~6,850** | **~6,450** |

The chassis captures **~52%** of the codebase. Each new product starts with 6,850 lines of battle-tested infrastructure.

---

## 12. Decision: When to Start

**Recommended trigger:** Start Phase 0 refactors now (they improve Ranvier's modularity regardless). Start Phase 1 only when you commit to building the API Gateway. Don't extract the chassis speculatively — the API Gateway is the forcing function that validates the extraction boundaries.

If the API Gateway never materializes, the Phase 0 refactors still pay off as better separation of concerns within Ranvier itself.
