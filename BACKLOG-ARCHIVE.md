# Ranvier Core - Backlog Archive (Completed Items)

> Items completed and archived from BACKLOG.md on 2026-03-22.
> For active backlog items, see [BACKLOG.md](BACKLOG.md).

---

## Fully Completed Sections

## 7. Security Audit Findings (Adversarial System Audit)

> **Criticality Score: 0/10** _(was 6/10 on 2026-01-14; all issues resolved)_
> Generated: 2026-01-11 | Updated: 2026-01-14
> Audit Scope: `src/` directory against `docs/claude-context.md` constraints

Structural issues identified across 4 lenses: Async Integrity, Edge-Case Crash, Architecture Drift, and Scale & Leak.

### 7.0 Adversarial Audit Findings (2026-01-15)

All HIGH severity issues resolved. MEDIUM/LOW issues tracked below for future hardening.

#### Remaining Issues (Future Hardening)

- [x] **[MEDIUM] Add stale circuit entry cleanup when backends removed** ✓
  _Location:_ `src/circuit_breaker.hpp`, `src/router_service.cpp`
  _Severity:_ Medium
  _Fixed:_ Added `remove_circuit(BackendId)` method to CircuitBreaker, shard-local callback in RouterService called from `unregister_backend_global()`, Prometheus metric `circuit_breaker_circuits_removed_total` (Rule #4)

- [x] **[MEDIUM] Consider lock-free queue for AsyncPersistenceManager** ✓
  _Location:_ `src/async_persistence.cpp`
  _Severity:_ Medium (acceptable tradeoff)
  _Fixed:_ Replaced mutex-guarded deque with lock-free SPSC ring buffer. See §21.3.

- [x] **[LOW] Audit _pending_acks cleanup in GossipService** (completed 2026-01-16)
  _Location:_ `src/gossip_service.cpp`
  _Severity:_ Low

#### Completed Issues (2026-01-14)

#### Async Integrity Violations

- [x] **Replace blocking std::ifstream with Seastar async I/O in K8s CA cert loading** ✓
  _Location:_ `src/k8s_discovery_service.cpp:224-265`
  _Severity:_ **Critical**
  _Fixed:_ PR #149 - Replaced with Seastar async file I/O (open_file_dma, make_file_input_stream)

- [x] **Add gate guard to connection pool reaper timer** ✓
  _Location:_ `src/connection_pool.hpp:108-127`
  _Severity:_ Medium
  _Fixed:_ PR #151 - Added _timer_gate, stop() closes gate before canceling timer (Rule #5)

#### Edge-Case Crash Scenarios

- [x] **Add null assertion in metrics() accessor** ✓
  _Location:_ `src/metrics_service.hpp:485-490`
  _Severity:_ Medium
  _Fixed:_ PR #152 - Implemented lazy-init pattern ensuring accessor never returns null (Rule #3)

#### Scale & Leak Vulnerabilities

- [x] **Fix memory leak in thread-local MetricsService allocation** ✓
  _Location:_ `src/metrics_service.hpp:495-501`
  _Severity:_ **High**
  _Fixed:_ PR #151 - Added stop_metrics() that calls stop() then deletes g_metrics (Rule #6)

- [x] **Cap per-backend metrics map to prevent unbounded growth** ✓
  _Location:_ `src/metrics_service.hpp:418-425`
  _Severity:_ **High**
  _Fixed:_ PR #150 - Added MAX_TRACKED_BACKENDS constant, overflow counter, fallback metrics (Rule #4)

- [x] **Add circuit cleanup when backends are removed** ✓
  _Location:_ `src/circuit_breaker.hpp:42, 59-74`
  _Severity:_ **High**
  _Fixed:_ PR #150 - Added MAX_CIRCUITS=10000 constant, bounds check in allow_request(), fail-open strategy (Rule #4)

- [x] **Fully erase map entries in connection pool clear_pool()** ✓
  _Location:_ `src/connection_pool.hpp:298-318`
  _Severity:_ Medium
  _Fixed:_ PR #152 - Verified _pools.erase(it) called at line 312, cleanup_expired() tracks empty pools (Rule #4)

- [x] **Add MAX_TRACKED_PEERS limit to gossip dedup structures** ✓
  _Location:_ `src/gossip_service.cpp:1208-1215`
  _Severity:_ Medium
  _Fixed:_ PR #150 - Added MAX_DEDUP_PEERS constant, bounds check, overflow counter (Rule #4)

### 7.1 Async Integrity Violations (No Locks/Async Only)

- [x] **Remove blocking mutex from `queue_depth()` in AsyncPersistenceManager** ✓
  _Location:_ `src/async_persistence.cpp:231-234`
  _Severity:_ High
  _Fixed:_ PR #118 - Replaced with `std::atomic<size_t>` for lock-free access

- [x] **Replace sequential awaits with parallel_for_each in K8s discovery** ✓
  _Location:_ `src/k8s_discovery_service.cpp:413-424`
  _Severity:_ Medium
  _Fixed:_ PR #122 - Added `max_concurrent_for_each` with 16-op limit, per-endpoint error handling

- [x] **Audit 16+ mutex usages in SQLite persistence layer** ✓
  _Location:_ `src/sqlite_persistence.cpp` (multiple locations)
  _Severity:_ Medium
  _Fixed:_ PR #123 - Documented threading model, added friend declarations and private constructor for compile-time access control

### 7.2 Edge-Case Crash Scenarios

- [x] **Add null check before sqlite3_column_text dereference** ✓
  _Location:_ `src/sqlite_persistence.cpp:156`
  _Severity:_ High
  _Fixed:_ PR #119 - Added `safe_column_text()` helper, skip records with NULL required fields

- [x] **Add port validation before stoi conversion in K8s discovery** ✓
  _Location:_ `src/k8s_discovery_service.cpp:243`
  _Severity:_ Medium
  _Fixed:_ PR #124 - Added `parse_port()` helper with validation (1-65535 range), 22 unit tests

- [x] **Handle DNS resolution exceptions in K8s discovery** ✓
  _Location:_ `src/k8s_discovery_service.cpp:276-279`
  _Severity:_ Medium
  _Fixed:_ PR #126 - Async DNS resolver with retry, exponential backoff, caching, and graceful degradation

- [x] **Fix silent exception swallowing in annotation parsing** ✓
  _Location:_ `src/k8s_discovery_service.cpp:682-686`
  _Severity:_ Low
  _Fixed:_ PR #135 - Log warnings with annotation name/value, add max constants, clamp out-of-range values

- [x] **Eliminate global static state race in tracing service** ✓
  _Location:_ `src/tracing_service.cpp:40-44`
  _Severity:_ Medium
  _Fixed:_ PR #127 - std::call_once for init, std::atomic for enabled flag, shutdown guard

- [x] **Add DNS resolution timeout in backend registration** ✓
  _Location:_ `src/http_controller.cpp:1469`
  _Severity:_ Medium
  _Fixed:_ Wrapped with `seastar::with_timeout()` using `lowres_clock` deadline; catches `seastar::timed_out_error` separately with clear error message; configurable via `dns_resolution_timeout_seconds` (default: 5s) in YAML and `RANVIER_DNS_RESOLUTION_TIMEOUT_SECONDS` env var

### 7.3 Architecture Drift

- [x] **Move token count limits from persistence layer to business layer** ✓
  _Location:_ `src/sqlite_persistence.cpp:173-196`
  _Severity:_ Low
  _Fixed:_ PR #137 - Added max_route_tokens config, validate in learn_route_global/remote, persistence only stores

- [x] **Move routing mode decisions from HttpController to RouterService** ✓
  _Location:_ `src/http_controller.cpp:506-566`
  _Severity:_ Medium
  _Fixed:_ PR #128 - RouteResult struct, unified route_request() API, single error path

- [x] **Encapsulate thread_local variables into ShardLocalState struct** ✓
  _Location:_ `src/router_service.cpp:46-79`
  _Severity:_ Low
  _Fixed:_ PR #136 - Unified ShardLocalState with Stats/Config structs, init/reset lifecycle, reset_for_testing()

### 7.4 Scale & Leak Vulnerabilities

- [x] **Add bounds checking to pending remote routes buffer** ✓
  _Location:_ `src/router_service.cpp:631-643`
  _Severity:_ High
  _Fixed:_ PR #120 - Added MAX_BUFFER_SIZE (10000) with batch-drop strategy and overflow metric

- [x] **Add connection pool max age reaping** ✓
  _Location:_ `src/connection_pool.hpp`
  _Severity:_ Medium
  _Fixed:_ PR #130 - Added created_at timestamp, is_too_old(), max_connection_age config (default 5min)

- [x] **Add RAII guards for timer callbacks** ✓
  _Location:_ `src/async_persistence.cpp:106-112`, `src/gossip_service.cpp`
  _Severity:_ Medium
  _Fixed:_ PR #131 - Added _timer_gate to both services, callbacks acquire holder, stop() closes gate first

- [x] **Add lifetime guards for metrics lambda captures** ✓
  _Location:_ `src/router_service.cpp:227-228`
  _Severity:_ Medium
  _Fixed:_ PR #132 - Added stop() that calls _metrics.clear() first, deregisters before destruction

- [x] **Add upper bound to StreamParser accumulator** ✓
  _Location:_ `src/stream_parser.cpp:26`
  _Severity:_ Medium
  _Fixed:_ PR #133 - Added max_accumulator_size (1MB), early check before append, rejection counter

---


## 11. System Architecture Audit (2026-02-02)

> **Audit Date:** 2026-02-02
> **Scope:** Holistic system audit covering architecture layers, Hard Rules (0-15), and async integrity

### 11.1 Audit Summary

| Category | Status | Issues |
|----------|--------|--------|
| Architecture Layers | ✓ Compliant | 0 violations |
| Hard Rules 0-7 | ⚠ Violations | 7 issues |
| Hard Rules 8-15 | ⚠ Violations | 27+ issues |
| Async Integrity | ⚠ Mixed | 3 issues |

### 11.2 Hard Rules Compliance Matrix

```
[X] Compliant: Rules #1, #3, #4, #6, #7, #8, #11, #13, #14, #15
[!] Violations Found: Rules #0, #2, #5, #9, #10, #12
[?] Not Applicable: (none)
```

### 11.3 Critical Violations (Requiring Immediate Attention)

#### Rule #5: Timer callbacks with [this] must acquire gate guards

- [x] **[CRITICAL] RouterService TTL timer missing gate guard** ✓
  _Completed:_ 2026-02-03. Added `_timer_gate` member, gate holder acquisition in `run_ttl_cleanup()`, and gate closure in `stop()`.

- [x] **[CRITICAL] RouterService batch flush timer missing gate guard** ✓
  _Completed:_ 2026-02-03. Shares `_timer_gate` with TTL timer; holder kept alive via `do_with`.

- [x] **[CRITICAL] RouterService draining reaper timer missing gate guard** ✓
  _Completed:_ 2026-02-03. Shares `_timer_gate` with TTL and batch flush timers.

- [x] **[HIGH] K8sDiscoveryService poll timer uses boolean instead of gate** ✓
  _Completed:_ 2026-02-03. Reuses existing `_gate` member; holder kept alive via `do_with`. Updated `stop()` to close gate before cancelling timer.

#### Rule #9: Every catch block must log at warn level

- [x] **[CRITICAL] 11 silent catch blocks missing warn-level logging** ✓
  _Completed:_ 2026-02-03. All 11 locations addressed with appropriate logging levels.

#### Rule #10: No bare std::stoi/stol/stof on external input

- [x] **[CRITICAL] 16 bare std::stoi/stoul calls on external input**
  _Completed:_ 2026-02-03. Created `src/parse_utils.hpp` with `parse_int32()`, `parse_uint32()`, `parse_port()`, `parse_token_id()`, `parse_backend_id()` using `std::from_chars`. All 16 locations across 5 files fixed.

### 11.4 Medium Severity Violations

#### Rule #0: Prefer unique_ptr over shared_ptr

- [x] **[MEDIUM] std::shared_ptr in Application**
  _Completed:_ 2026-02-03. Changed to `std::unique_ptr` - no shared ownership needed, all access is through `this` pointer in lambdas.

- [x] **[MEDIUM] std::shared_ptr in TracingService (OpenTelemetry)**

#### Rule #2: No co_await inside loops over external resources

- [x] **[HIGH] Sequential co_await in K8s endpoint removal loop**
  _Completed:_ 2026-02-03. Refactored to collect UIDs first, then process with `max_concurrent_for_each()` using `K8S_MAX_CONCURRENT_ENDPOINT_OPS` concurrency limit.

#### Rule #12: No std::ifstream/ofstream in Seastar code

- [x] **[MEDIUM] Blocking std::ifstream during startup**

### 11.5 Async Integrity Issues

#### Sequential Health Checks (Rule #2 Related)

- [x] **[HIGH] Health service sequential backend checks**
  _Completed:_ 2026-02-03. Refactored to collect backends first, then check with `max_concurrent_for_each()` using 16-way concurrency. Added per-check try/catch for resilience.

#### Fire-and-Forget Loop Lifecycle

- [x] **[HIGH] Health service run_loop() not properly tracked**
  _Completed:_ 2026-02-03. Store loop future in `_loop_future` member, hold gate for entire loop lifetime, and `co_await` both gate close and loop future in `stop()` for clean shutdown.

### 11.6 Architecture Compliance (PASSED)

The codebase maintains excellent layer separation:
- **Controller Layer** (http_controller.cpp): Only calls services, no direct persistence access ✓
- **Service Layer** (router_service, tokenizer_service, gossip_service): Contains all business logic ✓
- **Persistence Layer** (sqlite_persistence, async_persistence): Only stores/retrieves data ✓
- **Gossip Layer**: Uses callback pattern, no direct persistence calls ✓

### 11.7 Positive Patterns Observed

1. ✓ FFI boundary reallocations (Rule #15) properly implemented in tokenizer_thread_pool.cpp
2. ✓ Cross-shard data transfer uses foreign_ptr (Rule #14) in router_service.cpp
3. ✓ Metrics deregistration in stop() (Rule #6) across all services
4. ✓ Bounded containers with MAX_SIZE checks (Rule #4) in gossip_protocol.cpp
5. ✓ Null-guard helper for SQLite (Rule #3) in sqlite_persistence.cpp

---

## Completed Items

_Move completed items here with completion date and PR reference._

| Date | Item | PR |
|------|------|----|
| 2026-02-22 | **[Performance]** Fix cross-shard load sync SMP storm (3x → 2x tokenization regression). Incremental snapshot updates, vector serialization, default interval 100ms, feature disabled by default. Flush interval sweep confirmed 10ms as optimal (2-50ms tested). | - |
| 2026-02-13 | **[Security]** Add bounds checking to K8s discovery service to prevent OOM (Rule #4). MAX_RESPONSE_SIZE (16MB) for API responses, MAX_LINE_SIZE (1MB) for watch stream buffer, MAX_ENDPOINTS (1000) for endpoints map. Overflow counter metrics added. | #134 |
| 2026-02-01 | **[DX]** Add inline Hard Rule documentation to radix_tree.hpp. Comprehensive documentation for Rules #1, #4, #9, #14 covering lookup, insert, eviction, and slab allocation code paths. | #212 |
| 2026-02-01 | **[DX]** Add inline Hard Rule documentation to router_service.cpp. Documentation for Rules #1, #5, #6, #14 covering route_request, learn_route_global, get_backend_for_prefix, and timer callbacks. | #208 |
| 2026-02-01 | **[DX]** Consolidate 8 gossip debug metrics behind RANVIER_DEBUG_METRICS compile flag. Reduces Prometheus scrape overhead in production. | #209 |
| 2026-02-04 | **[DX]** Automated benchmark regression testing. GitHub Actions workflow runs Locust (100 users, 60s) against docker-compose.test.yml with pre-built GHCR image. Compares P99 latency (≤10% regression) and throughput (≤5% drop) against baseline. Posts results as PR comment. Manual trigger to update baseline. | - |
| 2026-01-31 | **[Testing]** E2E prefix routing test suite. Created `tests/integration/test_prefix_routing.py` with 8 comprehensive tests validating core value proposition: cache affinity, route learning, metrics verification, cluster propagation. | - |
| 2026-01-31 | **[Performance]** Offload tokenizer FFI via dedicated thread pool. Added `TokenizerWorker` and `TokenizerThreadPool` classes using `boost::lockfree::spsc_queue` and `seastar::alien::run_on()` for non-blocking tokenization. Per-shard worker threads with dedicated tokenizer instances. `encode_threaded_async()` API with priority fallback (cache → thread pool → cross-shard → local). Disabled by default (P3). Unit tests added. | - |
| 2026-01-29 | **[Performance]** Offload tokenizer FFI via cross-shard dispatch. Added `encode_cached_async()` with round-robin shard selection. On cache miss, dispatches tokenization to different shard via `smp::submit_to`, freeing calling reactor. Rule #14 compliant (text copied, sharded pointer captured). Prometheus metrics for dispatch tracking. | - |
| 2026-01-19 | **[Refactor]** Extract GossipService into three focused modules: GossipConsensus (~430 LOC), GossipTransport (~540 LOC), GossipProtocol (~870 LOC). Thin orchestrator (~350 LOC). Rule #14 compliant cross-shard dispatch. Robustness fixes for gate holder scoping and exception handling. | - |
| 2026-01-16 | **[Security Audit 7.0]** Add circuit entry cleanup when backends are deregistered (remove_circuit method, shard-local callback, Prometheus metric) | - |
| 2026-01-15 | **[Fix]** Use async I/O for config hot-reload to prevent reactor stalls (Rule #12), add 10s rate limiting | - |
| 2026-01-15 | **[Feature]** Add automatic cleanup timer to RateLimiter (Rule #5 gate guard pattern) | #158 |
| 2026-01-15 | **[Security]** Add MAX_BUCKETS bound to RateLimiter to prevent memory exhaustion (Rule #4) | #157 |
| 2026-01-15 | **[Feature]** Implement tree compaction for NodeSlab memory reclamation (removes empty nodes, shrinks oversized nodes) | #155 |
| 2026-01-15 | **[Feature]** Add fail-open mode for split-brain handling (random routing during quorum loss) | #156 |
| 2026-01-14 | **[Security Audit 7.0.1]** Replace blocking std::ifstream with async Seastar I/O in K8s CA cert loading | #149 |
| 2026-01-14 | **[Security Audit 7.0.2]** Add bounds checking to prevent OOM from unbounded containers (_per_backend_metrics, _circuits, _received_seq_sets) | #150 |
| 2026-01-14 | **[Security Audit 7.0.3]** Add timer gate guards and metrics deregistration for safe shutdown (connection_pool, metrics_service) | #151 |
| 2026-01-14 | **[Security Audit 7.0.4]** Add null-safety guards (metrics() lazy-init) and map entry cleanup (connection_pool clear_pool erase) | #152 |
| 2026-01-11 | **[Security Audit 7.3.1]** Move token count limits to business layer (max_route_tokens config) | #137 |
| 2026-01-11 | **[Security Audit 7.3.3]** Encapsulate thread_local into ShardLocalState (unified lifecycle, reset_for_testing) | #136 |
| 2026-01-11 | **[Security Audit 7.2.4]** Fix silent exception swallowing in annotation parsing (log warnings, clamp values) | #135 |
| 2026-01-11 | **[Security Audit 7.4.5]** Add upper bound to StreamParser accumulator (1MB limit, early rejection) | #133 |
| 2026-01-11 | **[Security Audit 7.4.4]** Add lifecycle guards for RouterService metrics lambdas (stop() clears metrics first) | #132 |
| 2026-01-11 | **[Security Audit 7.4.3]** Add RAII guards for timer callbacks (_timer_gate in AsyncPersistence/GossipService) | #131 |
| 2026-01-11 | **[Security Audit 7.4.2]** Add connection pool max age reaping (TTL, created_at, is_too_old) | #130 |
| 2026-01-11 | **[Security Audit 7.3.2]** Move routing mode decisions to RouterService (RouteResult struct, unified API) | #128 |
| 2026-01-11 | **[Security Audit 7.2.5]** Eliminate tracing service global static race (call_once, atomic, shutdown guard) | #127 |
| 2026-01-11 | **[Security Audit 7.2.3]** Handle DNS resolution exceptions in K8s discovery (async resolver, retry, caching) | #126 |
| 2026-01-11 | **[Security Audit 7.2.2]** Add port validation in K8s discovery (parse_port helper, 22 unit tests) | #124 |
| 2026-01-11 | **[Security Audit 7.1.3]** Audit SQLite mutex threading model (documentation, friend declarations, private constructor) | #123 |
| 2026-01-11 | **[Security Audit 7.1.2]** Parallelize K8s endpoint reconciliation (max_concurrent_for_each, 16-op limit) | #122 |
| 2026-01-11 | **[Security Audit 7.4.1]** Add bounds checking to pending remote routes buffer (MAX_BUFFER_SIZE=10000, batch-drop, overflow metric) | #120 |
| 2026-01-11 | **[Security Audit 7.2.1]** Add null guards for sqlite3_column_text (safe_column_text helper, skip NULL records) | #119 |
| 2026-01-11 | **[Security Audit 7.1.1]** Replace blocking mutex with atomic in queue_depth() (std::atomic for lock-free access) | #118 |
| 2026-01-11 | Optimize StreamParser with read-position tracking (zero-copy parsing, buffer compaction, size limits) | - |
| 2026-01-11 | Add validated factory functions for CrossShardRequestContext (size limits, memory estimation) | - |
| 2026-01-10 | Create rvctl CLI tool for Ranvier management (inspect routes/backends, cluster status, drain, route add) | #110 |
| 2026-01-09 | Refactor CryptoOffloader for clarity (unified template handling, queue depth limiting, dedicated execute_inline/offloaded methods) | - |
| 2026-01-09 | Add radix tree performance metrics (lookup hits/misses, node counts, slab utilization, path compression avg) | - |
| 2026-01-09 | Quorum enforcement and DTLS lockdown (recently-seen quorum check, mTLS lockdown mode, sequence number hardening) | - |
| 2026-01-08 | Implement shard-aware P2C load balancer (per-shard metrics, cross-shard dispatch, zero-copy transfer) | - |
| 2026-01-07 | Implement system-wide backpressure mechanism (semaphore concurrency limits, persistence queue integration, gossip gate protection) | - |
| 2026-01-06 | Use Seastar async file I/O for tokenizer loading (DMA file I/O, validation, proper cleanup) | - |
| 2026-01-06 | Encapsulate SQLite store within AsyncPersistenceManager (clean ownership, lifecycle methods) | - |
| 2026-01-06 | Refactor configuration for Seastar sharded container (per-core lock-free access, hot-reload) | - |
| 2026-01-06 | Implement Seastar-native signal handling (SIGINT hard kill, SIGTERM graceful, SIGHUP reload) | - |
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


## 12. Test Coverage Gaps (2026-02-06)

> **Analysis:** 851 unit tests across 20 files, 4 integration suites, 5 validation scripts.
> **Finding:** 14 source files (25%) have zero unit tests. 5 systematic weaknesses affect all tests.
>
> **Conventions for all test tasks below:**
> - Read `CLAUDE.md` and `.dev-context/claude-context.md` first (coding conventions, 16 Hard Rules).
> - **Do not build.** Static analysis only — Seastar deps are too heavy for sandbox.
> - Tests go in `tests/unit/<name>_test.cpp`. Use Google Test (`gtest`). Follow the pattern in `tests/unit/text_validator_test.cpp` (include header, `using namespace ranvier;`, `TEST_F` with fixture classes).
> - Wire the new test into `CMakeLists.txt` following the pattern of nearby pure-C++ tests (e.g., `text_validator_test`): `add_executable`, `target_include_directories(... PRIVATE ${CMAKE_SOURCE_DIR}/src)`, `target_link_libraries(... GTest::gtest_main)`, `gtest_discover_tests(...)`. Add `.cpp` source files from `src/` only if the header under test is not header-only.
> - Namespace is `ranvier`. All source headers are in `src/`.

### 12.1 P0 — New Unit Tests for Untested Pure-Function Files

- [x] **Add `parse_utils_test.cpp`** ✓
  _Completed:_ 2026-02-06. 5 fixture classes, ~40 test cases covering `parse_int32`, `parse_uint32`, `parse_port`, `parse_token_id`, `parse_backend_id`.

- [x] **Add `logging_test.cpp`** ✓
  _Completed:_ 2026-02-06. 3 fixture classes, ~25 test cases. Introduced `tests/stubs/` directory with minimal Seastar/fmt stubs for headers with lightweight Seastar deps.

- [x] **Add `shard_load_metrics_test.cpp`** ✓
  _Completed:_ 2026-02-06. 5 fixture classes, ~25 test cases covering RAII guards, load scoring, snapshot semantics, lifecycle, and type traits.

### 12.2 P1 — New Unit Tests for Core Infrastructure

- [x] **Add `metrics_service_test.cpp`** ✓
  _Completed:_ 2026-02-06. 7 fixture classes, ~30 test cases. Added `seastar/core/metrics.hh` and `metrics_registration.hh` stubs with histogram types and labeled metric overloads.

- [x] **Add `node_slab_test.cpp`** ✓
  _Completed:_ 2026-02-06. 7 fixture classes, ~30 test cases covering allocate/deallocate, free-list LIFO, peak tracking, auto-growth, make_node factories, and thread-local accessors.

- [x] **Add `cross_shard_request_test.cpp`** ✓
  _Completed:_ 2026-02-06. 8 fixture classes, ~35 test cases. Added `temporary_buffer`, `shared_ptr`/`foreign_ptr`, `http::request`/`reply` stubs. Updated `future.hh` and `smp.hh` stubs.

- [x] **Add `tracing_service_test.cpp` (W3C traceparent parsing only)** ✓
  _Completed:_ 2026-02-06. 7 fixture classes, ~35 test cases. Compiled with `RANVIER_NO_TELEMETRY`; W3C parse logic provided directly in test (no OTel dependency).

### 12.3 P2 — Strengthen Existing Tests

- [x] **Introduce clock injection for time-dependent tests** ✓
  _Completed:_ 2026-02-07. Template parameter `Clock` added to `BasicTokenBucket`, `BasicRateLimiter`, `BasicBackendCircuit`, `BasicCircuitBreaker`, and `BasicPeerState` with backward-compatible aliases. `TestClock` helper in `tests/unit/test_clock.hpp`. Pure algorithm split into `src/rate_limiter_core.hpp` (no Seastar deps); `src/rate_limiter.hpp` is Seastar service inheriting from core. 9 rate limiter timing tests, 7 circuit breaker timing tests, 10 quorum liveness timing tests — all deterministic, zero sleeps.

- [x] **Adopt Google Mock (`gmock`) for dependency isolation** ✓
  _Completed:_ 2026-02-07. Replaced 148-line hand-rolled `MockPersistenceStore` with gmock `MOCK_METHOD` declarations + `ON_CALL` defaults for open/close state tracking. Created reusable mock headers in `tests/unit/mocks/`: `mock_persistence_store.hpp` (gmock of `PersistenceStore` interface), `mock_health_checker.hpp` (abstract `HealthChecker` interface + gmock mock), `mock_udp_channel.hpp` (abstract `UdpChannel` interface + gmock mock). Linked `GTest::gmock` in `CMakeLists.txt` for `async_persistence_test`. All tests use `NiceMock<MockPersistenceStore>` to suppress uninteresting-call warnings.

- [x] **Add negative-path integration tests**
  _Location:_ Add as `tests/integration/test_negative_paths.py`. No CMake changes needed (Python tests).
  _Completed:_ 2026-02-07. Added `tests/integration/test_negative_paths.py` with 5 test cases: (1) Split-brain partition via `docker network disconnect`, verifying peer count drops and quorum state via `/admin/dump/cluster`, then reconnect and verify recovery. (2) Backend flap via rapid `docker stop`/`start` of mock backend, verifying `circuit_breaker_opens` metric increments. (3) Config reload with invalid YAML via `docker exec` + SIGHUP, verifying old config preserved and server continues operating. (4) Rate limiting by enabling low limits via config reload, flooding with concurrent requests, verifying 503 responses with `Retry-After` header. (5) Oversized request body (~5 MB), verifying rejection with 400/413/500 and server remains healthy. Follows same pattern as existing suites (Python unittest, `requests`, Docker Compose lifecycle).

### 12.4 P3 — Longer-Term Test Infrastructure

- [x] **Add `connection_pool_test.cpp`**
  _Completed:_ 2026-02-07. Templatized `ConnectionBundle` and `ConnectionPool` on `Clock` parameter (same pattern as `BasicCircuitBreaker<Clock>` and `BasicRateLimiter<Clock>`) with backward-compatible `using` aliases. Created `tests/unit/connection_pool_test.cpp` with 35 tests across 10 fixture classes: `ConnectionPoolConfigTest` (defaults, customization), `ConnectionBundleTest` (timestamps, expiry, TTL, touch semantics), `ConnectionPoolPerHostLimitTest` (per-host limit enforcement, independent host limits), `ConnectionPoolGlobalLimitTest` (global eviction across hosts), `ConnectionPoolIdleTimeoutTest` (deterministic expiry with TestClock, boundary conditions), `ConnectionPoolMaxAgeTest` (TTL enforcement independent of idle, counter accumulation), `ConnectionPoolHalfOpenTest` (non-half-open survival, invalid rejection), `ConnectionPoolStatsTest` (counter tracking), `ConnectionPoolClearPoolTest` (backend removal), `ConnectionPoolMapCleanupTest` (Rule #4 empty map entry removal), `ConnectionPoolEvictionOrderTest` (FIFO eviction semantics), `ConnectionPoolMixedTest` (multi-round cleanup, put-after-clear). Default-constructed Seastar streams have `close()` returning ready futures, enabling testing without a reactor. Wired into CMakeLists.txt under `if(Seastar_FOUND)` with `Seastar::seastar` and `GTest::gtest_main`.

- [x] **Add router service integration tests (Seastar-dependent)**
  _Completed:_ 2026-02-07. Created `tests/unit/router_service_test.cpp` with 55+ tests across 20 sections covering all 5 required test cases. Added test helper methods to `RouterService` (`register_backend_for_testing`, `insert_route_for_testing`, `set_backend_draining_for_testing`, `mark_backend_dead_for_testing`, `unregister_backend_for_testing`, `get_route_count_for_testing`) following the existing `reset_shard_state_for_testing` pattern to enable shard-local testing without a running Seastar reactor. Tests cover: route learning via ART insert+lookup, route TTL cleanup via reset, cross-shard batch structure verification, backend registration/deregistration with route cleanup, draining mode skipping, dead backend circuit breaker, routing mode dispatch (PREFIX/HASH/RANDOM), consistent hash determinism, weighted random selection with priority groups, prefix boundary routing, BackendRequestGuard RAII semantics, load tracking helpers, tree dump API, and edge cases. Wired into CMakeLists.txt under `if(Seastar_FOUND)` linking `router_service.cpp`, `node_slab.cpp`, `gossip_service.cpp`, `gossip_protocol.cpp`, `gossip_consensus.cpp`, `gossip_transport.cpp`, `crypto_offloader.cpp`, `dtls_context.cpp` with Seastar, abseil, OpenSSL, and GTest.

- [x] **Extract and test HTTP controller proxy logic**
  _Completed:_ 2026-02-08. Created `src/proxy_retry_policy.hpp` extracting four pure components from `http_controller.cpp`: (1) `ConnectionErrorType` enum with `classify_connection_error()` and `is_retryable_connection_error()` — classifies `std::system_error` exceptions for EPIPE/ECONNRESET; (2) `BasicProxyRetryPolicy<Clock>` template class — retry count tracking, exponential backoff with cap, fallback-before-retry priority, deadline enforcement; (3) `select_fallback_backend()` template — pure fallback selection with predicate-based filtering; (4) `check_concurrency()` / `check_persistence_backpressure()` — backpressure decision functions. Created `tests/unit/proxy_retry_policy_test.cpp` with 60+ tests across 10 fixture classes: connection error classification (null, broken pipe, connection reset, non-system errors), backoff calculation (exponential doubling, cap enforcement, custom multiplier, constant backoff), retry count enforcement (0/1/3 retries, should_continue), deadline handling (initial, exceeded, boundary, overrides retry/fallback), fallback decisions (preferred over retry, doesn't consume retry, disabled, no candidate, max attempts, zero attempts), combined scenarios (realistic fallback→retry→give-up, deadline during fallback, intermittent candidates), fallback selection (first available, skip failed/disallowed, empty list, large list), backpressure concurrency (under/at/over limit, unlimited), persistence queue (threshold, disabled, empty, large values), initial state and config access. Updated `http_controller.hpp` and `.cpp` to include `proxy_retry_policy.hpp` and use its definitions. Wired into CMakeLists.txt as pure C++ test with TestClock.

- [x] **Add concurrency stress tests for cross-shard components**

---


## 13. Adversarial System Audit (2026-02-12)

> **Criticality Score: 7/10**
> Generated: 2026-02-12
> Audit Scope: All 57 source files under `src/` against 16 Hard Rules, Seastar reactor model, 4 lenses
> Full report: `.dev-context/adversarial-audit-2026-02-12.md`

Cross-referenced with Section 7 (Jan 2026) and Section 11 (Feb 2). Only **new** findings not already tracked or resolved are listed below.

### 13.1 CRITICAL — Benchmark-Impacting

These items directly affect the validity of the current performance benchmark results.

- [x] **[CRITICAL] RadixTree route_count_ drift causes cascading eviction**

- [x] **[CRITICAL] Mutex on every proxy request via is_persistence_backpressured()**

### 13.2 CRITICAL — Async Integrity (Gate Holder Bugs)

Section 11 fixed gate guards in RouterService and K8s timers. These are **new** gate-holder bugs in the gossip and persistence subsystems.

- [x] **[CRITICAL] Gossip heartbeat timer callback has no gate guard**

- [x] **[CRITICAL] Gossip discovery timer gate holder drops before async work completes** ✓

- [x] **[CRITICAL] AsyncPersistence log_stats() gate holder drops at end of try block** ✓

### 13.3 CRITICAL — Cross-Shard Safety

- [x] **[CRITICAL] std::function broadcast across shards in GossipConsensus** ✓

### 13.4 HIGH — Unbounded Containers (Rule #4)

Section 7 fixed several unbounded containers. These are **new** ones.

- [x] **[HIGH] DTLS _sessions map has no MAX_SIZE — unbounded SSL session creation** ✓

- [x] **[HIGH] Gossip _peer_seq_counters map unbounded**

- [x] **[HIGH] K8s discovery: response body, watch buffer, and endpoints map all unbounded** ✓

- [x] **[HIGH] Connection pool _pools map has no MAX_BACKENDS limit**

### 13.5 HIGH — Edge-Case Crashes

- [x] **[HIGH] K8s token file read truncated at 4096 bytes**

- [x] **[HIGH] BackendId hash collision risk at scale** ✓
  _Fixed:_ Replaced weak polynomial hash with FNV-1a 64-bit hash (deterministic across restarts, much better distribution). Added `_backend_id_to_uid` reverse map for collision detection in `handle_endpoint_added()` with error logging and `k8s_backend_id_collisions` Prometheus metric. Reverse map cleaned up in `handle_endpoint_removed()`.

- [x] **[HIGH] TracingService shutdown race on g_tracer.reset()** ✓
  _Fixed:_ Removed g_tracer.reset() and g_provider.reset() from shutdown(). Globals are now process-lifetime objects — OS reclaims at exit. Added concurrency model tests in tracing_service_test.cpp.

- [x] **[HIGH] DTLS context uses blocking stat() and SSL_CTX file I/O on reactor**
  _Fixed:_ Replaced all blocking file I/O with Seastar async APIs (`open_file_dma`, `make_file_input_stream`, `file::stat`). Replaced `SSL_CTX_use_certificate_file`/`SSL_CTX_use_PrivateKey_file`/`SSL_CTX_load_verify_locations` with memory-based OpenSSL APIs (`PEM_read_bio_X509`, `PEM_read_bio_PrivateKey`, `X509_STORE_add_cert`) fed from async-read buffers. Both `initialize()` and `check_and_reload_certs()` now return futures. Added `load_certs_from_memory()` static helper with unit tests in `dtls_context_test.cpp`.

### 13.6 MEDIUM — Additional Issues

- [x] **[MEDIUM] gossip_transport.cpp uses std::shared_ptr for shard-local data (Rule #0)**

- [x] **[MEDIUM] sqlite_persistence.cpp has business validation in persistence layer (Rule #7)** ✓

- [x] **[MEDIUM] K8s HTTP status parsing is brittle — string search instead of code parse** ✓

- [x] **[MEDIUM] K8s watch 410 Gone causes infinite reconnect loop** ✓

- [x] **[MEDIUM] metrics_service.hpp fallback_metrics is a shared mutable singleton** ✓

### 13.7 Anti-Pattern Candidates for Hard Rules

Three systemic patterns observed across multiple findings:

1. **Gate-Holder-Scoping:** Gate holders scoped to `try` blocks or callbacks instead of the full async operation (A2, A3). _Proposed rule: "A gate holder must be scoped to the entire async operation it protects."_
2. **Unconditional-Counter-Increment:** Incrementing size counters after operations that may be no-ops (E1). _Proposed rule: "Mutation methods must signal insert-vs-update; counters only increment on actual size change."_
3. **Unbounded-External-Response:** Accumulating HTTP response bodies from external services without size limits (S3, S4). _Proposed rule: "All HTTP response accumulation from external services must have MAX_RESPONSE_SIZE."_

---


## 14. Router Service Review (2026-02-14)

Deep-dive review of `router_service.{hpp,cpp}` identifying correctness, maintainability, and performance improvements. Full analysis in `.dev-context/router-service-review.md`.

### 14.1 Replace Modular Hash with Jump Consistent Hash

- [x] **Replace `prefix_hash % live_backends.size()` with jump consistent hash** ✓
  _Location:_ `src/router_service.cpp`, `tests/unit/router_service_test.cpp`
  _Completed:_ 2026-02-14

### 14.2 Fix `cache_hit` Misreporting in `route_request()`

- [x] **Return accurate `cache_hit` from `route_request()` for PREFIX mode** ✓
  _Location:_ `src/router_service.hpp`, `src/router_service.cpp`, `tests/unit/router_service_test.cpp`
  _Completed:_ 2026-02-14

### 14.3 Extract Shared Live-Backend Filtering Helper

- [x] **Deduplicate the live-backend collection pattern into `ShardLocalState::get_live_backends()`** ✓
  _Location:_ `src/router_service.cpp`
  _Completed:_ 2026-02-14

### 14.4 Eliminate `std::map` Allocation on Hot Path in `get_random_backend()`

- [x] **Replace `std::map` priority grouping with a single-pass min-priority scan** ✓
  _Location:_ `src/router_service.cpp`
  _Completed:_ 2026-02-14

### 14.5 Preserve Original Backend Weight Across DRAINING→ACTIVE Transitions

- [x] **Stop overwriting backend weight during DRAINING, or store original weight for restoration** ✓
  _Location:_ `src/router_service.cpp`, `src/router_service.hpp`, `tests/unit/router_service_test.cpp`
  _Completed:_ 2026-02-14

### 14.6 Replace `std::atomic` with Plain `uint64_t` for Shard-Local Load Counter

- [x] **Remove unnecessary `std::atomic` from `BackendInfo::active_requests`** ✓
  _Location:_ `src/router_service.cpp`
  _Completed:_ 2026-02-14

### 14.7 Add Error Handling to Fire-and-Forget `unregister_backend_global` in Draining Reaper

- [x] **Attach `.handle_exception()` to discarded `unregister_backend_global()` futures** ✓
  _Location:_ `src/router_service.cpp`
  _Completed:_ 2026-02-14

---


## 16. Radix Tree Review (2026-02-14)

Deep-dive review of `src/radix_tree.hpp` — the #1 load-bearing file in the codebase (per Strategic Assessment). Findings cover correctness bugs, maintainability debt, and performance opportunities.

### 16.1 Fix Node256 Collision Lookup Failure When Preferred Slot Is Vacated

- [x] **Prevent `find_child()` from silently missing displaced children in Node256**
  _Location:_ `src/radix_tree.hpp` lines 629-636 (`find_child`), 679-687 (`extract_child`), 740-747 (`set_child`)

### 16.2 Fix `compact_children()` Using Slot Index Instead of Key for Node256 Removal

- [x] **Use `n->keys[i]` instead of `static_cast<TokenId>(i)` when marking Node256 children for removal**
  _Location:_ `src/radix_tree.hpp` line 1547

### 16.3 Fix `extract_child()` Leaving Stale Entries in Node4/Node16/Node48 Vectors

- [x] **Erase the key/child entry from vectors after extracting a child, or switch to swap-and-pop**
  _Location:_ `src/radix_tree.hpp` lines 643-691 (`extract_child`), called from line 1277 (`insert_recursive`)

### 16.4 Replace `std::vector` with Small-Buffer-Optimized Container for Node Prefix and Small Node Keys

- [x] **Eliminate heap allocations for short prefixes and small node key/child arrays**
  _Location:_ `src/radix_tree.hpp` lines 99 (`Node::prefix`), 109-110 (`Node4`), 119-120 (`Node16`)

### 16.5 Reduce Code Duplication via Unified Small-Node Template for Node4/Node16

- [x] **Consolidate duplicated switch-on-type code by templating Node4/Node16 operations**
  _Location:_ `src/radix_tree.hpp` — affects `find_child`, `extract_child`, `set_child`, `add_child`, `remove_child`, `compact_children`, `visit_children`, `child_count`

### 16.6 Replace O(n) Full-Tree Scan in `evict_oldest()` with an LRU Index Structure

- [x] **Add an intrusive LRU list to make eviction O(1) instead of O(n)**
  _Location:_ `src/radix_tree.hpp` lines 1404-1428 (`find_oldest_leaf`, `find_oldest_recursive`), lines 409-427 (`evict_oldest`, `evict_oldest_remote`)

### 16.7 Add MAX_SIZE Bound to Node Prefix Length

- [x] **Enforce a maximum prefix length to prevent unbounded memory growth from long token sequences**
  _Location:_ `src/radix_tree.hpp` lines 1282-1285 (new node creation in `insert_recursive`), line 950 (`split_node` prefix assignment)

---


## 17. Router Service Review Pass 2 (2026-02-14)

Second review pass of `src/router_service.cpp` and `src/router_service.hpp` after completing all items from section 14. Focuses on correctness, stale comments, lifecycle safety, and minor code quality issues.

### 17.1 Stale "atomic" / "lock-free" Comments in Header File

- [x] **Update `BackendRequestGuard` class comment and `apply_load_aware_selection` comment in header**
  _Location:_ `src/router_service.hpp:92`, `src/router_service.cpp:442`

### 17.2 `run_draining_reaper` Gate Holder Drops Before Fire-and-Forget Futures Complete

- [x] **Keep gate holder alive for the duration of `unregister_backend_global()` futures**
  _Location:_ `src/router_service.cpp:2009-2054`

### 17.3 `run_ttl_cleanup` Fire-and-Forget Future Missing Error Handling

- [x] **Attach `.handle_exception()` to the discarded future in `run_ttl_cleanup()`**
  _Location:_ `src/router_service.cpp:858`

### 17.4 `get_all_backend_states()` Uses `std::ostringstream` on Hot-ish Path

- [x] **Replace `std::ostringstream` address formatting with `fmt::format`**
  _Location:_ `src/router_service.cpp:1822-1854`

### 17.5 `learn_route_global` Captures `this` in Cross-Shard Gossip Lambda

- [x] **Guard `this` captures in `learn_route_global()` gossip `submit_to` lambdas with gate**
  _Location:_ `src/router_service.cpp:1368`, `src/router_service.cpp:1518`

### 17.6 `learn_route_global` Uses `_config` Instead of Shard-Local Config for `prefix_token_length`

- [x] **Use shard-local config for `prefix_token_length` in `learn_route_global()`**
  _Location:_ `src/router_service.cpp:1325,1338`

### 17.7 `learn_route_remote` Uses `_config.max_route_tokens` (Member on Shard 0 Only)

- [x] **Use shard-local config for `max_route_tokens` in `learn_route_remote()`**
  _Location:_ `src/router_service.cpp:1548,1471`

### 17.8 Duplicate Load-Tracking Stats: `cache_miss_due_to_load` and `load_aware_fallbacks`

- [x] **Remove duplicate stat `cache_miss_due_to_load` (identical to `load_aware_fallbacks`)**
  _Location:_ `src/router_service.cpp:128,145,473,743-745`

---


## 18. Connection Pool Review (2026-02-14)

Deep-dive review of `src/connection_pool.hpp` across correctness, performance, and maintainability.

### 18.1 Silent Exception Swallowing in `BasicConnectionBundle::close()` (Rule #9)

- [x] **Log exceptions in `BasicConnectionBundle::close()` instead of silently discarding them**
  _Location:_ `src/connection_pool.hpp:76`

### 18.2 Fragile `const_cast` in `evict_oldest_global()`

- [x] **Remove `const_cast` in `evict_oldest_global()` by storing the iterator or address by value**
  _Location:_ `src/connection_pool.hpp:518-538`

### 18.3 Manual `_total_idle_connections` Counter is Drift-Prone

- [x] **Replace manual `_total_idle_connections` tracking with a derived or RAII-guarded counter**
  _Location:_ `src/connection_pool.hpp` (all methods that modify `_total_idle_connections`)

### 18.4 `evict_oldest_global()` is O(B) on the `put()` Hot Path

- [x] **Avoid O(B) scan in `evict_oldest_global()` by maintaining a sorted eviction index**
  _Location:_ `src/connection_pool.hpp:518-538`

### 18.5 Replace `std::unordered_map` with `absl::flat_hash_map`

- [x] **Switch `_pools` from `std::unordered_map` to `absl::flat_hash_map`**
  _Location:_ `src/connection_pool.hpp:471`

### 18.6 `BasicConnectionBundle::close()` Self-Referencing Continuation is an API Footgun

- [x] **Make `BasicConnectionBundle::close()` safe for direct callers or mark it private**
  _Location:_ `src/connection_pool.hpp:71-77`

### 18.7 Add `connections_created` and `connections_reused` Counters for Operational Visibility

- [x] **Track connection reuse ratio with `connections_created` and `connections_reused` stats**
  _Location:_ `src/connection_pool.hpp` (Stats struct, `create_connection()`, `get()`)

---


## 19. Gossip Service Review (2026-02-14)

Code review of `gossip_service.{hpp,cpp}` and its three sub-modules (`gossip_consensus`, `gossip_protocol`, `gossip_transport`). Findings span correctness, performance, and maintainability.

### 19.1 Fire-and-Forget Route Prune Futures Bypass the Gate (Rule #5)

- [x] **Gate-protect the route prune `parallel_for_each` futures instead of discarding them**
  _Location:_ `src/gossip_consensus.cpp:139-153`, `191-206`, `248-264`

### 19.2 Unbounded `_peer_table` Violates Rule #4

- [x] **Add a MAX_PEERS bound to the peer table and DNS discovery insertion paths**
  _Location:_ `src/gossip_consensus.hpp:191`, `src/gossip_consensus.cpp:124-129`, `160-223`

### 19.3 Dead `update_quorum_state()` Diverges From Active `check_quorum()`

- [x] **Remove dead `update_quorum_state()` or unify with `check_quorum()`**
  _Location:_ `src/gossip_consensus.hpp:219`, `src/gossip_consensus.cpp:346-395`

### 19.4 `process_retries()` Gate Holder Does Not Cover Fire-and-Forget Sends (Rule #5)

- [x] **Extend the gate holder lifetime in `process_retries()` to cover the send futures**
  _Location:_ `src/gossip_protocol.cpp:764-832`

### 19.5 O(N) Peer Address Lookup on Every Incoming Packet

- [x] **Replace `std::find()` on `_peer_addresses` vector with an `absl::flat_hash_set` for O(1) peer validation**
  _Location:_ `src/gossip_protocol.cpp:430-434`

### 19.6 Per-Peer Token Vector Copy in `broadcast_route()`

- [x] **Serialize the route packet once and reuse across peers, stamping only the per-peer sequence number**
  _Location:_ `src/gossip_protocol.cpp:335-376`

### 19.7 Triplicated Route Prune Broadcast Code

- [x] **Extract route prune dispatch into a `broadcast_prune(BackendId)` helper method**
  _Location:_ `src/gossip_consensus.cpp:139-153`, `191-206`, `248-264`

### 19.8 No-Op `catch (...) { throw; }` Blocks Violate Rule #9

- [x] **Remove empty catch-rethrow blocks in `GossipService::start()` and `GossipTransport::start()`**
  _Location:_ `src/gossip_service.cpp:227-229`, `src/gossip_transport.cpp:45-47`

---


## 23. Hard Rules Compliance Audit (2026-02-28)

Audit the codebase against Hard Rules 16-23 (added 2026-02-28 from ScyllaDB/Seastar production experience). Rules 0-15 were audited during previous reviews; rules 16-23 have never been checked against existing code.

**Approach:** Component-by-component, all rules per component. Each component is audited in a single session checking all 8 new rules, since rule violations interact within a component and fixes are more coherent with full component context.

**Phase 1 — Mechanical grep pass (all components, fast)** _(completed 2026-03-20)_

- [x] **Scan for lambda coroutines passed to `.then()` without `seastar::coroutine::lambda()` wrapper (Rule 16)**

- [x] **Scan for loops without `maybe_yield()` preemption points (Rule 17)**

- [x] **Scan for raw `semaphore::wait()`/`signal()` pairs (Rule 19)**

- [x] **Scan for `do_with` lambdas missing `&` on parameters (Rule 20)**

- [x] **Scan for coroutines taking reference parameters (Rule 21)**

- [x] **Scan for `temporary_buffer::share()` stored to member variables (Rule 23)**

**Phase 2 — Component deep audit (all new rules per component)** _(completed 2026-03-20)_

Rules 18 (discarded futures) and 22 (exception-before-future) require understanding async flow and cannot be reliably grepped. These are checked during the component audit along with a second pass on all other new rules.

- [x] **Audit `http_controller.{hpp,cpp}`**

- [x] **Audit `application.{hpp,cpp}`**

- [x] **Audit `gossip_service.{hpp,cpp}`, `gossip_protocol.{hpp,cpp}`, `gossip_transport.{hpp,cpp}`, `gossip_consensus.{hpp,cpp}`**

- [x] **Audit `tokenizer_service.{hpp,cpp}`, `tokenizer_thread_pool.{hpp,cpp}`**

- [x] **Audit `router_service.{hpp,cpp}`, `radix_tree.hpp`, `node_slab.{hpp,cpp}`**

- [x] **Audit remaining services (`sqlite_persistence`, `async_persistence`, `health_service`, `k8s_discovery_service`, `connection_pool`, `circuit_breaker`, `rate_limiter`, `stream_parser`, `shard_load_balancer`)**

**Audit Summary (2026-03-20)**

_Total violations found:_ 47
_Critical (P1):_ 6 — ~~Rule 16 ×2 (use-after-free)~~ FIXED, ~~Rule 18 ×3 (discarded futures)~~ FIXED, ~~Rule 19 ×1 (semaphore leak)~~ FIXED
_High (P2):_ 41 — ~~Rule 21 ×15 (coroutine ref params)~~ FIXED, Rule 22 ×16 (exception-before-future) FIXED, ~~Rule 17 ×4 (reactor stall)~~ FIXED, ~~Rule 17 warnings ×6~~ 4 FIXED + 3 within bounds

| Rule | Violations | Severity | Most Affected Components |
|------|-----------|----------|--------------------------|
| 16 — Lambda Coroutine Fiasco | ~~2~~ 0 (all DONE) | ~~P1~~ FIXED | http_controller, k8s_discovery_service |
| 17 — Reactor Stall | ~~4~~ 0 (+3 warn: 2 sufficient bounds, 1 kept sync for safety) | ~~P2~~ FIXED | gossip_protocol, gossip_consensus, rate_limiter_core, router_service |
| 18 — Discarded Futures | ~~3~~ 0 (all DONE) | ~~P1~~ FIXED | application, gossip_service, gossip_protocol |
| 19 — Semaphore Leak | ~~1~~ 0 (all DONE) | ~~P1~~ FIXED | tokenizer_service |
| 20 — do_with Missing & | 0 | — | — |
| 21 — Coroutine Reference Params | ~~15~~ 0 (all DONE) | ~~P2~~ FIXED | k8s_discovery_service, http_controller, gossip_*, dtls_context |
| 22 — Exception-Before-Future | ~~16~~ 0 (all DONE) | ~~P2~~ FIXED | application, gossip_protocol, gossip_transport, tokenizer_service, router_service, shard_load_balancer, http_controller |
| 23 — Shared temporary_buffer Pin | 0 | — | — |

---


---

## Completed Items from Active Sections

## 1. Core Data Plane (completed items)

- [x] **Offload tokenizer FFI calls to avoid reactor stalls (cross-shard dispatch)** ✓
  _Location:_ `src/tokenizer_service.hpp`, `src/tokenizer_service.cpp`, `src/http_controller.cpp`, `src/application.cpp`
  _Completed:_ 2026-01-29

- [x] **Offload tokenizer FFI calls via dedicated thread pool** ✓
  _Location:_ `src/tokenizer_thread_pool.hpp`, `src/tokenizer_thread_pool.cpp`, `src/tokenizer_service.hpp`, `src/tokenizer_service.cpp`, `src/application.hpp`, `src/application.cpp`, `src/config.hpp`
  _Completed:_ 2026-01-31

- [x] **Rebuild tokenizers-cpp with statically-linked jemalloc allocator** ✓
  _Location:_ `CMakeLists.txt` (lines 158-214), `Dockerfile.base` (lines 59-82)
  _Completed:_ 2026-02-01

- [x] **Use Seastar async file I/O for tokenizer loading** ✓
  _Location:_ `src/application.hpp`, `src/application.cpp`

- [x] **Optimize StreamParser with read-position tracking** ✓
  _Location:_ `src/stream_parser.hpp`, `src/stream_parser.cpp`, `src/http_controller.cpp:925-940`

- [x] **Add validated factory functions for CrossShardRequestContext** ✓
  _Location:_ `src/cross_shard_request.hpp`

- [x] **Decouple SQLite persistence from reactor thread** ✓
  _Location:_ `src/async_persistence.hpp`, `src/async_persistence.cpp`, `src/http_controller.cpp`

- [x] **Encapsulate SQLite store within AsyncPersistenceManager** ✓
  _Location:_ `src/async_persistence.hpp`, `src/async_persistence.cpp`, `src/application.hpp`, `src/application.cpp`

- [x] **Replace `shared_ptr` with `unique_ptr` in RadixTree** ✓
  _Location:_ `src/radix_tree.hpp`

- [x] **Implement node pooling for Radix Tree allocations** ✓
  _Location:_ `src/node_slab.hpp`, `src/node_slab.cpp`, `src/router_service.cpp`

- [x] **Implement tree compaction to reclaim memory from tombstoned nodes** ✓
  _Location:_ `src/radix_tree.hpp:336-452`

- [x] **Add memory usage metrics per Radix Tree** ✓
  _Location:_ `src/radix_tree.hpp`, `src/metrics_service.hpp`, `src/router_service.cpp`

- [x] **Implement P2C load balancer for cross-shard request dispatch** ✓
  _Location:_ `src/shard_load_metrics.hpp`, `src/shard_load_balancer.hpp`, `src/cross_shard_request.hpp`, `src/http_controller.cpp`, `src/application.cpp`


## 2. Distributed Reliability (completed items)

- [x] **Implement split-brain detection** ✓
  _Location:_ `src/gossip_service.cpp:375-403`

- [x] **Finalize quorum enforcement and DTLS lockdown** ✓
  _Location:_ `src/gossip_service.cpp:649-713` (check_quorum), `src/gossip_service.cpp:1625-1695` (DTLS lockdown)

- [x] **Add reliable delivery with acknowledgments** ✓
  _Location:_ `src/gossip_service.cpp:217-262`

- [x] **Implement duplicate suppression** ✓
  _Location:_ `src/gossip_service.hpp`

- [x] **Batch remote route updates to prevent SMP storm** ✓
  _Location:_ `src/router_service.cpp:457-543`, `src/router_service.hpp:19-38`

- [x] **Prevent reactor stalls in DTLS crypto operations** ✓
  _Location:_ `src/crypto_offloader.hpp`, `src/crypto_offloader.cpp`, `src/gossip_service.cpp`


## 3. Observability (completed items)

- [x] **Add cache hit/miss ratio gauge** ✓
  _Location:_ `src/metrics_service.hpp`

- [x] **Add per-backend latency histograms** ✓
  _Location:_ `src/http_controller.cpp`, `src/metrics_service.hpp`

- [x] **Add route table size and memory metrics** ✓
  _Location:_ `src/radix_tree.hpp`, `src/metrics_service.hpp`

- [x] **Integrate OpenTelemetry SDK** ✓
  _Location:_ `src/http_controller.cpp`, `src/logging.hpp`

- [x] **Add spans for critical operations** ✓
  _Location:_ `src/http_controller.cpp`, `src/router_service.cpp`

- [x] **Propagate trace context to backends** ✓
  _Location:_ `src/http_controller.cpp:350+`


## 4. Infrastructure & Security (completed items)

- [x] **Run container as non-root user** ✓
  _Location:_ `Dockerfile.production:42+`

- [x] **Add read-only root filesystem** ✓
  _Location:_ `Dockerfile.production`, `docker-compose.test.yml`
  _Completed:_ 2026-03-22. Added `read_only: true` to all ranvier services in `docker-compose.test.yml` with `tmpfs: /tmp:size=64M` for SQLite WAL files and scratch space.

- [x] **Drop unnecessary Linux capabilities** ✓
  _Location:_ `docker-compose.test.yml`
  _Completed:_ 2026-03-22. Added `cap_drop: [ALL]` + `cap_add: [IPC_LOCK]` to all ranvier services. IPC_LOCK is required by Seastar for mlock/mlockall memory locking. NET_BIND_SERVICE not needed (ports 8080/9180 are >1024).

- [x] **Implement mTLS between Ranvier nodes** ✓
  _Location:_ `src/gossip_service.cpp`

- [x] **Implement API key rotation mechanism** ✓
  _Location:_ `src/config.hpp`, `src/http_controller.cpp`

- [x] **Implement system-wide backpressure mechanism** ✓
  _Location:_ `src/http_controller.hpp`, `src/http_controller.cpp`, `src/config.hpp`, `src/gossip_service.hpp`, `src/gossip_service.cpp`

- [x] **Add request body size limits** ✓
  _Location:_ `src/http_controller.cpp`, `src/config.hpp`
  _Fixed:_ Content-Length checked before body processing; chunked requests checked by actual size; HTTP 413 with JSON error; `ranvier_requests_rejected_body_size_total` Prometheus counter; configurable via YAML and `RANVIER_MAX_REQUEST_BODY_BYTES` env var

- [x] **Protect metrics endpoint** ✓
  _Location:_ `src/metrics_service.hpp`, `src/config.hpp`
  _Completed:_ 2026-03-22. Added `MetricsConfig` struct with `auth_token` and `allowed_ips` fields. Bearer token auth (constant-time comparison) and IP allowlist (exact + CIDR notation) with AND logic when both set. Env vars: `RANVIER_METRICS_AUTH_TOKEN`, `RANVIER_METRICS_ALLOWED_IPS`. YAML: `metrics.auth_token`, `metrics.allowed_ips`. Rate-limited warn logs for rejected scrapes. Handler wraps Seastar prometheus handler at GET /metrics.


## 5. Developer Experience (completed items)

- [x] **Add Production Readiness Validation Suite** ✓
  _Location:_ `validation/validate_v1.sh`, `validation/stall_watchdog.sh`, `validation/disk_stress.sh`, `validation/gossip_storm.py`, `validation/atomic_audit.sh`

- [x] **Add automated benchmark regression testing** ✓
  _Location:_ `.github/workflows/benchmark.yml`, `tests/integration/benchmark-baseline.json`, `tests/integration/run-benchmark.sh`
  _Fixed:_ Post-merge regression detection. Uses pre-built GHCR image to avoid 15min C++ compile.

- [x] **Generate OpenAPI specification** ✓

- [x] **Add Python admin SDK** ✓
  _Location:_ `tools/rvctl`

- [x] **Create Helm chart for Kubernetes deployment** ✓
  _Location:_ `deploy/helm/ranvier/`

- [x] **Refactor initialization into Application class** ✓
  _Location:_ `src/application.hpp`, `src/application.cpp`, `src/main.cpp`

- [x] **Implement Seastar-native signal handling** ✓
  _Location:_ `src/application.hpp`, `src/application.cpp`

- [x] **Refactor configuration for Seastar sharded container** ✓
  _Location:_ `src/sharded_config.hpp`, `src/application.hpp`, `src/application.cpp`

- [x] **Add `rvctl inspect metrics` command** ✓
  _Location:_ `tools/rvctl`

- [x] **Add `rvctl backend add` command** ✓
  _Location:_ `tools/rvctl`

- [x] **Add `rvctl backend delete` command** ✓
  _Location:_ `tools/rvctl`

- [x] **Add `rvctl route delete` command** ✓
  _Location:_ `tools/rvctl`

- [x] **Add `rvctl keys reload` command** ✓
  _Location:_ `tools/rvctl`

- [x] **Add `rvctl health` command** ✓
  _Location:_ `tools/rvctl`

- [x] **Add `--output json` global flag** ✓
  _Location:_ `tools/rvctl`

- [x] **Add `--watch` mode for continuous monitoring** ✓
  _Location:_ `tools/rvctl`

- [x] **Add pre-built Docker images to container registry** ✓
  _Location:_ `.github/workflows/`, `Dockerfile.production`

- [x] **Add development container (devcontainer)** ✓
  _Location:_ `.devcontainer/`


## 8. Strategic Assessment (2026-01-31) (completed items)

- [x] **[P0] Create E2E prefix routing test suite** ✓
  _Completed:_ 2026-01-31. Created comprehensive test suite with 8 tests: (1) same_prefix_routes_consistently, (2) route_learning_creates_cache_entry, (3) cache_hit_ratio_increases, (4) different_prefixes_can_route_differently, (5) route_propagation_across_cluster, (6) backend_affinity_under_load, (7) metrics_reflect_behavior, (8) all_nodes_route_consistently.

- [x] **[P0] Create graceful shutdown test suite** ✓
  _Completed:_ 2026-01-31. Created comprehensive test suite with 8 tests validating shutdown lifecycle guards: (1) healthy_state_returns_200, (2) metrics_accessible_when_healthy, (3) cluster_status_not_draining, (4) concurrent_requests_accepted, (5) health_returns_503_during_drain, (6) requests_rejected_during_drain, (7) cluster_status_shows_draining, (8) cluster_maintains_consensus_during_node_shutdown.

- [x] **[P1] Extract GossipConsensus class from gossip_service.cpp** ✓
  _Completed:_ 2026-01-19. GossipService reduced to thin orchestrator (~350 LOC). Three extracted modules: GossipConsensus (quorum/peer liveness, ~430 LOC), GossipTransport (UDP/DTLS, ~540 LOC), GossipProtocol (message handling/reliability, ~870 LOC). All 27 Prometheus metrics preserved. Rule #14 compliant cross-shard token dispatch using `seastar::do_with`. Added robustness fixes: proper gate holder scoping, exception handling for fire-and-forget futures, defensive null checks.

- [x] **[P1] Consolidate gossip debug metrics behind compile flag** ✓
  _Completed:_ 2026-02-01 (PR #209)

- [x] **[P2] Add inline Hard Rule documentation to radix_tree.hpp** ✓
  _Completed:_ 2026-02-01 (PR #212)

- [x] **[P2] Add inline Hard Rule documentation to router_service.cpp** ✓
  _Completed:_ 2026-02-01 (PR #208)


## 9. Benchmark Extensions (completed items)

- [x] **Investigate high incomplete (timeout) rate in benchmarks (~30-37%)** ✓

- [x] **Rebuild tokenizers-cpp with statically-linked jemalloc allocator** ✓
  _Location:_ `CMakeLists.txt` (lines 158-214), `Dockerfile.base` (lines 59-82)
  _Completed:_ 2026-02-01 (PR #204)


## 10. Load-Aware Prefix Routing (completed items)

- [x] Heavy load (30 users) achieves >35% TTFT improvement (vs current 24%)
- [x] Normal load performance unchanged
- [x] Configurable thresholds for different workloads
- [x] Metrics exposed for observability

- [x] Review existing `ShardLoadMetrics` for patterns (already tracks per-shard active requests)
- [x] Verify `BackendInfo` struct extension is compatible with cluster gossip serialization
- [x] Update BACKLOG.md with completion status

## 15. HTTP Controller Review (2026-02-14) (completed items)

- [x] **`select_target_shard()` computes a P2C target shard but never dispatches to it**
  _Location:_ `src/http_controller.cpp` (lines 725-731, 1839-1864), `src/http_controller.hpp` (lines 55-60, 247-248)
  _Completed:_ 2026-02-14 — Chose modified Option B: disabled by default, documented as advisory-only, removed misleading call from handle_proxy

- [x] **Extract `escape_json_string` into a shared utility and use it for all JSON string interpolation**
  _Location:_ `src/http_controller.cpp` (lines 181, 803, 1071, 1448, 1738), `src/parse_utils.hpp`
  _Completed:_ 2026-02-14 — Moved to parse_utils.hpp, applied to all 4 sites, removed local lambda

- [x] **Remove unnecessary `std::atomic` from shard-local metric counters in `HttpController`**
  _Location:_ `src/http_controller.hpp` (lines 259-264)
  _Completed:_ 2026-02-14 — Also converted _draining from std::atomic<bool> to plain bool, removed <atomic> include

- [x] **Sanitize outgoing header values and derive Host from target address**
  _Location:_ `src/http_controller.cpp` (lines 420-432)
  _Completed:_ 2026-02-14 — Added sanitize_header_value() to parse_utils.hpp, derived Host from ctx.current_addr, split write

- [x] **Add `seastar::future<> stop()` to `HttpController` for clean lifecycle management**
  _Location:_ `src/http_controller.hpp` (class declaration), `src/http_controller.cpp` (new method)
  _Completed:_ 2026-02-14 — Added stop() with _stopped guard for double-stop safety (wait_for_drain + stop)

- [x] **Add depth and size limits to `dump_node_to_json` and admin dump handlers**
  _Location:_ `src/http_controller.cpp` (lines 1871-1986, 2005-2077)
  _Completed:_ 2026-02-14 — Added remaining_depth param (default 32), children_truncated field, ?max_depth=N query param


## 20. Hot-Path Performance Audit (2026-02-15) (completed items)

- [x] **Move UTF-8 validation to JSON parse boundary; remove redundant scan before tokenizer**
  _Location:_ `src/http_controller.cpp:828-829`, `src/text_validator.hpp:37-112`

- [x] **Pre-compute prefix boundary during initial tokenization instead of re-tokenizing**
  _Location:_ `src/http_controller.cpp:886-1011` (boundary detection), `843` (initial tokenization), `939` (multi-depth), `982` (system message)

- [x] **Short-circuit load-aware selection and avoid duplicate calls in route_request()**
  _Location:_ `src/router_service.cpp:440-481` (function), `1202-1203` (first call), `1232-1233` (second call)

- [x] **Reduce per-request clock reads by batching latency measurements**
  _Location:_ `src/http_controller.cpp:646+` (handle_proxy), `src/metrics_service.hpp:383-444` (latency recording methods)

- [x] **Reorder admission checks and evaluate whether all three are needed simultaneously**
  _Location:_ `src/http_controller.cpp:684-716`

- [x] **Replace atomic operations with plain integers for shard-local load tracking**
  _Location:_ `src/shard_load_metrics.hpp:85-103` (methods), `132-134` (fields); `src/http_controller.cpp:732` (increment), `1202/1221/1246/1340/1411` (decrement)

- [x] **Mitigate burst routing decisions after concurrent thread pool completions**

- [x] **Pin tokenizers-cpp to a known-good commit hash for reproducible builds**

- [x] **Propagate speculative load increments across shards to prevent cross-shard burst-routing hot-spotting** ✓
  _Location:_ `src/router_service.cpp`, `src/router_service.hpp`, `src/config_schema.hpp`, `src/config_loader.cpp`
  _Completed:_ 2026-02-21 (initial implementation, 575dd78)


## 21. Request Lifecycle Performance Analysis (2026-02-20) (completed items)

- [x] **Accumulate learned routes in shard-local buffer and flush periodically instead of per-request cross-shard broadcast** ✓
  _Location:_ `src/router_service.cpp`, `src/router_service.hpp`
  _Completed:_ 2026-02-20

- [x] **Gate local tokenization fallback with a shard-local semaphore to prevent reactor stalls**
  _Location:_ `src/tokenizer_service.cpp:295-374`, `src/tokenizer_service.hpp`
  _Completed:_ 2026-02-21

- [x] **Replace mutex-guarded deque in `AsyncPersistenceManager` with a lock-free SPSC ring buffer** ✓
  _Location:_ `src/async_persistence.cpp:202-246`, `src/async_persistence.hpp`
  _Completed:_ 2026-03-21
  _Fixed:_ Implemented `MpscRingBuffer<T>` (Vyukov-style bounded MPSC with per-slot sequence numbers) with cache-line-aligned atomic tail (CAS for multi-producer), power-of-two capacity, and move-only type support. Replaced `std::deque` + `std::mutex` with lock-free ring buffer. Multiple reactor shards can enqueue concurrently without locking. `queue_clear_all()` uses atomic generation counter — consumer skips stale ops. All `std::lock_guard<std::mutex>` calls removed. Consumer paths (`extract_batch`, `drain_queue`) are also lock-free.

- [x] **Release semaphore unit before retry sleep to prevent dead-backend retries from exhausting concurrency** — Implemented option (b): circuit breaker short-circuit. After `record_failure()`, if circuit state is OPEN, remaining retries are skipped immediately (no backoff sleep). Added `retries_skipped_circuit_open_total` Prometheus counter.
  _Location:_ `src/http_controller.cpp:330-407` (establish_backend_connection), `699` (semaphore acquire)

- [x] **Short-circuit route learning with shard-local ART lookup to skip redundant broadcasts** — Added O(k) shard-local ART lookup in `learn_route_global()` after prefix truncation. If the same prefix→backend mapping already exists, skips buffering, broadcast, and persistence. Added `router_routes_deduplicated_total` counter. `learn_route_global` now returns `future<bool>` to allow http_controller to conditionally skip persistence.
  _Location:_ `src/router_service.cpp:1619-1670`, `src/http_controller.cpp:582-601`

