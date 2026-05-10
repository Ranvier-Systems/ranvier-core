# Ranvier Core - v1.0 Production Release TODO

> **Architectural Gap Analysis**
> Generated: 2025-12-27 | Reorganized: 2026-03-22
> Current State: Alpha (stable ~60ms P99 TTFT in Docker testbed)

This document tracks **open** backlog items for Ranvier Core v1.0.
Completed items have been archived in [BACKLOG-ARCHIVE.md](BACKLOG-ARCHIVE.md).

---

## Table of Contents

1. [Core Data Plane](#1-core-data-plane)
2. [Distributed Reliability](#2-distributed-reliability)
3. [Observability](#3-observability)
4. [Infrastructure & Security](#4-infrastructure-security)
5. [Developer Experience](#5-developer-experience)
6. [Integration Tests (End-to-End Validation)](#6-integration-tests-end-to-end-validation)
7. [Strategic Assessment (2026-01-31)](#7-strategic-assessment-2026-01-31)
8. [Benchmark Extensions](#8-benchmark-extensions)
9. [Load-Aware Prefix Routing](#9-load-aware-prefix-routing)
10. [HTTP Controller Review (2026-02-14)](#10-http-controller-review-2026-02-14)
11. [Hot-Path Performance Audit (2026-02-15)](#11-hot-path-performance-audit-2026-02-15)
12. [Request Lifecycle Performance Analysis (2026-02-20)](#12-request-lifecycle-performance-analysis-2026-02-20)
13. [Code Modularity (Low Priority)](#13-code-modularity-low-priority)
14. [Shard 0 Role Isolation Analysis (2026-03-06)](#14-shard-0-role-isolation-analysis-2026-03-06)
15. [Intelligence Layer Roadmap (2026-03-25)](#15-intelligence-layer-roadmap-2026-03-25)
16. [KV-Cache Compression-Aware Routing (2026-04-05)](#16-kv-cache-compression-aware-routing-2026-04-05)
17. [Hard Rules Audit Follow-ups (2026-05-05)](#17-hard-rules-audit-follow-ups-2026-05-05)
18. [Request Lifecycle Crash-Risk Audit Follow-ups (2026-05-08)](#18-request-lifecycle-crash-risk-audit-follow-ups-2026-05-08)

---

## 1. Core Data Plane
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

- [ ] **Partial tokenization for routing decisions**
  _Justification:_ Tokenization accounts for ~10.6ms of ~10.62ms total routing decision time (99.9% of overhead). The ART lookup itself is <0.01ms. Currently the full prompt is tokenized, but routing only needs enough tokens to match against the prefix tree depth. Truncating the input to a byte budget before tokenizing could significantly reduce per-request tokenization cost.
  _Nuance:_ The system supports rewriting with tokenized output depending on the endpoint, so partial tokenization must not interfere with downstream token reuse. May require a two-phase approach: partial tokenization for routing, full tokenization deferred to the forwarding path only when needed.
  _Current mitigation:_ Tokenization is offloaded to a dedicated thread pool (not blocking the reactor), so the 10.6ms wall-clock time does not stall the event loop. Real overhead is thread pool queue contention + context switch, much less than 10ms.
  _Benchmark evidence:_ 30m run (b63c165, 2026-02-28) — CodeLlama-13b, 20 users, 8 GPUs. Routing decision P50: 10.62ms (tokenization: 10.61ms, ART: 0.01ms). Despite this overhead, P99 TTFT improved 78.2% (4500ms → 980ms) and cache hit rate improved from 12.5% to 73.9%.
  _Location:_ `src/tokenizer_service.hpp`, `src/tokenizer_service.cpp`, `src/http_controller.cpp`
  _Complexity:_ Medium
  _Priority:_ P3 — Optimization. Not urgent given thread pool offloading and dominant TTFT improvement.

### 1.2 Zero-Copy SSE Parsing Refinements

- [ ] **Implement scatter-gather I/O for backend responses**
  _Justification:_ Currently SSE chunks are copied between buffers. Using Seastar's `scattered_message` can eliminate copies in the streaming path.
  _Location:_ `src/stream_parser.cpp`, `src/http_controller.cpp:400+`
  _Complexity:_ High

- [ ] **Add chunk coalescing for small SSE events**
  _Justification:_ Many small `data:` chunks cause syscall overhead. Coalescing into larger TCP segments improves throughput under high concurrency.
  _Location:_ `src/stream_parser.hpp`
  _Complexity:_ Medium

### 1.4 Memory Efficiency

- [ ] **Remove unnecessary atomics from ShardLoadMetrics**
  _Justification:_ `ShardLoadMetrics` uses `std::atomic<uint64_t>` for `_active_requests`, `_queued_requests`, and `_total_requests`, but since each shard has its own thread-local instance (`thread_local std::unique_ptr<ShardLoadMetrics>`), atomic operations are unnecessary overhead. With Seastar's shared-nothing model, regular `uint64_t` would suffice since there's no cross-thread access to the same instance.
  _Approach:_ Replace `std::atomic<uint64_t>` with `uint64_t` for all metrics counters. Update accessor methods to remove memory ordering parameters.
  _Location:_ `src/shard_load_metrics.hpp:132-134`
  _Complexity:_ Low

- [ ] **Batch CryptoOffloader statistics updates**
  _Justification:_ `CryptoOffloader` increments multiple atomic counters (`_total_ops`, `_inline_ops`, `_offloaded_ops`, etc.) on every crypto operation. While these are lightweight (relaxed memory order), they add overhead in high-throughput scenarios. More concerning is `_queue_depth` which uses `fetch_add`/`fetch_sub` for every offloaded operation.
  _Approach:_ Use per-operation local counters that batch into atomics periodically (e.g., every 100 ops or via timer). Consider non-atomic counters for same-shard-only statistics, exposing them via snapshot functions.
  _Location:_ `src/crypto_offloader.hpp:181-188`
  _Complexity:_ Medium

- [ ] **Audit codebase for abseil container opportunities**
  _Justification:_ The codebase uses `std::unordered_map` and `std::vector` in several places where abseil alternatives (`absl::flat_hash_map`, `absl::InlinedVector`) would provide better performance. Abseil is already a dependency (used in RadixTree and TokenizationCache).
  _Candidates:_
  - `absl::flat_hash_map`: Replace `std::unordered_map` for better cache locality and ~20-40% faster lookups. Already done for TokenizationCache.
  - `absl::InlinedVector<T, N>`: Replace `std::vector<T>` for small, bounded collections to avoid heap allocation. Good for: token vectors in cache entries (N=64), small config lists, temporary buffers.
  - `absl::flat_hash_set`: Replace `std::unordered_set` where used.
  _Files to audit:_
  - `src/circuit_breaker.hpp` - `_circuits` map
  - `src/rate_limiter.hpp` - `_buckets` map
  - `src/connection_pool.hpp` - `_pools` map
  - `src/gossip_service.cpp` - various peer tracking maps
  - `src/router_service.cpp` - pending routes vectors
  _Note:_ While individual gains are small (microseconds), cumulative effect across hot paths may be measurable. Low complexity since abseil is already linked.
  _Location:_ Multiple files (see candidates above)
  _Complexity:_ Low


---

## 2. Distributed Reliability
### 2.1 Network Partition Handling

- [ ] **Push-based cache eviction notifications from GPU backends**
  _Justification:_ Ranvier infers backend KV cache state from routing history, creating a staleness window of minutes-to-hours. With 1M-token contexts, backends evict more aggressively, causing misrouted requests and wasted prefill. Push notifications cut staleness to ~25ms.
  _Design doc:_ [`docs/architecture/push-cache-eviction-notifications.md`](docs/architecture/push-cache-eviction-notifications.md)
  _Approach:_ HTTP callback endpoint (`POST /v1/cache/events`) + `X-Ranvier-Prefix-Hash` header echoing. Optional sidecar for engines that can't implement directly. New gossip packet type for cluster propagation.
  _Complexity:_ High (4 phases: MVP, cluster propagation, load events + sidecar, upstream engagement)

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

- [ ] **Add anti-entropy protocol for periodic state sync**
  _Justification:_ Gossip only propagates new routes. Nodes that missed announcements have no catch-up mechanism. Periodic Merkle tree comparison ensures convergence.
  _Location:_ `src/gossip_service.cpp`
  _Complexity:_ High

---


---

## 3. Observability
### 3.1 Prometheus Metrics Enhancements

- [ ] **Add cluster health metrics**
  _Justification:_ Need visibility into gossip peer health, message rates, and sync lag.
  _Metrics:_ `ranvier_cluster_peers_alive`, `ranvier_gossip_lag_seconds`
  _Location:_ `src/gossip_service.cpp`, `src/metrics_service.hpp`
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


---

## 4. Infrastructure & Security
### 4.1 Container Security

- [ ] **Add seccomp profile**
  _Justification:_ Restrict syscalls to those required by Seastar. Blocks exploitation of kernel vulnerabilities.
  _Location:_ Create `seccomp-profile.json`
  _Complexity:_ Medium

### 4.2 Transport Security

- [ ] **Add mTLS for backend connections**
  _Justification:_ Backend connections are unencrypted. Sensitive prompt data exposed on network.
  _Location:_ `src/http_controller.cpp`, `src/connection_pool.hpp`
  _Complexity:_ Medium

- [ ] **Implement certificate rotation**
  _Justification:_ Static certificates require restart to rotate. Add file watcher for automatic reload.
  _Location:_ `src/main.cpp`, `src/config.hpp`
  _Complexity:_ Medium

### 4.3 Authentication & Authorization

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

- [ ] **Implement per-API-key rate limiting**
  _Justification:_ Current rate limiting is per-IP only. Shared infrastructure (NAT) causes false positives.
  _Location:_ `src/rate_limiter.hpp`, `src/http_controller.cpp`
  _Complexity:_ Medium

- [ ] **Add connection limits per client**
  _Justification:_ Single client can exhaust connection pool. Add `max_connections_per_client` config.
  _Location:_ `src/connection_pool.hpp`, `src/config.hpp`
  _Complexity:_ Low

---


---

## 5. Developer Experience
### 5.1 CI/CD Pipeline

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

- [ ] **Add runbook for common operational tasks**
  _Justification:_ No troubleshooting guide. Operators need documentation for: scaling, debugging, disaster recovery.
  _Location:_ `docs/runbook.md`
  _Complexity:_ Low

### 5.5 rvctl CLI Enhancements

The `rvctl` CLI tool (tools/rvctl) provides operator-friendly access to Ranvier's Admin API. Several endpoints and quality-of-life features are not yet exposed.

- [ ] **Refactor rvctl into package structure when >4000 lines**
  _Justification:_ Currently ~3300 lines as single file for easy deployment. If significant features are added, refactoring improves maintainability. Current section comments provide navigation.
  _Threshold:_ >4000 lines or >50 functions
  _Structure:_ `rvctl_lib/{cli.py, client.py, config.py, commands/, completions/}`
  _Location:_ `tools/rvctl`
  _Complexity:_ Medium
  _Priority:_ Low (defer until threshold reached)

- [ ] **Add unit tests for rvctl command functions**
  _Justification:_ CLI commands lack automated testing. Mock-based tests would catch regressions.
  _Location:_ `tests/unit/test_rvctl.py` (new)
  _Complexity:_ Medium
  _Priority:_ Low

- [ ] **Add `rvctl doctor` command for connectivity troubleshooting**
  _Justification:_ Operators need quick diagnosis of connection issues. Command would check: API reachability, auth validity, metrics endpoint, DNS resolution.
  _Usage:_ `rvctl doctor`
  _Location:_ `tools/rvctl`
  _Complexity:_ Low
  _Priority:_ Low

### 5.6 Build System

- [ ] **Add Windows/macOS cross-compilation support**
  _Justification:_ Contributors on non-Linux need Docker for development. Native builds improve DX.
  _Note:_ Seastar is Linux-only; may require abstraction layer.
  _Location:_ `CMakeLists.txt`
  _Complexity:_ High

---


---

## 6. Integration Tests (End-to-End Validation)
### 6.1 Test Infrastructure Setup

- [x] **Create shared test fixtures** ✅
  _Description:_ Created `tests/integration/conftest.py` with a `ranvier_cluster` session fixture, a `cluster_metrics` snapshot fixture, a `ClusterTestCase` unittest base class (sharing the same `_bring_up_cluster` / `_tear_down_cluster` helpers), and the deduplicated docker-compose / metric / request helpers. `test_cluster.py` is migrated to the new harness; `test_prefix_routing.py`, `test_load_aware_routing.py`, `test_negative_paths.py`, and `test_graceful_shutdown.py` are pending follow-up PRs. Landed alongside smoke tests in `test_intelligence_layer.py` (§15 1.2 / 1.4 + v2.1.0 partial tokenization).
  _Files:_ `tests/integration/conftest.py`, `tests/integration/test_cluster.py`, `tests/integration/test_intelligence_layer.py`
  _Complexity:_ Low

- [x] **Enhance mock backend capabilities** ✅
  _Description:_ Added per-chunk latency injection (`MOCK_LATENCY_MS` env, `POST /admin/latency?ms=N`, `X-Mock-Latency-Ms` header), sticky failure-mode simulation (`POST /admin/failure-mode?mode=none|status_500|status_503|timeout|reset`, `X-Mock-Failure-Mode` header — `reset` flushes one partial SSE chunk then `shutdown(SHUT_RDWR)`+`close()` for truncation tests; `timeout` blocks 60s without writing), a bounded (cap 200) ring-buffer request log via `GET /debug/requests` + `DELETE /debug/requests`, and prefix-echo mode (`MOCK_PREFIX_ECHO=1` env, `X-Mock-Prefix-Echo: 1` header) where the first SSE chunk's `delta.content` is the first 32 chars of the last user message. All knobs default off; existing `Response from backend N` happy-path text is unchanged.
  _Files:_ `tests/integration/mock_backend.py`
  _Complexity:_ Medium

- [x] **Add Docker Compose test profiles** ✅
  _Description:_ Added three profiles to `docker-compose.test.yml`: the default profile now covers only backend1 + backend2 + ranvier1 (single-node fast path); `full` adds ranvier2/ranvier3 to restore the historical 3-node topology; and `fault-injection` adds a new `backend-unhealthy` service (static IP 172.28.1.12, host port 21436, `BACKEND_ID=unhealthy`, `MOCK_FAILURE_MODE=status_503`) used by future §6.4 circuit-breaker tests to register an always-failing backend. Configurable backend response modes are already provided by the enhanced `mock_backend.py` (`MOCK_LATENCY_MS`, `MOCK_FAILURE_MODE`, `MOCK_PREFIX_ECHO` env vars + `/admin/*` endpoints + `X-Mock-*` headers); the compose file now documents these knobs in a comment block above backend1. `tests/integration/conftest.py::run_compose` passes `--profile full` by default (overridable via `RANVIER_COMPOSE_PROFILE`), so every existing `ClusterTestCase` keeps seeing the 3-node cluster. `make integration-up` activates `--profile full`; `make integration-down` tears down both `full` and `fault-injection` profiles.
  _Files:_ `docker-compose.test.yml`, `tests/integration/conftest.py`
  _Complexity:_ Low

- [x] **Add Makefile test targets** ✅
  _Description:_ Split the monolithic `test-integration` target: `test-integration-full` (new) runs all 9 multi-node suites (the historical behavior); `test-integration-fast` (new) runs only the three single-node-capable suites (`test_http_pipeline`, `test_streaming`, `test_metrics`), skipping the slow cluster/prefix/load-aware/negative-paths/graceful-shutdown/intelligence-layer suites for a faster inner loop; `test-integration` is now an alias for `test-integration-full` so existing CI scripts keep working. `test-integration-ci` already covers all 9 suites via a single pytest invocation with JUnit XML output and was left unchanged. Updated `help` target to advertise the new targets. NOTE: `test-integration-fast` still uses multi-node compose under the hood because each `ClusterTestCase` drives its own `_bring_up_cluster`; the speed win comes from running fewer suites. Truly single-node isolation (skipping ranvier2/ranvier3) is a follow-up once each suite is audited for single-node safety and can set `RANVIER_COMPOSE_PROFILE=""`.
  _Files:_ `Makefile`
  _Complexity:_ Low

### 6.2 HTTP Request Pipeline Tests

- [x] **Create HTTP pipeline test suite** ✅
  _Description:_ Created `tests/integration/test_http_pipeline.py` with `HttpPipelineTest` (tests 01–07, 10), `HttpPipelineNoBackendTest` (test 08), and `HttpPipelineTokenForwardingTest` (test 09) — three `ClusterTestCase` subclasses covering SSE streaming validation, X-Request-ID forwarding via `/debug/requests`, Content-Type enforcement, malformed-JSON rejection, 12-message array handling, 404/405/503 error responses, token-forwarding injection (prompt_token_ids), and disabled-forwarding body preservation.  Added to `test-integration` (Suite 7/7) and `test-integration-ci` pytest invocation in `Makefile`.
  _Files:_ `tests/integration/test_http_pipeline.py` (new), `Makefile`
  _Complexity:_ Medium

- [x] **Create streaming response test suite** ✅
  _Description:_ Created `tests/integration/test_streaming.py` with `StreamingTest` (`ClusterTestCase` subclass, `PROJECT_NAME="ranvier-streaming-test"`, `AUTO_REGISTER_BACKENDS=True`) covering SSE line-format validation (`test_01`), `Transfer-Encoding: chunked` with no `Content-Length` and multi-read delivery under `X-Mock-Latency-Ms` (`test_02`), `[DONE]` sentinel always last (`test_03`), mid-stream interruption via `/admin/failure-mode?mode=reset` asserting either a truncated stream without `[DONE]` or a `ChunkedEncodingError`/`ConnectionError` plus post-recovery healthy request (`test_04`), and header flush timing under a slow backend (`test_05`).  Also covers §6.7 "Test large payload handling": >1 MB honest request body via padded `messages` checked against the mock backend's `/debug/requests` log (`test_06`) and a VmRSS-bounded >10 MB streaming-response scaffold (`test_07`, currently `skipTest` pending a chunk-count knob on the mock backend).  Added to `test-integration` (Suite 8/8) and `test-integration-ci` pytest invocation in `Makefile`; renumbered the earlier banners to `x/8`.
  _Files:_ `tests/integration/test_streaming.py` (new), `Makefile`
  _Complexity:_ Medium

- [x] **Test request rewriting with token injection** ✅
  _Description:_ Covered by `tests/integration/test_http_pipeline.py`: `test_09_token_forwarding_injects_token_ids` (forwarding enabled, inspects `/debug/requests` for `prompt_token_ids`), `test_10_token_forwarding_disabled_preserves_original` (forwarding disabled, body unchanged), and `test_05_large_message_array` (12-message array).
  _Files:_ `tests/integration/test_http_pipeline.py`
  _Complexity:_ Low

### 6.3 Routing Logic Tests

- [x] **Create prefix affinity routing test suite** ✅
  _Description:_ Already covered by the pre-existing `tests/integration/test_prefix_routing.py` (8 tests): `test_01_same_prefix_routes_consistently`, `test_04_different_prefixes_can_route_differently`, `test_02_route_learning_creates_cache_entry` + `test_07_metrics_reflect_routing_behavior` (route learning via metrics), and `test_06_backend_affinity_persists_under_load`. Min token length threshold is exercised indirectly (compose sets `RANVIER_MIN_TOKEN_LENGTH=2`; all test prompts exceed it). This file predates the §6 backlog and already satisfies the acceptance criteria.
  _Files:_ `tests/integration/test_prefix_routing.py`
  _Complexity:_ Medium

- [x] **Extend route propagation tests** ✅
  _Description:_ Already covered across two pre-existing suites: `test_cluster.py::test_04_verify_route_propagation` asserts routes learned on Node1 are visible on Node2/Node3 after the gossip interval and `test_05_request_on_other_nodes` verifies propagated routes serve requests; `test_metrics.py::test_07_gossip_counters_increment` asserts `router_cluster_sync_sent` and `router_cluster_sync_received` deltas are positive over a 2-second window (gossip interval = 500ms).
  _Files:_ `tests/integration/test_cluster.py`, `tests/integration/test_metrics.py`
  _Complexity:_ Medium

- [x] **Test backend selection and lifecycle** ✅
  _Description:_ Already covered across pre-existing suites: `test_prefix_routing.py::test_01` through `test_08` exercise backend registration → routable backend (every test registers backends then routes through them); `test_health_circuit_breaker.py::test_05_fallback_to_healthy_backend` verifies requests route to healthy backends only when one is failing; `test_health_circuit_breaker.py::test_01_unhealthy_backend_detected` verifies unhealthy backends are detected. Backend removal stopping routing is covered by `test_cluster.py::test_06_stop_node_and_verify_peer_count` (node removal) and `test_health_circuit_breaker.py::test_07_recovery_after_backend_restart` (backend stop → traffic shifts to remaining backend).
  _Files:_ `tests/integration/test_prefix_routing.py`, `tests/integration/test_health_circuit_breaker.py`, `tests/integration/test_cluster.py`
  _Complexity:_ Low

### 6.4 Resilience and Fault Tolerance Tests

- [x] **Create health/circuit breaker test suite**
  _Description:_ Create `test_health_circuit_breaker.py` with tests for: unhealthy backend detection and removal, backend recovery and re-addition, health check interval configuration.
  _Files:_ `tests/integration/test_health_circuit_breaker.py` (new) — implemented in `HealthCircuitBreakerTest` (tests 01–10) using the mock backend's `/admin/failure-mode` injection and the `fault-injection` compose profile's always-failing backend.
  _Complexity:_ Medium

- [x] **Test circuit breaker state transitions**
  _Description:_ Verify: consecutive failures trigger open state, half-open state allows probes, successful probe closes circuit.
  _Files:_ `tests/integration/test_health_circuit_breaker.py` — implemented in `HealthCircuitBreakerTest` (tests 01–10).
  _Complexity:_ Medium

- [x] **Test connection pool resilience**
  _Description:_ Verify: connection reuse across requests, recovery after backend restart, timeout handling for slow backends.
  _Files:_ `tests/integration/test_health_circuit_breaker.py` — implemented in `HealthCircuitBreakerTest` (tests 01–10).
  _Complexity:_ Medium

- [x] **Test rate limiting behavior**
  _Description:_ Verify: requests exceeding limit return 429, limit resets after window, rate limit metrics exposed.
  _Files:_ `tests/integration/test_health_circuit_breaker.py` — implemented in `HealthCircuitBreakerTest` (tests 01–10); end-to-end enforcement is already covered by `test_negative_paths.py::test_04_rate_limit_exceeded`, so this suite validates metric registration (test_09) and defers behavioural coverage (test_10) to avoid duplicating the SIGHUP config-reload flow.
  _Complexity:_ Low

### 6.5 Observability Tests

- [x] **Create metrics test suite**
  _Description:_ Create `test_metrics.py` with tests for: `/metrics` returns valid Prometheus format, request count increments, latency histograms recorded, backend health metrics accurate.
  _Files:_ `tests/integration/test_metrics.py` (new) — implemented in `MetricsTest` (tests 01–05).
  _Complexity:_ Medium

- [x] **Test cluster metrics**
  _Description:_ Verify: `cluster_peers_alive` reflects actual peers, gossip counters increment during sync, per-shard metrics available.
  _Files:_ `tests/integration/test_metrics.py` — implemented in `MetricsTest` (tests 06–08).
  _Complexity:_ Low

### 6.6 Lifecycle and Persistence Tests

- [x] **Create graceful shutdown test suite** ✅
  _Description:_ Already covered by the pre-existing `tests/integration/test_graceful_shutdown.py`: `test_01_graceful_shutdown_completes_requests` sends SIGTERM and verifies in-flight requests complete (health endpoint returns 503 during drain, then connection refused after stop) and shutdown occurs within the timeout; `test_02_node_isolation_during_shutdown` verifies other nodes remain healthy during a peer's shutdown. The suite uses `signal_container_shutdown()` (SIGTERM via `docker kill`) and verifies clean exit via container state inspection. This file predates the §6 backlog and already satisfies the acceptance criteria.
  _Files:_ `tests/integration/test_graceful_shutdown.py`
  _Complexity:_ Medium

- [x] **Create persistence recovery test suite** ✅
  _Description:_ Covered by `tests/integration/test_persistence_recovery.py`: `test_01_backends_persist_in_sqlite` registers backends and asserts they appear in the on-disk `backends` table (observed by copying `/tmp/ranvier.db` out with `docker cp` and opening it with Python's sqlite3 module); `test_02_routes_persist_in_sqlite` warms a shared prefix and asserts `>= 1` row lands in the `routes` table; `test_03_wal_checkpoint_on_shutdown` confirms the shutdown log contains `Persistence shutdown summary:` / `Final WAL checkpoint complete` and not `checkpoint failed`; `test_04_corrupted_db_handled_gracefully` overwrites the DB, SIGKILLs, restarts via `docker start`, and accepts either the `integrity check failed` recovery path or the empty-store path (both satisfy "handled gracefully"); `test_05_empty_db_starts_clean` deletes the DB file and asserts the empty-store startup log. Environment note: the test compose file mounts `/tmp` as tmpfs, which doesn't persist across a docker stop/start cycle — so the "across restart" guarantee is observed via direct DB inspection rather than restart-roundtrip. WAL-mode concurrent-access is covered by `SqlitePersistence` unit tests; this suite focuses on the integration surface.
  _Files:_ `tests/integration/test_persistence_recovery.py`
  _Complexity:_ Medium

- [x] **Create configuration loading test suite** ✅
  _Description:_ Covered by `tests/integration/test_config_loading.py` (6 tests against the existing 3-node compose harness). `test_01_yaml_config_loaded_correctly` writes a valid YAML (`health.check_interval_seconds: 15`) and SIGHUPs ranvier1, then scans post-cutoff container logs for `Configuration reloaded successfully on all cores` (`application.cpp:1478`), fast-failing on any of the three `Config reload failed...` / `rate-limited` log variants. `test_02_env_vars_override_yaml` writes `routing.min_token_length: 99` while the compose env holds `RANVIER_MIN_TOKEN_LENGTH=2`, SIGHUPs, then sends six learning-eligible chat requests and asserts a positive `routes_total` delta — if YAML had won, the 99-token threshold would have blocked all route-learning at `http_controller.cpp:975`. `test_03_dry_run_validates_without_starting` runs `docker exec ranvier1 ./ranvier_server --dry-run --config /tmp/ranvier.yaml` (no `--smp`/`--memory` since `--dry-run` short-circuits before Seastar at `main.cpp:463`), asserts exit 0, `Dry Run Validation` banner, and `PASSED` result line, plus a post-exec chat request to prove the live parent process is unaffected. `test_04_dry_run_with_invalid_config_fails` writes `server.api_port: 0` (first rule in `RanvierConfig::validate()` at `config_loader.cpp:1569`; no `RANVIER_API_PORT` env var exists in the test compose so the value is guaranteed to reach validation) and asserts exit 1 with a `FAILED` result line. `test_05_invalid_yaml_at_startup_produces_clear_error` writes malformed YAML, drives ranvier1 through stop/start, and branches on the observed state: if the container stays down the test asserts the clean `Failed to parse config` line (`config_loader.cpp:1555` → `main.cpp:475`) appears; if the container comes up healthy (because Docker's tmpfs wipes `/tmp` on container stop) the test accepts the "fall back to defaults" path per §6.6 — both branches also assert `terminate called` is absent, guarding against a raw abort masquerading as a clean error. `test_06_missing_config_file_uses_defaults` removes `/tmp/ranvier.yaml`, drives a full stop/start, and probes the metrics endpoint both externally (via the `9181->9180` compose port mapping) and internally (`docker exec ranvier1 curl http://localhost:9180/metrics`) — the internal probe is what actually proves the server bound to the default `metrics_port=9180` from `config_infra.hpp:35` rather than some other value that happened to match the mapping target. Design guardrails: every test that writes `/tmp/ranvier.yaml` removes it in a `finally` block, and the SIGHUP cooldown chain is respected via a 12s wait on entry to tests that follow a reload (the application's `RELOAD_COOLDOWN` is 10s in `application.cpp:1416`) — cleanup also waits 12s before issuing a post-test SIGHUP to reload defaults. Suite is wired into `test-integration-full` as `12/12`, into `test-integration-fast` as `4/4`, and into the pytest-driven `test-integration-ci` target.
  _Files:_ `tests/integration/test_config_loading.py`
  _Complexity:_ Low

### 6.7 Edge Cases and Error Handling Tests

- [x] **Test error response validation** ✅
  _Description:_ Covered by `tests/integration/test_http_pipeline.py`: `test_06_unknown_endpoint_returns_404`, `test_07_wrong_method_returns_405`, `test_08_503_when_no_backends` (asserts JSON body with `error` key), and `test_04_invalid_json_returns_400` (structured error).
  _Files:_ `tests/integration/test_http_pipeline.py`
  _Complexity:_ Low

- [x] **Test large payload handling** ✅
  _Description:_ Covered by `tests/integration/test_streaming.py` alongside the §6.2 "Create streaming response test suite" item: `test_06_large_request_body_over_1mb` pads the last user message to >1.5 MB and asserts 200 plus `/debug/requests` preservation of the forwarded body within ±10 %; `test_07_large_streaming_response_over_10mb` scaffolds a VmRSS-bounded byte-counting loop (delta <50 MB, final line `data: [DONE]`) but currently `skipTest`s pending a chunk-count admin knob on the mock backend — a TODO points back to §6.1 "Enhance mock backend capabilities".  Concurrent-request coverage (§6.7) remains open.
  _Files:_ `tests/integration/test_streaming.py`
  _Complexity:_ Medium

- [x] **Test concurrent request handling** ✅
  _Description:_ Covered by `tests/integration/test_http_pipeline.py`: `test_11_100_concurrent_requests_without_errors` (20-worker `ThreadPoolExecutor` drives 100 streaming requests and asserts all 200 with non-empty bodies), `test_12_no_cross_contamination_under_load` (50 concurrent streams with unique per-request prompts and the mock backend's `/admin/prefix-echo` toggle assert each response echoes its own prompt prefix — direct shared-state detector), and `test_13_request_ordering_preserved_per_connection` (10 sequential requests on a single `requests.Session` assert keep-alive responses arrive in order).  Added `POST /admin/prefix-echo?enabled=1|0` to `mock_backend.py` because Ranvier constructs a fresh header set for the backend hop (see test_02's X-Custom-Header note).
  _Files:_ `tests/integration/test_http_pipeline.py`, `tests/integration/mock_backend.py`
  _Complexity:_ Medium

**§6 complete** — all 21 integration test items delivered across 12 test suites.


---

## 7. Strategic Assessment (2026-01-31)
### 8.6 Action Items (Tracking)

- [ ] **[P3] Track config.hpp complexity - split when >2000 LOC**
  _Description:_ Monitor `config.hpp` (currently 1,510 LOC). When exceeding 2000 LOC, split into `config_schema.hpp` (type definitions) and `config_loader.cpp` (YAML parsing logic).
  _Rationale:_ Prevent config.hpp from becoming the next "sprawling module". Current growth trajectory suggests split needed within 2-3 feature additions.
  _Files:_ `src/config.hpp` → `src/config_schema.hpp` (new), `src/config_loader.cpp` (new)
  _Complexity:_ Medium
  _Trigger:_ >2000 LOC or >50 config fields

---


---

## 8. Benchmark Extensions
### 9.1 Production Prompt Traces

- [ ] **Define trace format (JSON/JSONL) for production prompt patterns**
  _Justification:_ Synthetic prompts don't capture real-world prompt distribution. Production traces enable realistic performance validation.
  _Deliverables:_
  - Define schema capturing prompt content, timestamps, user sessions
  - Support prefix annotation for shared system messages
  - Include metadata: model, temperature, max_tokens
  _Location:_ `tests/integration/data/traces/` (new), schema doc
  _Complexity:_ Low

- [ ] **Modify locustfile_real.py to load and replay traces**
  _Justification:_ Current Locust test uses synthetic prompts with fixed distributions. Trace replay enables historical traffic patterns.
  _Approach:_ Add `TraceLoader` class, `--trace-file` parameter, timestamp-based replay scheduling
  _Location:_ `tests/integration/locustfile_real.py`
  _Complexity:_ Medium

- [ ] **Add --trace-file option to bench.sh**
  _Justification:_ Simplify trace-based benchmarking via CLI.
  _Location:_ `scripts/bench.sh`
  _Complexity:_ Low

- [ ] **Create tool to anonymize/sanitize production logs into trace format**
  _Justification:_ Production logs contain sensitive data. Anonymization tool enables safe trace collection.
  _Approach:_ PII detection, content hashing, prefix preservation, configurable anonymization rules
  _Location:_ `scripts/anonymize_traces.py` (new)
  _Complexity:_ Medium

### 9.2 Cache Pressure Scenarios

- [ ] **Add --unique-prefixes N option to control prefix diversity**
  _Justification:_ Current benchmarks use 5 unique prefixes. Real deployments may have thousands. Need to test cache behavior at scale.
  _Location:_ `tests/integration/locustfile_real.py`, `scripts/bench.sh`
  _Complexity:_ Low

- [ ] **Create "cache-pressure" prompt distribution**
  _Justification:_ Test behavior when unique prefixes exceed KV cache capacity. Validates eviction policies and degraded performance.
  _Approach:_ Generate N unique prefixes > vLLM block capacity, measure cache hit rate degradation curve
  _Location:_ `tests/integration/locustfile_real.py`
  _Complexity:_ Medium

- [ ] **Add metrics to detect cache evictions (if vLLM exposes this)**
  _Justification:_ Understanding cache eviction rate helps tune cache size and routing policy.
  _Approach:_ Query vLLM `/metrics` for `vllm:cache_evictions_total` or similar, add to benchmark report. See also: [Push-based cache eviction notifications](docs/architecture/push-cache-eviction-notifications.md) for a design that goes beyond metrics to actively update Ranvier's routing state.
  _Location:_ `tests/integration/locustfile_real.py`
  _Complexity:_ Low

- [ ] **Document expected behavior when cache overflows**
  _Justification:_ Operators need guidance on capacity planning and expected degradation.
  _Location:_ `docs/benchmarks/benchmark-guide-8xA100.md`
  _Complexity:_ Low

### 9.3 Larger Models

- [ ] **Update bench.sh to support tensor-parallel vLLM across multiple GPUs**
  _Justification:_ 70B+ models require tensor parallelism. Current bench.sh assumes single-GPU vLLM instances.
  _Approach:_ Add `--tensor-parallel N` flag, adjust vLLM launch command, GPU allocation logic
  _Location:_ `scripts/bench.sh`
  _Complexity:_ Medium

- [ ] **Add recommended configurations for 70B (8x A100) and 405B (multi-node)**
  _Justification:_ Users need reference configurations for large model deployments.
  _Deliverables:_ Sample configs, memory requirements, expected TTFT ranges
  _Location:_ `docs/benchmarks/benchmark-guide-8xA100.md`, `docs/benchmarks/benchmark-guide-405B.md` (new)
  _Complexity:_ Medium

- [ ] **Adjust expected TTFT thresholds based on model size**
  _Justification:_ Current thresholds calibrated for 8B-13B models. Larger models have different latency profiles.
  _Location:_ `tests/integration/locustfile_real.py`, benchmark documentation
  _Complexity:_ Low

- [ ] **Document memory requirements and GPU configurations**
  _Justification:_ Operators need clear guidance on hardware requirements per model size.
  _Location:_ `docs/deployment/hardware-requirements.md` (new)
  _Complexity:_ Low

### 9.4 Traffic Variability

- [ ] **Add traffic shapes to locustfile: ramp-up, spikes, diurnal patterns**
  _Justification:_ Real traffic is not steady-state. Bursty traffic tests cache warm-up and backpressure behavior.
  _Approach:_ Custom `LoadTestShape` classes for different patterns
  _Location:_ `tests/integration/locustfile_real.py`
  _Complexity:_ Medium

- [ ] **Add --traffic-pattern option (steady, bursty, ramp, spike)**
  _Justification:_ CLI option to select traffic shape without code changes.
  _Location:_ `scripts/bench.sh`, `tests/integration/locustfile_real.py`
  _Complexity:_ Low

- [ ] **Measure cold-start impact when traffic spikes after idle periods**
  _Justification:_ Production systems experience traffic spikes after quiet periods. Cache is cold, need to measure warm-up time.
  _Approach:_ Add idle period before spike, track cache hit rate over time, report warm-up duration
  _Location:_ `tests/integration/locustfile_real.py`
  _Complexity:_ Medium

- [ ] **Add metrics for cache warm-up time and hit rate over time**
  _Justification:_ Time-series cache metrics help operators understand warm-up behavior and set appropriate scaling policies.
  _Approach:_ Periodic cache hit rate sampling (every 10s), export as CSV alongside benchmark report
  _Location:_ `tests/integration/locustfile_real.py`
  _Complexity:_ Medium


---

## 9. Load-Aware Prefix Routing
### 10.1 Prerequisites

- [ ] Confirm vLLM `/metrics` endpoint exposes queue depth (optional enhancement - not required)

### 10.4 Post-Implementation Checklist

- [ ] Run `validation/validate_v1.sh` (reactor stall detection)
- [ ] Run `tests/integration/test_load_aware_routing.py`
- [ ] Run benchmark: `scripts/bench.sh --users 30` (verify >35% TTFT improvement)
- [ ] Consider follow-up: hot prefix replication (Option 2 from issue)

> **Implementation Complete**: All 8 steps completed in PRs #222, #223, #224 (2026-02-05)


---

## 10. HTTP Controller Review (2026-02-14)
### 15.5 Optimize Per-Chunk Flush in SSE Streaming Loop

- [ ] **Reduce flush frequency in `stream_backend_response()` via timer-based coalescing**
  _Justification:_ Every `bundle.in.read()` iteration triggers `write()` + `flush()` to the client. At typical LLM token rates (30-100 tok/s), each token arrives as a separate TCP segment, so this is effectively **one `sendmsg()` syscall per token** at steady state. Natural TCP batching only helps during bursts (multiple tokens arriving in one segment, which `StreamParser::push()` concatenates into a single `res.data`). At high concurrency (many concurrent streams), the per-token syscall overhead becomes measurable.

  **Previous attempt (2026-02-14, reverted in #280):** Flushed only on first write (TTFT) and `[DONE]`, relying on Seastar's 8KB `output_stream` buffer for intermediate events. This caused a **3.3x TTFT regression** because small SSE events (10-50 bytes each) never fill the 8KB buffer, stalling delivery for seconds until enough tokens accumulate. The regression was caught in production load testing and reverted.

  **Why "just skip flushes" fails:** Seastar's `output_stream` buffers 8KB by default. At 30 bytes/token, that's ~270 tokens (~9 seconds at 30 tok/s) before the buffer auto-flushes. SSE requires real-time delivery, so any approach that removes intermediate flushes without a bounded-latency fallback will break streaming.

  **Why "reactor tick coalescing" is not straightforward:** Seastar provides no primitive to check "is there more work pending in this reactor tick." `seastar::yield()` / `coroutine::maybe_yield()` are control-flow primitives, not introspection tools. You cannot peek at a future's readiness without consuming it, so "read-ahead to decide whether to flush" requires restructuring the entire loop.

  1. On entering the streaming loop, arm a periodic timer (~10ms interval, matching `lowres_clock` resolution).
  2. In the read loop, call `co_await client_out.write(res.data)` **without** `flush()`.
  3. The timer callback flushes whatever has accumulated: `(void)client_out.flush().handle_exception(...)` with a `seastar::gate` holder per Rule #5.
  4. Flush **immediately** on `res.done` (terminal event) and on stream errors/cleanup.
  5. Disarm the timer on loop exit (before `client_out.close()`).

  - The timer is **per-request** (created/destroyed with each `stream_backend_response()` call). This is fine — Seastar's `timer_set` is optimized for frequent create/cancel cycles. See existing patterns: `router_service.cpp:2058` (batch flush), `connection_pool.hpp:500` (reaper timer).
  - The timer callback is synchronous but `flush()` is async. Use the fire-and-forget pattern with gate guard: `(void)flush().finally([holder = std::move(holder)] {})`.
  - Must handle the race between timer-initiated flush and loop-initiated close. The gate ensures the flush completes before stream teardown.
  - Must handle `flush()` failure in the timer callback (client disconnect). Log and set a flag the main loop checks, or let the next `write()` catch the broken pipe.
  - **10ms worst-case latency** is acceptable for SSE token streaming (human-imperceptible). This coalesces 1-3 tokens per flush at typical rates, reducing syscalls by ~2-3x.

  - **TTFT test:** Measure time from first backend token to client receipt. Must be <15ms above baseline (currently ~1ms with per-read flush).
  - **Stall test:** Stream 100+ tokens at 30 tok/s, verify no token is delayed >20ms from backend receipt to client delivery.
  - **Burst test:** Send 10 tokens in <1ms from backend, verify they arrive at client in one batch (not 10 separate deliveries).
  - **Disconnect test:** Client disconnects mid-stream, verify timer is cleaned up and no use-after-free.
  - **Shutdown test:** Server drains while streams are active, verify gate prevents timer callback after stream teardown.



---

## 11. Hot-Path Performance Audit (2026-02-15)
### 20.7 Jump Consistent Hash May Reduce Cache Affinity vs Modular Hash

- [ ] **Evaluate whether jump consistent hash improves or degrades prefix cache locality**
  _Justification:_ The modular hash was replaced with jump consistent hash (`router_service.cpp:517-532`, called at line 1221) for better distribution uniformity when backends are added/removed. Jump consistent hash (Lamping & Veach, 2014) provides minimal disruption on topology changes but produces a different mapping than the previous modular hash. This means existing prefix→backend affinities were reshuffled. The benchmark regression may partly reflect degraded cache locality if the new hash distributes related prefixes differently across backends. This is speculative — the hash should be equally good for cache affinity in steady state — but worth validating.
  _What to change:_ Run a controlled A/B test comparing jump consistent hash vs the previous modular hash with identical workloads. If the steady-state cache hit rate and XLarge TTFT improvement are equivalent, the hash change is not a factor. If they differ, investigate whether the hash function's output distribution interacts poorly with the specific prefix patterns in the benchmark workload.
  _Location:_ `src/router_service.cpp:517-532` (implementation), `1221` (call site)
  _Complexity:_ Low (testing only)
  _Priority:_ P3 — Investigation; unlikely to be a factor but easy to rule out

### 20.8 Client-Token Path Redundant JSON Parse

- [ ] **Optimize `extract_prompt_token_ids()` to avoid full JSON parse**
  _Justification:_ When clients send pre-tokenized `prompt_token_ids`, the `extract_prompt_token_ids()` function parses the entire request body with RapidJSON (`doc.Parse(body.data(), body.size())`) just to extract the token array. For large requests with long system prompts, this is a full JSON parse of potentially tens of KB. The body is also parsed again downstream for forwarding/rewriting, making this parse redundant. Benchmark shows 0.35ms on the client-token path — 34x faster than server-side tokenization (12ms) but still the dominant cost on this path.
  _What to change:_ Options ranked by impact:
  (a) **Share the parse**: Parse the body once early in the request handler, pass the `rapidjson::Document` to both `extract_prompt_token_ids()` and downstream rewriting. Eliminates one full parse.
  (b) **Targeted extraction**: Use a SAX-style or on-demand parser to scan only for the `"prompt_token_ids"` key without parsing the entire document.
  (c) **SIMD token validation**: Replace the per-element bounds-checking loop (lines 731-755) with SIMD range checks — process 8-16 token IDs per instruction instead of one-by-one branch-heavy iteration.
  _Location:_ `src/request_rewriter.hpp:693-760` (implementation), `src/http_controller.cpp:801` (call site)
  _Complexity:_ Low (option a), Medium (options b/c)
  _Priority:_ P3 — Performance; 0.35ms is already fast but becomes significant at high concurrency with client-side tokenization


---

## 12. Request Lifecycle Performance Analysis (2026-02-20)
### 21.4 Avoid Heap Copy of Text for Tokenizer Cache Insertion

- [ ] **Eliminate redundant `std::string` copy captured in thread-pool tokenization continuation**
  _Justification:_ At line 325, the thread-pool tokenization path captures `text_copy = std::string(text)` in the `.then()` continuation lambda solely for cache insertion after tokenization completes. For a typical 4KB request body, this is a guaranteed heap allocation on every cache-miss tokenization dispatched to the thread pool. With 80–90% cache hit rates for system messages, this primarily affects user messages — but those are exactly the requests that take the full 5–13ms tokenization path, adding allocation pressure to an already-expensive operation.
  _What to change:_ (a) Examine the cache's `insert()` and `lookup()` signatures. If the cache uses hash-based lookup internally, compute the hash before dispatch (on the reactor, which already has the `string_view`) and pass only the hash + a pre-hashed key to the continuation. (b) If the cache API requires the full string for insertion (stores the key), consider having the thread pool worker do the cache insertion (it already has a local copy of the text for FFI per Rule #15). Ensure the thread pool worker's cache access is shard-safe. (c) Alternatively, return the text copy from the thread pool submission alongside the tokens, reusing the copy already made for FFI safety. The goal is to avoid a second copy in the continuation.
  _Location:_ `src/tokenizer_service.cpp:324-339`
  _Complexity:_ Low
  _Priority:_ P2 — Performance; ~4KB heap allocation per cache-miss tokenization

### 21.5 Reduce `sstring` Reallocations in StreamParser

- [ ] **Pre-size the `_accum` buffer or switch to a chunk chain to reduce reallocations during streaming**
  _Justification:_ `StreamParser::push()` at line 57 appends each network chunk to `_accum` (a `seastar::sstring`). After exceeding the 15-byte SSO threshold (virtually every HTTP response), each append may trigger a reallocation + copy. A 100KB response in 50 chunks causes ~6–8 reallocations. The compaction at line 114 adds another `memmove` when read position exceeds 50% of the buffer, which is correct but doubles the copy work after each compaction cycle.
  _What to change:_ (a) During `parse_headers()` (line 133), if a `Content-Length` header is present, extract it and pre-size `_accum` to at least that value. This eliminates all reallocations for non-streaming (non-chunked) responses. (b) For chunked responses (no Content-Length), pre-size `_accum` to `StreamParserConfig::initial_output_reserve` (4096) on first push. (c) Note: `seastar::sstring` does not have `reserve()`. You may need to switch `_accum` to `std::string` (which does have `reserve()`), or use a `seastar::temporary_buffer<char>` chain that avoids copying entirely. (d) If switching to a chain, `compact_if_needed()` is replaced by dropping consumed chunks. Measure tradeoff: the chain avoids copies but complicates parsing when a token (e.g., `\r\n`) spans two chunks.
  _Location:_ `src/stream_parser.cpp:57`, `src/stream_parser.hpp`
  _Complexity:_ Medium
  _Priority:_ P3 — Performance; 6–8 reallocations per response, each O(N) copy

### 21.8 Build Backend HTTP Headers Without Intermediate String Allocations

- [ ] **Replace `sstring` concatenation with `fmt::format` or pre-reserved append for HTTP header construction**
  _Justification:_ At lines 425–437, backend request headers are built via repeated `sstring` concatenation (`+` operator). Each `+` allocates a new `sstring`, copies left and right operands, and frees the old buffer. For 6 concatenation steps, that is 5 intermediate allocations. This runs on every proxied request.
  _What to change:_ (a) Replace with `fmt::format()` (Seastar includes fmt) for a single allocation. (b) Handle the conditional `traceparent` header (line 433–435) — either include it in the format with a conditional, or append it separately. (c) Append `"Connection: keep-alive\r\n\r\n"` (line 437) in the same operation. (d) Note: `fmt::format` returns `std::string` — verify that `bundle.out.write()` accepts it or convert to `sstring`. No behavior change — same header order, same values.
  _Location:_ `src/http_controller.cpp:425-437`
  _Complexity:_ Low
  _Priority:_ P3 — Performance; 5 temporary sstring allocations per request

### 21.9 Skip JSON Parse When Client Provides Tokens and Prefix Boundary

- [ ] **Avoid `extract_text_with_boundary_info()` JSON parse when client provides both tokens and prefix boundary**
  _Justification:_ At line 833, `extract_text_with_boundary_info()` is called unconditionally for all non-client-token requests. At line 953–957, it is called again if `text_extraction` was not populated (client-tokens path) and prefix boundary detection is enabled. The client-tokens path can still trigger a ~500µs JSON parse for boundary detection even though the client already provided both `prompt_token_ids` and `prefix_token_count`.
  _What to change:_ (a) In the client-tokens path, if `_config.enable_prefix_boundary` is enabled, eagerly call `extract_text_with_boundary_info()` after determining `used_client_tokens=true`, and cache the result in `text_extraction`. (b) Alternatively, add a fast path: if the client provides both `prompt_token_ids` AND `prefix_token_count` (already checked at line 935), skip the JSON parse entirely — both pieces of information needed for routing are already available. (c) The goal: when client provides both fields, zero JSON parsing should occur.
  _Location:_ `src/http_controller.cpp:795-957`
  _Complexity:_ Low
  _Priority:_ P3 — Performance; ~500µs JSON parse wasted when client provides all needed data

### 21.10 Capture `lowres_clock::now()` Once Per Streaming Loop Iteration

- [ ] **Cache `lowres_clock::now()` at the top of the streaming loop to eliminate duplicate call**
  _Justification:_ At lines 476 and 487 in `stream_backend_response()`, `lowres_clock::now()` is called twice within 10 lines. `lowres_clock` updates at ~100Hz (10ms resolution), so consecutive calls return the same value. This repeats for every chunk in the streaming loop (potentially hundreds of chunks per response).
  _What to change:_ Add `auto now = lowres_clock::now();` at the top of the while loop body (before line 476). Replace both `lowres_clock::now()` calls with `now`. No behavioral change.
  _Location:_ `src/http_controller.cpp:474-488`
  _Complexity:_ Trivial
  _Priority:_ P4 — Cleanup; negligible performance impact, improves code clarity

---


---

## 13. Code Modularity (Low Priority)
### 22.1 Extract BackendRegistry Interface from RouterService

- [ ] **Decouple HealthService and K8sDiscoveryService from RouterService**
  _Justification:_ HealthService and K8sDiscoveryService take `RouterService&` directly. They only use `get_all_backend_ids()`, `get_backend_address()`, and `set_backend_status()`. Extracting a `BackendRegistry` interface makes both services independently testable without constructing a full RouterService.
  _What to change:_ Define a `BackendRegistry` abstract class with the three methods above. Have RouterService implement it. Change HealthService and K8sDiscoveryService constructors to accept `BackendRegistry&`.
  _Location:_ `src/health_service.hpp`, `src/k8s_discovery_service.hpp`, `src/router_service.hpp`
  _Complexity:_ Low
  _Priority:_ **P2** — Promoted; prerequisite for §15 Tier 2 (vLLM metrics ingestion)

### 22.2 Split config_schema.hpp into Infrastructure and Product Configs

- [x] **Separate generic infrastructure configs from routing-specific configs**
  _Justification:_ `config_schema.hpp` is 618 lines mixing infrastructure configs (ServerConfig, PoolConfig, HealthConfig, TlsConfig, AuthConfig, etc.) with Ranvier-specific configs (RoutingConfig, AssetsConfig). Splitting reduces cognitive load and makes infrastructure configs independently reusable.
  _What to change:_ Move infrastructure config structs to `config_infra.hpp`. Keep RoutingConfig and AssetsConfig in `config_schema.hpp`. RanvierConfig includes both headers.
  _Location:_ `src/config_schema.hpp`
  _Complexity:_ Low
  _Priority:_ **P1** — Promoted; prerequisite for §15 Tier 1 (cost estimation + priority configs)

### 22.3 Split MetricsService into Helpers and Ranvier-Specific Counters

- [x] **Extract generic histogram/counter patterns from Ranvier-specific metrics**
  _Justification:_ `metrics_service.hpp` is 620 lines mixing reusable patterns (MetricHistogram class, bucket definitions, per-backend metrics map) with Ranvier-specific counters (tokenization, ART, prefix boundary). Splitting makes the generic patterns reusable and reduces file size.
  _What to change:_ Move MetricHistogram, bucket helpers, and the bounded per-backend metrics pattern to `metrics_helpers.hpp`. Keep Ranvier-specific counters in `metrics_service.hpp`.
  _Location:_ `src/metrics_service.hpp`
  _Complexity:_ Low
  _Priority:_ **P1** — Promoted; prerequisite for §15 Tier 1 (per-priority metrics)

### 22.4 Template ShardedConfig on Config Type

- [ ] **Make ShardedConfig generic instead of hardcoded to RanvierConfig**
  _Justification:_ Trivial change (`ShardedConfig<T>` with backward-compatible alias `using ShardedConfig = BasicShardedConfig<RanvierConfig>`). Follows the existing pattern used by ConnectionPool, CircuitBreaker, and RateLimiter.
  _Location:_ `src/sharded_config.hpp`
  _Complexity:_ Trivial
  _Priority:_ P4 — Consistency with existing template patterns

---


---

## 14. Shard 0 Role Isolation Analysis (2026-03-06)
### 23.1 Exclude Shard 0 from P2C Candidate Pool

- [ ] **When cross-shard dispatch is implemented, exclude shard 0 from P2C candidate selection**
  _Justification:_ Shard 0 runs all control plane services (gossip consensus, K8s discovery, health probes). Under peak data plane load, these compete for reactor time, risking gossip heartbeat deadline misses and delayed health state propagation. Excluding shard 0 from P2C candidates protects control plane latency with minimal data plane capacity loss (~6% on 16-core).
  _Prerequisite:_ Backlog item 15.1 Option A (implement actual cross-shard dispatch via `smp::submit_to`). Until then, this change has no effect since P2C is advisory-only.
  _What to change:_ In `shard_load_balancer.hpp`, modify `select_shard_p2c()` candidate generation to exclude shard 0: change `std::uniform_int_distribution<uint32_t> dist(0, _shard_count - 1)` to `dist(1, _shard_count - 1)`. Alternatively, add a configurable `_excluded_shards` set for flexibility. Also update `select_shard_async()` candidate generation to match.
  _Location:_ `src/shard_load_balancer.hpp` (lines 114, 164-169)
  _Complexity:_ Low
  _Priority:_ P2 — Deferred until cross-shard dispatch ships

### 23.2 Hard Shard 0 Isolation (Future — Evidence-Gated)

- [ ] **If shard 0 reactor utilization consistently exceeds 80% while other shards are below 50%, isolate shard 0 from data plane entirely**
  _Justification:_ In large clusters (8+ nodes, 20+ backends), control plane work becomes non-trivial: gossip protocol traffic scales with cluster size, health probes scale with backend count, K8s watch reconnects can spike during topology changes. If shard 0 becomes a bottleneck, full isolation prevents cascading degradation.
  _Evidence trigger:_ Monitor `seastar_reactor_utilization` on shard 0 vs other shards under production load. Only proceed if sustained asymmetry is observed.
  _What to change:_ In `application.cpp:697-699`, conditionally skip data plane route registration on shard 0. Keep `/health` endpoint accessible on shard 0 (or move to all shards). May require Seastar `SO_INCOMING_CPU` tuning to prevent OS from dispatching TCP connections to shard 0.
  _Location:_ `src/application.cpp` (lines 697-699), `src/http_controller.cpp` (route registration)
  _Complexity:_ Medium
  _Priority:_ P3 — Only if evidence warrants; premature on clusters <8 nodes or <16 cores
  _Tradeoff:_ Permanently loses 1/N data plane capacity (25% on 4-core, 12% on 8-core, 6% on 16-core). Shard 0 would be mostly idle between control plane bursts — wasted core on small deployments.

---

## 15. Intelligence Layer Roadmap (2026-03-25)

Prioritized implementation plan derived from [VISION.md](docs/architecture/VISION.md).
Chassis refactors (§13) are interleaved where they prevent rework on shared files.

### Tier 1: Foundation (do now)

- [x] **[P1] Split config_schema.hpp** (§13 item 22.2, promoted from P4)
  _Why now:_ Tier 1 features add CostEstimationConfig + PriorityQueueConfig. Splitting infra vs product configs first avoids a second move.
  _Location:_ `src/config_schema.hpp`
  _Complexity:_ Low

- [x] **[P1] Split metrics_service.hpp** (§13 item 22.3, promoted from P4)
  _Why now:_ Tier 1 features add per-priority metrics. Same logic.
  _Location:_ `src/metrics_service.hpp`
  _Complexity:_ Low

- [x] **[P1] Cost Estimation + Priority Tiers (VISION 1.1+1.2 merged)**
  _Effort:_ ~2.5 weeks
  _Scope:_ ProxyContext cost fields, input_tokens as initial cost proxy, PriorityLevel enum, PriorityQueue, X-Ranvier-Priority header, per-priority metrics. Heuristic decay deferred to v1.1.
  _Files:_ `src/http_controller.{hpp,cpp}`, `src/config_schema.hpp`, `src/metrics_service.hpp`
  _Dependencies:_ config + metrics splits above
  _Complexity:_ High

- [x] **[P1] Intent Classification (VISION 1.4)**
  _Effort:_ ~1 week
  _Scope:_ RequestIntent enum (AUTOCOMPLETE/CHAT/EDIT), FIM detection, wire-format inspection, intent-based routing hints, per-intent metrics.
  _Files:_ `src/http_controller.{hpp,cpp}`, `src/config_schema.hpp`, `src/config_loader.cpp`, `src/metrics_service.hpp`, `src/application.cpp`
  _Dependencies:_ 1.1+1.2
  _Complexity:_ Medium
  _Completed:_ 2026-03-28 — §15 Tier 1 fully complete.

### Tier 2: Cloud Intelligence

- [x] **[P2] Extract BackendRegistry interface** (§13 item 22.1, promoted from P4)
  _Why now:_ vLLM metrics ingestion wires into HealthService. Decoupling from RouterService first avoids deepening the coupling.
  _Location:_ `src/health_service.hpp`, `src/k8s_discovery_service.hpp`, `src/router_service.hpp`
  _Complexity:_ Low
  _Completed:_ 2026-03-31

- [x] **[P2] vLLM Metrics Ingestion (VISION 2.1)**
  _Effort:_ ~2 weeks
  _Scope:_ VLLMMetrics struct, /metrics scraping, Prometheus text parsing.
  _Files:_ `src/health_service.{hpp,cpp}`, `src/metrics_service.hpp`
  _Dependencies:_ BackendRegistry interface
  _Complexity:_ High
  _Completed:_ 2026-03-31

- [x] **[P2] Load-Aware Backend Selection (VISION 2.2)**
  _Effort:_ ~2 weeks (~30% infra exists from load-aware prefix routing)
  _Scope:_ P2C alternative selection, routing decision logic, thundering herd prevention.
  _Dependencies:_ 2.1
  _Complexity:_ High
  _Completed:_ 2026-03-31

- [x] **[P2] Cost-Based Routing (VISION 2.3)**
  _Effort:_ ~1.5 weeks
  _Scope:_ Per-backend cost budget, small-request fast lane, budget reservation/release.
  _Dependencies:_ 1.1+1.2, 2.2
  _Complexity:_ Medium
  _Completed:_ 2026-03-31 — §15 Tier 2 fully complete.

- [ ] **Re-benchmark: prefix + priority + load-aware vs baseline**

### Tier 3: Local Product

- [x] **[P2] Local Mode Config (VISION 1.3)**
  _Effort:_ ~1 week
  _Scope:_ LocalModeConfig, RANVIER_LOCAL_MODE env, conditional startup.
  _Files:_ `src/config_schema.hpp`, `src/application.cpp`
  _Complexity:_ Low

- [x] **[P2] Local Backend Discovery (VISION 3.1)**
  _Effort:_ ~1.5 weeks
  _Scope:_ Port scanning, semantic liveness, server type detection.
  _Dependencies:_ 1.3
  _Complexity:_ Medium

- [x] **[P2] Agent-Aware Request Handling (VISION 3.2)** ✓ Done (2026-03-29)
  _Effort:_ ~2 weeks
  _Scope:_ AgentRegistry, agent identification, pause/resume API.
  _Dependencies:_ 1.2, 1.4
  _Complexity:_ High

- [x] **[P3] Request Queuing with Pause/Resume (VISION 3.3)** ✓ Done (2026-03-29)
  _Effort:_ ~2 weeks | Risk: High
  _Scope:_ RequestScheduler pause-aware dequeue, per-agent queue depth limits, admin API stats.
  _Dependencies:_ 3.2
  _Complexity:_ High

### Tier 4: Polish (parallel, after Tier 3)

- [ ] **[P3] Single-Binary Local Distribution (VISION 4.1)**
  _Complexity:_ Medium

- [x] **[P3] Local Dashboard UI (VISION 4.2)** ✓ Done (2026-04-04)
  _Complexity:_ High
  _Delivered:_ Single-page dashboard at localhost:9180/dashboard with backend status, queue depths, agent table with pause/resume, and throughput stats. HTML embedded in binary via CMake. CORS gated behind DashboardConfig (auto-enabled in local mode).

- [x] **[P3] Documentation & Examples (VISION 4.3)** ✓ Done (2026-04-04)
  _Complexity:_ Low
  _Delivered:_ Getting Started (Local), Cloud Deployment Guide, IDE Integration Guide, Benchmark Reproduction Guide. README updated with Quick Start and Documentation sections.

### §15 Completion Note (2026-04-04)

**The Intelligence Layer Roadmap is complete.** All four tiers delivered:
- **Tier 1 (Foundation):** Cost estimation, priority tiers, intent classification — 2026-03-28
- **Tier 2 (Cloud Intelligence):** vLLM metrics, load-aware routing, cost-based routing — 2026-03-31
- **Tier 3 (Local Product):** Local mode, backend discovery, agent-aware handling, pause/resume — 2026-03-29
- **Tier 4 (Polish):** Documentation & examples, local dashboard UI — 2026-04-04

Remaining Tier 4 item (4.1 single-binary distribution) is deferred — not blocking the v1.0 release.

### Dashboard v2 (future improvements)

**Next iteration:**
- [ ] **[P2] Rolling request rate** — replace average-over-uptime req/s with a sliding window (60s ring buffer in `/dashboard/stats`). Current metric flatlines quickly and doesn't reflect real-time throughput.
  _Complexity:_ Low (add ring buffer to stats handler, ~50 lines)
- [ ] **[P2] Error breakdown** — split "Errors: N" into timeouts, connection errors, circuit breaker rejections. MetricsService already tracks these separately (`_requests_timeout`, `_requests_connection_error`, `_circuit_opens`); just expose via `/dashboard/stats`.
  _Complexity:_ Low (add fields to stats JSON, update dashboard JS)
- [ ] **[P3] Backend detail** — show circuit breaker state (closed/open/half-open) and active connections per backend. HealthService already tracks vLLM GPU metrics that could surface here too.
  _Complexity:_ Medium (need to expose circuit breaker state via admin API or new endpoint)

**Later:**
- [ ] **[P3] Cross-shard stats aggregation** — `/dashboard/stats` currently returns shard-0 counters only (correct for `--smp 1`). Multi-core needs `smp::invoke_on_all` to sum across shards, requiring an async handler instead of the sync `function_handler`.
  _Complexity:_ Medium (async handler pattern change + cross-shard summation)
- [ ] **[P4] WebSocket push** — replace 5s polling with push to eliminate the blind spot for transient queue depth spikes. Seastar has WebSocket support.
  _Complexity:_ High (new protocol, connection lifecycle management)
- [ ] **[P3] Request log tail** — last N requests with latency, status, backend, agent. Needs a bounded ring buffer per shard.
  _Complexity:_ Medium (bounded ring buffer + new endpoint + dashboard panel)
- [ ] **[P4] Model inventory per backend** — local discovery detects server type; could fetch `/api/tags` from Ollama to show available models on each backend.
  _Complexity:_ Medium (discovery service changes + dashboard UI)

### Chassis items deferred (no urgency)

- [ ] **[P4] Template ShardedConfig** (§13 item 22.4) — trivial, do whenever
- [ ] **[P4] Generalize gossip message types** — only when 2nd product committed

---

## 16. KV-Cache Compression-Aware Routing (2026-04-05)

Design exploration for making Ranvier's routing decisions aware of heterogeneous backend KV-cache capacity — motivated by Google's TurboQuant (~6x KV-cache compression, ICLR 2026), but applicable to any fleet where backends differ in effective cache capacity.

**Full proposal:** [`docs/architecture/kv-cache-compression-integration.md`](docs/architecture/kv-cache-compression-integration.md)

| Priority | Item | Complexity | TurboQuant-specific? |
|----------|------|------------|---------------------|
| P0 | Compression-aware load scoring (`load_score()` formula) | Low | No |
| P1 | Effective capacity in cost-based routing | Low | No |
| P1 | Fleet-wide cache efficiency metrics | Low | No |
| P2 | Capacity-aware hash fallback selection | Medium | No |
| P2 | Compression-aware route TTL | Low | No |
| P3 | Tiered compression signaling (Ranvier → backend) | High | Partially |

Core enabler: `compression_ratio` field in `BackendInfo` (static config initially, scraped later).

---

## 17. Hard Rules Audit Follow-ups (2026-05-05)

Code follow-ups from the Rules 2/12/13/20 doc-correction pass. Canonical rule wording was updated in `.dev-context/claude-context.md` on 2026-05-05; this section tracks the code sites that violated the corrected rules. Each item should land as its own PR — they have independent scope and risk profiles.

### P0 — Reactor stalls from `seastar::async` misuse (Rule #12)

The codebase wraps blocking SQLite and `std::ifstream` calls in `seastar::async(...)` under the assumption that this offloads to a thread pool. It does not — `seastar::async` runs in a `seastar::thread` that executes on the reactor, so the blocking call still stalls the shard. Reference fix pattern: `src/tokenizer_thread_pool.cpp` (dedicated OS worker thread + `seastar::alien::run_on` for completion signaling).

- [x] **[P0] Refactor `AsyncPersistenceManager` SQLite path to a dedicated worker thread**
  _Locations:_ `src/async_persistence.cpp:147` (shutdown flush), `src/async_persistence.cpp:284` (periodic flush), `src/async_persistence.hpp:25-44` and `src/sqlite_persistence.hpp:17-30` (false "thread pool" docstrings).
  _Justification:_ Every persisted route/backend mutation currently stalls the shard for the duration of the SQLite call. Highest-frequency offender.
  _Approach:_ Mirror `tokenizer_thread_pool` — MPSC queue drained by a `std::thread`, completion posted back via `seastar::alien::run_on`. Update the docstrings to reflect reality.
  _Verification:_ Latency measurement under sustained write load before/after in the Docker testbed.
  _Complexity:_ Medium-High (new thread, lifecycle, shutdown ordering).

- [x] **[P0] Remove blocking `ifstream` from hot config reload**
  _Location:_ `src/application.cpp:1435` wrapped `RanvierConfig::load(path)` (which called `std::ifstream` at `src/config_loader.cpp:795`) in `seastar::async`.
  _Justification:_ Hot reload stalled all shards via the subsequent `invoke_on_all`. Lower frequency than persistence but same root cause.
  _Resolution:_ Added `load_config_async()` (`src/config_loader_async.{hpp,cpp}`) using `seastar::open_file_dma` + `dma_read_bulk` with a 10MB cap. Sync `RanvierConfig::load()` was refactored to share its YAML parsing body with the async path via the new public `RanvierConfig::load_from_string()` helper, so startup callers in `main.cpp` and the unit tests retain identical behaviour. `Application::reload_config` now awaits `load_config_async(_config_path)` directly — no more `seastar::async` wrapper. Missing-file semantics (return defaults + env overrides) are preserved by catching `std::system_error{ENOENT}` from `open_file_dma`.

### P1 — Latency and lifecycle bugs

- [x] **[P1] Wire up `cleanup_shard_load_metrics()` (Rule #13)**
  _Location:_ `src/shard_load_metrics.hpp:140` — `thread_local std::unique_ptr<ShardLoadMetrics>`. `cleanup_shard_load_metrics()` is defined at line 165 but never called. The unique_ptr destructor runs at thread-exit, after the reactor has torn down.
  _Justification:_ Per the corrected Rule #13, `thread_local std::unique_ptr` only works for trivially-destructible state. If `~ShardLoadMetrics` ever touches reactor primitives (metric registrations etc.) this becomes a shutdown-order bug.
  _Resolution:_ Added a new step in `Application::stop_services()` (`src/application.cpp`) that runs `seastar::smp::invoke_on_all([] { cleanup_shard_load_metrics(); })` after `_load_balancer.stop()` and before `_tokenizer.stop()` — i.e. after both consumers (`HttpController` and `ShardLoadBalancer`) are stopped, while every shard's reactor is still alive. Mirrors the symmetric init at line 653-656. No started-flag added (resetting a null `unique_ptr` is a no-op; lazy init in `shard_load_metrics()` re-creates on demand). Today's `ShardLoadMetrics` is trivially destructible (four `uint64_t` counters + `uint32_t` shard id), so this is preventive — it makes destruction order deterministic before any future field that touches reactor state turns the latent issue into a live shutdown bug.
  _Complexity:_ Trivial.
  _Follow-up flagged:_ `stop_metrics()` in `src/metrics_service.hpp:843` is also defined but never called from `Application::stop_services()` (only from unit tests). The audit referenced it as a precedent, but the precedent itself is unwired. Deliberately not bundled here — deregistering Prometheus lambdas mid-scrape has its own risk profile.

- [x] **[P1] Wire up `stop_metrics()` (Rule #6 / Rule #13)**
  _Location:_ `src/metrics_service.hpp:843` — `stop_metrics()` is defined but never called from `Application::stop_services()`. Only `tests/unit/metrics_service_test.cpp:400,403,417` invoke it. The thread-local `g_metrics` at line 822 is a raw `MetricsService*` (not even a `unique_ptr`), so without an explicit call it is leaked at process exit and its `_metrics.clear()` / `_backend_metrics.clear()` lambda-deregistration step never runs.
  _Justification:_ The header comment at line 841 is explicit: "MUST be called before destruction to deregister metrics lambdas (Rule #6). After this call, Prometheus scrapes cannot access any metrics state." Today this is latent because the process exits shortly after `stop_services()` returns and the scrape endpoint is taken down with the HTTP servers in step 3. But it is a Rule #6 violation (lambda captures whose backing state may go away) and the symmetry with `cleanup_shard_load_metrics()` makes the gap more glaring after the §17 P1 fix above.
  _Resolution:_ Added a new step in `Application::stop_services()` (`src/application.cpp`) — step `4a3` — that runs `seastar::smp::invoke_on_all([] { stop_metrics(); })` immediately after the `cleanup_shard_load_metrics()` step (`4a2`) and before `_tokenizer.stop()`. Kept as a sibling `invoke_on_all` rather than folded into the shard-load-metrics lambda so a future async signature on `stop_metrics()` does not reshape the call site, and so attribution stays clean if a shutdown regression bisects to one or the other. No started-flag added — `stop_metrics()` already self-guards on `g_metrics != nullptr`, so shards that never lazy-initialised (and the metrics-HTTP-disabled config path) are no-ops; `metrics()` lazy-initialises on next access, preserving restart safety. Mirrors the symmetric init at line 653-656 (`init_metrics()` alongside `init_shard_load_metrics()` in the same `invoke_on_all`).
  _Scrape-race analysis:_ Two ordering preconditions had to be established. **(a) Every caller of `metrics()` is quiescent at this step.** Grepped the full caller set across `src/`: the producers are `http_controller.cpp` (the dominant user), `router_service.cpp`, `stream_parser.cpp`, and the circuit-cleanup callback registered against `HttpController` at `application.cpp:642-649`. All four are driven by `HttpController`, which stopped in step 4 (`_controller.stop()`). `TokenizerService`, `TokenizerThreadPool`, `AsyncPersistenceManager`, and `ShardedConfig` do not call `metrics()` (verified by grep — they use their own `seastar::metrics::metric_groups`), so the later steps in the chain are safe. `ShardLoadBalancer` only touches `shard_load_metrics()`, not `metrics()`. **(b) No Prometheus scrape can be in flight when deregistration runs.** The metrics HTTP server (`_metrics_server`, a `seastar::httpd::http_server_control`) is stopped in step 3 via `stop_servers()` at `application.cpp:951-964`, well before this step. By inspection of the Seastar contract, `http_server::stop()` waits for in-flight connections to drain before its returned future resolves — the existing comment at `application.cpp:870` ("Application outlives the metrics server") relies on the same property. Concern resolved by inspection; no ad-hoc barrier added.
  _Caveat:_ The drain guarantee is by reading the Seastar API contract, not from a unit test in this repo. A hot-path integration test that issues sustained scrapes during shutdown would harden this — flagged in the verification scenarios on the implementation PR but not blocking.
  _Complexity:_ Low.

- [ ] **[P1] Parallelize gossip SRV lookups (Rule #2)**
  _Location:_ `src/gossip_protocol.cpp:716-729` — serial `co_await _dns_resolver.get_host_by_name(srv.target)` over an unbounded SRV record list (comment at line 727 admits it could reach 500+).
  _Justification:_ Per-discovery-cycle latency = N × DNS RTT. The `maybe_yield()` at line 728 helps other shard work but does not parallelize this loop.
  _Approach:_ `seastar::max_concurrent_for_each(srv_records, 16, ...)`.
  _Complexity:_ Low.

- [ ] **[P1] Pin foreign worker threads to non-reactor cores**
  _Locations:_ `src/async_persistence.cpp` (single persistence worker, post-Rule-#12 refactor), `src/tokenizer_thread_pool.cpp` (one worker per shard). Both spawn plain `std::thread` workers with no CPU affinity set.
  _Justification:_ Reactor threads are pinned to specific cores. Foreign threads with no affinity are placed by the kernel scheduler, which can land them on a reactor core and cause context-switch latency spikes when the worker is active. The deployment workaround — running with `--smp=N-1` on an N-core box so the kernel has a free core — is config-only and not guaranteed (the scheduler may still preempt). An explicit pin makes it deterministic. The tokenizer pool has the same exposure and should be done in the same change for consistency; the persistence worker is a single thread so it just needs one non-reactor core, while the tokenizer pool has `smp::count` workers and would need a different placement strategy (e.g. round-robin across non-reactor cores, or accept that they share with reactors and rely on `--smp=N-K`).
  _Approach:_ `pthread_setaffinity_np` in `start()` after the `std::thread` is constructed. Take the target cpuset from config or compute it as "all cores not in Seastar's cpuset." Side note: revisit whether `SCHED_BATCH` / nice value adjustments are also worthwhile to lower the worker's priority relative to reactors.
  _Verification:_ Same latency benchmark as the Rule #12 refactor — sustained-write load with and without the pin, looking for tail-latency differences. Confirm with `taskset -p $(pgrep -f ranvier)` that the worker landed where expected.
  _Complexity:_ Low (one syscall per worker), but the shared design decision across persistence + tokenizer pool needs a brief writeup before coding.

### P2 — Cleanup

- [ ] **[P2] Drop unnecessary `do_with` in coroutine broadcasts (Rule #20)**
  _Locations:_ `src/router_service.cpp:3494` (`broadcast_gpu_load`), `src/router_service.cpp:3517` (`broadcast_cache_headroom`).
  _Justification:_ Both are already inside a coroutine; the coroutine frame already keeps `shared` alive across the awaited `invoke_on_all`. The current `do_with` adds no safety, just noise. Per the corrected Rule #20, coroutines are the preferred pattern.
  _Approach:_ Replace `co_await seastar::do_with(std::move(shared), [](auto& shared) { return invoke_on_all(...); })` with the inner call directly using the local.
  _Complexity:_ Trivial.

### Optional follow-up

- [ ] **[P3] Add CI grep for `seastar::async(`**
  _Justification:_ The misuse was systemic enough that a lint will catch the next instance faster than another audit. Allowlist the legitimate sites once they're identified.

---

## 18. Request Lifecycle Crash-Risk Audit Follow-ups (2026-05-08)

Follow-ups from the request-lifecycle crash-risk assessment in [`docs/audits/request-lifecycle-crash-audit.md`](docs/audits/request-lifecycle-crash-audit.md). The audit was static-only and produced 25 HIGH+MED findings of mixed quality. A second triage pass re-read the source around each finding; this section reflects the triage verdicts, not the raw audit list.

**Triage outcome:**
- 8 CONFIRMED → fix tickets below
- 10 MITIGATED → closed with a one-line evidence note (see audit doc)
- 4 HYPOTHETICAL → defensive-only, rolled into a single sweep ticket
- 3 INVESTIGATE-FURTHER → empirical follow-ups (sanitiser / fuzz)

Even some CONFIRMED items have only hypothetical reachability (operator-misconfiguration or "if a future caller…"). They are kept on the list because each fix is trivial; do not treat them as security findings.

Empirical companion: libFuzzer harnesses live at [`tests/fuzz/`](tests/fuzz/). Anything that survives a multi-hour fuzz run on its corresponding boundary should be moved from this section to "MITIGATED-BY-FUZZ" in the audit doc.

### Cross-cutting (do these first — each closes several findings)

- [x] **[P1] Adopt a `with_timeout` helper on the request hot path** — Fixed 2026-05-09
  _Closes:_ H3, M5; tightens M15.
  _Fix:_ `with_request_timeout(deadline_or_duration, fut, label)` in
  `src/request_timeout.hpp` translates `seastar::timed_out_error` into
  a labeled `request_timeout_error`. Applied at the three call sites
  in `http_controller.cpp` (tokenize, stream chunk-read) and
  `tokenizer_service.cpp` (thread-pool future); on tokenize timeout
  the request falls back to round-robin routing.

- [x] **[P1] Document and assert the shutdown lifetime contract** — Fixed 2026-05-09
  _Closes:_ H5, M14. (Triage already MITIGATED H4 and M1 via the gate-holder pattern.)
  _Fix:_ Single-depth route-learning `.then()` in
  `http_controller.cpp:stream_backend_response` now captures
  `_request_gate.hold()` alongside `this`, so `_request_gate.close()`
  blocks on the fire-and-forget tail (the project-idiomatic equivalent
  of `seastar::shared_ptr<HttpController>`). `seastar::alien::instance`
  is a non-movable singleton, so by-value capture in `TokenizerWorker`
  is not feasible; instead `~TokenizerThreadPool` asserts that
  `stop_worker()` was called (warn-and-force-stop fallback) and the
  capture site is documented as `SHUTDOWN-CONTRACT`. A canonical
  invariant block lives at the top of `class HttpController`
  (`http_controller.hpp`) and `class TokenizerThreadPool`
  (`tokenizer_thread_pool.hpp`).

### P1 — CONFIRMED HIGHs

- [x] **[P1] H1: Replace `*slot.units` with checked access** — Fixed 2026-05-09 (P1 trivial sweep).

- [x] **[P1] H2: Validate Content-Length before `uint64_t→size_t` cast** — Fixed 2026-05-09 (P1 trivial sweep).

- [x] **[P1] H3: Wrap local-FFI tokenizer fallback in `with_timeout`** — Fixed 2026-05-09 (subsumed by `with_timeout` ticket above).

- [x] **[P1] H5: Fix alien-instance capture lifetime** — Fixed 2026-05-09 (subsumed by shutdown-contract ticket above).

- [x] **[P1] H7: Saturate `total_weight` in weighted random selection** — Fixed 2026-05-09 (P1 trivial sweep).

- [x] **[P1] H9: Cap exponential backoff before `double→int64_t` cast** — Fixed 2026-05-09 (P1 trivial sweep).

### P2 — CONFIRMED MEDs not subsumed by cross-cutting

- [x] **[P2] M5: Add timeout to `TokenizerThreadPool::submit_async` callers** — Fixed 2026-05-09 (subsumed by `with_timeout` ticket above).

- [x] **[P2] M14: Stop capturing raw `[this]` in fire-and-forget route-learning** — Fixed 2026-05-09 (subsumed by shutdown-contract ticket above).

### M6 — UPGRADED-BY-FUZZ, fixed (2026-05-08)

- [x] **[P1] M6: RapidJSON stack overflow on deeply-nested JSON**
  _Original verdict:_ HYPOTHETICAL (defensive only). _Actual:_ CONFIRMED via fuzz. The request-rewriter harness reproduced an ASan stack-overflow within ~10 minutes; the input was a deeply-nested JSON array that recursed through `ParseArray`/`ParseValue` ~250+ levels until the OS stack was exhausted. Reachable from request body — adversarial client could crash any shard.
  _Fix:_ Passed `rapidjson::kParseIterativeFlag` to `Document::Parse(...)` at all nine call sites in `request_rewriter.hpp`. The iterative parser keeps state on the heap (in the existing `MemoryPoolAllocator`) and can't stack-overflow regardless of nesting; total memory remains bounded by `max_request_body_bytes` upstream. No policy / threshold to tune.
  _Reproducer:_ `crash-9ebccad46252e559406b7dcff51ac746fb050996` (libFuzzer artifact).
  _Lesson for future audits:_ static triage said "parser may fail and return `HasParseError()` — correctly handled at line 344"; that was wrong. RapidJSON's default flags (`kParseDefaultFlags == kParseNoFlags`) impose no depth limit on the recursive descent parser. Worth grepping any future review of JSON-handling code for missing iterative or depth-checked configurations.

### P3 — Defensive only (HYPOTHETICAL findings, single sweep)

- [x] **[P3] Defensive sweep: M3, M9, M11** — Mitigated 2026-05-10 — single bundled pass: M3 pinned via block comment at `tokenizer_service.cpp:213` (signature change to `std::string&&` rejected as more code than the latent risk); M9 documented via cast-safety comment at `router_service.cpp:1135` citing the `MAX_KNOWN_BACKENDS = 64` cap in `local_discovery.hpp:81`; M11 confirmed already mitigated by the `\r\n\r\n` guard at `stream_parser.cpp:141` (no code change). See audit-doc closure addendum near the top of `docs/audits/request-lifecycle-crash-audit.md`.
  _Coverage:_
  - **M3** (`tokenizer_service.cpp:243-312`) — invariant pinned in code as a block comment at the top of `encode_cached_async`; no `co_await` may precede the `text_copy` / `text_for_cache` materialisations.
  - **M9** (`router_service.cpp:1145`) — cast-safety comment cites the upstream backend-count cap; cast is provably lossless under existing caps, no runtime check.
  - **M11** (`stream_parser.cpp:152-157`) — re-read confirms `parse_headers` returns "need more data" until the full `\r\n\r\n` terminator is buffered, so the status line cannot be split when the snoop runs. Already mitigated; recorded as MITIGATED.

### P2 — INVESTIGATE FURTHER (empirical, not fix tickets)

- [x] **[P2] M4 investigation: confirm `std::bad_alloc` propagation across `smp::submit_to`** — Mitigated 2026-05-10 — diagnostic test at `tests/unit/cross_shard_exception_propagation_test.cpp` throws `std::bad_alloc` inside a `seastar::smp::submit_to` lambda and observes it at the initiating shard with demangled typeid `std::bad_alloc` (propagated cleanly, not repackaged), so the existing `catch (const std::exception&)` around `encode_threaded_async` at `http_controller.cpp:1340` already covers it; see audit-doc closure addendum near the top of `docs/audits/request-lifecycle-crash-audit.md`.

- [x] **[P3] M7 investigation: cross-shard `get_live_backends` race during reconfiguration** — Mitigated 2026-05-10 — code-read confirms the reader (`ShardLocalState::get_live_backends`, `src/router_service.cpp:529-540`) is synchronous (no `co_await`) and every cross-shard writer that mutates `backends` / `backend_ids` / `dead_backends` / `is_draining` (`register_backend_global`, `unregister_backend_global`, `report_backend_health`, `drain_backend_global`, `handle_node_state_change`) dispatches via `seastar::smp::submit_to`, so the writer runs as its own reactor task on the target shard and cannot interleave with the reader's snapshot loop; see audit-doc closure addendum near the top of `docs/audits/request-lifecycle-crash-audit.md`.

- [x] **[P3] M10 investigation: hard cap on fallback walker** — Mitigated 2026-05-10 — verified structural cap at `http_controller.cpp:218-231`: the loop iterates a value-copy of `get_all_backend_ids()` (snapshot taken at call time) and returns on first allowed backend, so the walker is bounded by the local copy size and cannot loop independently of the registry.

### Verification & follow-up

- [x] **[P2] libFuzzer harnesses for the audit's input boundaries**
  Landed at `tests/fuzz/` — one harness each for `RadixTree::insert/lookup`, `RequestRewriter::extract_*`, and `StreamParser::push`. Build with `-DRANVIER_BUILD_FUZZERS=ON` (clang only); the `Dockerfile.fuzz` image is the recommended environment. See `tests/fuzz/README.md`.
  _30-min run results (2026-05-08):_
  - `radix_tree_fuzz`: clean, 4,959,251 execs → MITIGATED-BY-FUZZ for H8, L9.
  - `request_rewriter_fuzz`: first run crashed at ~564k execs (deeply-nested JSON → stack overflow). Surfaced as new ticket "M6 — UPGRADED-BY-FUZZ" above; fixed via `kParseIterativeFlag`. Post-fix run clean at 5,552,208 execs → MITIGATED-BY-FUZZ for M6, L5.
  - `stream_parser_fuzz`: blocked by Seastar/libFuzzer allocator interaction (Hard Rule #15 — Seastar overrides global new/delete; libFuzzer never boots the reactor). Crashed in libFuzzer's internal cleanup, not in StreamParser. Not a Ranvier bug. See `tests/fuzz/README.md` for the exact diagnosis. H10, M11, M12 remain at static MITIGATED.

- [ ] **[P3] Unblock Seastar-dependent fuzzing**
  _Where:_ `tests/fuzz/stream_parser_fuzz.cpp` (and any future Seastar-touching harness).
  _Approach:_ rebuild Seastar with `-DSeastar_USE_DEFAULT_ALLOCATOR=ON` in a `Dockerfile.fuzz`-derived image. Trade-off: bypasses Seastar's per-shard allocator entirely, so any allocator-specific bug becomes invisible to the fuzzer; everything else (parsing, state machine, integer math) is correctly exercised. Worth the one-time ~30-min image build if H10/M11/M12 ever need to be promoted from static MITIGATED to MITIGATED-BY-FUZZ.
  _Complexity:_ Medium (one-time Docker layer + Seastar build).

- [ ] **[P2] Run unit tests + benchmarks under ASan/UBSan**
  _Justification:_ Catches the integer-overflow / cast findings (H2, H9) and any heap UAF along the request path with no harness work.
  _Approach:_ Add a `Dockerfile.sanitize` (or a flag in the existing test container) that builds with `-fsanitize=address,undefined`. Run the full unit-test suite and the smoke benchmarks. Triage any new failures separately from this audit.
  _Complexity:_ Low (build config) + variable (triage of any failures found).

- [x] **[P3] Re-run the audit after fixes land** — Completed 2026-05-10 — all 8 CONFIRMED fixes verified MITIGATED-BY-FIX against current source, no MITIGATED-item downgrades, slow-path sweep added 1 HIGH + 1 MED + 2 LOW under a single root cause (RapidJSON recursive parser missed by the M6 sweep on non-request-path JSON). See "Re-run (2026-05-10)" addendum near the top of `docs/audits/request-lifecycle-crash-audit.md`. New fix tickets opened below; LOWs in the closure addendum.

### P1 — Slow-path findings from the 2026-05-10 audit re-run

- [x] **[P1] S1: Apply `kParseIterativeFlag` to cache-event JSON parser** — Fixed 2026-05-10. `src/cache_event_parser.hpp:64` now passes `<rapidjson::kParseIterativeFlag>` to `Document::Parse(...)`, matching the M6 pattern. The iterative parser keeps state on the heap (its `MemoryPoolAllocator`), bounded by the existing request-body cap, so a deeply-nested `POST /v1/cache/events` body can no longer stack-overflow regardless of auth state.

### P2 — Slow-path findings from the 2026-05-10 audit re-run

- [x] **[P2] S2: Apply `kParseIterativeFlag` to discovery-layer JSON parsers** — Fixed 2026-05-10. All four cited sites now pass `<rapidjson::kParseIterativeFlag>`: `src/local_discovery.cpp:214` (backend `/v1/models`), `src/k8s_discovery_service.cpp:687` (EndpointSlice list), `src/k8s_discovery_service.cpp:974` (EndpointSlice watch event — line drifted by one), `src/k8s_discovery_service.cpp:1094` (generic K8s JSON helper). Same one-line fix shape as S1 and M6.

### Closed by triage (no action — recorded for traceability)

The following findings from the original audit were re-read against current source and judged MITIGATED. The audit doc carries the per-item evidence.

- **HIGHs MITIGATED:** H4 (TokenizerThreadPool promise race — promises set before `_pending_jobs` clear), H6 (`live_backends[0]` — empty check is at the immediately preceding line), H8 (ART `prefix[MAX_PREFIX_LENGTH]` — buffer is over-allocated), H10 (`_chunk_bytes_needed + 2` — `max_chunk_size` cap upstream).
- **MEDs MITIGATED:** M1 (streaming-lambda `[this]` — gate holder lifetime), M2 (optional-units pattern — guarded), M8 (modulo on live_backends — empty check at preceding line), M12 (Content-Length parse — comparator is safe for valid `size_t`), M13 (`bundle.is_valid` — single-coroutine access only), M15 (non-EPIPE rethrow — Seastar-handler-safe by design).
- **LOWs from 2026-05-10 audit re-run (defensive only, no action required):**
  - **S3** (`src/sqlite_persistence.cpp:362-367`) — `save_routes_batch()` returns plain `bool` for any `sqlite3_step()` failure; `SQLITE_FULL` / `SQLITE_IOERR_*` cannot be distinguished from transient `SQLITE_BUSY`. Not a crash (rollback keeps the table consistent and route writes are best-effort) but disk-full silently drops every subsequent batch. Defensive fix would branch on the rc and emit distinct metrics. LOW.
  - **S4** (`src/local_discovery.cpp:328`, `src/local_discovery.hpp:86`) — `_next_backend_id` is `int32_t` (alias `BackendId`) starting at 10000 and incremented per discovered backend. Signed-int overflow at 2^31 is UB but requires ~2.1B churns over the process lifetime; the `MAX_KNOWN_BACKENDS = 64` cap bounds concurrent membership but not the monotonic counter. Realistically unreachable; recorded for completeness. LOW.

---

---

## References

- [Ranvier Architecture](./architecture.md)
- [API Reference](./api-reference.md)
- [Request Flow Diagrams](./request-flow.md)
- [Integration Test Guide](../tests/integration/README.md)


---
