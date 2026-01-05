# Ranvier Core - v1.0 Production Release TODO

> **Architectural Gap Analysis**
> Generated: 2025-12-27
> Current State: Alpha (stable ~60ms P99 TTFT in Docker testbed)

This document identifies missing features and optimizations required to promote Ranvier Core from Alpha to a Production-Ready v1.0 release. Items are categorized by domain and prioritized by production impact.

---

## Table of Contents

1. [Core Data Plane](#1-core-data-plane)
2. [Distributed Reliability](#2-distributed-reliability)
3. [Observability](#3-observability)
4. [Infrastructure & Security](#4-infrastructure--security)
5. [Developer Experience](#5-developer-experience)

---

## 1. Core Data Plane

Performance optimizations for the hot path: tokenization, routing, and response streaming.

### 1.1 SIMD Optimization for Radix Tree Lookups

- [ ] **Implement SIMD key search in Node16**
  _Justification:_ Node16 uses linear search over 16 keys. AVX2/SSE4.2 can compare all 16 keys in a single instruction, reducing lookup from O(16) to O(1) comparisons.
  _Location:_ `src/radix_tree.hpp:84-92`
  _Complexity:_ Medium

- [ ] **Add SIMD-accelerated prefix comparison**
  _Justification:_ Path compression stores multi-token prefixes. Using `_mm_cmpeq_epi32` for batch comparison can speed up prefix matching by 4-8x for long prefixes.
  _Location:_ `src/radix_tree.hpp:227-231` (insert), `548-555` (lookup)
  _Complexity:_ Medium

- [ ] **Evaluate memory-mapped tokenizer vocabulary**
  _Justification:_ Current tokenizer loads vocabulary into heap. Memory-mapping enables zero-copy access and reduces cold-start time for large vocabularies (100k+ tokens).
  _Location:_ `src/tokenizer_service.cpp`
  _Complexity:_ Low

### 1.2 Zero-Copy SSE Parsing Refinements

- [ ] **Implement scatter-gather I/O for backend responses**
  _Justification:_ Currently SSE chunks are copied between buffers. Using Seastar's `scattered_message` can eliminate copies in the streaming path.
  _Location:_ `src/stream_parser.cpp`, `src/http_controller.cpp:400+`
  _Complexity:_ High

- [ ] **Add chunk coalescing for small SSE events**
  _Justification:_ Many small `data:` chunks cause syscall overhead. Coalescing into larger TCP segments improves throughput under high concurrency.
  _Location:_ `src/stream_parser.hpp`
  _Complexity:_ Medium

### 1.3 Persistence I/O

- [x] **Decouple SQLite persistence from reactor thread** ✓
  _Justification:_ Synchronous SQLite calls (`save_route`, `save_backend`) in the request path block Seastar's reactor thread, causing multi-millisecond stalls that affect all concurrent requests.
  _Approach:_ Implement `AsyncPersistenceManager` with fire-and-forget queue API. Background timer drains queue and writes to SQLite in batches via `seastar::async`. Semaphore serializes batches; gate tracks in-flight operations for clean shutdown.
  _Location:_ `src/async_persistence.hpp`, `src/async_persistence.cpp`, `src/http_controller.cpp`
  _Complexity:_ Medium

### 1.4 Memory Efficiency

- [x] **Replace `shared_ptr` with `unique_ptr` in RadixTree** ✓
  _Justification:_ `std::shared_ptr` uses atomic reference counting (`lock xadd` on x86), adding ~20 cycles per copy/destroy. Seastar's shared-nothing model means nodes are never shared across threads, making `unique_ptr` the correct ownership model.
  _Approach:_ Replaced all `std::shared_ptr<Node>` with `std::unique_ptr<Node>`. Added `extract_child()` for ownership transfer during mutations. `find_child()` returns raw `Node*` for traversal. Converted recursive lookup to iterative for additional performance gains.
  _Location:_ `src/radix_tree.hpp`
  _Complexity:_ Medium

- [ ] **Implement node pooling for Radix Tree allocations**
  _Justification:_ Frequent `std::unique_ptr<Node>` allocations fragment the heap. A slab allocator per shard reduces allocation overhead and improves cache locality.
  _Location:_ `src/radix_tree.hpp`
  _Complexity:_ High

- [ ] **Add memory usage metrics per Radix Tree**
  _Justification:_ No visibility into per-shard memory consumption. Required for capacity planning and debugging memory leaks.
  _Location:_ `src/radix_tree.hpp`, `src/metrics_service.hpp`
  _Complexity:_ Low

---

## 2. Distributed Reliability

Hardening the gossip protocol and cluster coordination for production multi-node deployments.

### 2.1 Network Partition Handling

- [x] **Implement split-brain detection** ✓
  _Justification:_ Current gossip uses timeout-based failure detection only. Nodes cannot distinguish between peer failure and network partition, risking divergent state.
  _Approach:_ Add quorum-aware health checks; require N/2+1 peers visible before accepting new routes.
  _Location:_ `src/gossip_service.cpp:375-403`
  _Complexity:_ High

- [ ] **Add partition healing with route reconciliation**
  _Justification:_ After partition heals, nodes have divergent route tables. Need incremental sync protocol to merge without full state transfer.
  _Location:_ `src/gossip_service.cpp`
  _Complexity:_ High

- [ ] **Implement protocol version negotiation**
  _Justification:_ Rolling upgrades require backward compatibility. Gossip packets have `version` field but no negotiation or feature flags.
  _Location:_ `src/gossip_service.hpp:54`
  _Complexity:_ Medium

### 2.2 Dynamic Cluster Re-balancing

- [ ] **Add weighted route distribution across backends**
  _Justification:_ All routes treated equally. GPUs with more VRAM or faster interconnect should receive proportionally more prefixes.
  _Location:_ `src/router_service.cpp:189-246`
  _Complexity:_ Medium

- [ ] **Implement route migration on backend scale-down**
  _Justification:_ When a backend is removed, its routes are orphaned. Proactive migration to healthy backends preserves cache locality.
  _Location:_ `src/router_service.cpp`, `src/gossip_service.cpp`
  _Complexity:_ High

- [ ] **Add hot-spot detection and load shedding**
  _Justification:_ Popular prefixes can overwhelm a single backend. Detect hot prefixes and replicate routes to multiple backends.
  _Location:_ `src/radix_tree.hpp`, `src/router_service.cpp`
  _Complexity:_ High

### 2.3 Gossip Protocol Reliability

- [x] **Add reliable delivery with acknowledgments** ✓
  _Justification:_ Current UDP gossip is fire-and-forget. Critical route updates can be lost. Add lightweight ACK/retry for route announcements.
  _Location:_ `src/gossip_service.cpp:217-262`
  _Complexity:_ Medium

- [x] **Implement duplicate suppression** ✓
  _Justification:_ Same route may be announced multiple times on retransmit. Add sequence numbers to deduplicate.
  _Location:_ `src/gossip_service.hpp`
  _Complexity:_ Low

- [ ] **Add anti-entropy protocol for periodic state sync**
  _Justification:_ Gossip only propagates new routes. Nodes that missed announcements have no catch-up mechanism. Periodic Merkle tree comparison ensures convergence.
  _Location:_ `src/gossip_service.cpp`
  _Complexity:_ High

- [x] **Batch remote route updates to prevent SMP storm** ✓
  _Justification:_ When GossipService on Shard 0 learns routes from remote peers, immediately broadcasting each route to all shards via `smp::submit_to` generates O(routes × shards) cross-core messages. With 1000 routes/sec and 64 shards, this creates 64,000 SMP messages/sec, risking Seastar's internal message bus saturation.
  _Approach:_ Buffer incoming routes on Shard 0, flush batches via timer (10ms) or when buffer reaches 100 routes. Single message per shard contains entire batch. Reduces SMP traffic by 99% (64,000 → 640 messages/sec).
  _Location:_ `src/router_service.cpp:457-543`, `src/router_service.hpp:19-38`
  _Complexity:_ Medium

- [x] **Prevent reactor stalls in DTLS crypto operations** ✓
  _Justification:_ OpenSSL encryption/decryption blocks Seastar's reactor thread. With 50+ peers, sequential crypto operations cause multi-millisecond stalls affecting all network I/O.
  _Approach:_ Adaptive offloading based on packet size (>1KB) and peer count (>10). Use `seastar::async` for large packets, `seastar::thread` with batching for high fan-out broadcasts. Add timing watchdog with 100μs threshold.
  _Location:_ `src/gossip_service.cpp:1074-1176` (send_encrypted), `src/gossip_service.cpp:1269-1357` (broadcast_encrypted)
  _Complexity:_ Medium

---

## 3. Observability

Metrics, tracing, and logging improvements for production monitoring and debugging.

### 3.1 Prometheus Metrics Enhancements

- [x] **Add cache hit/miss ratio gauge** ✓
  _Justification:_ Counters exist but no pre-computed ratio. Operators need instant visibility into routing efficiency.
  _Metric:_ `ranvier_cache_hit_ratio` (gauge)
  _Location:_ `src/metrics_service.hpp`
  _Complexity:_ Low

- [x] **Add per-backend latency histograms** ✓
  _Justification:_ Current histograms are global. Need per-backend breakdown to identify slow GPUs.
  _Metric:_ `ranvier_backend_latency_seconds{backend_id="..."}`
  _Location:_ `src/http_controller.cpp`, `src/metrics_service.hpp`
  _Complexity:_ Low

- [ ] **Add route table size and memory metrics**
  _Justification:_ No visibility into route table growth. Critical for capacity planning.
  _Metrics:_ `ranvier_routes_total`, `ranvier_radix_tree_bytes`
  _Location:_ `src/radix_tree.hpp`, `src/metrics_service.hpp`
  _Complexity:_ Low

- [ ] **Add cluster health metrics**
  _Justification:_ Need visibility into gossip peer health, message rates, and sync lag.
  _Metrics:_ `ranvier_cluster_peers_alive`, `ranvier_gossip_lag_seconds`
  _Location:_ `src/gossip_service.cpp`, `src/metrics_service.hpp`
  _Complexity:_ Low

### 3.2 Distributed Tracing (OpenTelemetry)

- [x] **Integrate OpenTelemetry SDK** ✓
  _Justification:_ Current request IDs are correlation tokens only. No span propagation or distributed trace visualization.
  _Approach:_ Add OTLP exporter, propagate W3C trace context headers.
  _Location:_ `src/http_controller.cpp`, `src/logging.hpp`
  _Complexity:_ Medium

- [x] **Add spans for critical operations** ✓
  _Justification:_ Need visibility into time spent in tokenization, routing, backend selection, and response streaming.
  _Spans:_ `tokenize`, `route_lookup`, `backend_connect`, `stream_response`
  _Location:_ `src/http_controller.cpp`, `src/router_service.cpp`
  _Complexity:_ Medium

- [x] **Propagate trace context to backends** ✓
  _Justification:_ End-to-end tracing requires context propagation to vLLM. Add `traceparent` header forwarding.
  _Location:_ `src/http_controller.cpp:350+`
  _Complexity:_ Low

### 3.3 Structured Logging Improvements

- [ ] **Add structured JSON logging option**
  _Justification:_ Plain text logs are difficult to parse. JSON logs enable integration with ELK/Splunk/Loki.
  _Location:_ `src/main.cpp`, `src/config.hpp`
  _Complexity:_ Low

- [ ] **Add audit logging for admin operations**
  _Justification:_ No record of who registered/removed backends. Required for security compliance.
  _Events:_ `backend_registered`, `backend_removed`, `route_cleared`, `config_reloaded`
  _Location:_ `src/http_controller.cpp`
  _Complexity:_ Low

- [ ] **Implement log sampling for high-volume events**
  _Justification:_ Debug logs at high QPS can overwhelm storage. Add configurable sampling rate.
  _Location:_ `src/logging.hpp`, `src/config.hpp`
  _Complexity:_ Low

---

## 4. Infrastructure & Security

Hardening for production deployment environments.

### 4.1 Container Security

- [x] **Run container as non-root user** ✓
  _Justification:_ Current Dockerfile runs as root (default). Container escape vulnerabilities grant full host access.
  _Fix:_ Add `USER ranvier` directive and adjust file permissions.
  _Location:_ `Dockerfile.production:42+`
  _Complexity:_ Low
  _Priority:_ **Critical**

- [ ] **Add read-only root filesystem**
  _Justification:_ Reduces attack surface. Mutable data should be in explicit volume mounts.
  _Fix:_ Add `--read-only` flag compatibility, use `/tmp` for scratch.
  _Location:_ `Dockerfile.production`, `docker-compose.test.yml`
  _Complexity:_ Low

- [ ] **Drop unnecessary Linux capabilities**
  _Justification:_ Container runs with full capability set. Drop all except `NET_BIND_SERVICE` (if using privileged ports).
  _Fix:_ Add `--cap-drop=ALL` to compose/runtime.
  _Location:_ `docker-compose.test.yml`
  _Complexity:_ Low

- [ ] **Add seccomp profile**
  _Justification:_ Restrict syscalls to those required by Seastar. Blocks exploitation of kernel vulnerabilities.
  _Location:_ Create `seccomp-profile.json`
  _Complexity:_ Medium

### 4.2 Transport Security

- [x] **Implement mTLS between Ranvier nodes** ✓
  _Justification:_ Gossip protocol uses plaintext UDP. Enables route table poisoning and cluster hijacking.
  _Approach:_ Add DTLS encryption for gossip channel.
  _Location:_ `src/gossip_service.cpp`
  _Complexity:_ High
  _Priority:_ **Critical**

- [ ] **Add mTLS for backend connections**
  _Justification:_ Backend connections are unencrypted. Sensitive prompt data exposed on network.
  _Location:_ `src/http_controller.cpp`, `src/connection_pool.hpp`
  _Complexity:_ Medium

- [ ] **Implement certificate rotation**
  _Justification:_ Static certificates require restart to rotate. Add file watcher for automatic reload.
  _Location:_ `src/main.cpp`, `src/config.hpp`
  _Complexity:_ Medium

### 4.3 Authentication & Authorization

- [x] **Implement API key rotation mechanism** ✓
  _Justification:_ Single static API key with no rotation. Compromised key requires restart to change.
  _Approach:_ Support multiple keys with validity periods; add `/admin/keys` endpoint.
  _Location:_ `src/config.hpp`, `src/http_controller.cpp`
  _Complexity:_ Medium

- [ ] **Add role-based access control (RBAC)**
  _Justification:_ Single admin key grants all permissions. Need separation between "read metrics" and "modify routes".
  _Roles:_ `admin`, `operator`, `viewer`
  _Location:_ `src/http_controller.cpp`
  _Complexity:_ Medium

- [ ] **Integrate external secrets management**
  _Justification:_ API keys stored in plaintext config. Support Kubernetes Secrets, HashiCorp Vault, or AWS Secrets Manager.
  _Location:_ `src/config.hpp`
  _Complexity:_ Medium

### 4.4 Rate Limiting & DoS Protection

- [ ] **Add request body size limits**
  _Justification:_ No maximum request size. Large payloads can exhaust memory.
  _Config:_ `max_request_body_bytes` (default: 10MB)
  _Location:_ `src/http_controller.cpp`, `src/config.hpp`
  _Complexity:_ Low

- [ ] **Implement per-API-key rate limiting**
  _Justification:_ Current rate limiting is per-IP only. Shared infrastructure (NAT) causes false positives.
  _Location:_ `src/rate_limiter.hpp`, `src/http_controller.cpp`
  _Complexity:_ Medium

- [ ] **Add connection limits per client**
  _Justification:_ Single client can exhaust connection pool. Add `max_connections_per_client` config.
  _Location:_ `src/connection_pool.hpp`, `src/config.hpp`
  _Complexity:_ Low

- [ ] **Protect metrics endpoint**
  _Justification:_ Prometheus endpoint (port 9180) is unauthenticated. Exposes internal state.
  _Approach:_ Add optional bearer token or IP allowlist.
  _Location:_ `src/metrics_service.hpp`, `src/config.hpp`
  _Complexity:_ Low

---

## 5. Developer Experience

Tooling, testing, and documentation improvements for contributors and operators.

### 5.1 CI/CD Pipeline

- [ ] **Add automated benchmark regression testing**
  _Justification:_ Performance regressions detected manually. Add CI job that fails if P99 latency increases >10%.
  _Approach:_ Run Locust benchmark in CI, compare against baseline.
  _Location:_ `.github/workflows/`, `tests/integration/`
  _Complexity:_ Medium

- [ ] **Add fuzzing for gossip protocol parser**
  _Justification:_ Gossip deserialization handles untrusted input. Fuzzing detects buffer overflows and parsing bugs.
  _Tool:_ libFuzzer or AFL++
  _Location:_ `tests/fuzz/`
  _Complexity:_ Medium

- [ ] **Add SAST (Static Application Security Testing)**
  _Justification:_ Automated detection of security vulnerabilities in CI.
  _Tools:_ CodeQL, Semgrep, or Clang Static Analyzer
  _Location:_ `.github/workflows/`
  _Complexity:_ Low

- [ ] **Add dependency vulnerability scanning**
  _Justification:_ Third-party dependencies may have CVEs. Automate detection.
  _Tools:_ Dependabot, Trivy, or Snyk
  _Location:_ `.github/workflows/`
  _Complexity:_ Low

### 5.2 Testing Infrastructure

- [ ] **Add chaos testing for cluster scenarios**
  _Justification:_ Integration tests use clean network. Need to test packet loss, latency, and node failures.
  _Tools:_ Toxiproxy, tc (traffic control), or Chaos Mesh
  _Location:_ `tests/integration/`
  _Complexity:_ Medium

- [ ] **Increase unit test coverage to 80%+**
  _Justification:_ Current coverage unknown. Many edge cases untested.
  _Focus:_ Error paths in gossip, connection pool edge cases, config validation
  _Location:_ `tests/unit/`
  _Complexity:_ Medium

- [ ] **Add property-based testing for Radix Tree**
  _Justification:_ Current tests use fixed inputs. Property-based testing finds edge cases automatically.
  _Tool:_ RapidCheck (C++) or custom generators
  _Location:_ `tests/unit/radix_tree_test.cpp`
  _Complexity:_ Medium

### 5.3 Client SDKs & Documentation

- [x] **Generate OpenAPI specification** ✓
  _Justification:_ API documented manually. OpenAPI enables auto-generated clients and validation.
  _Output:_ `docs/openapi.yaml`
  _Complexity:_ Low

- [ ] **Add Python admin SDK**
  _Justification:_ Operators script backend registration. SDK simplifies integration.
  _Features:_ Backend CRUD, route inspection, metrics fetching
  _Location:_ `sdk/python/`
  _Complexity:_ Medium

- [x] **Create Helm chart for Kubernetes deployment** ✓
  _Justification:_ K8s deployment requires manual YAML authoring. Helm chart enables one-command deployment.
  _Location:_ `deploy/helm/ranvier/`
  _Complexity:_ Medium

- [ ] **Add runbook for common operational tasks**
  _Justification:_ No troubleshooting guide. Operators need documentation for: scaling, debugging, disaster recovery.
  _Location:_ `docs/runbook.md`
  _Complexity:_ Low

### 5.4 Application Bootstrap

- [x] **Refactor initialization into Application class** ✓
  _Justification:_ All service initialization and shutdown logic was inline in `main.cpp`, making it difficult to test, maintain, and understand the startup/shutdown sequence. The Application class centralizes service lifecycle management.
  _Approach:_ Created `Application` class (`src/application.hpp/cpp`) that:
  - Owns all `seastar::sharded` service instances as private members
  - Implements `startup()` with correct service initialization order
  - Implements `shutdown()` with reverse-order service termination
  - Uses `seastar::gate` to ensure startup completes before shutdown
  - Handles graceful draining of in-flight requests
  - Supports configuration hot-reload via SIGHUP
  _Location:_ `src/application.hpp`, `src/application.cpp`, `src/main.cpp`
  _Complexity:_ Medium

### 5.5 Build System

- [ ] **Add Windows/macOS cross-compilation support**
  _Justification:_ Contributors on non-Linux need Docker for development. Native builds improve DX.
  _Note:_ Seastar is Linux-only; may require abstraction layer.
  _Location:_ `CMakeLists.txt`
  _Complexity:_ High

- [x] **Add pre-built Docker images to container registry** ✓
  _Justification:_ Users must build from source. Publish to Docker Hub/GHCR for easy deployment.
  _Location:_ `.github/workflows/`, `Dockerfile.production`
  _Complexity:_ Low

- [x] **Add development container (devcontainer)** ✓
  _Justification:_ Complex build dependencies. Devcontainer provides reproducible environment.
  _Location:_ `.devcontainer/`
  _Complexity:_ Low

---

## Priority Matrix

| Priority | Category | Item | Effort | Status |
|----------|----------|------|--------|--------|
| **P0 - Critical** | Security | Non-root container execution | Low | ✅ Done |
| **P0 - Critical** | Security | mTLS for gossip protocol | High | ✅ Done |
| **P1 - High** | Reliability | Split-brain detection | High | ✅ Done |
| **P1 - High** | Reliability | Reliable gossip delivery | Medium | ✅ Done |
| **P1 - High** | Security | API key rotation | Medium | ✅ Done |
| **P1 - High** | Observability | OpenTelemetry integration | Medium | ✅ Done |
| **P1 - High** | Performance | Async persistence (reactor stall fix) | Medium | ✅ Done |
| **P1 - High** | Performance | Batch remote route updates (SMP storm fix) | Medium | ✅ Done |
| **P1 - High** | Performance | Replace shared_ptr with unique_ptr in RadixTree | Medium | ✅ Done |
| **P2 - Medium** | Performance | SIMD Node16 optimization | Medium | |
| **P2 - Medium** | Performance | Node pooling for Radix Tree | High | |
| **P2 - Medium** | DX | Benchmark regression CI | Medium | |
| **P2 - Medium** | DX | Helm chart | Medium | ✅ Done |
| **P3 - Low** | Performance | Memory-mapped tokenizer | Low | |
| **P3 - Low** | DX | OpenAPI specification | Low | ✅ Done |
| **P3 - Low** | DX | Pre-built Docker images | Low | ✅ Done |
| **P2 - Medium** | DX | Application bootstrap refactoring | Medium | ✅ Done |

---

## Completed Items

_Move completed items here with completion date and PR reference._

| Date | Item | PR |
|------|------|----|
| 2026-01-05 | Refactor initialization into Application class (service lifecycle management) | - |
| 2026-01-05 | Replace shared_ptr with unique_ptr in RadixTree (Seastar optimization) | - |
| 2026-01-05 | Batch remote route updates to prevent SMP storm (99% message reduction) | - |
| 2026-01-05 | Decouple SQLite persistence from reactor thread (AsyncPersistenceManager) | - |
| 2026-01-05 | Prevent reactor stalls in DTLS crypto operations (adaptive offloading) | - |
| 2025-01-04 | Implement split-brain detection with quorum-aware health checks | - |
| 2025-01-04 | Add development container (devcontainer) | - |
| 2025-01-04 | Generate OpenAPI 3.0 specification for HTTP API | - |
| 2025-01-04 | Implement mTLS/DTLS encryption for gossip protocol (P0 security) | - |
| 2025-01-04 | Add pre-built Docker images to GHCR | - |
| 2025-01-03 | Create Helm chart for Kubernetes deployment | - |
| 2025-01-03 | API key rotation mechanism with expiry and hot-reload | - |
| 2025-01-03 | Add cache hit/miss ratio gauge (Prometheus) | - |
| 2025-01-03 | Add per-backend latency histograms (Prometheus) | - |
| 2025-01-02 | Add reliable delivery with acknowledgments (gossip protocol) | - |
| 2025-01-02 | Implement duplicate suppression (gossip protocol) | - |
| 2025-01-01 | Fix: Seastar output_stream assertion failure under load | - |
| 2025-01-01 | Fix: Persistence state corruption after crash | - |
| 2024-12-XX | Accept pre-tokenized client input | #50 |
| 2024-12-XX | Locust load testing infrastructure | #49 |
| 2024-12-XX | Abseil high-performance containers | #48 |
| 2024-12-XX | Multi-node integration tests | #47 |
| 2024-12-XX | Request rewriting for token forwarding | #46 |

---

## References

- [Ranvier Architecture](./architecture.md)
- [API Reference](./api-reference.md)
- [Request Flow Diagrams](./request-flow.md)
- [Integration Test Guide](../tests/integration/README.md)
