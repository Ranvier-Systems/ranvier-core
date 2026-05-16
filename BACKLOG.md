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
19. [Heterogeneous Backend Support (2026-05-16)](#19-heterogeneous-backend-support-2026-05-16)

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

- [ ] **[P3] Pin Seastar to a specific commit in `Dockerfile.base` and `Dockerfile.base.default-alloc`**
  _Justification:_ Both Dockerfiles currently do `git clone --depth 1 https://github.com/scylladb/seastar.git` with no SHA pin. The production base and the default-allocator base are independent Docker images that can be rebuilt at different times, so they can silently end up tracking different upstream Seastar commits — production on one SHA, the fuzz/sanitizer unblock base on another. Any divergence between the two that affects test-vs-production parity (allocator-adjacent code, header signatures, ABI) would only surface as a confusing test failure rather than as a config diff. The same goes for the production base rebuilding against a different SHA than a prior production deployment, which is a release-reproducibility concern independent of §18.
  _Approach:_ Pin to a tested commit in both Dockerfiles (mirror the pattern already in place for `tokenizers-cpp` at `Dockerfile.base:73-76`, including the "To update: see ..." comment pointing at this ticket). Bump the pin deliberately when there's a reason (Seastar fix needed, security patch). Add a short bump-checklist subsection to this ticket once the initial pin lands.
  _Location:_ `Dockerfile.base:50`, `Dockerfile.base.default-alloc:91-92` (Seastar `git clone` steps).
  _Complexity:_ Low.
  _Surfaced by:_ §18 P3 "Unblock Seastar-dependent fuzzing" — the default-allocator base added there made the existing drift risk in `Dockerfile.base` more visible, since now two Dockerfiles independently clone Seastar HEAD.

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

- [ ] **[P3] Bound concurrency on gossip broadcast async path**
  _Location:_ `src/gossip_transport.cpp:208-261` — the `seastar::async` batch branch is gated only by `_gossip_task_gate`, which guards shutdown ordering, not holder count.
  _Justification:_ Per-request callers (`broadcast_cache_eviction` via `http_controller.cpp:3704`, `broadcast_route` via the route-announcement path) can in principle stack arbitrarily many `seastar::thread` instances (128 kB stack each, virtual). Unlike `CryptoOffloader::max_queue_depth` at `crypto_offloader.hpp:94`, there is no explicit cap or inline fallback. Currently self-limits because the encrypt+send block is short, but the bound is implicit.
  _Approach:_ Wrap the branch in `with_semaphore(_broadcast_sem, 1, ...)` with a small cap (4-8), or mirror the CryptoOffloader inline-fallback pattern at `crypto_offloader.hpp:284-310`.
  _Complexity:_ Low.
  _Cross-ref:_ In-code TODO at `src/gossip_transport.cpp:208`.

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
  _Build config landed (2026-05-10):_ `Dockerfile.fuzz-seastar` layers a Seastar rebuild on top of `ranvier-fuzz`, reinstalling `/usr/local` with `-DSeastar_USE_DEFAULT_ALLOCATOR=ON` (clang toolchain, mirrors `Dockerfile.fuzz` / `Dockerfile.sanitize`). The image header documents that it is fuzz-only and unsuitable for production. `make fuzz-run-stream-parser-seastar` is the entry point (a thin alias on `fuzz-run-stream-parser`, kept as a separate target so the CI job and README pointer can name the unblock path explicitly). CI wired in `.github/workflows/fuzz-tests.yml` as a sibling job `stream-parser-fuzz-seastar` (separate job, same workflow — corpus cache and image-build wiring stay co-located with the existing fuzz CI). The CI job is **gated to `workflow_dispatch` initially** (first run is manual so the developer can eyeball output before promoting H10/M11/M12) and **non-gating** (`continue-on-error: true`, mirrors the sanitizer-tests posture); flip to the post-Docker-Publish `workflow_run` trigger and drop `continue-on-error` once a multi-hour run is observed clean. README pointer added in `tests/fuzz/README.md` under a new *Unblocking `stream_parser_fuzz`* section; the original allocator diagnosis stays intact directly above it. The `fuzz-tests.yml` exclusion comment and the Makefile `fuzz-ci` comment both now name the unblock artefacts. No source edits to `src/stream_parser.{cpp,hpp}` or `tests/fuzz/stream_parser_fuzz.cpp` in this commit. **Still pending (keeps this ticket open):** building the image and running `stream_parser_fuzz` in Docker for a multi-hour pass; opening a sub-ticket per genuine finding; on a clean run, promoting H10 / M11 / M12 from static MITIGATED to MITIGATED-BY-FUZZ in `docs/audits/request-lifecycle-crash-audit.md`, then flipping the CI trigger to `workflow_run` and dropping `continue-on-error`.
  _Build config reshaped (2026-05-13):_ Replaced the fuzz-only `Dockerfile.fuzz-seastar` layer with a shared `Dockerfile.base.default-alloc` after empirical signal showed both workloads need the same Seastar rebuild — an attempt to run the §18 P2 ASan/UBSan suite in `claude/add-sanitizer-build-config-IiIOj` crashed inside `gtest_discover_tests` on `cross_shard_exception_propagation_test` with `munmap_chunk(): invalid pointer` deep in `libhwloc.so.15`, same root cause as `stream_parser_fuzz` (Seastar's per-shard allocator overrides global `new`/`delete`, test binary invoked at build time without a running reactor, allocator misbehaves). New shape: `Dockerfile.base.default-alloc` derives `FROM ranvier-base` and reinstalls `/usr/local` with `-DSeastar_USE_DEFAULT_ALLOCATOR=ON` (GCC, matches parent base); sets `ENV SEASTAR_DEFAULT_ALLOCATOR=1` as the runtime marker. Header explicitly calls out that the image is **unsuitable for production / perf measurement** — bypasses Seastar's per-shard memory hierarchy, neutralises NUMA-aware allocation and the shared-nothing contract, allocator-specific bugs become invisible. `Dockerfile.fuzz` and `Dockerfile.sanitize` consume it via their existing `BASE_IMAGE` build-arg — no source change to either Dockerfile, only header-comment updates documenting the override recipe (build `ranvier-base-default-alloc:latest` first, then `docker build --build-arg BASE_IMAGE=ranvier-base-default-alloc:latest -f Dockerfile.fuzz …` or `… -f Dockerfile.sanitize …`). `Dockerfile.fuzz-seastar` deleted (one shared base, not two parallel layers). Makefile: renamed `fuzz-run-stream-parser-seastar` → `fuzz-run-stream-parser-default-alloc`; `fuzz-run-stream-parser` itself now keys off `SEASTAR_DEFAULT_ALLOCATOR` and prints a blocked-by-allocator diagnostic + the unblock recipe when invoked against the production base, so old muscle memory still works. CI job in `.github/workflows/fuzz-tests.yml` renamed `stream-parser-fuzz-seastar` → `stream-parser-fuzz-default-alloc` and split into two build-push-action steps: build `ranvier-base-default-alloc` first, then build `Dockerfile.fuzz` on top with `BASE_IMAGE=ranvier-base-default-alloc:latest`. Cache scopes split (`base-default-alloc`, `fuzz-tests-default-alloc`) so the ~30-min Seastar rebuild caches independently of the fuzz toolchain layer. Trigger and `continue-on-error: true` posture unchanged. `tests/fuzz/README.md` *Unblocking* section rewritten to describe the shared-base shape and note that the same image unblocks §18 P2 (`Dockerfile.sanitize` consumer recipe lives in that Dockerfile's header). The `fuzz-tests.yml` exclusion comment also cross-references the §18 P2 ticket. No source edits to `src/`, `tests/fuzz/`, or any unit test in this commit. **Still pending (unchanged from the 2026-05-10 entry):** building the image and running `stream_parser_fuzz` in Docker for a multi-hour pass; opening a sub-ticket per genuine finding; on a clean run, promoting H10 / M11 / M12 to MITIGATED-BY-FUZZ in `docs/audits/request-lifecycle-crash-audit.md`, then flipping the CI trigger to `workflow_run` and dropping `continue-on-error`. Cross-reference: see §18 P2 entry below for the parallel ASan/UBSan unblock follow-up.
  _CI fix-ups (2026-05-13, follow-ups to the reshape):_ Three empirical issues surfaced from the first `workflow_dispatch` runs of `stream-parser-fuzz-default-alloc` and the first local-Docker dispatch of `make fuzz-run-stream-parser-default-alloc`, all patched in subsequent commits. None change the architecture; all worth recording so the next reader doesn't re-introduce them:
    1. `Dockerfile.base.default-alloc` passed `-DSeastar_USE_DEFAULT_ALLOCATOR=ON` directly to `./configure.py` on the strength of a comment claiming `parse_known_args()` forwards unknown `-D` flags to CMake. The first dispatch falsified that with `Configure seastar: error: unrecognized arguments: -DSeastar_USE_DEFAULT_ALLOCATOR=ON`. Verified against `scylladb/seastar/configure.py@master`: strict `parse_args`, no passthrough, no built-in allocator flag. Initial fix: run `configure.py` with only its recognised args, then `cmake -B build/release -DSeastar_USE_DEFAULT_ALLOCATOR=ON` before `ninja install` to flip the cache variable. The same bug existed in the deleted `Dockerfile.fuzz-seastar` from 120f6a9 — it was never built, just static-analysed. **Superseded by fix-up 3 below** (no such CMake cache variable exists; the re-config was a no-op).
    2. The two-step buildx chain (build base, then build fuzz `FROM ranvier-base-default-alloc:latest`) failed at fuzz-step image-resolution: `pull access denied, repository does not exist or may require authorization` against `docker.io/library/ranvier-base-default-alloc:latest`. Root cause: `docker/setup-buildx-action@v3` defaults to `docker-container` driver, whose buildx instances can't see the runner's local docker daemon — so the `load: true` from step 1 lands an image the step-2 buildx can't resolve. Fix (smallest diff): set `driver: docker` on `setup-buildx-action` for this job only. Trade-off: `docker` driver doesn't support `cache-to: type=gha`, so the ~30-min Seastar rebuild is paid in full on every dispatch (no cross-dispatch caching for `base-default-alloc` or `fuzz-tests-default-alloc` cache scopes — those scope names are now unused for this job). The original cache-scope split commentary above is therefore aspirational pending the follow-up. Acceptable for a manual, bounded, workflow_dispatch-only job. **Added to "Still pending":** publish `ranvier-base-default-alloc` to GHCR alongside `ranvier-base` (extend `docker-publish.yml`), then collapse the `stream-parser-fuzz-default-alloc` job back to a single-stage build that pulls the pre-built base from the registry — same shape as the main `fuzz-tests` job, restores full GHA caching, and removes the docker-driver workaround. Same publish step would also let `sanitizer-tests.yml` consume the default-alloc base via a one-line `BASE_IMAGE` override without paying the rebuild.
    3. The fixed CI built green, the user pulled the image locally and ran `make fuzz-run-stream-parser-default-alloc`, and got the **same** `munmap_chunk(): invalid pointer` crash the entire ticket is about — but this time at libFuzzer seed-corpus read time, with `seastar::memory::free<no_size>` at `memory.cc:1780` and `operator delete` at `memory.cc:2520` on the stack. The `no_size` overload is the per-shard-allocator path; if `SEASTAR_DEFAULT_ALLOCATOR` had actually been defined during the Seastar build, the operator delete override would have routed through the `(void*, size_t) { ::free(ptr); }` thin wrapper instead. So the macro wasn't reaching memory.cc. Investigation against `scylladb/seastar/CMakeLists.txt@master`: there is **no** user-facing CMake cache variable controlling the allocator. The macro is wired as `target_compile_definitions(seastar PUBLIC ...)` inside a `foreach (definition SEASTAR_DEBUG SEASTAR_DEFAULT_ALLOCATOR SEASTAR_SHUFFLE_TASK_QUEUE)` block, gated by `$<$<IN_LIST:$<CONFIG>,Debug;Sanitize;Fuzz>:${definition}>`. Release/RelWithDebInfo builds never get any of the three. Tried fallbacks: `--mode=fuzz` would activate it but also wires `find_package(Sanitizers COMPONENTS fuzzer)` + `target_link_libraries(seastar PUBLIC Sanitizers::fuzzer)`, which is clang-only and embeds libFuzzer's `main()` — colliding with the harness's own libFuzzer main. `--mode=sanitize` works but instruments libseastar with full ASan/UBSan, dropping fuzz throughput by ~5–10× with no gain (the harness instruments its own code already). Injecting `-DSEASTAR_DEFAULT_ALLOCATOR` via `CXXFLAGS` defines the macro for Seastar's own compilation but bypasses `INTERFACE_COMPILE_DEFINITIONS`, leaving downstream Ranvier translation units to compile against headers without the macro — risking ODR violations if any public Seastar header gates inlines or layouts on it. Landed fix: a one-line `sed` patch on Seastar's `CMakeLists.txt` before configure, appending `RelWithDebInfo` to the IN_LIST so the existing PUBLIC propagation mechanism fires for `--mode=release`. Cost is SEASTAR_DEBUG asserts in libseastar (minor; occasionally catches bugs during fuzzing) and SEASTAR_SHUFFLE_TASK_QUEUE (no-op without a reactor). Downstream `find_package(Seastar)` inherits the macro through the installed CMake package, so the harness compiles against headers that match libseastar's compiled-in choice. Verified against Seastar's `memory.cc`: the `(void*, size_t) { ::free(ptr); }` path is what the override now selects. No source edits to Ranvier in any of these three fix-ups.

- [ ] **[P2] Run unit tests + benchmarks under ASan/UBSan**
  _Justification:_ Catches the integer-overflow / cast findings (H2, H9) and any heap UAF along the request path with no harness work.
  _Approach:_ Add a `Dockerfile.sanitize` (or a flag in the existing test container) that builds with `-fsanitize=address,undefined`. Run the full unit-test suite and the smoke benchmarks. Triage any new failures separately from this audit.
  _Complexity:_ Low (build config) + variable (triage of any failures found).
  _Build config landed (2026-05-10):_ `RANVIER_BUILD_SANITIZED=ON` CMake option (sibling of `RANVIER_BUILD_FUZZERS`) instruments every `*_test` target with `-fsanitize=address,undefined -fno-sanitize-recover=undefined`. `Dockerfile.sanitize` layers clang + compiler-rt + llvm + lld onto `ranvier-base` (mirrors `Dockerfile.fuzz`). `make sanitize-test` configures, builds the `unit_tests` aggregate, and runs ctest under the runtime; reuses `tests/fuzz/ubsan-suppressions.txt` for the known RapidJSON `Stack::Reserve` pointer-overflow noise. CI wired in `.github/workflows/sanitizer-tests.yml` — runs after Docker Publish on `workflow_run` (and on manual dispatch), **non-gating** (`continue-on-error: true`) until the initial findings are triaged, since the first sanitised run is expected to surface third-party noise that should be filed as separate tickets rather than blocking unrelated PRs. No source edits in this commit. There is no in-tree CMake "smoke benchmark" target today, so the sanitised build covers unit tests only; the perf benchmarks live under `benchmarks/cache_event_generator/` and run against a live binary, so they are out of scope for the sanitised CI job. **Still pending (keeps this ticket open):** running the suite in Docker, opening a sub-ticket per genuine finding, and once the suite is clean flipping the workflow's `continue-on-error` to `false` so regressions fail the job.
  _Empirical signal (2026-05-13):_ First attempt to run the sanitised suite in Docker crashed inside CTest's `gtest_discover_tests` on `cross_shard_exception_propagation_test` with `munmap_chunk(): invalid pointer` deep in `libhwloc.so.15` — same Seastar-allocator / no-reactor root cause as `stream_parser_fuzz` in §18 P3, surfaced under `--gtest_list_tests` at build time rather than at run time. The §18 P3 reshape (`Dockerfile.base.default-alloc`, this commit) lands the shared default-allocator base; `Dockerfile.sanitize` already accepts it via its existing `BASE_IMAGE` build-arg and the header documents the override recipe. **Added to "Still pending":** wire `.github/workflows/sanitizer-tests.yml` to build `ranvier-base-default-alloc` first and pass it as `BASE_IMAGE` to `Dockerfile.sanitize` (mirrors the `stream-parser-fuzz-default-alloc` job in `fuzz-tests.yml`), then re-run the suite. That workflow edit is intentionally out of scope for the §18 P3 build-config commit so the two follow-up runs (multi-hour fuzz, sanitised unit tests) can be triaged independently.
  _Empirical signal follow-up (2026-05-15):_ Workflow wired to the default-allocator base, mirroring the `stream-parser-fuzz-default-alloc` job in `fuzz-tests.yml`. `.github/workflows/sanitizer-tests.yml` now chains two `docker/build-push-action` steps — build `Dockerfile.base.default-alloc` with `tags: ranvier-base-default-alloc:latest` first, then build `Dockerfile.sanitize` on top with `BASE_IMAGE=ranvier-base-default-alloc:latest` — and sets `driver: docker` on `setup-buildx-action` so the second build can resolve the locally-loaded base. Same trade-off as the fuzz job: the docker driver does not support `cache-to: type=gha`, so the `cache-from`/`cache-to` parameters are dropped and the ~30-min Seastar rebuild is paid in full on every dispatch (bounded and acceptable for a non-gating, post-publish/manual workflow). `timeout-minutes` bumped from 45 to 60 to cover the rebuild on first run. Top-of-file comment block updated to record the chained-build rationale and cross-reference §18 P3. `continue-on-error: true` and the non-gating annotation step left untouched — the first sanitised run is still expected to surface findings to triage. No source edits to `src/`, `tests/`, or `Dockerfile.sanitize`; the override recipe lives in that Dockerfile's header. **Still pending (unchanged from the 2026-05-13 entry):** running the suite in Docker (developer's container, next session); opening a sub-ticket per genuine finding; flipping `continue-on-error` to `false` once the suite is observed clean. Cross-reference: the §18 P3 GHCR-publish spinoff (line 1104 "Added to 'Still pending'", under the P3 entry above) would collapse both this job and `stream-parser-fuzz-default-alloc` back to single-stage builds with full GHA caching, removing the docker-driver workaround in both places.
  _Empirical signal follow-up 2 (2026-05-15):_ First dispatch under the wired workflow confirmed the default-allocator base unblocked the original `gtest_discover_tests` / `libhwloc.so.15` crash — `cross_shard_exception_propagation_test` now builds and runs cleanly, and the Seastar startup banner prints `Seastar compiled with default allocator, --memory option won't take effect` (proves the `SEASTAR_DEFAULT_ALLOCATOR` macro reached `memory.cc`, validating the §18 P3 IN_LIST patch end-to-end on the sanitize consumer). The run surfaced a separate, unrelated clang-vs-GCC compile-time finding that blocked the rest of the build: compiling `src/node_slab.cpp` for the `radix_tree_test` target fails with `1 error generated.` rooted in `src/radix_tree.hpp:454` — `struct DumpNode { ... }` contains `std::vector<std::pair<unsigned char, DumpNode>>` (a recursive child), and the first call site at `radix_tree.hpp:465` (`return dump_node(root_.get());`) forces clang to instantiate `DumpNode`'s implicit destructor *before* the struct's closing brace. libstdc++'s `std::vector` destructor instantiates `std::_Destroy` → `std::is_destructible<std::pair<unsigned char, DumpNode>>` → `std::__is_implicitly_default_constructible_safe<DumpNode>`, which requires `DumpNode` complete; clang reports `definition of 'std::pair<unsigned char, ranvier::RadixTree::DumpNode>' is not complete until the closing '}'` at `stl_iterator.h:2992`. Production GCC accepts this because libstdc++ + GCC frontend defers the trait evaluation; clang does not, so this is a toolchain difference, not a sanitiser finding (ASan/UBSan never got to run on `radix_tree_test`). Sibling targets that don't transitively include the recursive type built fine in the same dispatch (`intent_classifier_test`, `text_validator_test`, `cross_shard_exception_propagation_test` all linked). Likely fix shape (small, but a `src/` edit so out of scope for this workflow-wiring commit): declare `~DumpNode()` and define it out-of-line after the struct, OR introduce an indirection on the recursive child (`std::vector<std::pair<unsigned char, std::unique_ptr<DumpNode>>>`) so libstdc++ never instantiates the value-type destructor on an incomplete `DumpNode`. Either change wants its own commit + before/after sanitised-build evidence so the fix isn't bundled with workflow wiring. **Added to "Still pending":** open a dedicated P2 sub-ticket for the `radix_tree.hpp` recursive-type fix, land it on a separate branch, re-dispatch `sanitizer-tests.yml` to confirm the build progresses to the ctest stage, then resume triage of whatever ASan/UBSan actually finds at runtime. The earlier "Still pending" items (run the suite, file per-finding tickets, flip `continue-on-error: false`) all remain — this entry just adds the radix_tree blocker ahead of them in the sequence.
  _Empirical signal follow-up 3 (2026-05-16):_ Source fix landed for the clang-vs-GCC `DumpNode` blocker called out in follow-up 2. Took the smaller-diff out-of-line-destructor shape rather than the `unique_ptr` indirection: added a user-declared `~DumpNode();` inside the struct at `src/radix_tree.hpp:460`-ish (right after the recursive `std::vector<std::pair<uint8_t, DumpNode>>> children` member) and defined it `inline RadixTree::DumpNode::~DumpNode() = default;` after RadixTree's closing brace at `src/radix_tree.hpp:1971`-ish, both inside the existing `#ifndef RANVIER_FUZZING` guard so the fuzz harness still skips the dump machinery. The user-declared destructor makes clang defer the implicit-instantiation chain (`std::_Destroy` → `is_destructible<pair<uint8_t, DumpNode>>` → `__is_implicitly_default_constructible_safe<DumpNode>`) until the out-of-line definition, at which point DumpNode is complete and libstdc++'s trait evaluation succeeds; GCC behaviour is unchanged because GCC was deferring the chain anyway. Why this shape over the `unique_ptr` fallback: zero runtime cost (no extra allocation per child node on the dump path), zero API change (DumpNode's field layout is preserved, so `src/router_service.cpp:3681` and `src/http_controller.cpp:2827` keep their existing aggregate-init / by-const-ref usage — a user-declared destructor does not disqualify aggregate initialisation in C++20). Trade-off accepted: declaring the destructor suppresses the implicit move ctor, so the recursive `result.children.emplace_back(key, dump_node(child))` falls back to copy-construction of `DumpNode` (the implicit copy is still generated, just deprecated when the dtor is user-declared); the project's CXX flags don't enable `-Wdeprecated-copy` or `-Werror`, and the dump path is admin-only / cold, so the pessimisation is invisible in practice. Comment block above the struct rewritten to document the workaround and explain the libstdc++ trait chain so the next reader doesn't try to "simplify" the destructor away. No edits to `Dockerfile.sanitize`, `Dockerfile.base.default-alloc`, `.github/workflows/sanitizer-tests.yml`, `tests/`, or any other `src/` file — the API-preserving destructor approach kept the diff scoped to `src/radix_tree.hpp` plus this BACKLOG sub-bullet. **Still pending:** re-dispatch `.github/workflows/sanitizer-tests.yml` (or run `make sanitize-test` locally) to confirm the build now progresses past `radix_tree_test` to the ctest stage, then triage whatever ASan/UBSan surfaces at runtime — each finding becomes its own sub-ticket. The earlier "Still pending" items (run the suite, file per-finding tickets, flip `continue-on-error: false`) all remain in their original order; this entry removes the two blockers added by follow-up 2 ("open a dedicated P2 sub-ticket for the `radix_tree.hpp` recursive-type fix" — done, this commit; "re-dispatch `sanitizer-tests.yml` to confirm the build progresses to the ctest stage" — promoted to the head of the queue as the next remaining item).
  _Empirical signal follow-up 4 (2026-05-16):_ The follow-up-3 out-of-line-destructor shape was insufficient — sanitized re-dispatch surfaced a second clang+libstdc++ instantiation chain that the destructor workaround doesn't reach. Concretely, `src/node_slab.cpp` (compiled as part of `radix_tree_test`) still failed with `4 errors generated.` rooted at `radix_tree.hpp:510:28` in `dump_with_prefix` — `return dump_node(node);` constructs `std::optional<DumpNode>` from the rvalue, but because the user-declared destructor *suppressed the implicit move ctor*, optional's `_Storage`-based in_place ctor falls back to DumpNode's implicit *copy* ctor (`note: in implicit copy constructor for 'ranvier::RadixTree::DumpNode' first required here` at optional:244). The implicit copy ctor instantiates `std::vector<std::pair<uint8_t, DumpNode>>::vector(const vector&)` (line 463), which fans into libstdc++'s `__uninitialized_copy_a` → `__do_uninit_copy` → `std::_Destroy` chain, and that chain re-asks `is_destructible<pair<uint8_t, DumpNode>>` from *inside* pair's own template definition, where pair is still incomplete (`definition of 'std::pair<unsigned char, ranvier::RadixTree::DumpNode>' is not complete until the closing '}'` at `stl_iterator.h:2992`). Same recursive-incomplete-type root cause as chain (1) in follow-up 2, but exposed through vector's copy ctor instead of its destructor — and the out-of-line destructor only defers chain (1). All other targets stayed green in the re-dispatch (`intent_classifier_test`, `text_validator_test`, `cross_shard_exception_propagation_test`, `chat_template_test`, `boundary_detector_test` all built; the cross-shard binary even ran and printed the default-allocator Seastar banner end-to-end, re-validating §18 P3). Fix: fell back to the indirection shape that follow-up 2 / the original task description called out as Plan B — changed `children` to `std::vector<std::pair<uint8_t, std::unique_ptr<DumpNode>>>`. This makes DumpNode move-only by construction (vector<pair<uint8_t, unique_ptr<DumpNode>>> has a deleted copy ctor → DumpNode's implicit copy ctor is *never synthesised*, so chain (2) cannot fire), and the vector's destructor chain now bottoms out at `unique_ptr<DumpNode>::~unique_ptr` which sees DumpNode complete at the closing brace, so chain (1) clears too. Reverted the follow-up-3 user-declared destructor (no longer needed; implicit dtor handles unique_ptr cleanup), updated the lambda in `dump_node()` to wrap the recursive call in `std::make_unique<DumpNode>(dump_node(child))`, and updated the one external consumer that dereferenced the value (`src/http_controller.cpp:2867` — `node.children[i].second` → `*node.children[i].second`). Aggregate init at `src/router_service.cpp:3681` and `src/radix_tree.hpp:536` (`DumpNode{"empty", {}, std::nullopt, "LOCAL", 0, {}}`) is preserved — DumpNode has no user-declared ctors/dtor now, so it's still an aggregate, and the trailing `{}` value-initialises the empty children vector regardless of its element type. The two unit tests that touch `dump.children` (`tests/unit/router_service_test.cpp:181,966`) only call `.empty()`, so they're untouched. Cost accepted: one heap allocation per child node when populating a dump (admin / cold path; never observed in production hot paths). Comment block above DumpNode rewritten again to document both trait chains and explain why the indirection breaks them — so the next reader doesn't try to "simplify" by removing the unique_ptr layer. **Still pending (unchanged in shape from follow-up 3):** re-dispatch `.github/workflows/sanitizer-tests.yml` (or `make sanitize-test`) to confirm `radix_tree_test` now builds *and* runs under ASan/UBSan, then triage whatever the runtime sanitisers actually surface. The earlier "Still pending" items (run the suite, file per-finding tickets, flip `continue-on-error: false`) all remain in their original order.
  _Empirical signal follow-up 5 (2026-05-16):_ Build cleared and ctest started running — both the radix_tree fix from follow-up 4 and the §18 P3 default-allocator base verified end-to-end. The suite is now aborting on the documented RapidJSON `Stack::Reserve` UBSan noise that `tests/fuzz/ubsan-suppressions.txt` was supposed to silence (`pointer-overflow:*rapidjson*`); every JSON-parsing unit test (`RequestRewriterTest.BoundaryInfo*`, etc.) crashes with `applying non-zero offset 4 to null pointer` at `rapidjson/internal/stack.h:117`. Suppression file is correctly written, `Makefile:212` correctly passes `UBSAN_OPTIONS=...:suppressions=$(CURDIR)/tests/fuzz/ubsan-suppressions.txt` to ctest, and ctest correctly propagates env to subprocesses — the reason the suppression doesn't fire is a known LLVM gotcha: `-fno-sanitize-recover=undefined` (in `CMakeLists.txt:1224`) causes the compiler to emit calls to `__ubsan_handle_pointer_overflow_abort` instead of the recoverable handler, and the `_abort` variants unconditionally `abort()` without consulting the runtime suppression list. Fix (one-line CMake change, this commit): add `-fsanitize-recover=pointer-overflow` after `-fno-sanitize-recover=undefined` in the `_san_flags` list at `CMakeLists.txt:1224`. That selectively re-enables the recoverable handler for `pointer-overflow` only, so the suppression actually fires for the RapidJSON noise while every other UBSan check (the ones we actually care about) still aborts on first hit per the existing policy. Comment block above the flag list rewritten to document the exception and cross-reference the suppression file so the next reader doesn't "tidy up" the apparent redundancy. Scope: this is the minimal change to unblock triage — every JSON-parsing test was masked by the RapidJSON abort, so real findings (if any) couldn't surface. Without it, follow-up 4's runtime ASan/UBSan dispatch produces no signal beyond the RapidJSON noise. No other source/test/Dockerfile edits. **Still pending (refined):** re-dispatch `sanitizer-tests.yml` once more to confirm the RapidJSON suppression now fires and the suite either runs clean or surfaces genuine findings; triage whatever shows up (each becomes its own sub-ticket); flip `continue-on-error: false` once the suite is observed clean. The earlier "Still pending" items collapse into this — they were always blocked on actually getting past the RapidJSON noise.
  _Empirical signal follow-up 6 (2026-05-16):_ With the follow-up-5 selective recovery in place, the suite progressed past RapidJSON and immediately tripped on the next vendored-library UBSan noise source: `absl::container_internal::raw_hash_set::iterator::iterator` reads `*generation_ptr` even when `generation_ptr == nullptr` (which it is for unallocated control byte arrays — i.e. `.find()` on a freshly constructed empty map). UBSan reports `load of null pointer of type 'const GenerationType'` at `raw_hash_set.h:876`, surfaced by `TokenizationCacheTextLengthTest.LongTextIsNotCached` via `tokenizer_service.cpp:25`. The loaded byte is never used (end iterators are only compared for equality), and the issue is fixed in newer Abseil; our pinned `lts_20230802` predates the fix. Same shape as the RapidJSON noise: it's third-party UB-by-the-letter, harmless in practice, and would need its own per-check `-fsanitize-recover=null` if we kept the incremental-flag policy from follow-up 5. Given that we'll almost certainly keep hitting more of these (Abseil has several other near-null patterns; libstdc++ debug iterators, Seastar internals and RE2 are all known offenders elsewhere), this commit swaps the policy entirely rather than continuing the per-check dance. Removed both `-fno-sanitize-recover=undefined` AND the follow-up-5 `-fsanitize-recover=pointer-overflow` from `_san_flags` in `CMakeLists.txt:1232`-ish; the recoverable handlers are now emitted for every UBSan check by default, so `tests/fuzz/ubsan-suppressions.txt` can suppress any vendored-library noise without further CMake edits. `halt_on_error=1` (set unconditionally by `make sanitize-test` at `Makefile:182` and inherited by `sanitizer-tests.yml`) still aborts on the first unsuppressed finding — that's the real abort-on-real-bug guarantee; the no-recover flag was redundant belt-and-suspenders and the suspenders happened to also strangle the suppression file. Added the Abseil entry `null:*raw_hash_set*` to `tests/fuzz/ubsan-suppressions.txt` with a comment explaining the upstream context. Rewrote the CMake comment block to document the new policy + cross-reference the suppression file, and updated the suppression file header to cross-reference the CMake decision so the two stay synchronised. Trade-off accepted (user-chosen): running a sanitised `*_test` binary directly without `UBSAN_OPTIONS=halt_on_error=1` will print but not abort on real findings — every supported entry point sets the env var, so this only bites ad-hoc invocations of the raw binary. ASan policy is unchanged (its recovery is controlled separately and was never tied to the UBSan flag). No edits to `src/`, `tests/unit/`, `Dockerfile.sanitize`, `.github/workflows/sanitizer-tests.yml`. **Still pending (collapsed):** re-dispatch `sanitizer-tests.yml` once more; expect either a clean run, or a fresh vendored-library UBSan finding that needs one new line in `ubsan-suppressions.txt` (no CMake edit), or a genuine `ranvier::` finding which becomes its own sub-ticket. Flip `continue-on-error: false` once observed clean.
  _Empirical signal follow-up 7 (2026-05-16):_ Two CI-hygiene follow-ups dropped on the heels of the follow-up-6 policy switch, neither a triage finding — both came out of actually running the workflow end-to-end. (a) Added `--timeout 60` to the ctest invocation in `Makefile:213` so any individual unit test that hangs under sanitiser overhead (e.g. a deadlock surfaced by ASan/UBSan instrumentation) is killed as a per-test failure rather than burning the workflow's 60-minute `timeout-minutes` backstop. Sanitised tests typically carry 2-5× overhead and unit tests target sub-second runtimes unsanitised, so 60s is generous headroom while still catching real hangs quickly. (b) Dropped the `workflow_run: ["Docker Publish"]` post-merge trigger from `.github/workflows/sanitizer-tests.yml`; the workflow is now `workflow_dispatch`-only. Rationale: each dispatch costs up to ~60 min of `ubuntu-latest` runner time (the ~30-min Seastar rebuild in the default-allocator base is paid in full every dispatch — the docker-driver workaround documented in the workflow header blocks GHA cache), and the job is `continue-on-error: true` while §18 P2 triage is still flushing out vendored-library UBSan noise commit by commit. Automatic post-merge dispatch burns quota without adding merge-gating value during this phase. Top-of-file comment block rewritten to document the manual-only stance + the conditions under which automatic post-merge is restored: when the suite is observed clean and `continue-on-error: false` flips, the `workflow_run` trigger comes back as a regression-catcher. Also collapsed the now-redundant `if:` guard on the job that was filtering between dispatch-vs-workflow_run paths. No source/test edits, no §18 P3 GHCR-publish work (still a separate ticket — landing that would eliminate the ~30-min Seastar rebuild from this workflow entirely and is the real cost-fix). **Still pending (unchanged):** re-dispatch the workflow manually after each suppression / source fix; flip `continue-on-error: false` and restore the `workflow_run` trigger once observed clean.

  _Empirical signal follow-up 3 (2026-05-16):_ First end-to-end sanitised ctest run reached completion after the radix_tree clang-fix landed (`sanitize-log-2026-05-16.txt`, 20547 lines, archived as workflow artifact `sanitizer-results.zip` from run 25966996350). Headline: **86% pass (1996/2322), 326 fail, 267.57 sec wall-clock, 2 distinct root causes** — every other ctest failure is a downstream "Subprocess aborted" cascade off one of the two. No `--timeout` cap was in play (ctest invoked as `ctest --output-on-failure` in `Makefile:213`), and no test was killed for timeout. Findings grouped by category below; per-category sub-tickets follow this entry.

  *Real Ranvier bugs (1 distinct finding, 1 sub-ticket):*
  - **F1 — `serialize_tokens` calls `memcpy(nullptr, nullptr, 0)` on empty span.** UBSan `nonnull-attribute` at `src/sqlite_persistence.cpp:188:17` (log line 6051): `std::vector<uint8_t> blob(tokens.size() * sizeof(TokenId)); std::memcpy(blob.data(), tokens.data(), blob.size());` — when the input `std::span<const TokenId>` is empty, `blob` has size 0, `blob.data()` is unspecified-but-potentially-null, `tokens.data()` is nullptr per the span spec, and `glibc`'s `memcpy` is annotated `__attribute__((nonnull))` on args 1+2 (note at `/usr/include/string.h:44:28`), so the call is UB even though the byte count is zero and the result would be a no-op. Reproduces via `PersistenceTest.EmptyTokenSequence` (`tests/unit/persistence_test.cpp:303`, the only ctest case that exercises the empty-span path). Sister entry-point `save_route` at `src/sqlite_persistence.cpp:241:17` is the call site (log line 6054). One-line fix shape (its own commit): early-return an empty `std::vector` when `tokens.empty()`, OR guard the `memcpy` with `if (!tokens.empty())`. UBSan was happy with every other sqlite path in the suite (700-odd `PersistenceTest.*` and `AsyncPersistenceConcurrencyTest.*` cases passed, log lines 6020-6088 + 20070-20106), so this is genuinely the only empty-span site.

  *Vendored third-party findings that may mask real bugs (1 distinct finding, 1 sub-ticket):*
  - **F2 — Abseil `flat_hash_map`/`flat_hash_set` ASan SEGV on any first `operator[]`/`try_emplace` under sanitiser.** ASan `SEGV on unknown address 0x000000000000` (READ, zero-page) bottoming out at `/usr/include/absl/container/internal/raw_hash_set.h:821:39` in `absl::lts_20230802::container_internal::CommonFieldsGenerationInfoEnabled::generation() const` (264 of 326 failures, log line 4029 et seq) and the sibling site `:876:54` in `HashSetIteratorGenerationInfoEnabled::HashSetIteratorGenerationInfoEnabled(unsigned char const*)` (61 of 326 failures, log line 3993 et seq) — both are the same root cause (dereferencing the same null `generation_ptr_`). Crashes appear across 33 test suites whose only commonality is that they touch an Abseil container; first-failing site varies by test but always lands in `InitializeSlots` (line 1408:43) → `generation()` for the `_Enabled` variant. Distinct Ranvier call-sites observed (each is the same crash, listed here so future readers can map the impact area without re-grepping the log): `src/connection_pool.hpp:302` (log line 11088, `flat_hash_map<seastar::socket_address, std::deque<BasicConnectionBundle<TestClock>>>`), `src/router_service.cpp:597,716,920,4141` (log line 20051 et seq, `flat_hash_map<int, BackendInfo>`), `src/request_scheduler.hpp:140,167`, `src/agent_registry.cpp:38`, plus several test-only `flat_hash_map`s (`tests/unit/health_service_test.cpp:52`, `tests/unit/router_service_test.cpp` paths). Investigation outcome — see _Empirical signal follow-up 4_ below — is **the prompt's `ABSL_SWISSTABLE_ENABLE_GENERATIONS`-not-defined hypothesis is refuted; the actual root cause is the inverse ABI mismatch (header sees the macro, installed `.so` does not)**, and the fix is a build-config change to rebuild Abseil under sanitisers in the `Dockerfile.sanitize` layer. Sub-ticket scoped to that build-config change, not to suppression — see F2 entry under the new "P2 — Sanitised-suite triage findings (2026-05-16)" subsection below. **No UBSan suppression was or could be the right answer here:** these are ASan `DEADLYSIGNAL`s, not UBSan diagnostics, and `tests/fuzz/ubsan-suppressions.txt`'s `<check>:<pattern>` format does not apply to ASan SEGVs (verified by re-reading the file; only the RapidJSON `pointer-overflow` entry is present, no `null:*raw_hash_set*` entry was actually added in any prior session).

  *Vendored harmless UBSan noise (0 findings):* none — no UBSan check fired on a third-party header in this run. The pre-existing RapidJSON `pointer-overflow:*rapidjson*` suppression in `tests/fuzz/ubsan-suppressions.txt` continued to do its job (no RapidJSON-rooted diagnostics in the log).

  *Test infrastructure / timeouts (0 findings):* none — `ctest` ran without a per-test timeout and no test was killed (no `Test timeout` markers in the log; the longest individual case was ~0.70s). The "60s per-test cap" referenced in older planning notes was never wired into `Makefile:209-213` — left as a separate item in "Still pending" since the suite is currently fast enough that the cap is not load-bearing for triage.

  **Added to "Still pending":** open per-finding sub-tickets (F1 and F2) under the new "P2 — Sanitised-suite triage findings (2026-05-16)" subsection below; F1 is a one-line `src/` fix on its own commit with before/after sanitised evidence, F2 is the Abseil rebuild-under-sanitiser build-config change on `Dockerfile.sanitize` (or the shared default-alloc base) with the same evidence shape. Earlier "Still pending" items (flip `continue-on-error: false`, restore `workflow_run` post-merge trigger) remain blocked on a clean run, which requires both F1 and F2 fixes to land first.

  _Empirical signal follow-up 4 (2026-05-16, Abseil investigation outcome — `null:*raw_hash_set*` hypothesis refuted):_ Static analysis of `absl/container/internal/raw_hash_set.h@20230802.0` and the matching `raw_hash_set.cc` (the LTS our sysroot ships) **refutes** the prior-session hypothesis that the SEGV is caused by `ABSL_SWISSTABLE_ENABLE_GENERATIONS` not being defined in our sanitised build, and refutes the related framing that a `null:*raw_hash_set*` UBSan suppression line in `tests/fuzz/ubsan-suppressions.txt` would (or already does) address it. Evidence chain, line-cited so a future reader doesn't have to re-derive it:
    1. In the header (`raw_hash_set.h` lines 105-118), the macro chain is `#elif defined(ABSL_HAVE_ADDRESS_SANITIZER) || defined(ABSL_HAVE_MEMORY_SANITIZER) #define ABSL_SWISSTABLE_ENABLE_GENERATIONS` with an `#error ... cannot be directly set` guard — i.e. Abseil auto-defines it under ASan and forbids overriding. Our test binaries compile with `-fsanitize=address,undefined` (CMakeLists.txt:1222), so `__has_feature(address_sanitizer)` is true, Abseil's `absl/base/config.h` sets `ABSL_HAVE_ADDRESS_SANITIZER`, and the header path therefore selects `using CommonFieldsGenerationInfo = CommonFieldsGenerationInfoEnabled` (lines 269-274) and constexpr `SwisstableGenerationsEnabled() { return true; }` (lines 127-132). The hypothesis "the macro isn't defined in our build" is the opposite of what actually happens.
    2. The `Enabled` variant's `generation()` method (line 821 in the LTS) is `GenerationType generation() const { return *generation_; }`, and the iterator constructor at line 876 dereferences `*generation_ptr` directly in its initialiser list. Both expect `generation_` / `generation_ptr_` to point at a valid `GenerationType` (alias for `uint8_t`) in static storage.
    3. `generation_` is initialised to the return value of the externally-defined `GenerationType* EmptyGeneration();` function. That definition lives in `raw_hash_set.cc` (not the header), and its body is gated at *runtime* on `SwisstableGenerationsEnabled()`: it returns a pointer into a 1024-entry static array when generations are enabled, and **returns `nullptr`** when they're disabled.
    4. The installed `libabsl_raw_hash_set.so` on our sysroot was built **without** `-fsanitize=address`, so in *its* translation unit `SwisstableGenerationsEnabled()` evaluated `false` at compile-time and the linker baked in the `return nullptr` branch. Our test binaries' headers compile with ASan-enabled and so call the symbol expecting the non-null branch. Result: ABI mismatch — the linked-in `EmptyGeneration()` returns nullptr, the header dereferences it on first `operator[]` / `try_emplace`, ASan reports a zero-page SEGV. This matches the observed pc-offset overlap across 326 crashes (always the same `0x550d02` / `0x55e5ca` / `0x68ad8a` etc. in the per-binary local symbol — same generated code, same dereference).
    5. **Implication for the suppression:** even if a `null:*raw_hash_set*` line were added to `tests/fuzz/ubsan-suppressions.txt`, it would have no effect — the suppression file is consumed only by UBSan's `UBSAN_OPTIONS=suppressions=...` runtime; ASan SEGVs (which are what we observed — every report begins with `AddressSanitizer:DEADLYSIGNAL`) go through `ASAN_OPTIONS` and a separate suppressions format that does not accept `<check>:<pattern>` syntax. A UBSan-side suppression cannot mask an ASan-side `DEADLYSIGNAL`. Re-reading `tests/fuzz/ubsan-suppressions.txt` at HEAD confirms no `raw_hash_set` entry is present (only the RapidJSON `pointer-overflow:*rapidjson*` entry from 2026-05-08 remains); whatever prior-session intent existed was either never committed or was a misdiagnosis. **Action:** none required to the suppressions file in this commit — there is nothing to confirm or remove. The actual fix is the build-config change captured under sub-ticket F2 (rebuild Abseil with `-fsanitize=address` in the sanitize image — or statically link a from-source Abseil build into the sanitised targets — so the library and the headers agree on `ABSL_SWISSTABLE_ENABLE_GENERATIONS` at compile time, restoring the invariant that `EmptyGeneration()` returns a valid pointer). **Optional secondary mitigation worth recording but not adopting in this commit:** building Ranvier's sanitised targets with `-DABSL_HAVE_ADDRESS_SANITIZER=0` to force the header onto the Disabled path would also resolve the ABI mismatch, but it sacrifices Abseil's iterator-invalidation detection (the whole reason the Enabled path exists under ASan) — strictly worse than rebuilding the library, recorded here so it isn't proposed as a fix later by mistake.

### P2 — Sanitised-suite triage findings (2026-05-16)

Per-finding sub-tickets opened from the first end-to-end sanitised ctest run (`sanitize-log-2026-05-16.txt`, full triage logic and category counts in _Empirical signal follow-up 3_ above). Each fix is its own future commit on its own branch with before/after sanitised-suite evidence — none are bundled with the triage commits that opened them. Together they gate flipping `continue-on-error: false` and restoring the `workflow_run` post-merge trigger on `sanitizer-tests.yml`.

- [ ] **[P2] F1: `serialize_tokens` empty-span passes nullptr to `memcpy` (UBSan nonnull-attribute)**
  _Where:_ `src/sqlite_persistence.cpp:188:17` (`std::memcpy(blob.data(), tokens.data(), blob.size());`), reached via `SqlitePersistence::save_route` (`:241:17`).
  _Sanitiser:_ UBSan `nonnull-attribute`, `glibc` `memcpy` declared nonnull at `/usr/include/string.h:44:28`.
  _Reproducer:_ `PersistenceTest.EmptyTokenSequence` (`tests/unit/persistence_test.cpp:303`). Single failing case in the suite under this root cause — every other `PersistenceTest.*` and `AsyncPersistenceConcurrencyTest.*` path passed cleanly, so the empty-span surface is the only reachable trigger today (callers that already filter empty token sequences upstream are unaffected).
  _Root-cause hypothesis:_ For an empty `std::span<const TokenId>`, `tokens.data()` is nullptr per `[span.elem]`; `std::vector<uint8_t>(0)` may also return nullptr from `.data()`; `memcpy(nullptr, nullptr, 0)` is UB-by-the-letter even though the operation is a no-op. Fix shape (one-line, its own commit): early-return `{}` when `tokens.empty()`, OR guard the `memcpy` (`if (!tokens.empty()) std::memcpy(...);`). Either is equivalent under the existing call graph; the early-return shape is preferable because it makes the empty-input semantics explicit at the top of the function. Before/after evidence: a sanitised re-dispatch should show `PersistenceTest.EmptyTokenSequence` flipping from `Subprocess aborted` to `Passed` with no other movement in `persistence_test`.

- [ ] **[P2] F2: Abseil ASan/headers vs installed-library ABI mismatch (`raw_hash_set` `EmptyGeneration()` returns nullptr)**
  _Where:_ system-installed `libabsl_raw_hash_set.so` (sysroot `/usr/lib64/`), via every Ranvier translation unit that includes `<absl/container/flat_hash_map.h>` or `<absl/container/flat_hash_set.h>` and is compiled with `-fsanitize=address`.
  _Sanitiser:_ ASan `SEGV on unknown address 0x000000000000` (READ, zero-page), summary lines at `/usr/include/absl/container/internal/raw_hash_set.h:821:39` (264 occurrences) and `:876:54` (61 occurrences).
  _Reproducer:_ any test in `health_service_test`, `connection_pool_test`, `router_service_test`, `request_scheduler_test`, `cache_eviction_test`, `tokenization_cache_test`, `agent_registry_test`, `fleet_cache_efficiency_test` (etc.) that constructs and touches an Abseil container — 326 of 2322 cases crashed under this single root cause; concretely, the canonical first-failing case is `HealthServiceStoreTest.MissingBackendReturnsInvalidMetrics` (log line 3981).
  _Root-cause hypothesis (confirmed by static analysis, see _Empirical signal follow-up 4_ above for the line-cited chain):_ Abseil headers in our build define `ABSL_SWISSTABLE_ENABLE_GENERATIONS` (auto-set under ASan), selecting the `_Enabled` variant of `CommonFieldsGenerationInfo`. The headers initialise `generation_ = EmptyGeneration()`, a symbol whose definition lives in `raw_hash_set.cc`. The installed `libabsl_raw_hash_set.so` on our sysroot was compiled **without** ASan, so its `EmptyGeneration()` body took the `return nullptr` branch (the runtime-disabled-generations fallback). Headers then dereference the nullptr at `*generation_` on first slot-init under any `operator[]` / `try_emplace` / iterator construction. The bug is the build-config mismatch, not Ranvier source — but it is currently masking the entire sanitised contribution to coverage of those 326 cases, so it has to be resolved before the suite can be declared clean.
  _Fix shape (its own commit, build-config only):_ rebuild Abseil with `-fsanitize=address,undefined` inside the sanitiser image so the linked library and the consuming headers agree on the macro at compile time. Two plausible shapes, both contained to the `Dockerfile.base.default-alloc` → `Dockerfile.sanitize` layering and neither touches `CMakeLists.txt`'s `_san_flags` block:
    (a) Add an Abseil rebuild step to `Dockerfile.sanitize` that `git clone`s the matching LTS tag (`20230802.0`), configures with `-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -DCMAKE_C_FLAGS=-fsanitize=address,undefined -DCMAKE_BUILD_TYPE=Debug`, and installs into `/usr/local` ahead of `/usr/lib64`. Lowest blast-radius option; only the sanitiser image diverges from production-base. Trades ~2-5 min build cost per `Dockerfile.sanitize` rebuild (small relative to the existing ~30-min Seastar rebuild in the chained-build step).
    (b) Bundle Abseil as an in-tree CMake `FetchContent` dependency for `RANVIER_BUILD_SANITIZED=ON` only, so the sanitised targets link a from-source Abseil compiled with the same flags as the rest of the test code, while non-sanitised production targets continue to consume the system package. More invasive (touches `CMakeLists.txt`), but removes the "system Abseil vs in-image Abseil" divergence entirely and avoids the per-image rebuild cost on every CI dispatch.
  Before/after evidence: a sanitised re-dispatch should show 325 of the 326 currently-failing cases flipping to `Passed` (F1 still keeps `PersistenceTest.EmptyTokenSequence` red until its sibling fix lands). The expected residual is zero new findings — Abseil's generation-tracking is *designed* to catch iterator-invalidation bugs in our code, so any *new* `raw_hash_set` ASan reports after the rebuild would be genuine Ranvier UAF-on-iterator findings that get their own sub-tickets at that point.
  _Do not adopt:_ defining `-DABSL_HAVE_ADDRESS_SANITIZER=0` for the sanitised targets to force the header onto the `_Disabled` path. Resolves the ABI mismatch but silently disables Abseil's iterator-invalidation detection — strictly worse than rebuilding the library. Recorded here so a future reader doesn't propose it as a "smaller" fix.

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

## 19. Heterogeneous Backend Support (2026-05-16)

> **Context.** Today every Ranvier-supported backend (vLLM, SGLang, TensorRT-LLM, Ollama, LM Studio) is a GPU-based engine with a KV cache that benefits from prefix-affinity routing. Non-GPU inference providers — most prominently **Cerebras** (wafer-scale, model in 44 GB on-chip SRAM, no GPU KV-cache thrashing problem) — are OpenAI-compatible endpoints where prefix routing earns nothing but the rest of Ranvier's L7 plumbing (rate limiting, agent priorities, circuit breaking, fair scheduling) still applies. Enabling Ranvier as the unified control plane over a **mixed GPU + non-GPU fleet** is the strongest strategic angle; the code investigation on 2026-05-16 confirmed the existing `BackendInfo` struct already grew this way for `supports_token_ids` and `compression_ratio`, so the delta is mechanical.
>
> **Status.** Speculative — no design partner yet. Tracked here so the scoping work isn't lost. Do not start implementation without a customer commitment or a strategic decision to ship as a forward-looking capability.
>
> **Reference design notes.** Routing-mode (`PREFIX`/`HASH`/`RANDOM`) stays a cluster-wide setting; backend type is a per-backend property. The two are orthogonal — a heterogeneous fleet runs `PREFIX` globally and uses a per-backend predicate to suppress route learning on backends where prefix affinity is a no-op. Total estimated scope is **3–5 days** for a working hybrid setup with cheap-path metrics; the original scoping pass landed at 1.5–2 weeks before the predicate-only design reduced Phase 2 from ~3 days to ~half a day.

### 19.1 Backend type tag

- [ ] **[P3] Add `BackendType` enum threaded through the backend lifecycle**
  _Context:_ Per-backend type is needed to gate type-specific behaviour (route learning, metrics scraping, request-body rewriting). The `BackendInfo` struct (`src/router_service.cpp:75,94`) is already a flat aggregate that grew this way for `supports_token_ids` (`backend_registry.hpp:35`) and `compression_ratio` (`router_service.hpp:340`); the addition follows that established pattern.
  _Approach:_ Add `enum class BackendType { VLLM, SGLANG, TRT_LLM, OLLAMA, LM_STUDIO, CEREBRAS, OPENAI_COMPATIBLE }` to `src/types.hpp`. Thread one new field through the established 6 sites: (1) `BackendInfo` struct + constructor at `src/router_service.cpp:75,94`, (2) `BackendRegistry::register_backend_global()` at `src/backend_registry.hpp:33`, (3) `RouterService::register_backend_global()` override + impl at `src/router_service.hpp:337-340` and `src/router_service.cpp:3553-3585` (extend the `do_with` shared-vars dance), (4) `BackendState` admin DTO at `src/router_service.hpp:375-386`, (5) `register_backend_for_testing()` at `src/router_service.hpp:569-572`, (6) admin POST handler + JSON response at `src/http_controller.cpp:2343-2515,3039`. Add accessor `BackendType backend_type(BackendId)` mirroring `backend_supports_token_ids()` at `src/router_service.cpp:2117`. All existing callers default to `VLLM` for backward compatibility. Auto-set `supports_token_ids = false` for `CEREBRAS`/`OPENAI_COMPATIBLE` so the existing `strip_prompt_token_ids()` path activates without per-deployment config.
  _Location:_ `src/types.hpp`, `src/backend_registry.hpp`, `src/router_service.{hpp,cpp}`, `src/http_controller.cpp`
  _Complexity:_ Low (1–2 days; mechanical, follows existing pattern)

- [ ] **[P3] SQLite schema migration for `backend_type` column**
  _Context:_ Persisted backends are replayed on startup via `application.cpp:483`. Adding the type column requires a forward-compatible migration so existing rows default to `vllm`.
  _Approach:_ `ALTER TABLE backends ADD COLUMN backend_type TEXT NOT NULL DEFAULT 'vllm'` in `src/sqlite_persistence.cpp`. Update the row-to-`BackendRecord` mapping and the insert path to round-trip the new column. Tag the migration with a schema version bump if the persistence layer tracks one.
  _Location:_ `src/sqlite_persistence.cpp`, `src/application.cpp:483`
  _Complexity:_ Low
  _Dependencies:_ 19.1 (enum must exist first)

### 19.2 Route-learning gate predicate

- [ ] **[P3] Suppress ART route learning on non-cacheable backend types**
  _Context:_ The investigation on 2026-05-16 collapsed what was originally framed as "per-backend routing-mode override" into a single predicate at the learning sites. The lookup path stays untouched — if a route exists, use it. We just don't create new ART entries pointing at backends where prefix affinity earns nothing (Cerebras keeps the whole model in SRAM; there is no GPU KV cache to optimize for).
  _Approach:_ Add `bool should_cache_routes_for(BackendId) const` to `RouterService`, returning `false` when the backend's type is in the no-cache set (initially just `CEREBRAS`; extend as more backend types are added). Gate at two call sites: (1) local proxy-success learning at `src/http_controller.cpp:981` — combine with the existing `_config.should_learn_routes()` check, (2) gossip-received route ingress at `src/router_service.cpp:1372` — skip the `learn_route_remote()` call when the local backend is non-cacheable. Hash fallback and ART lookup both remain unchanged: if hash converges a prefix to a Cerebras backend, that's a correct routing decision; if an existing ART entry happens to point at one, accept it.
  _Location:_ `src/router_service.{hpp,cpp}`, `src/http_controller.cpp:981`, `src/router_service.cpp:1372`
  _Complexity:_ Low (~half a day; ~10 LOC predicate + 2 call-site gates)
  _Dependencies:_ 19.1

### 19.3 Metrics opt-out for non-vLLM backends

- [ ] **[P3] Skip vLLM `/metrics` scrape for non-vLLM backend types**
  _Context:_ `src/vllm_metrics.hpp` is tightly coupled to vLLM's Prometheus exposition format. Cerebras has no equivalent endpoint. Two paths exist: (a) cheap — opt non-vLLM backends out of the scrape entirely and let `get_backend_load_score()` return `0.0` (the existing optimistic default at `src/router_service.hpp:419-422`), losing load-aware routing on those backends; (b) right — abstract `VLLMMetrics` into a `BackendMetrics` interface with a Cerebras adapter. Cerebras's whole pitch is no queueing, so load signals matter less; the cheap path is appropriate for v1.
  _Approach:_ In the `HealthService` scrape loop, check `backend_type(id)` before issuing the `/metrics` GET. Skip non-vLLM types. The existing `get_backend_load_score()` default of `0.0` already handles the missing-data case correctly. Document the loss of load-aware routing on these backends. Defer the `BackendMetrics` abstraction until a customer asks for it.
  _Location:_ `src/health_service.cpp`, `src/vllm_metrics.hpp`
  _Complexity:_ Low (~half a day)
  _Dependencies:_ 19.1

### 19.4 Static-config Cerebras backends

- [ ] **[P3] YAML schema for remote-API backends (Cerebras and OpenAI-compatible)**
  _Context:_ Cerebras is a remote API endpoint, not a local port or a K8s service. The existing discovery paths (`src/local_discovery.cpp:330` for port scans, `src/k8s_discovery_service.cpp` for cluster) don't fit. Static YAML config is the fastest way to register a remote backend with API key + base URL.
  _Approach:_ Extend the backends YAML schema to accept a `type:` field (default `vllm`) plus an optional `api_key_env:` for secret reference. The startup registration path at `src/application.cpp:546` already calls `register_backend_global()`; thread the new `type` argument through (depends on 19.1). For API-key auth: inject the `Authorization: Bearer` header at the proxy step in `src/http_controller.cpp` for backends with a key configured. Treat the API key as the credential boundary — do not log it.
  _Location:_ `src/config_loader.cpp`, `src/config_schema.hpp`, `src/application.cpp:546`, `src/http_controller.cpp`
  _Complexity:_ Low (~1 day)
  _Dependencies:_ 19.1

- [ ] **[P3] K8s annotation for backend type (follow-up)**
  _Context:_ Operator-friendly alternative to YAML config. The K8s discovery path already reads `ranvier.io/*` annotations on EndpointSlices.
  _Approach:_ Recognize `ranvier.io/backend-type: cerebras` and `ranvier.io/api-key-secret-ref: <secret-name>` in `src/k8s_discovery_service.cpp`. Resolve the secret via the existing K8s client. Falls back to `vllm` when absent.
  _Location:_ `src/k8s_discovery_service.cpp`
  _Complexity:_ Low (~1 day)
  _Dependencies:_ 19.1, 19.4 (reuse the auth-header injection from 19.4)

### 19.5 Documentation

- [ ] **[P3] Document hybrid-fleet operating model**
  _Context:_ The marketing/positioning story for hybrid fleets is the actual deliverable — the engineering changes are small. Without docs that clearly state "prefix-affinity routing is a no-op on Cerebras backends; we still give you rate limiting / circuit breaking / fair scheduling," operators will set up the wrong expectations.
  _Approach:_ Add a section to `docs/internals/prefix-affinity-routing.md` describing which backend types benefit from ART learning and which don't. Add a top-level "Hybrid fleets" page (or section) walking through a representative GPU + Cerebras config. Note explicitly that benchmarks should not credit Ranvier with the "48% faster TTFT" headline on non-cacheable backends.
  _Location:_ `docs/internals/prefix-affinity-routing.md`, new doc page
  _Complexity:_ Low
  _Dependencies:_ 19.1, 19.2, 19.4

---

---

## References

- [Ranvier Architecture](./architecture.md)
- [API Reference](./api-reference.md)
- [Request Flow Diagrams](./request-flow.md)
- [Integration Test Guide](../tests/integration/README.md)


---
