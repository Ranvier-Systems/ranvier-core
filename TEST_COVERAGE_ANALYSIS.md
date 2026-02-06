# Test Coverage Analysis — Ranvier Core

## Executive Summary

Ranvier Core has **851 unit test cases across 20 test files** and **4 integration test suites**.
Coverage is strong for data structures and pure-logic components but has significant gaps
in networking, orchestration, and cross-cutting concerns. This document identifies concrete
areas for improvement, ordered by effort-to-value ratio.

---

## Current Coverage Map

### Unit Tests (20 files, 851 test cases)

| Source Component | Test File | Cases | Quality |
|---|---|---|---|
| `radix_tree.hpp` | `radix_tree_test.cpp` | 109 | Excellent — node growth, LRU eviction, compaction, instrumentation |
| `config*.hpp/cpp` | `config_test.cpp` | 81 | Excellent — YAML, env overrides, TLS, API keys, validation |
| `k8s_discovery_service` | `k8s_discovery_test.cpp` | 67 | Excellent — JSON parsing, endpoint slices, edge cases |
| `request_rewriter.hpp` | `request_rewriter_test.cpp` | 67 | Excellent — text extraction, token injection, Unicode |
| `gossip_protocol` | `gossip_protocol_test.cpp` | 64 | Good — serialization, wire format, crypto offload thresholds |
| `gossip_consensus` | `quorum_test.cpp` | 42 | Good — quorum math, fail-open, sequence windows |
| `async_persistence` | `async_persistence_test.cpp` | 34 | Good — queueing, backpressure, mock store |
| `tokenizer_thread_pool` | `tokenizer_thread_pool_test.cpp` | 32 | Good — config, job/result structs, submit behavior |
| `shard_load_balancer.hpp` | `load_aware_routing_test.cpp` | 32 | Good — threshold, draining, RAII guards |
| `application` | `application_test.cpp` | 30 | Good — lifecycle state machine, config copying |
| `tokenizer_service` | `tokenization_cache_test.cpp` | 28 | Good — LRU, text-length filtering, stats |
| `sqlite_persistence` | `persistence_test.cpp` | 27 | Good — CRUD, reopen durability, edge cases |
| `crypto_offloader` | `crypto_offloader_test.cpp` | 25 | Good — offload decisions, size thresholds |
| `text_validator.hpp` | `text_validator_test.cpp` | 24 | Good — UTF-8, null bytes, length limits |
| `prefix_affinity` (routing) | `prefix_affinity_test.cpp` | 24 | Good — hash determinism, distribution, failover |
| `circuit_breaker.hpp` | `circuit_breaker_test.cpp` | 21 | Good — state transitions, removal, stats |
| `rate_limiter.hpp` | `rate_limiter_test.cpp` | 21 | Adequate — token bucket, per-client buckets, cleanup |
| `stream_parser` | `stream_parser_test.cpp` | 20 | Adequate — chunked parsing, error detection |
| `route_batching` | `route_batching_test.cpp` | 18 | Adequate — batch accumulation, flush, move semantics |
| `sharded_config.hpp` | `sharded_config_test.cpp` | 16 | Adequate — construction, copy/move, updates |

### Integration Tests (4 suites, Docker Compose-based)

| Suite | What it covers |
|---|---|
| `test_cluster.py` | Backend registration, gossip route propagation, peer failure detection |
| `test_prefix_routing.py` | Prefix affinity, cache hit metrics, cross-node route propagation, LRU eviction |
| `test_load_aware_routing.py` | Load-aware diversion under heavy load, TTFT improvement, metric correctness |
| `test_graceful_shutdown.py` | Health endpoint 200→503, request rejection during drain, in-flight completion |

### Validation Scripts (5 scripts)

| Script | Purpose |
|---|---|
| `validate_v1.sh` | Full validation suite |
| `atomic_audit.sh` | Atomic operation correctness |
| `disk_stress.sh` | Disk I/O stress |
| `gossip_storm.py` | Gossip protocol stress |
| `stall_watchdog.sh` | Reactor stall detection |

---

## Gap Analysis

### 1. Completely Untested Source Files

These source files have **zero** dedicated unit tests:

| File | LOC (approx) | Testability | Priority |
|---|---|---|---|
| `parse_utils.hpp` | ~60 | **Very Easy** — pure functions, no deps | **P0** |
| `logging.hpp` | ~100 | **Easy** — request ID gen, header extraction | **P0** |
| `shard_load_metrics.hpp` | ~120 | **Easy** — atomics, RAII guards, load score | **P0** |
| `metrics_service.hpp` | ~200 | **Easy** — histogram logic, bounded containers | **P1** |
| `node_slab.hpp/cpp` | ~250 | **Moderate** — slab allocator, free lists | **P1** |
| `cross_shard_request.hpp` | ~180 | **Moderate** — validation functions are pure | **P1** |
| `connection_pool.hpp` | ~300 | **Moderate** — pool eviction, TTL, limits | **P2** |
| `health_service.hpp/cpp` | ~200 | **Moderate** — check loop logic, thresholds | **P2** |
| `tracing_service.hpp/cpp` | ~250 | **Moderate** — W3C traceparent parsing is pure | **P2** |
| `dtls_context.hpp/cpp` | ~400 | **Hard** — OpenSSL state machine | **P3** |
| `router_service.hpp/cpp` | ~700 | **Very Hard** — Seastar sharding, cross-shard RPC | **P3** |
| `http_controller.hpp/cpp` | ~1200 | **Very Hard** — full Seastar async, network I/O | **P3** |
| `gossip_service.hpp/cpp` | ~300 | **Very Hard** — orchestrator, network loops | **P3** |
| `gossip_transport.hpp/cpp` | ~500 | **Very Hard** — UDP + DTLS + cert reload | **P3** |

### 2. Systematic Weaknesses in Existing Tests

#### A. No Concurrency / Thread-Safety Tests
Not a single test file exercises concurrent access patterns. For a shared-nothing architecture
this is partially mitigated by design (each shard is single-threaded), but several components
cross shard boundaries or use atomics:
- `shard_load_metrics.hpp` — atomic counters accessed cross-shard
- `async_persistence` — concurrent queue submissions
- `tokenizer_thread_pool` — cross-thread job submission
- `rate_limiter` — per-client bucket accessed from HTTP handler context
- `cross_shard_request.hpp` — foreign_ptr transfer semantics

#### B. No Mocking Framework Adoption
Only `async_persistence_test.cpp` uses a hand-rolled mock. The codebase does not use
Google Mock (`gmock`) at all. This makes it impossible to test components with external
dependencies (network, filesystem, timers) in isolation.

#### C. No Timing / Clock-Dependent Tests
Several components have time-sensitive behavior with no test coverage:
- **Rate limiter** — token refill rates are untested (only bucket capacity is tested)
- **Circuit breaker** — timeout-to-HALF_OPEN transition uses real clock
- **Connection pool** — idle timeout, max-age TTL enforcement
- **Gossip consensus** — peer liveness window, recently-seen expiration
- **Route TTL** — LRU eviction timing in radix tree

#### D. No Error Injection / Fault Tolerance Tests
- No tests simulate SQLite corruption or disk-full scenarios
- No tests for partial network failures (packet loss, delayed responses)
- No tests for OpenSSL handshake failures or certificate expiration
- No tests for Seastar reactor stalls under load

#### E. No Negative-Path Integration Tests
The 4 integration suites focus on happy-path scenarios:
- No test for **split-brain recovery** after network partition
- No test for **backend flapping** (rapid up/down transitions)
- No test for **config hot-reload failures** (invalid YAML pushed via SIGHUP)
- No test for **rate limiting under load** (concurrent requests from many clients)
- No test for **circuit breaker tripping** during real proxied traffic

---

## Recommendations

### P0 — Low-Hanging Fruit (easy, high value)

These are pure-function files that can be tested without any Seastar dependency.

#### 1. `parse_utils_test.cpp` — NEW

`parse_utils.hpp` contains `parse_int32`, `parse_uint32`, and `parse_port` functions
using `std::from_chars`. Proposed test cases:

- Valid integers (positive, negative, zero, boundaries)
- Overflow / underflow for int32 and uint32
- Trailing garbage rejection ("123abc")
- Empty string, whitespace-only input
- Port range validation (0, 1, 65535, 65536)
- Negative values rejected for unsigned parse

#### 2. `logging_test.cpp` — NEW

`logging.hpp` contains `generate_request_id()` and header extraction helpers.
Proposed test cases:

- Request ID format: `{shard}-{timestamp}-{seq}` structure
- Monotonically increasing sequence numbers
- `extract_request_id_view` prefers X-Request-ID over X-Correlation-ID
- `extract_traceparent_view` case-insensitive header lookup
- Missing headers return empty string_view
- Empty header values handled

#### 3. `shard_load_metrics_test.cpp` — NEW

`shard_load_metrics.hpp` is self-contained with atomics and RAII guards.
Proposed test cases:

- `ActiveRequestGuard` increments on construction, decrements on destruction
- `QueuedRequestGuard` same RAII semantics
- `load_score()` = active × 2.0 + queued
- `ShardLoadSnapshot` captures point-in-time values
- Move semantics of guards (no double-decrement)
- Underflow protection (decrement below zero)

### P1 — Moderate Effort, High Value

#### 4. `metrics_service_test.cpp` — NEW

Test histogram bucket correctness, bounded container enforcement (max 10K backends
per Rule #4), cache-hit-ratio divide-by-zero protection, and metric recording.

#### 5. `node_slab_test.cpp` — NEW

Test slab allocation/deallocation round-trips, free-list integrity after
allocate→free→reallocate cycles, peak tracking, and pool exhaustion behavior.

#### 6. `cross_shard_request_test.cpp` — NEW

Test the validation functions in isolation: body size limit (128 MB), token count
limit (128K), string length limit (4 KB), and `force_local_allocation()` copy behavior.

#### 7. `tracing_service_test.cpp` — NEW (W3C parsing only)

The W3C traceparent parsing (`version-traceid-spanid-flags`) is pure string logic.
Test valid/invalid formats, hex validation, field length enforcement, and flag parsing.

### P2 — Strengthen Existing Tests

#### 8. Add Clock Mocking to Time-Dependent Tests

Introduce a `TestClock` abstraction (or use `std::chrono::steady_clock` injection)
for:
- `rate_limiter_test.cpp` — test token refill over simulated time
- `circuit_breaker_test.cpp` — test OPEN→HALF_OPEN after exact timeout
- `quorum_test.cpp` — test peer liveness window expiration

#### 9. Adopt Google Mock for Dependency Isolation

Replace the hand-rolled mock in `async_persistence_test.cpp` with `gmock` and
introduce mock interfaces for:
- `PersistenceStore` — already has an interface
- `ConnectionPool` — mock for `http_controller` testing
- `HealthChecker` — mock for `health_service` testing
- `UdpChannel` — mock for `gossip_transport` testing

#### 10. Add Negative-Path Integration Tests

Extend the Docker Compose test suite with:
- **Split-brain test**: partition network between nodes, verify quorum detection and recovery
- **Backend flap test**: rapidly start/stop mock backends, verify circuit breaker engages
- **Config reload test**: send SIGHUP with invalid config, verify old config preserved
- **Rate limit test**: send concurrent requests exceeding rate limits, verify 429 responses
- **Oversized request test**: send bodies exceeding max size, verify rejection

### P3 — Longer-Term Improvements

#### 11. Connection Pool Unit Tests

Extract pool management logic behind an interface and test:
- Per-host connection limits
- Global connection limits
- Idle timeout eviction
- Max-age TTL enforcement
- Half-open connection detection

#### 12. Router Service Integration Tests

The router is the core brain but has no dedicated test. Even a Seastar-dependent
integration test would be valuable:
- Route learning from proxied requests
- Route TTL expiration and cleanup
- Cross-shard route broadcast
- Backend registration/deregistration with route cleanup
- Draining mode with timeout

#### 13. HTTP Controller Proxy Logic

The retry/fallback state machine (`ProxyContext`) in `http_controller.cpp` is the
most complex untested code. Consider:
- Extracting the retry/backoff logic into a testable pure function
- Testing the admin API endpoints in isolation
- Testing backpressure semaphore behavior

---

## Coverage by Architectural Layer

```
┌───────────────────────────────────────────────────────────┐
│  HTTP Layer                                               │
│  http_controller ──── ✗ NO UNIT TESTS                     │
│  stream_parser ────── ✓ 20 tests                          │
│  rate_limiter ─────── ✓ 21 tests (no timing tests)        │
│  connection_pool ──── ✗ NO UNIT TESTS                     │
├───────────────────────────────────────────────────────────┤
│  Routing Engine                                           │
│  router_service ───── ✗ NO UNIT TESTS                     │
│  radix_tree ───────── ✓ 109 tests (excellent)             │
│  prefix_affinity ──── ✓ 24 tests                          │
│  load_aware_routing ─ ✓ 32 tests                          │
│  request_rewriter ─── ✓ 67 tests                          │
│  circuit_breaker ──── ✓ 21 tests (no timing tests)        │
├───────────────────────────────────────────────────────────┤
│  Tokenization                                             │
│  tokenizer_service ── ✓ 28 tests (cache only)             │
│  tokenizer_pool ───── ✓ 32 tests (config/struct only)     │
├───────────────────────────────────────────────────────────┤
│  Cluster / Gossip                                         │
│  gossip_service ───── ✗ NO UNIT TESTS                     │
│  gossip_protocol ──── ✓ 64 tests                          │
│  gossip_consensus ─── ✓ 42 tests                          │
│  gossip_transport ─── ✗ NO UNIT TESTS                     │
│  dtls_context ─────── ✗ NO UNIT TESTS                     │
│  crypto_offloader ─── ✓ 25 tests                          │
├───────────────────────────────────────────────────────────┤
│  Discovery & Health                                       │
│  k8s_discovery ────── ✓ 67 tests                          │
│  health_service ───── ✗ NO UNIT TESTS                     │
├───────────────────────────────────────────────────────────┤
│  Persistence                                              │
│  async_persistence ── ✓ 34 tests (only mock in codebase)  │
│  sqlite_persistence ─ ✓ 27 tests                          │
├───────────────────────────────────────────────────────────┤
│  Observability                                            │
│  metrics_service ──── ✗ NO UNIT TESTS                     │
│  tracing_service ──── ✗ NO UNIT TESTS                     │
├───────────────────────────────────────────────────────────┤
│  Infrastructure                                           │
│  config/schema ────── ✓ 81 tests (excellent)              │
│  parse_utils ──────── ✗ NO UNIT TESTS                     │
│  logging ──────────── ✗ NO UNIT TESTS                     │
│  shard_load_metrics ─ ✗ NO UNIT TESTS                     │
│  cross_shard_request  ✗ NO UNIT TESTS                     │
│  node_slab ────────── ✗ NO UNIT TESTS                     │
│  sharded_config ───── ✓ 16 tests                          │
│  text_validator ───── ✓ 24 tests                          │
└───────────────────────────────────────────────────────────┘
```

## Summary Statistics

| Metric | Value |
|---|---|
| Source files | 56 |
| Source files with unit tests | 20 (36%) |
| Source files without unit tests | 14 (25%) |
| Header-only infra without tests | 6 |
| Total unit test cases | 851 |
| Integration test suites | 4 |
| Validation scripts | 5 |
| Test files using mocks | 1 of 20 (5%) |
| Test files with timing tests | 0 of 20 (0%) |
| Test files with concurrency tests | 0 of 20 (0%) |
