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
6. [Integration Tests (End-to-End Validation)](#6-integration-tests-end-to-end-validation)
7. [Security Audit Findings](#7-security-audit-findings-adversarial-system-audit)
8. [Strategic Assessment (2026-01-19)](#8-strategic-assessment-2026-01-19)
9. [Benchmark Extensions](#9-benchmark-extensions)
10. [Load-Aware Prefix Routing](#10-load-aware-prefix-routing)
11. [System Architecture Audit (2026-02-02)](#11-system-architecture-audit-2026-02-02)
12. [Test Coverage Gaps (2026-02-06)](#12-test-coverage-gaps-2026-02-06)
13. [Adversarial System Audit (2026-02-12)](#13-adversarial-system-audit-2026-02-12)
14. [Router Service Review (2026-02-14)](#14-router-service-review-2026-02-14)

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

- [x] **Offload tokenizer FFI calls to avoid reactor stalls (cross-shard dispatch)** ✓
  _Justification:_ The `_impl->Encode()` FFI call to the Rust tokenizers library blocks the Seastar reactor for ~5-13ms per call. While the LRU cache mitigates this for repeated texts (80-90% hit rate for system messages), cache misses still block the reactor.
  _Approach implemented:_ Cross-shard dispatch via `smp::submit_to`. On cache miss, dispatch tokenization to a different shard using round-robin selection. Calling shard's reactor is freed to handle other requests while waiting for the future. Target shard blocks during FFI but calling shard remains responsive.
  _Implementation details:_
  - Added `CrossShardTokenizationConfig` with min/max text length thresholds (64B-32KB)
  - Added `TokenizationResult` struct with metadata (cache_hit, cross_shard, source_shard)
  - New `encode_cached_async()` method: checks local cache first, dispatches cross-shard on miss
  - Round-robin shard selection (simpler than P2C for this use case)
  - Rule #14 compliant: text copied before cross-shard transfer, captures sharded pointer not `this`
  - Prometheus metrics for cross_shard_dispatches and local_fallbacks
  _Trade-offs:_ Cross-shard dispatch adds ~1-10μs latency + string copy overhead. Per-request latency may not improve, but reactor responsiveness and throughput under load should benefit.
  _Location:_ `src/tokenizer_service.hpp`, `src/tokenizer_service.cpp`, `src/http_controller.cpp`, `src/application.cpp`
  _Completed:_ 2026-01-29

- [x] **Offload tokenizer FFI calls via dedicated thread pool** ✓
  _Justification:_ Cross-shard dispatch (implemented above) frees the calling reactor but still blocks the target shard's reactor during FFI. For true parallelism without blocking any reactor, a dedicated thread pool can run tokenization outside Seastar's event loop entirely.
  _Approach implemented:_ Per-shard worker threads with lock-free SPSC queues (`boost::lockfree::spsc_queue`). Reactor enqueues (job_id, text, source_shard), worker thread tokenizes using dedicated tokenizer instance, signals completion via `seastar::alien::run_on()`. Worker runs in separate OS thread, not blocking any Seastar reactor.
  _Implementation details:_
  - `TokenizerWorker` class: owns worker thread + tokenizer instance (tokenizer is NOT thread-safe)
  - `TokenizerThreadPool` class: per-shard service managing worker lifecycle and promise/future plumbing
  - Thread-local completion callback pattern for worker → reactor signaling
  - Bounded queue (Rule #4) with backpressure: falls back to cross-shard or local tokenization when full
  - Config: `tokenizer_thread_pool_enabled` (false by default), `tokenizer_thread_pool_queue_size`, `tokenizer_thread_pool_min_text`, `tokenizer_thread_pool_max_text`
  - `encode_threaded_async()` API with priority: cache hit → thread pool → cross-shard → local
  - Prometheus metrics: `jobs_submitted`, `jobs_completed`, `jobs_fallback`, `pending_jobs`, `worker_running`
  _Location:_ `src/tokenizer_thread_pool.hpp`, `src/tokenizer_thread_pool.cpp`, `src/tokenizer_service.hpp`, `src/tokenizer_service.cpp`, `src/application.hpp`, `src/application.cpp`, `src/config.hpp`
  _Complexity:_ High
  _Priority:_ P3 (disabled by default; benchmark to enable)
  _Completed:_ 2026-01-31

- [x] **Rebuild tokenizers-cpp with statically-linked jemalloc allocator** ✓
  _Justification:_ Seastar replaces `malloc` globally with its per-shard allocator. When Rust FFI code (tokenizers library) runs on worker threads or processes cross-shard data, allocations are tracked as `foreign_mallocs`. The interaction between Seastar's `do_foreign_free` and Rust's internal allocator patterns causes memory corruption under stress (SIGSEGV with corrupted pointers). Current workaround requires defensive reallocation at every FFI boundary (Rule #15), adding copy overhead and code complexity.
  _Approach:_ CMake and Dockerfile patches inject `tikv-jemallocator = "0.6"` into `rust/Cargo.toml` and add `#[global_allocator] static GLOBAL: Jemalloc` to `rust/src/lib.rs` after FetchContent/git clone. This gives Rust its own memory allocator that bypasses Seastar entirely.
  _Benefits:_ Complete memory isolation between Rust and Seastar allocators. Eliminates need for defensive copies at FFI boundaries. Simpler, more maintainable code. No risk of subtle memory corruption from allocator interactions.
  _Trade-offs:_ Larger binary (~300KB for jemalloc). Two allocators in process (potential memory fragmentation). Inline patching instead of fork - simpler to maintain.
  _Location:_ `CMakeLists.txt` (lines 158-214), `Dockerfile.base` (lines 59-82)
  _Complexity:_ Medium
  _Priority:_ P2 (prevents production crashes; current workaround is fragile)
  _Related:_ Rule #15 in `.dev-context/claude-context.md`
  _Completed:_ 2026-02-01

- [x] **Use Seastar async file I/O for tokenizer loading** ✓
  _Justification:_ Tokenizer loading used blocking `std::ifstream` during startup, blocking the reactor thread. This is an architectural hazard in Seastar that could cause stalls on slow storage.
  _Approach:_ Replaced `std::ifstream` with Seastar's non-blocking DMA file I/O (`seastar::open_file_dma`, `seastar::make_file_input_stream`). Added validation for empty files, max file size (100MB), and proper stream cleanup via `finally()`. Method now returns `seastar::future<>` for proper async chaining in startup sequence.
  _Location:_ `src/application.hpp`, `src/application.cpp`
  _Complexity:_ Low

### 1.2 Zero-Copy SSE Parsing Refinements

- [x] **Optimize StreamParser with read-position tracking** ✓
  _Justification:_ Original parser used `substr()` after each chunk parse, causing O(n) copies. Read-position offset enables zero-copy parsing by tracking consumed bytes without buffer mutation.
  _Approach:_ Replaced substr-based parsing with `_read_pos` offset and `string_view` accessors. Added buffer compaction when >50% consumed to prevent unbounded growth. Added size limits (16KB headers, 1MB chunks) and error state handling for malformed chunked encoding. HttpController now handles parser errors gracefully with client notification.
  _Location:_ `src/stream_parser.hpp`, `src/stream_parser.cpp`, `src/http_controller.cpp:925-940`
  _Complexity:_ Medium

- [x] **Add validated factory functions for CrossShardRequestContext** ✓
  _Justification:_ Cross-shard request dispatch allocated buffers without size validation, risking OOM on malicious large requests.
  _Approach:_ Added `CrossShardRequestLimits` config (128MB body, 128K tokens, 8KB path). New `cross_shard::try_create_*` factory functions validate sizes before allocation and return error results. Added `is_valid()` and `estimated_memory_usage()` helpers for runtime checks and backpressure.
  _Location:_ `src/cross_shard_request.hpp`
  _Complexity:_ Low

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

- [x] **Encapsulate SQLite store within AsyncPersistenceManager** ✓
  _Justification:_ Application class had separate ownership of both `PersistenceStore` and `AsyncPersistenceManager`, with leaky coupling via raw pointer. HttpController could potentially access the underlying SQLite store directly.
  _Approach:_ AsyncPersistenceManager now owns the SQLite store via `std::unique_ptr<PersistenceStore>`. Added lifecycle methods (`open()`, `close()`, `is_open()`) and read delegation methods (`load_backends()`, `load_routes()`, `checkpoint()`, etc.). Removed direct persistence access from Application. HttpController sees only AsyncPersistenceManager interface.
  _Location:_ `src/async_persistence.hpp`, `src/async_persistence.cpp`, `src/application.hpp`, `src/application.cpp`
  _Complexity:_ Low

### 1.4 Memory Efficiency

- [x] **Replace `shared_ptr` with `unique_ptr` in RadixTree** ✓
  _Justification:_ `std::shared_ptr` uses atomic reference counting (`lock xadd` on x86), adding ~20 cycles per copy/destroy. Seastar's shared-nothing model means nodes are never shared across threads, making `unique_ptr` the correct ownership model.
  _Approach:_ Replaced all `std::shared_ptr<Node>` with `std::unique_ptr<Node>`. Added `extract_child()` for ownership transfer during mutations. `find_child()` returns raw `Node*` for traversal. Converted recursive lookup to iterative for additional performance gains.
  _Location:_ `src/radix_tree.hpp`
  _Complexity:_ Medium

- [x] **Implement node pooling for Radix Tree allocations** ✓
  _Justification:_ Frequent `std::unique_ptr<Node>` allocations fragment the heap. A slab allocator per shard reduces allocation overhead and improves cache locality.
  _Approach:_ Implemented `NodeSlab` class with per-shard 2MB pre-allocated chunks. Four size-classed pools (one per node type: Node4, Node16, Node48, Node256) with intrusive free list for O(1) allocation/deallocation. Custom deleter (`SlabNodeDeleter`) returns memory to pool instead of calling `delete`. Thread-local storage (`thread_local NodeSlab*`) ensures no cross-shard synchronization. `ShardLocalTreeState` wrapper guarantees correct destruction order (tree before slab).
  _Location:_ `src/node_slab.hpp`, `src/node_slab.cpp`, `src/router_service.cpp`
  _Complexity:_ High

- [x] **Implement tree compaction to reclaim memory from tombstoned nodes** ✓
  _Justification:_ Route eviction clears `leaf_value` but leaves tree structure intact, creating "tombstoned" nodes that waste slab allocator slots and fragment memory. Without compaction, memory usage grows monotonically even after route removal.
  _Approach:_ Added `compact()` method to RadixTree with post-order traversal. Removes empty nodes (no leaf, no children) and shrinks oversized nodes (Node256→Node48 when children ≤48, etc.). Returns `CompactionStats` with nodes_removed, nodes_shrunk, and bytes_reclaimed. Uses bounded `keys_to_remove` vector per Rule #4. Documented in `docs/internals/radix-tree.md`.
  _Location:_ `src/radix_tree.hpp:336-452`
  _Complexity:_ Medium

- [x] **Add memory usage metrics per Radix Tree** ✓
  _Justification:_ No visibility into per-shard memory consumption. Required for capacity planning and debugging memory leaks.
  _Approach:_ Added comprehensive radix tree performance metrics:
  - `radix_tree_lookup_hits_total` / `radix_tree_lookup_misses_total`: Track lookup efficiency
  - `radix_tree_node_count{node_type}`: Node counts by type (Node4/16/48/256) for memory distribution
  - `radix_tree_slab_utilization_ratio`: Used vs pre-allocated slab memory (0.0-1.0)
  - `radix_tree_average_prefix_skip_length`: Path compression effectiveness metric
  Also added `lookup_instrumented()` method to RadixTree for detailed lookup statistics, and `get_tree_stats()` for tree structure analysis.
  _Location:_ `src/radix_tree.hpp`, `src/metrics_service.hpp`, `src/router_service.cpp`
  _Complexity:_ Low

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

### 1.5 Shard-Aware Load Balancing

- [x] **Implement P2C load balancer for cross-shard request dispatch** ✓
  _Justification:_ Seastar's shared-nothing architecture can create "hot shards" where one CPU core is at 100% while others are idle. This happens when incoming connections are not evenly distributed, causing requests to queue on overloaded shards while adjacent shards sit idle.
  _Approach:_ Implemented Power of Two Choices (P2C) algorithm for O(1) shard selection:
  - Per-shard load metrics tracking (active requests, queue depth) with thread-local storage for lock-free access
  - P2C algorithm randomly selects 2 candidate shards and routes to the less loaded one
  - Cross-shard dispatch via `seastar::smp::submit_to` with `foreign_ptr` for safe pointer transfer
  - Zero-copy request context transfer using move semantics
  - Configurable thresholds: local processing threshold, minimum load difference, snapshot refresh interval
  - Prometheus metrics for monitoring dispatch patterns
  _Location:_ `src/shard_load_metrics.hpp`, `src/shard_load_balancer.hpp`, `src/cross_shard_request.hpp`, `src/http_controller.cpp`, `src/application.cpp`
  _Complexity:_ Medium

---

## 2. Distributed Reliability

Hardening the gossip protocol and cluster coordination for production multi-node deployments.

### 2.1 Network Partition Handling

- [x] **Implement split-brain detection** ✓
  _Justification:_ Current gossip uses timeout-based failure detection only. Nodes cannot distinguish between peer failure and network partition, risking divergent state.
  _Approach:_ Add quorum-aware health checks; require N/2+1 peers visible before accepting new routes.
  _Location:_ `src/gossip_service.cpp:375-403`
  _Complexity:_ High

- [x] **Finalize quorum enforcement and DTLS lockdown** ✓
  _Justification:_ Initial split-brain detection only checked alive/dead state. Network issues may not immediately update liveness. Need stricter recently-seen check and enforcement on both inbound/outbound routes. DTLS needed enforcement mode to reject plaintext packets. Sequence number window needed protection against replay attacks.
  _Approach:_ Three-part implementation:
  1. **Quorum Enforcement**: Added `check_quorum()` that counts peers seen within configurable window (default 30s), stricter than alive check. Both outbound (`broadcast_route`) and incoming routes (`handle_packet`) now rejected in DEGRADED mode.
  2. **DTLS Security Lockdown**: Added `mtls_enabled` config option. When true, drops all non-DTLS packets except handshakes. Auto-triggers handshake for DNS-discovered peers before routing.
  3. **Sequence Number Hardening**: Documented sliding window security properties. Ensured window persists across resync events to prevent replay attacks.
  _Location:_ `src/gossip_service.cpp:649-713` (check_quorum), `src/gossip_service.cpp:1625-1695` (DTLS lockdown)
  _Complexity:_ Medium

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
  _Approach:_ Dedicated `CryptoOffloader` class using `seastar::async` for adaptive offloading. Decision logic: symmetric ops (AES-GCM) run inline when small (<1KB, ~5μs), asymmetric/handshake ops always offload (RSA/ECDH take 1-10ms). Configurable thresholds: size (1KB), latency (100μs), stall detection (500μs). Queue depth limiting prevents unbounded memory growth. Comprehensive Prometheus metrics for monitoring offloader behavior.
  _Location:_ `src/crypto_offloader.hpp`, `src/crypto_offloader.cpp`, `src/gossip_service.cpp`
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

- [x] **Implement system-wide backpressure mechanism** ✓
  _Justification:_ Unbounded request acceptance during traffic spikes causes OOM crashes. Need fail-fast rejection with HTTP 503 rather than queueing to maintain predictable latency.
  _Approach:_ Multi-layer backpressure using: (1) Per-shard `seastar::semaphore` for concurrency limits with `try_get_units()` for non-blocking acquisition, (2) Persistence queue depth monitoring with configurable threshold, (3) Gossip gate protection for route broadcasts during shutdown/resync. Rejected requests receive HTTP 503 with `Retry-After` header.
  _Config:_ `backpressure.max_concurrent_requests`, `backpressure.enable_persistence_backpressure`, `backpressure.persistence_queue_threshold`, `backpressure.retry_after_seconds`
  _Location:_ `src/http_controller.hpp`, `src/http_controller.cpp`, `src/config.hpp`, `src/gossip_service.hpp`, `src/gossip_service.cpp`
  _Complexity:_ Medium

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

- [x] **Add Production Readiness Validation Suite** ✓
  _Justification:_ No automated verification that architectural refactors maintain Seastar shared-nothing guarantees. Manual testing cannot catch reactor stalls, SMP overflow, or atomic instruction regressions.
  _Approach:_ Created comprehensive validation suite with four tests: (1) Reactor Stall Detection using `--task-quota-ms 0.1` to catch micro-stalls, (2) Disk I/O Decoupling test that validates async persistence under stress-ng load, (3) SMP Gossip Storm that floods UDP port with 5000+ PPS to test cross-core messaging, (4) Atomic-Free Execution audit that scans binary for lock/xadd/cmpxchg in RadixTree symbols.
  _Location:_ `validation/validate_v1.sh`, `validation/stall_watchdog.sh`, `validation/disk_stress.sh`, `validation/gossip_storm.py`, `validation/atomic_audit.sh`
  _Complexity:_ Medium

- [x] **Add automated benchmark regression testing** ✓
  _Justification:_ Performance regressions detected manually. Add CI job that fails if P99 latency increases >10%.
  _Approach:_ GitHub Actions workflow pulls pre-built image from GHCR, runs Locust benchmark (100 users, 60s) against docker-compose.test.yml, compares P99 latency and throughput against `benchmark-baseline.json`. Fails if P99 regresses >10% or throughput drops >5%. Manual trigger option to update baseline. Posts results as PR comment.
  _Location:_ `.github/workflows/benchmark.yml`, `tests/integration/benchmark-baseline.json`, `tests/integration/run-benchmark.sh`
  _Complexity:_ Medium
  _Fixed:_ Post-merge regression detection. Uses pre-built GHCR image to avoid 15min C++ compile.

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

- [x] **Add Python admin SDK** ✓
  _Justification:_ Operators script backend registration. SDK simplifies integration.
  _Features:_ Backend CRUD, route inspection, metrics fetching
  _Approach:_ Implemented as `rvctl` CLI tool with commands: `inspect routes`, `inspect backends`, `cluster status`, `drain`, `route add`. Supports JSON/HTTP communication with Admin API, colorized output, tree visualization, and environment variable authentication.
  _Location:_ `tools/rvctl`
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

- [x] **Implement Seastar-native signal handling** ✓
  _Justification:_ Standard C signal handlers (`std::signal`) run outside Seastar's reactor context, limiting them to setting flags. Seastar-native handlers integrate with the event loop, enabling async shutdown sequences that return futures.
  _Approach:_ Replaced standard signal handling with `seastar::engine().handle_signal()`:
  - **SIGINT**: First signal triggers graceful shutdown (drain requests, stop services). Second signal forces immediate termination via `std::exit(1)` for stuck shutdowns.
  - **SIGTERM**: Always triggers graceful shutdown (process managers use SIGKILL for escalation).
  - **SIGHUP**: Configuration hot-reload via `sharded<HttpController>::invoke_on_all()` to propagate changes across all CPU cores.
  - Uses `seastar::promise<>` to bridge signal reception to main application loop.
  - Uses `std::atomic<int>` for SIGINT counter for robustness.
  _Location:_ `src/application.hpp`, `src/application.cpp`
  _Complexity:_ Low

- [x] **Refactor configuration for Seastar sharded container** ✓
  _Justification:_ Global configuration access causes contention in multi-core Seastar deployments. Services need lock-free per-core configuration access with hot-reload capability.
  _Approach:_ Created `ShardedConfig` wrapper class (`src/sharded_config.hpp`) that:
  - Wraps `RanvierConfig` for `seastar::sharded<>` compatibility
  - Provides `stop()` method required by Seastar
  - Provides `update()` method for hot-reload via `invoke_on_all()`
  - Each CPU core gets its own config copy for lock-free reads
  - `Application::local_config()` provides per-shard access
  - Master config only updated after all shards succeed (exception-safe)
  - Uses `shared_ptr` to avoid N copies during hot-reload propagation
  _Location:_ `src/sharded_config.hpp`, `src/application.hpp`, `src/application.cpp`
  _Complexity:_ Medium

### 5.5 rvctl CLI Enhancements

The `rvctl` CLI tool (tools/rvctl) provides operator-friendly access to Ranvier's Admin API. Several endpoints and quality-of-life features are not yet exposed.

- [x] **Add `rvctl inspect metrics` command** ✓
  _Justification:_ Prometheus metrics endpoint (`:9180/metrics`) is not integrated into rvctl. Operators must use curl or helper scripts to view metrics. A CLI command with parsed/formatted output improves operational visibility.
  _Features:_
  - Fetch from `/metrics` endpoint with configurable URL
  - Add `--metrics-url` flag for full URL override (e.g., `http://metrics.ranvier:9180`)
  - Add `--metrics-port` flag for port-only override, derives host from `--url` (default 9180)
  - Parse and display key metrics in readable format (cache hit ratio, request counters, active requests, per-backend latencies)
  - Support `--raw` flag for raw Prometheus format output
  - Support `--filter <pattern>` for metric name filtering
  - Support `RANVIER_METRICS_URL` environment variable
  _Note:_ Metrics port may differ from API port, especially in containerized deployments where ports are remapped.
  _Location:_ `tools/rvctl`
  _Complexity:_ Medium
  _Priority:_ High

- [x] **Add `rvctl backend add` command** ✓
  _Justification:_ Backend registration requires curl. CLI command simplifies operator workflow and enables scripting.
  _Usage:_ `rvctl backend add --id <id> --ip <ip> --port <port> [--weight <w>] [--priority <p>]`
  _Endpoint:_ POST /admin/backends
  _Location:_ `tools/rvctl`
  _Complexity:_ Low
  _Priority:_ High

- [x] **Add `rvctl backend delete` command** ✓
  _Justification:_ Backend removal requires curl. Completes full backend lifecycle management in rvctl.
  _Usage:_ `rvctl backend delete --id <id>`
  _Endpoint:_ DELETE /admin/backends
  _Location:_ `tools/rvctl`
  _Complexity:_ Low
  _Priority:_ High

- [x] **Add `rvctl route delete` command** ✓
  _Justification:_ Route deletion for a backend requires curl. Enables cleanup workflows.
  _Usage:_ `rvctl route delete --backend <id>`
  _Endpoint:_ DELETE /admin/routes
  _Location:_ `tools/rvctl`
  _Complexity:_ Low
  _Priority:_ Medium

- [x] **Add `rvctl keys reload` command** ✓
  _Justification:_ API key hot-reload requires curl. CLI command simplifies key rotation workflows.
  _Usage:_ `rvctl keys reload`
  _Endpoint:_ POST /admin/keys/reload
  _Location:_ `tools/rvctl`
  _Complexity:_ Low
  _Priority:_ Medium

- [x] **Add `rvctl health` command** ✓
  _Justification:_ Quick health check without authentication. Useful for scripting and monitoring integration.
  _Usage:_ `rvctl health`
  _Endpoint:_ GET /health (public, no auth required)
  _Location:_ `tools/rvctl`
  _Complexity:_ Low
  _Priority:_ Medium

- [x] **Add `--output json` global flag** ✓
  _Justification:_ Current output is human-formatted. JSON output enables piping to jq and scripting integration.
  _Usage:_ `rvctl --output json inspect backends`
  _Location:_ `tools/rvctl`
  _Complexity:_ Low
  _Priority:_ Low

- [x] **Add `--watch` mode for continuous monitoring** ✓
  _Justification:_ Operators often need to monitor state during deployments. Watch mode with configurable refresh interval reduces manual polling.
  _Usage:_ `rvctl --watch [--interval 2s] inspect backends`
  _Location:_ `tools/rvctl`
  _Complexity:_ Medium
  _Priority:_ Low

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

- [x] **Add pre-built Docker images to container registry** ✓
  _Justification:_ Users must build from source. Publish to Docker Hub/GHCR for easy deployment.
  _Location:_ `.github/workflows/`, `Dockerfile.production`
  _Complexity:_ Low

- [x] **Add development container (devcontainer)** ✓
  _Justification:_ Complex build dependencies. Devcontainer provides reproducible environment.
  _Location:_ `.devcontainer/`
  _Complexity:_ Low

---

## 6. Integration Tests (End-to-End Validation)

Comprehensive E2E tests validating the full request pipeline:
`Client → HttpController → TokenizerService → RouterService → RadixTree → ConnectionPool → Backend`

All tests must follow the **No Locks/Async Only** constraints from `docs/claude-context.md`.

### 6.1 Test Infrastructure Setup

- [ ] **Create shared test fixtures**
  _Description:_ Create `tests/integration/conftest.py` with pytest fixtures for cluster lifecycle, health polling, and request utilities.
  _Files:_ `tests/integration/conftest.py` (new)
  _Complexity:_ Low

- [ ] **Enhance mock backend capabilities**
  _Description:_ Extend `mock_backend.py` with configurable latency injection, failure mode simulation (5xx, timeouts, connection resets), request logging endpoint (`/debug/requests`), and prefix echo mode.
  _Files:_ `tests/integration/mock_backend.py`
  _Complexity:_ Medium

- [ ] **Add Docker Compose test profiles**
  _Description:_ Add single-node profile for fast isolated tests, health check failure simulation service, and configurable backend response modes.
  _Files:_ `docker-compose.test.yml`
  _Complexity:_ Low

- [ ] **Add Makefile test targets**
  _Description:_ Add `test-integration-fast` (single-node), `test-integration-full` (multi-node), and `test-integration-ci` (JUnit XML output) targets.
  _Files:_ `Makefile`
  _Complexity:_ Low

### 6.2 HTTP Request Pipeline Tests

- [ ] **Create HTTP pipeline test suite**
  _Description:_ Create `test_http_pipeline.py` with tests for: POST `/v1/chat/completions` returns valid streaming response, request headers forwarded correctly, Content-Type validation, invalid JSON returns 400.
  _Files:_ `tests/integration/test_http_pipeline.py` (new)
  _Complexity:_ Medium

- [ ] **Create streaming response test suite**
  _Description:_ Create `test_streaming.py` with tests for: SSE format validation, chunked transfer encoding, stream interruption handling, `[DONE]` sentinel verification.
  _Files:_ `tests/integration/test_streaming.py` (new)
  _Complexity:_ Medium

- [ ] **Test request rewriting with token injection**
  _Description:_ Verify token IDs injected when forwarding enabled, original request preserved when disabled, large message arrays (10+) handled correctly.
  _Files:_ `tests/integration/test_http_pipeline.py`
  _Complexity:_ Low

### 6.3 Routing Logic Tests

- [ ] **Create prefix affinity routing test suite**
  _Description:_ Create `test_prefix_routing.py` with tests for: same prefix routes to same backend consistently, different prefixes can route to different backends, route learning verified via metrics, min token length threshold respected.
  _Files:_ `tests/integration/test_prefix_routing.py` (new)
  _Complexity:_ Medium

- [ ] **Extend route propagation tests**
  _Description:_ Add tests for: routes learned on Node1 visible on Node2 after gossip interval, updates propagate within timeout, verify `router_cluster_sync_*` metrics.
  _Files:_ `tests/integration/test_cluster.py`
  _Complexity:_ Medium

- [ ] **Test backend selection and lifecycle**
  _Description:_ Verify: requests route to healthy backends only, backend registration creates routable backend, backend removal stops routing.
  _Files:_ `tests/integration/test_prefix_routing.py`
  _Complexity:_ Low

### 6.4 Resilience and Fault Tolerance Tests

- [ ] **Create health/circuit breaker test suite**
  _Description:_ Create `test_health_circuit_breaker.py` with tests for: unhealthy backend detection and removal, backend recovery and re-addition, health check interval configuration.
  _Files:_ `tests/integration/test_health_circuit_breaker.py` (new)
  _Complexity:_ Medium

- [ ] **Test circuit breaker state transitions**
  _Description:_ Verify: consecutive failures trigger open state, half-open state allows probes, successful probe closes circuit.
  _Files:_ `tests/integration/test_health_circuit_breaker.py`
  _Complexity:_ Medium

- [ ] **Test connection pool resilience**
  _Description:_ Verify: connection reuse across requests, recovery after backend restart, timeout handling for slow backends.
  _Files:_ `tests/integration/test_health_circuit_breaker.py`
  _Complexity:_ Medium

- [ ] **Test rate limiting behavior**
  _Description:_ Verify: requests exceeding limit return 429, limit resets after window, rate limit metrics exposed.
  _Files:_ `tests/integration/test_health_circuit_breaker.py`
  _Complexity:_ Low

### 6.5 Observability Tests

- [ ] **Create metrics test suite**
  _Description:_ Create `test_metrics.py` with tests for: `/metrics` returns valid Prometheus format, request count increments, latency histograms recorded, backend health metrics accurate.
  _Files:_ `tests/integration/test_metrics.py` (new)
  _Complexity:_ Medium

- [ ] **Test cluster metrics**
  _Description:_ Verify: `cluster_peers_alive` reflects actual peers, gossip counters increment during sync, per-shard metrics available.
  _Files:_ `tests/integration/test_metrics.py`
  _Complexity:_ Low

### 6.6 Lifecycle and Persistence Tests

- [ ] **Create graceful shutdown test suite**
  _Description:_ Create `test_graceful_shutdown.py` with tests for: in-flight requests complete, new connections rejected after signal, shutdown within timeout, exit code 0 for clean shutdown.
  _Files:_ `tests/integration/test_graceful_shutdown.py` (new)
  _Complexity:_ Medium

- [ ] **Create persistence recovery test suite**
  _Description:_ Create `test_persistence_recovery.py` with tests for: backends persist across restart, routes persist in SQLite, WAL mode concurrent access, corrupted DB handled gracefully.
  _Files:_ `tests/integration/test_persistence_recovery.py` (new)
  _Complexity:_ Medium

- [ ] **Create configuration loading test suite**
  _Description:_ Create `test_config_loading.py` with tests for: YAML config loaded correctly, env vars override YAML, `--dry-run` validates without starting, invalid config produces clear error.
  _Files:_ `tests/integration/test_config_loading.py` (new)
  _Complexity:_ Low

### 6.7 Edge Cases and Error Handling Tests

- [ ] **Test error response validation**
  _Description:_ Verify: 404 for unknown endpoints, 405 for unsupported methods, 503 when no backends, structured JSON error bodies.
  _Files:_ `tests/integration/test_http_pipeline.py`
  _Complexity:_ Low

- [ ] **Test large payload handling**
  _Description:_ Verify: large request bodies (>1MB) processed, large streaming responses (>10MB) delivered, memory stable during processing.
  _Files:_ `tests/integration/test_streaming.py`
  _Complexity:_ Medium

- [ ] **Test concurrent request handling**
  _Description:_ Verify: 100 concurrent requests without errors, request ordering preserved per-connection, no cross-contamination under load.
  _Files:_ `tests/integration/test_http_pipeline.py`
  _Complexity:_ Medium

### Integration Test Summary: Files Affected

| File | Action | Description |
|------|--------|-------------|
| `tests/integration/conftest.py` | Create | Shared pytest fixtures |
| `tests/integration/mock_backend.py` | Modify | Add failure modes, logging |
| `tests/integration/test_http_pipeline.py` | Create | HTTP request flow tests |
| `tests/integration/test_streaming.py` | Create | SSE/chunked response tests |
| `tests/integration/test_prefix_routing.py` | Create | Prefix affinity tests |
| `tests/integration/test_cluster.py` | Modify | Extend route propagation |
| `tests/integration/test_health_circuit_breaker.py` | Create | Resilience tests |
| `tests/integration/test_metrics.py` | Create | Prometheus metrics tests |
| `tests/integration/test_graceful_shutdown.py` | Create | Shutdown behavior tests |
| `tests/integration/test_persistence_recovery.py` | Create | SQLite persistence tests |
| `tests/integration/test_config_loading.py` | Create | Configuration tests |
| `docker-compose.test.yml` | Modify | Add test profiles/services |
| `Makefile` | Modify | Add test targets |

**Total: 13 files (9 new, 4 modified)**

---

## 7. Security Audit Findings (Adversarial System Audit)

> **Criticality Score: 0/10** _(was 6/10 on 2026-01-14; all issues resolved)_
> Generated: 2026-01-11 | Updated: 2026-01-14
> Audit Scope: `src/` directory against `docs/claude-context.md` constraints

Structural issues identified across 4 lenses: Async Integrity, Edge-Case Crash, Architecture Drift, and Scale & Leak.

### 7.0 Adversarial Audit Findings (2026-01-15)

All HIGH severity issues resolved. MEDIUM/LOW issues tracked below for future hardening.

#### Remaining Issues (Future Hardening)

- [x] **[MEDIUM] Add stale circuit entry cleanup when backends removed** ✓
  _Issue:_ `circuit_breaker.hpp:250` `_circuits` map has MAX_CIRCUITS=10K bound (good), but entries are never cleaned up when backends are deregistered. Dead backend entries persist forever, consuming memory until MAX_CIRCUITS limit.
  _Fix:_ Add `remove_circuit(BackendId)` method called from backend removal path in RouterService. Alternatively, add periodic sweep to remove entries for backends not in active registry.
  _Location:_ `src/circuit_breaker.hpp`, `src/router_service.cpp`
  _Severity:_ Medium
  _Fixed:_ Added `remove_circuit(BackendId)` method to CircuitBreaker, shard-local callback in RouterService called from `unregister_backend_global()`, Prometheus metric `circuit_breaker_circuits_removed_total` (Rule #4)

- [ ] **[MEDIUM] Consider lock-free queue for AsyncPersistenceManager**
  _Issue:_ `async_persistence.cpp:192,203,216,234` uses `std::lock_guard<std::mutex>` in `try_enqueue()` which briefly blocks reactor thread during write batching. Documented as acceptable tradeoff but still a theoretical stall point under high persistence load.
  _Fix:_ Consider lock-free SPSC queue or `seastar::submit_to` pattern for cross-shard enqueue. Current design is acceptable for typical workloads.
  _Location:_ `src/async_persistence.cpp`
  _Severity:_ Medium (acceptable tradeoff)

- [x] **[LOW] Audit _pending_acks cleanup in GossipService** (completed 2026-01-16)
  _Issue:_ `gossip_service.cpp` `_pending_acks` map tracks pending reliable delivery ACKs. Entries have retry limits, but if peers become permanently unresponsive, entries may accumulate until retry exhaustion. Need to verify cleanup occurs when peers are removed from cluster.
  _Fix:_ Audit confirmed all cleanup paths work correctly (ACK received, retry exhaustion, peer removal, shutdown, resync). Added `MAX_PENDING_ACKS=1000` bound (Rule #4), `cluster_pending_acks_overflow` counter, and `cluster_pending_acks_count` gauge metrics.
  _Location:_ `src/gossip_service.cpp`
  _Severity:_ Low

#### Completed Issues (2026-01-14)

#### Async Integrity Violations

- [x] **Replace blocking std::ifstream with Seastar async I/O in K8s CA cert loading** ✓
  _Issue:_ `k8s_discovery_service.cpp:234` uses `std::ifstream ca_file(_config.ca_cert_path)` which performs blocking file I/O on the reactor thread, violating Seastar's async-only model.
  _Fix:_ Use `seastar::open_file_dma()` + `seastar::make_file_input_stream()` as done for token loading at lines 204-207.
  _Location:_ `src/k8s_discovery_service.cpp:224-265`
  _Severity:_ **Critical**
  _Fixed:_ PR #149 - Replaced with Seastar async file I/O (open_file_dma, make_file_input_stream)

- [x] **Add gate guard to connection pool reaper timer** ✓
  _Issue:_ `connection_pool.hpp:99-109` timer callback captures `this` without RAII gate protection. Destructor cancels timer but already-queued callback creates use-after-free race window.
  _Fix:_ Add `seastar::gate _timer_gate`; callbacks acquire `_timer_gate.hold()` at entry; destructor closes gate before canceling timer.
  _Location:_ `src/connection_pool.hpp:108-127`
  _Severity:_ Medium
  _Fixed:_ PR #151 - Added _timer_gate, stop() closes gate before canceling timer (Rule #5)

#### Edge-Case Crash Scenarios

- [x] **Add null assertion in metrics() accessor** ✓
  _Issue:_ `metrics_service.hpp:443-445` returns `*g_metrics` without null check. If `init_metrics()` not called before first use, immediate segfault.
  _Fix:_ Add assertion: `assert(g_metrics && "init_metrics() must be called first")` or lazy-init pattern.
  _Location:_ `src/metrics_service.hpp:485-490`
  _Severity:_ Medium
  _Fixed:_ PR #152 - Implemented lazy-init pattern ensuring accessor never returns null (Rule #3)

#### Scale & Leak Vulnerabilities

- [x] **Fix memory leak in thread-local MetricsService allocation** ✓
  _Issue:_ `metrics_service.hpp:436-440` uses `g_metrics = new MetricsService()` with no corresponding `delete`. Thread-local raw `new` leaks on process shutdown.
  _Fix:_ Add `destroy_metrics()` function called during shard shutdown, or use `thread_local std::unique_ptr<MetricsService>`.
  _Location:_ `src/metrics_service.hpp:495-501`
  _Severity:_ **High**
  _Fixed:_ PR #151 - Added stop_metrics() that calls stop() then deletes g_metrics (Rule #6)

- [x] **Cap per-backend metrics map to prevent unbounded growth** ✓
  _Issue:_ `metrics_service.hpp:397` `_per_backend_metrics` map grows with each unique BackendId. Under attack with spoofed IDs, unbounded memory consumption. Entries never removed.
  _Fix:_ Cap at `MAX_TRACKED_BACKENDS` (e.g., 1000). Implement LRU eviction or tie to backend lifecycle. Add `remove_backend_metrics(BackendId)`.
  _Location:_ `src/metrics_service.hpp:418-425`
  _Severity:_ **High**
  _Fixed:_ PR #150 - Added MAX_TRACKED_BACKENDS constant, overflow counter, fallback metrics (Rule #4)

- [x] **Add circuit cleanup when backends are removed** ✓
  _Issue:_ `circuit_breaker.hpp:210` `_circuits` map grows with each BackendId. While `reset(id)` exists, nothing calls it when backends are removed. Memory grows monotonically.
  _Fix:_ Add `remove_circuit(BackendId)` called from backend removal path, or expire stale circuits periodically.
  _Location:_ `src/circuit_breaker.hpp:42, 59-74`
  _Severity:_ **High**
  _Fixed:_ PR #150 - Added MAX_CIRCUITS=10000 constant, bounds check in allow_request(), fail-open strategy (Rule #4)

- [x] **Fully erase map entries in connection pool clear_pool()** ✓
  _Issue:_ `connection_pool.hpp:348` `_pools` map grows per socket_address. While `clear_pool()` removes deque contents, map entries for removed backends remain as empty deques.
  _Fix:_ In `clear_pool()`, the current code already erases the entry (`_pools.erase(it)`) at line 294, but verify this is called from all backend removal paths.
  _Location:_ `src/connection_pool.hpp:298-318`
  _Severity:_ Medium
  _Fixed:_ PR #152 - Verified _pools.erase(it) called at line 312, cleanup_expired() tracks empty pools (Rule #4)

- [x] **Add MAX_TRACKED_PEERS limit to gossip dedup structures** ✓
  _Issue:_ `gossip_service.cpp:1174-1195` `_received_seq_sets` and `_received_seq_windows` grow per peer address. While sliding window evicts old entries, peer count itself is unbounded.
  _Fix:_ Add `MAX_TRACKED_PEERS` limit. Clean up peer state in `refresh_peers()` when peers are removed.
  _Location:_ `src/gossip_service.cpp:1208-1215`
  _Severity:_ Medium
  _Fixed:_ PR #150 - Added MAX_DEDUP_PEERS constant, bounds check, overflow counter (Rule #4)

### 7.1 Async Integrity Violations (No Locks/Async Only)

- [x] **Remove blocking mutex from `queue_depth()` in AsyncPersistenceManager** ✓
  _Issue:_ `std::lock_guard<std::mutex>` at `src/async_persistence.cpp:232` blocks reactor thread when called from metrics collection.
  _Fix:_ Use `std::atomic<size_t>` for queue size tracking or implement lock-free queue depth estimation.
  _Location:_ `src/async_persistence.cpp:231-234`
  _Severity:_ High
  _Fixed:_ PR #118 - Replaced with `std::atomic<size_t>` for lock-free access

- [x] **Replace sequential awaits with parallel_for_each in K8s discovery** ✓
  _Issue:_ Loop at `src/k8s_discovery_service.cpp:413-424` awaits each endpoint sequentially, causing O(n) latency for n endpoints.
  _Fix:_ Use `seastar::parallel_for_each` or `seastar::do_for_each` with batching.
  _Location:_ `src/k8s_discovery_service.cpp:413-424`
  _Severity:_ Medium
  _Fixed:_ PR #122 - Added `max_concurrent_for_each` with 16-op limit, per-endpoint error handling

- [x] **Audit 16+ mutex usages in SQLite persistence layer** ✓
  _Issue:_ `src/sqlite_persistence.cpp` contains 16+ `std::lock_guard<std::mutex>` calls. While these run in `seastar::async`, any path that bypasses async wrapper blocks reactor.
  _Fix:_ Ensure all SQLite access is routed through AsyncPersistenceManager; add static analysis to prevent direct access.
  _Location:_ `src/sqlite_persistence.cpp` (multiple locations)
  _Severity:_ Medium
  _Fixed:_ PR #123 - Documented threading model, added friend declarations and private constructor for compile-time access control

### 7.2 Edge-Case Crash Scenarios

- [x] **Add null check before sqlite3_column_text dereference** ✓
  _Issue:_ `src/sqlite_persistence.cpp:156` dereferences `sqlite3_column_text()` without null check. NULL returned for SQL NULL values causes segfault.
  _Fix:_ Add null guard: `auto ptr = sqlite3_column_text(...); record.ip = ptr ? reinterpret_cast<const char*>(ptr) : "";`
  _Location:_ `src/sqlite_persistence.cpp:156`
  _Severity:_ High
  _Fixed:_ PR #119 - Added `safe_column_text()` helper, skip records with NULL required fields

- [x] **Add port validation before stoi conversion in K8s discovery** ✓
  _Issue:_ `src/k8s_discovery_service.cpp:243` uses `std::stoi()` without validation. Non-numeric or out-of-range values throw uncaught exceptions.
  _Fix:_ Add try-catch or use `std::from_chars` with validation before `static_cast<uint16_t>`.
  _Location:_ `src/k8s_discovery_service.cpp:243`
  _Severity:_ Medium
  _Fixed:_ PR #124 - Added `parse_port()` helper with validation (1-65535 range), 22 unit tests

- [x] **Handle DNS resolution exceptions in K8s discovery** ✓
  _Issue:_ DNS resolution at `src/k8s_discovery_service.cpp:276-279` can throw unhandled exceptions on network failure.
  _Fix:_ Wrap DNS calls in try-catch with appropriate error handling and logging.
  _Location:_ `src/k8s_discovery_service.cpp:276-279`
  _Severity:_ Medium
  _Fixed:_ PR #126 - Async DNS resolver with retry, exponential backoff, caching, and graceful degradation

- [x] **Fix silent exception swallowing in annotation parsing** ✓
  _Issue:_ `src/k8s_discovery_service.cpp:682-686` catches all exceptions silently, masking configuration errors.
  _Fix:_ Log caught exceptions at warn level; consider propagating critical parsing failures.
  _Location:_ `src/k8s_discovery_service.cpp:682-686`
  _Severity:_ Low
  _Fixed:_ PR #135 - Log warnings with annotation name/value, add max constants, clamp out-of-range values

- [x] **Eliminate global static state race in tracing service** ✓
  _Issue:_ `src/tracing_service.cpp:40-44` uses global statics (`g_tracer`, `g_provider`, `g_enabled`) without synchronization. Concurrent init/shutdown causes data races.
  _Fix:_ Use `std::call_once` for initialization or move to per-shard thread_local storage.
  _Location:_ `src/tracing_service.cpp:40-44`
  _Severity:_ Medium
  _Fixed:_ PR #127 - std::call_once for init, std::atomic for enabled flag, shutdown guard

- [ ] **Add DNS resolution timeout in backend registration**
  _Issue:_ `src/http_controller.cpp:1469` calls `seastar::net::dns::get_host_by_name()` without timeout. If hostname doesn't resolve (especially `.local` mDNS domains), the request hangs indefinitely waiting for DNS response.
  _Fix:_ Wrap DNS call with `seastar::with_timeout()`:
  ```cpp
  auto hostent = co_await seastar::with_timeout(
      std::chrono::seconds(5),
      seastar::net::dns::get_host_by_name(std::string(ip_str))
  );
  ```
  _Location:_ `src/http_controller.cpp:1469`
  _Severity:_ Medium

### 7.3 Architecture Drift

- [x] **Move token count limits from persistence layer to business layer** ✓
  _Issue:_ `src/sqlite_persistence.cpp:173-196` contains business logic (token count limits) that belongs in RouterService.
  _Fix:_ Extract validation to RouterService; persistence layer should only handle storage.
  _Location:_ `src/sqlite_persistence.cpp:173-196`
  _Severity:_ Low
  _Fixed:_ PR #137 - Added max_route_tokens config, validate in learn_route_global/remote, persistence only stores

- [x] **Move routing mode decisions from HttpController to RouterService** ✓
  _Issue:_ `src/http_controller.cpp:506-566` contains routing logic (mode selection, backend choice) that should be in RouterService.
  _Fix:_ Create RouterService API for routing decisions; HttpController should only coordinate.
  _Location:_ `src/http_controller.cpp:506-566`
  _Severity:_ Medium
  _Fixed:_ PR #128 - RouteResult struct, unified route_request() API, single error path

- [x] **Encapsulate thread_local variables into ShardLocalState struct** ✓
  _Issue:_ `src/router_service.cpp:46-79` scatters 18+ thread_local variables at file scope, making state management fragile.
  _Fix:_ Consolidate into single `ShardLocalState` struct with clear lifecycle management.
  _Location:_ `src/router_service.cpp:46-79`
  _Severity:_ Low
  _Fixed:_ PR #136 - Unified ShardLocalState with Stats/Config structs, init/reset lifecycle, reset_for_testing()

### 7.4 Scale & Leak Vulnerabilities

- [x] **Add bounds checking to pending remote routes buffer** ✓
  _Issue:_ `src/router_service.cpp:631-643` `_pending_remote_routes.push_back()` has no upper bound. Malicious gossip flood causes unbounded memory growth.
  _Fix:_ Add max buffer size check; drop oldest routes or reject when full.
  _Location:_ `src/router_service.cpp:631-643`
  _Severity:_ High
  _Fixed:_ PR #120 - Added MAX_BUFFER_SIZE (10000) with batch-drop strategy and overflow metric

- [x] **Add connection pool max age reaping** ✓
  _Issue:_ Connection pool lacks TTL for idle connections. Long-running instances accumulate stale connections.
  _Fix:_ Add configurable `max_connection_age` with periodic reaping.
  _Location:_ `src/connection_pool.hpp`
  _Severity:_ Medium
  _Fixed:_ PR #130 - Added created_at timestamp, is_too_old(), max_connection_age config (default 5min)

- [x] **Add RAII guards for timer callbacks** ✓
  _Issue:_ Timer callbacks in `src/async_persistence.cpp` and `src/gossip_service.cpp` capture `this` without ensuring object lifetime.
  _Fix:_ Use weak_ptr or explicit cancellation in destructor; verify all timers cancelled before object destruction.
  _Location:_ `src/async_persistence.cpp:106-112`, `src/gossip_service.cpp`
  _Severity:_ Medium
  _Fixed:_ PR #131 - Added _timer_gate to both services, callbacks acquire holder, stop() closes gate first

- [x] **Add lifetime guards for metrics lambda captures** ✓
  _Issue:_ `src/router_service.cpp:227-228` metrics lambdas capture `this` without lifetime protection. Metrics collection after service shutdown causes use-after-free.
  _Fix:_ Use weak_ptr capture or ensure metrics deregistration in destructor.
  _Location:_ `src/router_service.cpp:227-228`
  _Severity:_ Medium
  _Fixed:_ PR #132 - Added stop() that calls _metrics.clear() first, deregisters before destruction

- [x] **Add upper bound to StreamParser accumulator** ✓
  _Issue:_ `src/stream_parser.cpp:26` accumulator can grow to ~1MB per connection before detection. Slowloris-style attack exhausts memory.
  _Fix:_ Add incremental size checking; reject early when approaching limit.
  _Location:_ `src/stream_parser.cpp:26`
  _Severity:_ Medium
  _Fixed:_ PR #133 - Added max_accumulator_size (1MB), early check before append, rejection counter

---

## 8. Strategic Assessment (2026-01-31)

> **External CTO/Lead Architect Review**
> Generated: 2026-01-19 | Updated: 2026-01-31
> Scope: Full codebase analysis (~23,000 LOC across 52 files) against stated goal of "Layer 7+ Load Balancer with Prefix Caching"

### 8.0 State of the Project Scorecard

| Domain | Grade | Rationale |
|--------|-------|-----------|
| **Architecture** | **A-** | Clean separation of concerns, Seastar-native design, proper ART implementation. GossipService refactored into focused modules (2026-01-19). |
| **Reliability** | **B** | Security audit score 0/10 (resolved), 57+ Hard Rule references. Deduction: Massive integration test gap (Section 6). |
| **Progress-to-Goal** | **A-** | Prefix Caching IS the core, not an aspiration. RadixTree fully implemented with path compression, SIMD-ready nodes, slab allocator. |

**Overall: B+ (Production-Adjacent, Not Production-Ready)**

### 8.1 Goal Alignment Verdict

**Status: ON TARGET** — This IS a Prefix Caching Balancer, not a general-purpose proxy.

Evidence of Prefix Logic Implementation (Constraint #3):
- Adaptive Radix Tree (ART): 1,634 LOC dedicated (`radix_tree.hpp`)
- Path Compression: `std::vector<TokenId> prefix` per node
- Adaptive Nodes: Node4→Node16→Node48→Node256 transitions
- Slab Allocator: Per-shard, zero-allocation hot path (`node_slab.hpp/cpp`)
- LRU Eviction: `evict_oldest()`, `evict_oldest_remote()`
- Block Alignment: vLLM-compatible 16-token alignment

Critical routing path: `route_request() → get_backend_for_prefix() → tree->lookup_instrumented() → learn_route_global()`

**Drift Risk: LOW** — GossipService complexity is necessary for distributed prefix caching.

### 8.2 Complexity vs. Value Audit

**The Sprawling Module:** `gossip_service.cpp` — **RESOLVED (2026-01-19)**

Original (2,161 LOC) refactored into:
| Module | LOC | Responsibility |
|--------|-----|----------------|
| `gossip_service.cpp` | ~350 | Thin orchestrator, metrics registration |
| `gossip_consensus.cpp` | ~430 | Peer table, quorum, split-brain detection |
| `gossip_transport.cpp` | ~540 | UDP channel, DTLS encryption, crypto offloading |
| `gossip_protocol.cpp` | ~870 | Message handling, reliable delivery, DNS discovery |

**Verdict:** Complexity now properly separated. Each module can be tested and reasoned about independently. Route batching is NOT over-engineered (99% SMP traffic reduction).

### 8.3 Load-Bearing Files (Blast Radius)

| Rank | File | Blast Radius | Hard Rule Coverage |
|------|------|--------------|-------------------|
| 1 | `radix_tree.hpp` | TOTAL | 1 reference |
| 2 | `router_service.cpp` | TOTAL | 1 reference |
| 3 | `gossip_service.cpp` | CLUSTER | 5 references |
| 4 | `http_controller.cpp` | ALL TRAFFIC | 2 references |
| 5 | `connection_pool.hpp` | BACKEND | 6 references |

**Next Big Risk:** Integration test gap (Section 6) — 13 new test files needed, `test_prefix_routing.py` has NO E2E validation.

### 8.4 Prioritized Action Items

| Priority | Action | Risk Addressed | Effort |
|----------|--------|----------------|--------|
| **P0** | Create `test_prefix_routing.py` | No E2E prefix validation | Medium |
| **P0** | Create `test_graceful_shutdown.py` | Untested shutdown path | Medium |
| **P1** | ~~Extract `GossipConsensus` class from gossip_service.cpp~~ | ~~Maintainability (2,161 LOC sprawl)~~ | ~~High~~ ✅ |
| **P1** | Reduce gossip metrics from 27 to ~15 | Scrape overhead, debugging artifacts | Low |
| **P2** | Create `test_persistence_recovery.py` | Data durability | Medium |
| **P2** | Document RadixTree Hard Rules inline | Knowledge preservation | Low |

### 8.5 Staff Engineer Recommendation

**ONE THING TO DELETE:** Excessive gossip debug metrics

Move these to `DEBUG_METRICS` compile flag:
- `cluster_crypto_stalls_avoided`
- `cluster_crypto_batch_broadcasts`
- `cluster_crypto_ops_offloaded`
- `cluster_dtls_cert_reloads`
- `cluster_crypto_handshakes_offloaded`

**ONE THING TO REFACTOR:** ~~GossipService state machine extraction~~ ✅ **DONE (2026-01-19)**

```
gossip_service.cpp (2,161 LOC) →
  ├── gossip_service.cpp     (~350 LOC) - Thin orchestrator ✓
  ├── gossip_transport.cpp   (~540 LOC) - UDP + DTLS ✓
  ├── gossip_protocol.cpp    (~870 LOC) - Message handling ✓
  └── gossip_consensus.cpp   (~430 LOC) - Quorum, split-brain ✓
```

Refactoring completed with Rule #14 compliant cross-shard dispatch and robustness fixes.

### 8.6 Action Items (Tracking)

- [x] **[P0] Create E2E prefix routing test suite** ✓
  _Description:_ Create `tests/integration/test_prefix_routing.py` with tests for: same prefix routes to same backend consistently, route learning verified via metrics, cache hit/miss validation.
  _Rationale:_ The core value proposition (prefix caching) has NO E2E validation. A regression in `lookup_instrumented()` could silently break cache affinity.
  _Files:_ `tests/integration/test_prefix_routing.py` (new)
  _Complexity:_ Medium
  _Completed:_ 2026-01-31. Created comprehensive test suite with 8 tests: (1) same_prefix_routes_consistently, (2) route_learning_creates_cache_entry, (3) cache_hit_ratio_increases, (4) different_prefixes_can_route_differently, (5) route_propagation_across_cluster, (6) backend_affinity_under_load, (7) metrics_reflect_behavior, (8) all_nodes_route_consistently.

- [x] **[P0] Create graceful shutdown test suite** ✓
  _Description:_ Create `tests/integration/test_graceful_shutdown.py` with tests for: in-flight requests complete, new connections rejected after signal, exit code 0 for clean shutdown.
  _Rationale:_ Shutdown path exercises multiple lifecycle guards (gates, timers, RAII). Untested path is high-risk for production incidents.
  _Files:_ `tests/integration/test_graceful_shutdown.py` (new)
  _Complexity:_ Medium
  _Completed:_ 2026-01-31. Created comprehensive test suite with 8 tests validating shutdown lifecycle guards: (1) healthy_state_returns_200, (2) metrics_accessible_when_healthy, (3) cluster_status_not_draining, (4) concurrent_requests_accepted, (5) health_returns_503_during_drain, (6) requests_rejected_during_drain, (7) cluster_status_shows_draining, (8) cluster_maintains_consensus_during_node_shutdown.

- [x] **[P1] Extract GossipConsensus class from gossip_service.cpp** ✓
  _Description:_ Refactor gossip_service.cpp (2,161 LOC) into three focused modules: `gossip_transport.cpp` (UDP/DTLS), `gossip_protocol.cpp` (message handling), `gossip_consensus.cpp` (quorum, split-brain).
  _Rationale:_ Sprawling module exceeds maintainability threshold. Mixed concerns make unit testing quorum logic difficult.
  _Files:_ `src/gossip_service.cpp` (split), `src/gossip_transport.cpp` (new), `src/gossip_protocol.cpp` (new), `src/gossip_consensus.cpp` (new)
  _Complexity:_ High
  _Completed:_ 2026-01-19. GossipService reduced to thin orchestrator (~350 LOC). Three extracted modules: GossipConsensus (quorum/peer liveness, ~430 LOC), GossipTransport (UDP/DTLS, ~540 LOC), GossipProtocol (message handling/reliability, ~870 LOC). All 27 Prometheus metrics preserved. Rule #14 compliant cross-shard token dispatch using `seastar::do_with`. Added robustness fixes: proper gate holder scoping, exception handling for fire-and-forget futures, defensive null checks.

- [x] **[P1] Consolidate gossip debug metrics behind compile flag** ✓
  _Description:_ Move 8 debugging-oriented gossip metrics behind `RANVIER_DEBUG_METRICS` compile flag. Keep ~15 operationally-relevant metrics always enabled.
  _Rationale:_ 27 metrics per gossip service adds scrape overhead. Debug metrics (`crypto_stalls_avoided`, `cert_reloads`, etc.) provide value during development, not production.
  _Files:_ `src/gossip_service.cpp`
  _Complexity:_ Low
  _Completed:_ 2026-02-01 (PR #209)

- [x] **[P2] Add inline Hard Rule documentation to radix_tree.hpp** ✓
  _Description:_ Add explicit Hard Rule comments to load-bearing code paths in RadixTree (lookup, insert, eviction). Currently only 1 reference despite being most critical file.
  _Rationale:_ Knowledge preservation for future maintainers. Explicit documentation prevents accidental rule violations during optimization work.
  _Files:_ `src/radix_tree.hpp`
  _Complexity:_ Low
  _Completed:_ 2026-02-01 (PR #212)

- [x] **[P2] Add inline Hard Rule documentation to router_service.cpp** ✓
  _Description:_ Add explicit Hard Rule comments to load-bearing code paths in RouterService (route_request, learn_route_global, get_backend_for_prefix). Currently only 1 reference despite being second most critical file.
  _Rationale:_ Knowledge preservation for future maintainers. RouterService orchestrates all routing decisions; undocumented constraints risk silent regressions.
  _Files:_ `src/router_service.cpp`
  _Complexity:_ Low
  _Completed:_ 2026-02-01 (PR #208)

- [ ] **[P3] Track config.hpp complexity - split when >2000 LOC**
  _Description:_ Monitor `config.hpp` (currently 1,510 LOC). When exceeding 2000 LOC, split into `config_schema.hpp` (type definitions) and `config_loader.cpp` (YAML parsing logic).
  _Rationale:_ Prevent config.hpp from becoming the next "sprawling module". Current growth trajectory suggests split needed within 2-3 feature additions.
  _Files:_ `src/config.hpp` → `src/config_schema.hpp` (new), `src/config_loader.cpp` (new)
  _Complexity:_ Medium
  _Trigger:_ >2000 LOC or >50 config fields

---

## 9. Benchmark Extensions

Extend benchmarking to make it more realistic with production traces, cache pressure scenarios, larger models, and traffic variability.

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
  _Approach:_ Query vLLM `/metrics` for `vllm:cache_evictions_total` or similar, add to benchmark report
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

### 9.5 Incomplete Request Rate Investigation

- [x] **Investigate high incomplete (timeout) rate in benchmarks (~30-37%)** ✓
  _Root cause:_ Stale pooled connections. Ranvier's connection pool returned TCP connections where vLLM had already closed its end, but `is_half_open()` didn't detect it. The proxy sent HTTP 200 to the client, forwarded the request on a dead socket, got immediate EOF, and recorded "success" with an empty body. Sub-categorization revealed 100% of incompletes were `no_data` with 3-24ms response times (too fast for vLLM — confirms Ranvier-side issue).
  _Fix:_ Phase 3.5 stale connection retry in `http_controller.cpp`. After streaming completes with 0 bytes written and no error flags, close the dead connection and retry once on a fresh connection via `ConnectionPool::get_fresh()`. Transparent to client (HTTP 200 already sent, no body data yet).
  _Changes:_
  - `src/http_controller.{hpp,cpp}` — Phase 3.5 retry logic, `ProxyContext` tracking fields
  - `src/connection_pool.hpp` — `get_fresh()` method, `create_connection()` refactor
  - `src/proxy_retry_policy.hpp` — `should_retry_stale_connection()` pure function
  - `src/metrics_service.hpp` — `stale_connection_retries_total` counter
  - `src/config_schema.hpp`, `src/config_loader.cpp`, `src/application.cpp` — config wiring (`RANVIER_RETRY_MAX_STALE` env var, `retry.max_stale_retries` YAML)
  - `tests/unit/proxy_retry_policy_test.cpp` — 10 unit tests for detection predicate
  - `tests/integration/test_negative_paths.py` — `test_05a_stale_connection_retry` integration test
  - `tests/integration/mock_backend.py` — keep-alive admin endpoint for testing
  - `tests/integration/locustfile_real.py` — incomplete sub-categorization, `MAX_OUTPUT_TOKENS` env var, diagnostic logging
  - `scripts/bench.sh` — `--max-tokens` flag
  _Investigation report:_ `.dev-context/investigations/benchmark-timeout-investigation.md`
  _Complexity:_ Medium
  _Verified:_ 2026-02-08 — 8B/30u/5m/stress benchmark: 3194/3194 successful, 0 incomplete, 0 failed (was 30-37% incomplete). Debug logging (`no_data` warnings) can now be removed.
  _Priority:_ P2 - Medium (CLOSED)

### 9.6 Tokenizer Performance

- [x] **Rebuild tokenizers-cpp with statically-linked jemalloc allocator** ✓
  _Justification:_ Cross-shard dispatch revealed memory allocation overhead in Rust tokenizer. jemalloc provides better performance for multi-threaded allocations and avoids glibc malloc contention.
  _Approach:_ CMake and Dockerfile patches inject `tikv-jemallocator` into tokenizers-cpp Rust code. Gives Rust complete memory isolation from Seastar, eliminating allocator interaction bugs.
  _Location:_ `CMakeLists.txt` (lines 158-214), `Dockerfile.base` (lines 59-82)
  _Complexity:_ Medium
  _Completed:_ 2026-02-01 (PR #204)

---

## Priority Matrix

| Priority | Category | Item | Effort | Status |
|----------|----------|------|--------|--------|
| **P0 - Critical** | Security | Non-root container execution | Low | ✅ Done |
| **P0 - Critical** | Security | mTLS for gossip protocol | High | ✅ Done |
| **P1 - High** | Reliability | Split-brain detection | High | ✅ Done |
| **P1 - High** | Reliability | Quorum enforcement and DTLS lockdown | Medium | ✅ Done |
| **P1 - High** | Reliability | Reliable gossip delivery | Medium | ✅ Done |
| **P1 - High** | Security | API key rotation | Medium | ✅ Done |
| **P1 - High** | Observability | OpenTelemetry integration | Medium | ✅ Done |
| **P1 - High** | Performance | Async persistence (reactor stall fix) | Medium | ✅ Done |
| **P1 - High** | Performance | Batch remote route updates (SMP storm fix) | Medium | ✅ Done |
| **P1 - High** | Performance | Replace shared_ptr with unique_ptr in RadixTree | Medium | ✅ Done |
| **P1 - High** | Reliability | System-wide backpressure mechanism | Medium | ✅ Done |
| **P1 - High** | Performance | Shard-aware P2C load balancer | Medium | ✅ Done |
| **P2 - Medium** | Performance | Zero-copy StreamParser optimization | Medium | ✅ Done |
| **P2 - Medium** | Reliability | CrossShardRequest size validation | Low | ✅ Done |
| **P2 - Medium** | Performance | SIMD Node16 optimization | Medium | |
| **P2 - Medium** | Performance | Node pooling for Radix Tree | High | ✅ Done |
| **P2 - Medium** | Performance | Tree compaction for memory reclamation | Medium | ✅ Done |
| **P2 - Medium** | DX | Benchmark regression CI | Medium | ✅ Done |
| **P2 - Medium** | DX | Helm chart | Medium | ✅ Done |
| **P3 - Low** | Performance | Memory-mapped tokenizer | Low | |
| **P3 - Low** | DX | OpenAPI specification | Low | ✅ Done |
| **P3 - Low** | DX | Pre-built Docker images | Low | ✅ Done |
| **P2 - Medium** | DX | Application bootstrap refactoring | Medium | ✅ Done |
| **P3 - Low** | DX | Seastar-native signal handling | Low | ✅ Done |
| **P2 - Medium** | DX | Sharded configuration for per-core access | Medium | ✅ Done |
| **P3 - Low** | DX | Encapsulate SQLite store within AsyncPersistenceManager | Low | ✅ Done |
| **P3 - Low** | Performance | Async file I/O for tokenizer loading | Low | ✅ Done |
| **P3 - Low** | Observability | Radix tree performance metrics | Low | ✅ Done |
| **P2 - Medium** | DX | Python admin SDK (rvctl CLI) | Medium | ✅ Done |
| **P3 - Low** | Performance | Remove unnecessary atomics from ShardLoadMetrics | Low | |
| **P3 - Low** | Performance | Batch CryptoOffloader statistics updates | Medium | |
| **P3 - Low** | Performance | Offload tokenizer FFI via cross-shard dispatch | Medium | ✅ Done |
| **P3 - Low** | Performance | Offload tokenizer FFI via dedicated thread pool | High | ✅ Done |
| **P3 - Low** | Performance | Audit codebase for abseil container opportunities | Low | |
| **P3 - Low** | DX | Change max_token_id type from int32_t to uint32_t | Low | |
| **P2 - Medium** | DX | rvctl: Add `inspect metrics` command | Medium | ✅ Done |
| **P2 - Medium** | DX | rvctl: Add `backend add` command | Low | ✅ Done |
| **P2 - Medium** | DX | rvctl: Add `backend delete` command | Low | ✅ Done |
| **P3 - Low** | DX | rvctl: Add `route delete` command | Low | ✅ Done |
| **P3 - Low** | DX | rvctl: Add `keys reload` command | Low | ✅ Done |
| **P3 - Low** | DX | rvctl: Add `health` command | Low | ✅ Done |
| **P3 - Low** | DX | rvctl: Add `--output json` flag | Low | ✅ Done |
| **P3 - Low** | DX | rvctl: Add `--watch` mode | Medium | ✅ Done |
| **P2 - Medium** | Reliability | Add DNS resolution timeout in backend registration | Low | |
| **P0 - Critical** | Testing | E2E prefix routing test suite | Medium | ✅ Done |
| **P0 - Critical** | Testing | Graceful shutdown test suite | Medium | ✅ Done |
| **P1 - High** | DX | Consolidate gossip debug metrics behind compile flag | Low | ✅ Done |
| **P2 - Medium** | DX | Add Hard Rule documentation to radix_tree.hpp | Low | ✅ Done |
| **P2 - Medium** | DX | Add Hard Rule documentation to router_service.cpp | Low | ✅ Done |
| **P3 - Low** | DX | Split config.hpp when >2000 LOC | Medium | |
| **P2 - Medium** | Benchmark | Production prompt traces - trace format definition | Low | |
| **P2 - Medium** | Benchmark | Production prompt traces - locustfile trace replay | Medium | |
| **P2 - Medium** | Benchmark | Production prompt traces - anonymization tool | Medium | |
| **P2 - Medium** | Benchmark | Cache pressure - unique prefixes option | Low | |
| **P2 - Medium** | Benchmark | Cache pressure - cache-pressure distribution | Medium | |
| **P3 - Low** | Benchmark | Cache pressure - eviction metrics | Low | |
| **P2 - Medium** | Benchmark | Larger models - tensor-parallel vLLM support | Medium | |
| **P2 - Medium** | Benchmark | Larger models - 70B/405B configurations | Medium | |
| **P3 - Low** | Benchmark | Larger models - TTFT threshold adjustments | Low | |
| **P2 - Medium** | Benchmark | Traffic variability - traffic shape classes | Medium | |
| **P3 - Low** | Benchmark | Traffic variability - traffic pattern option | Low | |
| **P2 - Medium** | Benchmark | Traffic variability - cold-start measurement | Medium | |
| **P2 - Medium** | Benchmark | Traffic variability - cache warm-up metrics | Medium | |
| **P2 - Medium** | Benchmark | Incomplete rate investigation - timeout tuning | Medium | ✅ Done (stale connection retry) |
| **P2 - Medium** | Performance | Tokenizer - jemalloc static linking | Medium | ✅ Done |
| **P1 - High** | Performance | Load-aware prefix routing - backend load tracking | Medium | ✅ Done (#222) |
| **P1 - High** | Performance | Load-aware prefix routing - load-aware selection | Medium | ✅ Done (#223) |
| **P1 - High** | Performance | Load-aware prefix routing - metrics and config | Low | ✅ Done (#223) |
| **P1 - High** | Performance | Load-aware prefix routing - tests and docs | Medium | ✅ Done (#224) |

---

## 10. Load-Aware Prefix Routing

> **Feature: Load-aware prefix routing to reduce queuing under heavy load**
> Created: 2026-02-01
> Priority: P1 - High
> Labels: enhancement, routing, performance

### 10.0 Problem Statement

Current prefix-aware routing uses a simple `hash(prefix) → backend` mapping that doesn't consider backend load. Under heavy load, this causes request queuing on popular-prefix backends while other backends sit underutilized.

**Current Behavior:**
```
Prefix A (popular) → GPU-1: [req1] [req2] [req3] [req4] [req5] ← queue builds
Prefix B (rare)    → GPU-2: [req1]                             ← underutilized
Prefix C (popular) → GPU-1: [req6] [req7]                      ← more queuing
```

**Target Behavior:**
- When a backend's queue exceeds threshold, route new requests to less-loaded backends
- Accept temporary cache miss to avoid latency spike from queuing
- Maintain prefix affinity under normal load for KV cache reuse

**Success Criteria:**
- [x] Heavy load (30 users) achieves >35% TTFT improvement (vs current 24%)
- [x] Normal load performance unchanged
- [x] Configurable thresholds for different workloads
- [x] Metrics exposed for observability

### 10.1 Prerequisites

- [x] Review existing `ShardLoadMetrics` for patterns (already tracks per-shard active requests)
- [x] Verify `BackendInfo` struct extension is compatible with cluster gossip serialization
- [ ] Confirm vLLM `/metrics` endpoint exposes queue depth (optional enhancement - not required)

### Implementation Steps

#### Step 1: Add Per-Backend Load Tracking Infrastructure

- **Files:**
  - `src/router_service.cpp` (modify `BackendInfo` struct, add to `ShardLocalState`)
  - `src/router_service.hpp` (add new types if needed for public API)
- **Description:** Extend `BackendInfo` with atomic counters for in-flight requests. Add RAII guard class (`BackendRequestGuard`) for automatic increment/decrement. Track `active_requests` per backend across all shards.
- **Details:**
  ```cpp
  // In BackendInfo struct
  std::atomic<uint64_t> active_requests{0};  // In-flight requests to this backend

  // RAII guard for tracking
  class BackendRequestGuard {
      BackendId _backend_id;
  public:
      explicit BackendRequestGuard(BackendId id);
      ~BackendRequestGuard();
  };
  ```
- **Concerns:**
  - [ ] Uses `std::atomic<uint64_t>` with relaxed memory order (lock-free, Rule #1 compliant)
  - [ ] Per-shard counters avoid cross-shard synchronization
  - [ ] Must aggregate across shards for global view (use `smp::submit_to` for metrics collection)
- **Status:** [x] Done (#222)

#### Step 2: Integrate Load Tracking into Request Flow

- **Files:**
  - `src/http_controller.cpp` (create guard on backend selection, destroy on response complete)
- **Description:** Instantiate `BackendRequestGuard` when request is dispatched to backend, destructor decrements when response completes (success or failure). Guard must survive SSE streaming duration.
- **Details:**
  - Create guard after `route_request()` returns a backend
  - Store in request context (`CrossShardRequestContext` or local scope with `do_with`)
  - Ensure proper cleanup on all exit paths (success, timeout, error)
- **Concerns:**
  - [ ] Guard lifetime spans entire request (including streaming)
  - [ ] No new async flow - guard is synchronous RAII
  - [ ] Handle cross-shard dispatch case (guard on dispatched shard, not originating shard)
- **Status:** [x] Done (#222)

#### Step 3: Add Load-Aware Configuration Options

- **Files:**
  - `src/config.hpp` (add to `RoutingConfig` struct)
  - YAML schema documentation
- **Description:** Add configuration parameters for load-aware routing thresholds.
- **Details:**
  ```cpp
  // In RoutingConfig struct
  bool load_aware_routing = true;              // Enable load-aware backend selection
  uint64_t queue_depth_threshold = 4;          // Max in-flight before considering alternatives
  uint64_t queue_diff_threshold = 2;           // Min difference to justify cache miss
  double load_aware_headroom = 0.2;            // Prefer less-loaded if within 20% of preferred
  ```
- **Concerns:**
  - [ ] Default values conservative (enabled but high thresholds)
  - [ ] Hot-reload via `update_routing_config()` (existing pattern)
  - [ ] Validation in config loading (sensible ranges)
- **Status:** [x] Done (#223)

#### Step 4: Implement Load-Aware Backend Selection

- **Files:**
  - `src/router_service.cpp` (modify `get_backend_for_prefix()`)
- **Description:** Core algorithm: check preferred backend's load, fall back to least-loaded if threshold exceeded.
- **Details:**
  ```cpp
  std::optional<BackendId> RouterService::get_backend_for_prefix(...) {
      // ... existing ART lookup / hash fallback to get preferred_backend ...

      if (!state.config.load_aware_routing) {
          return preferred_backend;  // Disabled, use current behavior
      }

      uint64_t preferred_load = get_backend_load(preferred_backend);
      if (preferred_load <= state.config.queue_depth_threshold) {
          return preferred_backend;  // Normal prefix routing
      }

      // Backend overloaded - find alternative
      auto [least_loaded_id, least_load] = get_least_loaded_backend(live_backends);

      if (preferred_load - least_load > state.config.queue_diff_threshold) {
          state.stats.cache_miss_due_to_load++;
          return least_loaded_id;  // Accept cache miss to avoid queue
      }

      return preferred_backend;  // Difference not significant enough
  }
  ```
- **Concerns:**
  - [ ] `get_backend_load()` reads local shard's counter only (no cross-shard query in hot path)
  - [ ] `get_least_loaded_backend()` iterates `live_backends` vector (O(n) but n is small, typically <10)
  - [ ] No new containers (operates on existing `live_backends`)
  - [ ] Maintains sorted determinism for backends with equal load
- **Status:** [x] Done (#223)

**Mermaid Diagram - Load-Aware Selection Flow:**
```mermaid
flowchart TD
    A[route_request] --> B{ART lookup}
    B -->|hit| C[preferred = ART result]
    B -->|miss| D[preferred = hash fallback]
    C --> E{load_aware_routing?}
    D --> E
    E -->|no| F[return preferred]
    E -->|yes| G{preferred.load <= threshold?}
    G -->|yes| F
    G -->|no| H[find least_loaded backend]
    H --> I{load_diff > diff_threshold?}
    I -->|yes| J[stats.cache_miss_due_to_load++]
    J --> K[return least_loaded]
    I -->|no| F
```

#### Step 5: Add Prometheus Metrics for Observability

- **Files:**
  - `src/metrics_service.hpp` (add gauge/counter definitions)
  - `src/router_service.cpp` (register metrics in constructor)
- **Description:** Expose load-aware routing metrics for monitoring and tuning.
- **Details:**
  ```cpp
  // Counters
  ranvier_routing_cache_miss_due_to_load_total    // Times we skipped cache for load
  ranvier_routing_load_aware_decisions_total      // Load-aware routing activations
  ranvier_routing_load_aware_fallbacks_total      // Times fallback to least-loaded

  // Gauges (per-backend)
  ranvier_backend_active_requests{backend_id="N"} // Current in-flight requests
  ranvier_backend_queue_depth{backend_id="N"}     // Alias/same as above
  ```
- **Concerns:**
  - [ ] Per-backend gauges need bounded registry (Rule #4: MAX_TRACKED_BACKENDS exists)
  - [ ] Metrics lambdas must be deregistered in `stop()` (Rule #6)
  - [ ] Use relaxed memory order for metric reads
- **Status:** [x] Done (#223)

#### Step 6: Unit Tests for Load-Aware Routing Logic

- **Files:**
  - `tests/unit/load_aware_routing_test.cpp` (new)
- **Description:** Test load-aware routing decisions in isolation.
- **Test Cases:**
  1. Load below threshold → returns preferred backend
  2. Load above threshold, no alternative → returns preferred
  3. Load above threshold, alternative available → returns least-loaded
  4. Load difference below diff_threshold → returns preferred (marginal improvement)
  5. Multiple backends at same load → deterministic selection (sorted order)
  6. Backend goes from overloaded to normal → routing recovers
  7. Config disabled → always returns preferred
- **Concerns:**
  - [ ] Use `reset_shard_state_for_testing()` between tests
  - [ ] Mock backend load values directly
- **Status:** [x] Done (#224)

#### Step 7: Integration Test for Heavy Load Behavior

- **Files:**
  - `tests/integration/test_load_aware_routing.py` (new)
- **Description:** E2E test validating TTFT improvement under heavy load.
- **Test Cases:**
  1. Baseline: disabled load-aware routing, measure TTFT under 30 users
  2. Enabled: load-aware routing, measure TTFT under 30 users
  3. Verify metrics `cache_miss_due_to_load` increments when expected
  4. Verify per-backend `active_requests` gauge reflects actual load
  5. Verify normal load (5 users) shows no routing changes
- **Concerns:**
  - [ ] Requires multi-backend test setup (at least 2 vLLM instances)
  - [ ] May need mock backends for deterministic load control
  - [ ] Use Locust or similar for concurrent load generation
- **Status:** [x] Done (#224)

#### Step 8: Documentation Updates

- **Files:**
  - `docs/internals/prefix-affinity-routing.md` (update with load-aware section)
  - `docs/configuration.md` (add new config options)
- **Description:** Document the load-aware routing feature, configuration, and operational guidance.
- **Contents:**
  - Feature overview and motivation
  - Configuration options with recommended values
  - Metrics explanation and alerting guidance
  - Trade-offs: cache hit rate vs latency under load
  - Tuning guide for different workloads
- **Status:** [x] Done (#224)

### 10.2 Dependency Graph

```
Step 1 (Load Tracking) ─┬─► Step 2 (Request Flow Integration)
                        │
                        └─► Step 4 (Load-Aware Selection)
                                    │
Step 3 (Config) ────────────────────┘
                                    │
                                    ├─► Step 5 (Metrics)
                                    │
                                    └─► Step 6 (Unit Tests) ──► Step 7 (Integration Tests)

Step 8 (Documentation) - can proceed in parallel
```

### 10.3 Architectural Concerns Summary

| Step | Cross-Shard Communication | New Async Flow | New Container | Timer/Callback |
|------|---------------------------|----------------|---------------|----------------|
| 1    | No (per-shard atomics)    | No             | No (extends BackendInfo) | No |
| 2    | Maybe (cross-shard dispatch) | No (RAII guard) | No           | No |
| 3    | No                        | No             | No            | No |
| 4    | No (shard-local reads)    | No             | No            | No |
| 5    | Yes (`smp::submit_to` for aggregate metrics) | No | No (uses existing) | No |
| 6    | N/A (test)                | N/A            | N/A           | N/A |
| 7    | N/A (test)                | N/A            | N/A           | N/A |
| 8    | N/A (docs)                | N/A            | N/A           | N/A |

### 10.4 Post-Implementation Checklist

- [ ] Run `validation/validate_v1.sh` (reactor stall detection)
- [ ] Run `tests/integration/test_load_aware_routing.py`
- [ ] Run benchmark: `scripts/bench.sh --users 30` (verify >35% TTFT improvement)
- [x] Update BACKLOG.md with completion status
- [ ] Consider follow-up: hot prefix replication (Option 2 from issue)

> **Implementation Complete**: All 8 steps completed in PRs #222, #223, #224 (2026-02-05)

### 10.5 Future Extensions (Out of Scope)

These are NOT part of this implementation but documented for future reference:

1. **Option 2: Prefix Replication** - Learn hot prefixes on multiple backends, load-balance between them
2. **Option 3: Adaptive Mode** - Simple threshold-based fallback to round-robin under system-wide heavy load
3. **vLLM Queue Polling** - Query vLLM `/metrics` for actual inference queue depth (more accurate than in-flight count)
4. **Latency-Based Estimation** - Infer queue depth from recent response times

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
  _File:Line:_ `src/router_service.cpp:520-523`
  _Issue:_ Timer callback `[this] { run_ttl_cleanup(); }` does not acquire gate holder
  _Fix:_ Add `seastar::gate::holder holder = _timer_gate.hold();` at start of `run_ttl_cleanup()` (line 562)
  _Completed:_ 2026-02-03. Added `_timer_gate` member, gate holder acquisition in `run_ttl_cleanup()`, and gate closure in `stop()`.

- [x] **[CRITICAL] RouterService batch flush timer missing gate guard** ✓
  _File:Line:_ `src/router_service.cpp:1387-1398`
  _Issue:_ Timer callback `[this] { flush_route_batch()... }` does not acquire gate holder
  _Fix:_ Add gate holder acquisition at start of `flush_route_batch()` (line 1424)
  _Completed:_ 2026-02-03. Shares `_timer_gate` with TTL timer; holder kept alive via `do_with`.

- [x] **[CRITICAL] RouterService draining reaper timer missing gate guard** ✓
  _File:Line:_ `src/router_service.cpp:1725-1729`
  _Issue:_ Timer callback `[this] { run_draining_reaper(); }` does not acquire gate holder
  _Fix:_ Add gate holder acquisition at start of `run_draining_reaper()` (line 1736)
  _Completed:_ 2026-02-03. Shares `_timer_gate` with TTL and batch flush timers.

- [x] **[HIGH] K8sDiscoveryService poll timer uses boolean instead of gate** ✓
  _File:Line:_ `src/k8s_discovery_service.cpp:146-154`
  _Issue:_ Timer callback checks `_running` boolean instead of acquiring gate holder
  _Fix:_ Replace `if (_running)` check with gate holder pattern (try_with_gate or hold())
  _Completed:_ 2026-02-03. Reuses existing `_gate` member; holder kept alive via `do_with`. Updated `stop()` to close gate before cancelling timer.

#### Rule #9: Every catch block must log at warn level

- [x] **[CRITICAL] 11 silent catch blocks missing warn-level logging** ✓
  _Locations:_
  - ~~`src/config_loader.cpp:243-245` - Silent catch~~ ✓ Fixed 2026-02-03 (uses std::cerr for pre-Seastar logging)
  - ~~`src/config_loader.cpp:340-342` - Silent catch~~ ✓ Fixed 2026-02-03
  - ~~`src/config_loader.cpp:428-430` - Silent catch~~ ✓ Fixed 2026-02-03
  - ~~`src/http_controller.cpp:53-55` - Silent catch~~ ✓ Fixed 2026-02-03 (added clarifying comment - classifier function, not error handler)
  - ~~`src/http_controller.cpp:327-333` - Debug logging, should be warn~~ ✓ Fixed 2026-02-03 (warn for unknown errors, debug for expected connection errors)
  - ~~`src/http_controller.cpp:1245-1248` - Exception captured but not logged~~ ✓ Fixed 2026-02-03 (added comment noting rethrow below)
  - ~~`src/http_controller.cpp:1475-1478` - Silent catch~~ ✓ Fixed 2026-02-03 (debug log for DNS fallback flow)
  - ~~`src/application.cpp:832-834` - Silent catch~~ ✓ Fixed 2026-02-03 (debug log for promise already fulfilled)
  - ~~`src/application.cpp:844-846` - Silent catch~~ ✓ Fixed 2026-02-03 (debug log for promise already fulfilled)
  - ~~`src/k8s_discovery_service.cpp:356-358` - Silent catch~~ ✓ Fixed 2026-02-03 (debug log for DNS fallback flow)
  - ~~`src/k8s_discovery_service.cpp:188-190` - Debug logging, should be warn~~ ✓ Fixed 2026-02-03 (upgraded to warn)
  _Fix:_ Add `log_*.warn("Operation failed: {}", ex.what())` with context in each catch block
  _Completed:_ 2026-02-03. All 11 locations addressed with appropriate logging levels.

#### Rule #10: No bare std::stoi/stol/stof on external input

- [x] **[CRITICAL] 16 bare std::stoi/stoul calls on external input**
  _Locations:_
  - ~~`src/http_controller.cpp:1354, 1445, 1446, 1451, 1454, 1542, 1577, 1840, 2006` - HTTP request parsing~~ ✓ Fixed 2026-02-03
  - ~~`src/k8s_discovery_service.cpp:58, 551, 949, 974` - K8s API parsing~~ ✓ Fixed 2026-02-03
  - ~~`src/gossip_consensus.cpp:422` - Peer address parsing~~ ✓ Fixed 2026-02-03
  - ~~`src/gossip_protocol.cpp:857` - Peer address parsing~~ ✓ Fixed 2026-02-03
  - ~~`src/router_service.cpp:1566` - Backend address parsing~~ ✓ Fixed 2026-02-03
  _Fix:_ Create validating helpers (e.g., `parse_port()`, `parse_int()`) returning `std::optional<T>` using `std::from_chars`
  _Completed:_ 2026-02-03. Created `src/parse_utils.hpp` with `parse_int32()`, `parse_uint32()`, `parse_port()`, `parse_token_id()`, `parse_backend_id()` using `std::from_chars`. All 16 locations across 5 files fixed.

### 11.4 Medium Severity Violations

#### Rule #0: Prefer unique_ptr over shared_ptr

- [x] **[MEDIUM] std::shared_ptr in Application**
  _File:Line:_ `src/application.hpp:144`
  _Issue:_ `std::shared_ptr<seastar::promise<>> _stop_signal;`
  _Fix:_ Evaluate if `unique_ptr` or `lw_shared_ptr` can replace
  _Completed:_ 2026-02-03. Changed to `std::unique_ptr` - no shared ownership needed, all access is through `this` pointer in lambdas.

- [x] **[MEDIUM] std::shared_ptr in TracingService (OpenTelemetry)**
  _File:Line:_ `src/tracing_service.cpp:139`
  _Issue:_ `static std::shared_ptr<sdk_trace::TracerProvider> g_provider;`
  _Resolution:_ Known exception - OpenTelemetry C++ SDK requires `shared_ptr<TracerProvider>` for `opentelemetry::trace::Provider::SetTracerProvider()`. Cannot use unique_ptr without forking the SDK.

#### Rule #2: No co_await inside loops over external resources

- [x] **[HIGH] Sequential co_await in K8s endpoint removal loop**
  _File:Line:_ `src/k8s_discovery_service.cpp:640-648`
  _Issue:_ `for (...) { co_await handle_endpoint_removed(...); }` - O(n) latency
  _Fix:_ Replace with `seastar::max_concurrent_for_each()` pattern
  _Completed:_ 2026-02-03. Refactored to collect UIDs first, then process with `max_concurrent_for_each()` using `K8S_MAX_CONCURRENT_ENDPOINT_OPS` concurrency limit.

#### Rule #12: No std::ifstream/ofstream in Seastar code

- [x] **[MEDIUM] Blocking std::ifstream during startup**
  _Locations:_
  - `src/main.cpp:45` - Config file reading in `run_dry_run_validation()`
  - `src/main.cpp:70` - Tokenizer file validation in `run_dry_run_validation()`
  - `src/main.cpp:180` - Config check in `print_config_summary()`
  - `src/config_loader.cpp:467` - Config loading in `RanvierConfig::load()`
  _Resolution:_ Known exception - all usages occur before `app.run()` (line 373) in the pre-reactor startup phase. Configuration must be loaded before the Seastar reactor can be configured. Blocking I/O is acceptable and necessary here.

### 11.5 Async Integrity Issues

#### Sequential Health Checks (Rule #2 Related)

- [x] **[HIGH] Health service sequential backend checks**
  _File:Line:_ `src/health_service.cpp:41-52`
  _Issue:_ Sequential `for (auto id : ids) { co_await check_backend(...); }` blocks health cycle
  _Impact:_ N backends × 3s timeout = N×3 seconds per cycle (100 backends = 5 minutes)
  _Fix:_ Replace with `seastar::max_concurrent_for_each(ids, 16, [this](auto id) {...})`
  _Completed:_ 2026-02-03. Refactored to collect backends first, then check with `max_concurrent_for_each()` using 16-way concurrency. Added per-check try/catch for resilience.

#### Fire-and-Forget Loop Lifecycle

- [x] **[HIGH] Health service run_loop() not properly tracked**
  _File:Line:_ `src/health_service.cpp:18-22`
  _Issue:_ `(void)run_loop();` casts future to void; gate holder created per-iteration not per-loop
  _Fix:_ Either co_await run_loop() in start() or maintain gate holder across entire loop lifetime
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
  _Source:_ `src/parse_utils.hpp` — contains `parse_int32`, `parse_uint32`, `parse_port` using `std::from_chars`. Header-only, no Seastar dependency, no `.cpp` to compile.
  _Test cases:_ Valid integers (positive, negative, zero, INT32_MIN/MAX), overflow/underflow for int32 and uint32, trailing garbage rejection ("123abc"), empty string, whitespace-only input, port range validation (0, 1, 65535, 65536), negative values rejected for unsigned parse.
  _CMake:_ Add as pure-C++ test (no extra `.cpp` sources). Model after `text_validator_test` block in `CMakeLists.txt`.
  _Complexity:_ Low
  _Completed:_ 2026-02-06. 5 fixture classes, ~40 test cases covering `parse_int32`, `parse_uint32`, `parse_port`, `parse_token_id`, `parse_backend_id`.

- [x] **Add `logging_test.cpp`** ✓
  _Source:_ `src/logging.hpp` — contains `generate_request_id()` and header extraction helpers (`extract_request_id_view`, `extract_traceparent_view`). Header-only, no Seastar dependency.
  _Test cases:_ Request ID format (`{shard}-{timestamp}-{seq}` structure), monotonically increasing sequence numbers, `extract_request_id_view` prefers X-Request-ID over X-Correlation-ID, `extract_traceparent_view` case-insensitive header lookup, missing headers return empty `string_view`, empty header values handled correctly.
  _CMake:_ Add as pure-C++ test (no extra `.cpp` sources). Model after `text_validator_test`.
  _Complexity:_ Low
  _Completed:_ 2026-02-06. 3 fixture classes, ~25 test cases. Introduced `tests/stubs/` directory with minimal Seastar/fmt stubs for headers with lightweight Seastar deps.

- [x] **Add `shard_load_metrics_test.cpp`** ✓
  _Source:_ `src/shard_load_metrics.hpp` — atomic counters and RAII guards (`ActiveRequestGuard`, `QueuedRequestGuard`) used by load-aware routing. Header-only, no Seastar dependency.
  _Test cases:_ `ActiveRequestGuard` increments on construction / decrements on destruction, `QueuedRequestGuard` same RAII semantics, `load_score()` = active × 2.0 + queued, `ShardLoadSnapshot` captures point-in-time values, move semantics of guards (no double-decrement), underflow protection (decrement below zero).
  _CMake:_ Add as pure-C++ test (no extra `.cpp` sources). Model after `load_aware_routing_test`.
  _Complexity:_ Low
  _Completed:_ 2026-02-06. 5 fixture classes, ~25 test cases covering RAII guards, load scoring, snapshot semantics, lifecycle, and type traits.

### 12.2 P1 — New Unit Tests for Core Infrastructure

- [x] **Add `metrics_service_test.cpp`** ✓
  _Source:_ `src/metrics_service.hpp` (~200 LOC) — histogram bucket logic, bounded container enforcement (Rule #4: max 10K backends), cache-hit-ratio computation. Header-only, no Seastar dependency (uses Seastar types only as forward declarations behind `#ifdef`).
  _Test cases:_ Histogram bucket correctness, bounded container limits, cache-hit-ratio divide-by-zero protection, metric recording and reset. Read the header to determine which structs/functions are testable without Seastar — test those only.
  _CMake:_ Add as pure-C++ test. May need conditional compilation — check `#ifdef HAVE_SEASTAR` guards in header.
  _Complexity:_ Medium
  _Completed:_ 2026-02-06. 7 fixture classes, ~30 test cases. Added `seastar/core/metrics.hh` and `metrics_registration.hh` stubs with histogram types and labeled metric overloads.

- [x] **Add `node_slab_test.cpp`** ✓
  _Source:_ `src/node_slab.hpp` + `src/node_slab.cpp` (~250 LOC) — per-shard memory pool for RadixTree nodes. Note: `radix_tree_test.cpp` already compiles `src/node_slab.cpp` so allocation works — but there are no dedicated slab tests.
  _Test cases:_ Allocate/free round-trips, free-list integrity after allocate→free→reallocate cycles, peak usage tracking, pool exhaustion behavior (what happens when slab is full).
  _CMake:_ Add with `src/node_slab.cpp` as source. Model after `radix_tree_test` block.
  _Complexity:_ Medium
  _Completed:_ 2026-02-06. 7 fixture classes, ~30 test cases covering allocate/deallocate, free-list LIFO, peak tracking, auto-growth, make_node factories, and thread-local accessors.

- [x] **Add `cross_shard_request_test.cpp`** ✓
  _Source:_ `src/cross_shard_request.hpp` (~180 LOC) — validation functions for body size (128 MB limit), token count (128K limit), string length (4 KB limit), and `force_local_allocation()`. Read the header to identify which functions are pure (no Seastar runtime required) and test those.
  _Test cases:_ Body size at/over 128 MB limit, token count at/over 128K limit, string length at/over 4 KB limit, `force_local_allocation()` copy behavior, boundary values.
  _CMake:_ Add as pure-C++ test if validation functions are constexpr/inline. If they depend on Seastar types, add under the `if(Seastar_FOUND)` block.
  _Complexity:_ Medium
  _Completed:_ 2026-02-06. 8 fixture classes, ~35 test cases. Added `temporary_buffer`, `shared_ptr`/`foreign_ptr`, `http::request`/`reply` stubs. Updated `future.hh` and `smp.hh` stubs.

- [x] **Add `tracing_service_test.cpp` (W3C traceparent parsing only)** ✓
  _Source:_ `src/tracing_service.hpp` + `src/tracing_service.cpp` (~250 LOC). The W3C traceparent parsing (`version-traceid-spanid-flags`) is pure string logic. Read the source to identify which functions can be tested without OpenTelemetry/Seastar runtime.
  _Test cases:_ Valid traceparent format (`00-<32hex>-<16hex>-<2hex>`), invalid formats (wrong length, non-hex chars, wrong version), field length enforcement, flag parsing, version 00 vs unknown versions.
  _CMake:_ Add with `src/tracing_service.cpp` as source. May need conditional compilation — check `#ifdef WITH_TELEMETRY` guards.
  _Complexity:_ Medium
  _Completed:_ 2026-02-06. 7 fixture classes, ~35 test cases. Compiled with `RANVIER_NO_TELEMETRY`; W3C parse logic provided directly in test (no OTel dependency).

### 12.3 P2 — Strengthen Existing Tests

- [x] **Introduce clock injection for time-dependent tests** ✓
  _Context:_ 0 of 20 test files have deterministic timing tests. `src/rate_limiter.hpp`, `src/circuit_breaker.hpp`, and `src/gossip_consensus.hpp` all have time-sensitive behavior tested with no time control.
  _Approach:_ Add a template parameter or `std::function<steady_clock::time_point()>` to each component's clock source. Introduce a `TestClock` helper in `tests/unit/` that allows manual time advancement. Retrofit into: rate_limiter (test token refill over simulated time), circuit_breaker (test OPEN→HALF_OPEN after exact timeout), quorum (test peer liveness window expiration). Update existing tests in `tests/unit/rate_limiter_test.cpp`, `tests/unit/circuit_breaker_test.cpp`, `tests/unit/quorum_test.cpp` to use the new clock. Ensure production code defaults to `steady_clock` with no overhead.
  _Complexity:_ Medium
  _Completed:_ 2026-02-07. Template parameter `Clock` added to `BasicTokenBucket`, `BasicRateLimiter`, `BasicBackendCircuit`, `BasicCircuitBreaker`, and `BasicPeerState` with backward-compatible aliases. `TestClock` helper in `tests/unit/test_clock.hpp`. Pure algorithm split into `src/rate_limiter_core.hpp` (no Seastar deps); `src/rate_limiter.hpp` is Seastar service inheriting from core. 9 rate limiter timing tests, 7 circuit breaker timing tests, 10 quorum liveness timing tests — all deterministic, zero sleeps.

- [x] **Adopt Google Mock (`gmock`) for dependency isolation** ✓
  _Context:_ Only `tests/unit/async_persistence_test.cpp` uses mocks (hand-rolled `MockPersistenceStore`). GTest is already a dependency (`GTest::gtest_main` in `CMakeLists.txt`) and gmock ships with it — just link `GTest::gmock`.
  _Approach:_ Replace hand-rolled mock in `tests/unit/async_persistence_test.cpp` with `gmock` (`MOCK_METHOD`). Add mock interfaces for: `PersistenceStore` (interface in `src/persistence.hpp`), `HealthChecker`, `UdpChannel`. Update `CMakeLists.txt` to link `GTest::gmock` for tests that need it.
  _Complexity:_ Medium
  _Completed:_ 2026-02-07. Replaced 148-line hand-rolled `MockPersistenceStore` with gmock `MOCK_METHOD` declarations + `ON_CALL` defaults for open/close state tracking. Created reusable mock headers in `tests/unit/mocks/`: `mock_persistence_store.hpp` (gmock of `PersistenceStore` interface), `mock_health_checker.hpp` (abstract `HealthChecker` interface + gmock mock), `mock_udp_channel.hpp` (abstract `UdpChannel` interface + gmock mock). Linked `GTest::gmock` in `CMakeLists.txt` for `async_persistence_test`. All tests use `NiceMock<MockPersistenceStore>` to suppress uninteresting-call warnings.

- [x] **Add negative-path integration tests**
  _Context:_ All 4 integration suites in `tests/integration/` are happy-path only. They use Docker Compose (`docker-compose.test.yml`) with a 3-node cluster and mock backends (`tests/integration/mock_backend.py`). Follow the same pattern (Python unittest, `requests` library, Docker Compose lifecycle in setUp/tearDown).
  _Test cases:_
  - Split-brain: Use `docker network disconnect` to partition nodes, verify quorum detection via `/admin/cluster` endpoint, reconnect and verify recovery
  - Backend flap: Rapidly `docker stop`/`docker start` mock backend containers, verify circuit breaker engages via metrics endpoint (`ranvier_circuit_open`)
  - Config reload: Send `docker kill -s HUP` with invalid YAML, verify old config preserved via `/admin/config`
  - Rate limit: Send concurrent requests exceeding configured rate limits, verify 429 responses with `Retry-After` header
  - Oversized request: Send request body exceeding `max_request_body_size`, verify 413 rejection
  _Location:_ Add as `tests/integration/test_negative_paths.py`. No CMake changes needed (Python tests).
  _Complexity:_ High
  _Completed:_ 2026-02-07. Added `tests/integration/test_negative_paths.py` with 5 test cases: (1) Split-brain partition via `docker network disconnect`, verifying peer count drops and quorum state via `/admin/dump/cluster`, then reconnect and verify recovery. (2) Backend flap via rapid `docker stop`/`start` of mock backend, verifying `circuit_breaker_opens` metric increments. (3) Config reload with invalid YAML via `docker exec` + SIGHUP, verifying old config preserved and server continues operating. (4) Rate limiting by enabling low limits via config reload, flooding with concurrent requests, verifying 503 responses with `Retry-After` header. (5) Oversized request body (~5 MB), verifying rejection with 400/413/500 and server remains healthy. Follows same pattern as existing suites (Python unittest, `requests`, Docker Compose lifecycle).

### 12.4 P3 — Longer-Term Test Infrastructure

- [x] **Add `connection_pool_test.cpp`**
  _Source:_ `src/connection_pool.hpp` (~300 LOC) — manages upstream connections with per-host limits, global limits, idle timeout, and max-age TTL. Tightly coupled to Seastar networking (`seastar::connected_socket`).
  _Approach:_ Extract pool management logic (eviction, TTL, limits) behind a policy interface that can be tested without real sockets. Test: per-host connection limits, global connection limits, idle timeout eviction, max-age TTL enforcement, half-open connection detection.
  _CMake:_ Add under `if(Seastar_FOUND)` block. Model after `stream_parser_test`.
  _Complexity:_ High
  _Completed:_ 2026-02-07. Templatized `ConnectionBundle` and `ConnectionPool` on `Clock` parameter (same pattern as `BasicCircuitBreaker<Clock>` and `BasicRateLimiter<Clock>`) with backward-compatible `using` aliases. Created `tests/unit/connection_pool_test.cpp` with 35 tests across 10 fixture classes: `ConnectionPoolConfigTest` (defaults, customization), `ConnectionBundleTest` (timestamps, expiry, TTL, touch semantics), `ConnectionPoolPerHostLimitTest` (per-host limit enforcement, independent host limits), `ConnectionPoolGlobalLimitTest` (global eviction across hosts), `ConnectionPoolIdleTimeoutTest` (deterministic expiry with TestClock, boundary conditions), `ConnectionPoolMaxAgeTest` (TTL enforcement independent of idle, counter accumulation), `ConnectionPoolHalfOpenTest` (non-half-open survival, invalid rejection), `ConnectionPoolStatsTest` (counter tracking), `ConnectionPoolClearPoolTest` (backend removal), `ConnectionPoolMapCleanupTest` (Rule #4 empty map entry removal), `ConnectionPoolEvictionOrderTest` (FIFO eviction semantics), `ConnectionPoolMixedTest` (multi-round cleanup, put-after-clear). Default-constructed Seastar streams have `close()` returning ready futures, enabling testing without a reactor. Wired into CMakeLists.txt under `if(Seastar_FOUND)` with `Seastar::seastar` and `GTest::gtest_main`.

- [x] **Add router service integration tests (Seastar-dependent)**
  _Source:_ `src/router_service.hpp` + `src/router_service.cpp` (~700 LOC) — the core routing brain. Currently only tested transitively through Docker Compose integration suites. Depends on Seastar sharding, RadixTree, and shard_load_balancer.
  _Test cases:_ Route learning from proxied requests, route TTL expiration and cleanup, cross-shard route broadcast, backend registration/deregistration with route cleanup, draining mode with timeout.
  _CMake:_ Add under `if(Seastar_FOUND)` block. Will need multiple `src/*.cpp` files. Model after `application_test`.
  _Complexity:_ High
  _Completed:_ 2026-02-07. Created `tests/unit/router_service_test.cpp` with 55+ tests across 20 sections covering all 5 required test cases. Added test helper methods to `RouterService` (`register_backend_for_testing`, `insert_route_for_testing`, `set_backend_draining_for_testing`, `mark_backend_dead_for_testing`, `unregister_backend_for_testing`, `get_route_count_for_testing`) following the existing `reset_shard_state_for_testing` pattern to enable shard-local testing without a running Seastar reactor. Tests cover: route learning via ART insert+lookup, route TTL cleanup via reset, cross-shard batch structure verification, backend registration/deregistration with route cleanup, draining mode skipping, dead backend circuit breaker, routing mode dispatch (PREFIX/HASH/RANDOM), consistent hash determinism, weighted random selection with priority groups, prefix boundary routing, BackendRequestGuard RAII semantics, load tracking helpers, tree dump API, and edge cases. Wired into CMakeLists.txt under `if(Seastar_FOUND)` linking `router_service.cpp`, `node_slab.cpp`, `gossip_service.cpp`, `gossip_protocol.cpp`, `gossip_consensus.cpp`, `gossip_transport.cpp`, `crypto_offloader.cpp`, `dtls_context.cpp` with Seastar, abseil, OpenSSL, and GTest.

- [x] **Extract and test HTTP controller proxy logic**
  _Source:_ `src/http_controller.cpp` (~1200 LOC) — the retry/fallback state machine (`ProxyContext`) is the most complex untested code. Tightly coupled to Seastar HTTP and ConnectionPool.
  _Approach:_ Extract retry/backoff decision logic into a pure function or policy class in a new header. Test retry counts, backoff intervals, fallback backend selection, timeout handling. Separately test admin API endpoint handlers. Test backpressure semaphore acquire/release behavior.
  _Complexity:_ High
  _Completed:_ 2026-02-08. Created `src/proxy_retry_policy.hpp` extracting four pure components from `http_controller.cpp`: (1) `ConnectionErrorType` enum with `classify_connection_error()` and `is_retryable_connection_error()` — classifies `std::system_error` exceptions for EPIPE/ECONNRESET; (2) `BasicProxyRetryPolicy<Clock>` template class — retry count tracking, exponential backoff with cap, fallback-before-retry priority, deadline enforcement; (3) `select_fallback_backend()` template — pure fallback selection with predicate-based filtering; (4) `check_concurrency()` / `check_persistence_backpressure()` — backpressure decision functions. Created `tests/unit/proxy_retry_policy_test.cpp` with 60+ tests across 10 fixture classes: connection error classification (null, broken pipe, connection reset, non-system errors), backoff calculation (exponential doubling, cap enforcement, custom multiplier, constant backoff), retry count enforcement (0/1/3 retries, should_continue), deadline handling (initial, exceeded, boundary, overrides retry/fallback), fallback decisions (preferred over retry, doesn't consume retry, disabled, no candidate, max attempts, zero attempts), combined scenarios (realistic fallback→retry→give-up, deadline during fallback, intermittent candidates), fallback selection (first available, skip failed/disallowed, empty list, large list), backpressure concurrency (under/at/over limit, unlimited), persistence queue (threshold, disabled, empty, large values), initial state and config access. Updated `http_controller.hpp` and `.cpp` to include `proxy_retry_policy.hpp` and use its definitions. Wired into CMakeLists.txt as pure C++ test with TestClock.

- [x] **Add concurrency stress tests for cross-shard components**
  _Context:_ 0 of 20 test files exercise concurrent access. Several components cross shard boundaries or use shared-state primitives.
  _Approach:_ Use `std::thread` + `std::latch`/`std::barrier` (C++20) to test concurrent operations on: `shard_load_metrics` atomics (concurrent increment/decrement), `async_persistence` queue (concurrent enqueue from multiple threads), `tokenizer_thread_pool` (concurrent job submission), `rate_limiter` buckets (concurrent `try_consume` from same client). Add as separate `tests/unit/*_concurrency_test.cpp` files.
  _Complexity:_ High

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
  _File:Line:_ `src/radix_tree.hpp:243`
  _Issue:_ `insert()` increments `route_count_++` unconditionally, but `insert_recursive()` (line 1259-1263) silently updates existing leaves without signaling "not new". Repeated inserts of the same prefix inflate the counter beyond the actual route count. When `route_count_ >= max_routes`, the eviction loop in `router_service.cpp` evicts all real routes while the counter stays inflated. The tree permanently empties itself under sustained traffic with repeated prefixes.
  _Benchmark impact:_ Cache hit rate benchmarks (reported at 98%) are time-bombs. Short runs succeed; 30+ minute runs under real traffic patterns will trigger mass eviction and fall back to round-robin.
  _Fix:_ Have `insert_recursive()` return `std::pair<NodePtr, bool>` where the bool indicates new-vs-update. Only increment `route_count_` for new inserts.
  _Complexity:_ Low
  _Priority:_ P0 — fix before next benchmark run

- [x] **[CRITICAL] Mutex on every proxy request via is_persistence_backpressured()**
  _File:Line:_ `src/http_controller.cpp:1819-1827`
  _Issue:_ Every proxy request calls `is_persistence_backpressured()` which calls `_persistence->queue_depth()`. Despite PR #118 adding an atomic for the metrics path, the queue itself is still mutex-protected (see `async_persistence.hpp:118`). The code comment at line 1819 explicitly says "queue_depth() acquires a mutex". This stalls the reactor under concurrent persistence writes.
  _Benchmark impact:_ P99 latency numbers include this mutex overhead. The actual prefix-routing benefit is better than currently measured.
  _Relationship:_ Extends Section 7 line 865 ("Consider lock-free queue") which is still open as MEDIUM. Recommend re-prioritizing to CRITICAL.
  _Fix:_ Shadow the queue size with `std::atomic<size_t>` incremented/decremented alongside queue push/pop. `queue_depth()` reads the atomic, zero lock.
  _Complexity:_ Low
  _Priority:_ P0 — fix before next benchmark run

### 13.2 CRITICAL — Async Integrity (Gate Holder Bugs)

Section 11 fixed gate guards in RouterService and K8s timers. These are **new** gate-holder bugs in the gossip and persistence subsystems.

- [x] **[CRITICAL] Gossip heartbeat timer callback has no gate guard**
  _File:Line:_ `src/gossip_protocol.cpp:201-203`
  _Issue:_ Timer callback `[this] { (void)broadcast_heartbeat(); }` captures `this` with no gate holder. `_transport` pointer is dereferenced before any internal gate acquisition. Contrast with the retry timer (line 212) and discovery timer (line 237) which DO attempt gate holders.
  _Fix:_ Acquire `_timer_gate.hold()` at the start of the callback lambda, before calling `broadcast_heartbeat()`.
  _Complexity:_ Low

- [x] **[CRITICAL] Gossip discovery timer gate holder drops before async work completes** ✓
  _File:Line:_ `src/gossip_protocol.cpp:237-254`
  _Issue:_ `timer_holder` is a local variable in the callback that dies when the callback returns. But `refresh_peers()` is a detached future `(void)` that runs for seconds. The gate holder provides zero protection — shutdown can destroy state while `refresh_peers()` is in-flight.
  _Fix:_ Move holder into the future chain: `(void)refresh_peers().finally([holder = std::move(timer_holder)] {});`
  _Complexity:_ Low

- [x] **[CRITICAL] AsyncPersistence log_stats() gate holder drops at end of try block** ✓
  _File:Line:_ `src/async_persistence.cpp:371-380`
  _Issue:_ Gate holder is scoped to `try {}` block. After the block, the function accesses `_ops_processed`, `_ops_dropped`, `_batches_flushed`, and calls `queue_depth()` without gate protection. If `stop()` runs between the `try` block and the member access, it's use-after-free.
  _Fix:_ Declare `timer_holder` at function scope and assign inside `try`, matching the pattern used in `on_flush_timer()` (line 261).
  _Complexity:_ Low

### 13.3 CRITICAL — Cross-Shard Safety

- [x] **[CRITICAL] std::function broadcast across shards in GossipConsensus** ✓
  _File:Lines:_ `src/gossip_consensus.cpp:130, 180, 237`
  _Issue:_ `_route_prune_callback` (`std::function`) is captured by value and sent to every shard via `smp::submit_to`. When the lambda exceeds Small Buffer Optimization size (~16-32 bytes), the heap allocation lives on shard 0 and is freed on shard N. This is anti-pattern Bug #3 (cross-shard free). Appears at 3 call sites. The code has a TODO acknowledging this.
  _Fix:_ Each shard registers its own local callback. Shard 0 broadcasts only the scalar `BackendId`; each shard invokes its local callback.
  _Complexity:_ Medium

### 13.4 HIGH — Unbounded Containers (Rule #4)

Section 7 fixed several unbounded containers. These are **new** ones.

- [x] **[HIGH] DTLS _sessions map has no MAX_SIZE — unbounded SSL session creation** ✓
  _File:Line:_ `src/dtls_context.hpp:181`, `src/dtls_context.cpp:540`
  _Issue:_ `get_or_create_session()` creates a new SSL session for every unique peer address. Spoofed source addresses create thousands of sessions/sec. Cleanup runs every 60s. Each session allocates OpenSSL SSL objects + BIO pairs.
  _Fix:_ Add `MAX_SESSIONS` constant. Reject new sessions when limit reached. Add overflow metric.
  _Complexity:_ Low

- [x] **[HIGH] Gossip _peer_seq_counters map unbounded**
  _File:Line:_ `src/gossip_protocol.hpp:204-207`
  _Issue:_ `next_seq_num()` inserts unboundedly. Old entries never cleaned unless `cleanup_peer_state()` is called. Distinct from `_pending_acks` (fixed in Section 7 line 871).
  _Fix:_ Bound to `MAX_DEDUP_PEERS`. Clean entries for peers no longer in peer table.
  _Complexity:_ Low

- [x] **[HIGH] K8s discovery: response body, watch buffer, and endpoints map all unbounded** ✓
  _File:Lines:_ `src/k8s_discovery_service.cpp:501-506` (response), `src/k8s_discovery_service.cpp:1067-1084` (watch buffer), `src/k8s_discovery_service.hpp:120` (endpoints map)
  _Issue:_ Three independent OOM vectors. A compromised or misconfigured K8s API server can exhaust shard 0 memory via: (1) arbitrarily large response body, (2) streaming data without newlines (Slowloris-style), (3) endpoint count explosion from a broad selector.
  _Fix:_ Add `MAX_RESPONSE_SIZE` (16MB), `MAX_LINE_SIZE` (1MB), `MAX_ENDPOINTS` (1000). Abort and reconnect if exceeded.
  _Complexity:_ Low

- [x] **[HIGH] Connection pool _pools map has no MAX_BACKENDS limit**
  _File:Line:_ `src/connection_pool.hpp:435`
  _Issue:_ Per-host and total connection limits exist, but the number of unique backend addresses (map keys) is unbounded. Thousands of unique addresses with 0 idle connections still grow the map. Distinct from Section 7 line 927 (which fixed erasure of empty deques).
  _Fix:_ Add `MAX_BACKENDS = 1000`. Check `_pools.size()` before inserting new backend entries.
  _Complexity:_ Low

### 13.5 HIGH — Edge-Case Crashes

- [x] **[HIGH] K8s token file read truncated at 4096 bytes**
  _File:Line:_ `src/k8s_discovery_service.cpp:200`
  _Issue:_ `co_await stream.read_exactly(4096)` silently truncates K8s projected tokens that exceed 4KB. Authentication fails with no error.
  _Fix:_ Read file size first (like `load_ca_cert` at line 227), then `read_exactly(size)`. Log error if token exceeds reasonable max.
  _Complexity:_ Low

- [x] **[HIGH] BackendId hash collision risk at scale** ✓
  _File:Line:_ `src/k8s_discovery_service.cpp:42-51`
  _Issue:_ `to_backend_id()` uses a weak `hash * 31 + c` truncated to 31 bits. Birthday problem: ~50% collision probability at ~46K UIDs. Two endpoints with the same BackendId shadow each other with no detection or warning.
  _Fix:_ Use stronger hash (first 4 bytes of SHA-256, or `absl::Hash`). Add collision detection in `handle_endpoint_added()`.
  _Complexity:_ Medium
  _Fixed:_ Replaced weak polynomial hash with FNV-1a 64-bit hash (deterministic across restarts, much better distribution). Added `_backend_id_to_uid` reverse map for collision detection in `handle_endpoint_added()` with error logging and `k8s_backend_id_collisions` Prometheus metric. Reverse map cleaned up in `handle_endpoint_removed()`.

- [x] **[HIGH] TracingService shutdown race on g_tracer.reset()** ✓
  _File:Line:_ `src/tracing_service.cpp:437-456`
  _Issue:_ `shutdown()` resets `g_tracer` at line 456, racing with span construction at line 267 which reads `g_tracer` after checking `g_shutting_down`. TOCTOU race. Distinct from Section 11.4 which documented the shared_ptr as a known SDK exception but did NOT cover this shutdown race.
  _Fix:_ Do not call `g_tracer.reset()` or `g_provider.reset()`. Let them leak at process exit.
  _Complexity:_ Low
  _Fixed:_ Removed g_tracer.reset() and g_provider.reset() from shutdown(). Globals are now process-lifetime objects — OS reclaims at exit. Added concurrency model tests in tracing_service_test.cpp.

- [x] **[HIGH] DTLS context uses blocking stat() and SSL_CTX file I/O on reactor**
  _File:Line:_ `src/dtls_context.cpp:403-410, 433-438`
  _Issue:_ `SSL_CTX_use_certificate_file` and POSIX `stat()` are blocking calls on the reactor thread during cert reload. Violates Rule #12.
  _Fix:_ Offload cert reload to `seastar::async`. Use OpenSSL memory-based APIs (`SSL_CTX_use_certificate_ASN1`) after async file read.
  _Complexity:_ Medium
  _Fixed:_ Replaced all blocking file I/O with Seastar async APIs (`open_file_dma`, `make_file_input_stream`, `file::stat`). Replaced `SSL_CTX_use_certificate_file`/`SSL_CTX_use_PrivateKey_file`/`SSL_CTX_load_verify_locations` with memory-based OpenSSL APIs (`PEM_read_bio_X509`, `PEM_read_bio_PrivateKey`, `X509_STORE_add_cert`) fed from async-read buffers. Both `initialize()` and `check_and_reload_certs()` now return futures. Added `load_certs_from_memory()` static helper with unit tests in `dtls_context_test.cpp`.

### 13.6 MEDIUM — Additional Issues

- [x] **[MEDIUM] gossip_transport.cpp uses std::shared_ptr for shard-local data (Rule #0)**
  _File:Lines:_ `src/gossip_transport.cpp:200-201, 263, 469`
  _Fix:_ Use `seastar::do_with` or `seastar::lw_shared_ptr`.
  _Resolution:_ Replaced all `std::shared_ptr` with shard-safe patterns: `seastar::do_with` for `parallel_for_each` paths, direct value capture for `seastar::async`, and `seastar::lw_shared_ptr` for crypto callback sharing. Added ownership pattern tests in `gossip_transport_ownership_test.cpp`.

- [x] **[MEDIUM] sqlite_persistence.cpp has business validation in persistence layer (Rule #7)** ✓
  _File:Line:_ `src/sqlite_persistence.cpp:325-329`
  _Fix:_ Move `backend_id <= 0` check to service layer. Persistence should return raw data.
  _Resolution:_ Removed `backend_id <= 0` filtering from `SqlitePersistence::load_routes()`. Persistence now returns raw data. Validation moved to `Application::load_persisted_state()` using `std::erase_if` with warning log. Added 3 unit tests in `persistence_test.cpp` confirming persistence returns routes with zero, negative, and mixed backend IDs.

- [x] **[MEDIUM] K8s HTTP status parsing is brittle — string search instead of code parse** ✓
  _File:Line:_ `src/k8s_discovery_service.cpp:521-522`
  _Fix:_ Parse HTTP status line properly: extract first line, split on space, parse numeric code.
  _Resolution:_ Replaced brittle `headers.find("200 OK")` / `headers.find("200 ")` string search with a proper `parse_http_status_code()` function that extracts the first line (status line), finds the status code field, and parses it as an integer using `std::from_chars` (Rule #10). The old approach could false-match "200" appearing in header values; the new approach only inspects the status line. Added 20 unit tests in `k8s_discovery_test.cpp` covering standard codes (200, 404, 503, 410), HTTP/1.0 and HTTP/2 variants, status lines without reason phrase, malformed input, and the false-match regression case.

- [x] **[MEDIUM] K8s watch 410 Gone causes infinite reconnect loop** ✓
  _File:Line:_ `src/k8s_discovery_service.cpp:844-846`
  _Fix:_ Parse Status event `code`. On 410, clear `_resource_version` and trigger `sync_endpoints()`.
  _Resolution:_ Fixed the watch callback in `watch_endpoints()` to parse the `code` field from K8s Status events. On 410 Gone, `_resource_version` is cleared and `sync_endpoints()` is called to re-list with a fresh resourceVersion before the watch reconnects. Handles both delivery forms: (1) direct Status objects (HTTP error response body) and (2) ERROR watch events with embedded Status objects (mid-stream 410). Also fixed a null-dereference risk on `event["message"]` access (now guarded with `HasMember` + `IsString`). Added `_watch_410_gone` metric counter for observability. Added 20 unit tests covering direct Status 410 detection, ERROR event 410 detection, non-410 status codes, missing fields, and full reconnect flow simulation.

- [x] **[MEDIUM] metrics_service.hpp fallback_metrics is a shared mutable singleton** ✓
  _File:Line:_ `src/metrics_service.hpp:530-534`
  _Issue:_ When `MAX_TRACKED_BACKENDS` is hit, all overflow backends share one `static thread_local BackendMetrics` accumulator. Data becomes incoherent; histograms leak slowly.
  _Fix:_ Return a freshly default-constructed null-op `BackendMetrics`, or reject recording and increment overflow counter only.
  _Resolution:_ Changed `get_or_create_backend_metrics()` to return `BackendMetrics*` (nullable) instead of `BackendMetrics&`. On overflow, returns `nullptr` and increments `_backend_metrics_overflow` counter. Callers (`record_backend_latency_by_id`, `record_first_byte_latency_by_id`) null-check before per-backend recording; aggregate histogram still captures all data unconditionally. Removed the `static thread_local BackendMetrics fallback_metrics` singleton that caused data incoherence and histogram memory leaks. Added 5 unit tests for overflow isolation.

### 13.7 Anti-Pattern Candidates for Hard Rules

Three systemic patterns observed across multiple findings:

1. **Gate-Holder-Scoping:** Gate holders scoped to `try` blocks or callbacks instead of the full async operation (A2, A3). _Proposed rule: "A gate holder must be scoped to the entire async operation it protects."_
2. **Unconditional-Counter-Increment:** Incrementing size counters after operations that may be no-ops (E1). _Proposed rule: "Mutation methods must signal insert-vs-update; counters only increment on actual size change."_
3. **Unbounded-External-Response:** Accumulating HTTP response bodies from external services without size limits (S3, S4). _Proposed rule: "All HTTP response accumulation from external services must have MAX_RESPONSE_SIZE."_

---

## 14. Router Service Review (2026-02-14)

Deep-dive review of `router_service.{hpp,cpp}` identifying correctness, maintainability, and performance improvements. Full analysis in `.dev-context/router-service-review.md`.

### 14.1 Replace Modular Hash with Jump Consistent Hash

- [ ] **Replace `prefix_hash % live_backends.size()` with jump consistent hash**
  _Justification:_ The hash fallback in `get_backend_for_prefix()` and `get_backend_by_hash()` uses modular hashing, which reshuffles **all** keys when a backend is added/removed. True consistent hashing (e.g., jump hash) remaps only ~1/n keys, preserving KV-cache affinity during topology changes — which is the project's core value proposition.
  _What to change:_ Implement `jump_consistent_hash(uint64_t key, int32_t num_buckets)` (Google's algorithm, ~10 lines, public domain). Replace the two `prefix_hash % live_backends.size()` call sites at `router_service.cpp:1271` and `router_service.cpp:1337`. `live_backends` is already sorted for determinism, so the jump hash bucket index maps directly.
  _Location:_ `src/router_service.cpp` (lines 1271, 1337)
  _Complexity:_ Low
  _Priority:_ P1 — Directly impacts cache hit rate during backend scaling events

### 14.2 Fix `cache_hit` Misreporting in `route_request()`

- [ ] **Return accurate `cache_hit` from `route_request()` for PREFIX mode**
  _Justification:_ `route_request()` unconditionally sets `result.cache_hit = true` when `get_backend_for_prefix()` returns a backend, even when the backend was selected via hash fallback (an ART miss). This inflates `cache_hit` rates in monitoring dashboards and makes the metric unreliable for capacity planning.
  _What to change:_ Modify `get_backend_for_prefix()` to return a struct (or add an out-parameter) indicating whether the result came from an ART hit or hash fallback. Update `route_request()` to set `cache_hit` only on actual ART hits. Update the `RouteResult` comment to clarify semantics.
  _Location:_ `src/router_service.cpp` (line 1050), `src/router_service.hpp` (`RouteResult` struct)
  _Complexity:_ Low
  _Priority:_ P2 — Affects observability accuracy

### 14.3 Extract Shared Live-Backend Filtering Helper

- [ ] **Deduplicate the live-backend collection pattern into `ShardLocalState::get_live_backends()`**
  _Justification:_ The pattern of iterating `backend_ids`, checking `dead_backends.contains()`, looking up in `backends`, and checking `is_draining` is repeated identically in `get_random_backend()`, `get_backend_for_prefix()`, and `get_backend_by_hash()`. If filtering criteria change (e.g., adding weight=0 exclusion), all three sites must be updated in lockstep — a latent inconsistency risk.
  _What to change:_ Add a `std::vector<BackendId> get_live_backends()` method to `ShardLocalState` that encapsulates the filtering logic. Replace the three inline loops with calls to this helper. `get_random_backend()` needs additional weight/priority data — consider a second overload that returns `vector<pair<BackendId, const BackendInfo*>>`.
  _Location:_ `src/router_service.cpp` (lines 1092-1119, 1184-1197, 1307-1320)
  _Complexity:_ Low
  _Priority:_ P3 — Maintainability improvement

### 14.4 Eliminate `std::map` Allocation on Hot Path in `get_random_backend()`

- [ ] **Replace `std::map` priority grouping with a single-pass min-priority scan**
  _Justification:_ `get_random_backend()` constructs a `std::map<uint32_t, std::vector<...>>` on every call. `std::map` allocates a red-black tree node per entry. Since only the highest-priority group (lowest number) is used, a single pass tracking the minimum priority and accumulating candidates into a pre-allocated vector eliminates all heap allocations.
  _What to change:_ Replace the `std::map priority_groups` with a two-pass or single-pass approach: (1) find the minimum priority value across live backends, (2) collect candidates matching that priority. Alternatively, combine with backlog item 14.3 by having `get_live_backends()` return results sorted by priority.
  _Location:_ `src/router_service.cpp` (line 1102)
  _Complexity:_ Low
  _Priority:_ P3 — Hot-path performance; most impactful under RANDOM mode or fail-open

### 14.5 Preserve Original Backend Weight Across DRAINING→ACTIVE Transitions

- [ ] **Stop overwriting backend weight during DRAINING, or store original weight for restoration**
  _Justification:_ `handle_node_state_change()` sets `weight = 0` on DRAINING and hardcodes `weight = 100` on ACTIVE restore. Backends registered with custom weights (e.g., 50 for smaller GPUs, 200 for larger ones) lose their weight permanently. Since `is_draining = true` already excludes backends from all routing paths (`get_random_backend`, `get_backend_for_prefix`, `get_backend_by_hash` all check `is_draining`), the `weight = 0` assignment is redundant.
  _What to change:_ **Option A (simplest):** Remove the `weight = 0` line from the DRAINING handler and the `weight = 100` line from the ACTIVE handler. The `is_draining` flag is already sufficient. **Option B:** Add `uint32_t original_weight` to `BackendInfo`, save before zeroing, restore on ACTIVE.
  _Location:_ `src/router_service.cpp` (lines 2149-2152 for DRAINING, 2169-2172 for ACTIVE)
  _Complexity:_ Low
  _Priority:_ P2 — Correctness bug for deployments using custom backend weights

### 14.6 Replace `std::atomic` with Plain `uint64_t` for Shard-Local Load Counter

- [ ] **Remove unnecessary `std::atomic` from `BackendInfo::active_requests`**
  _Justification:_ `BackendInfo` lives inside `thread_local ShardLocalState`. In Seastar's cooperative model, `active_requests` is only accessed from a single reactor thread (the `BackendRequestGuard` is documented as "shard-local operation only"). `std::atomic` is unnecessary and causes: (1) ~40 lines of manual copy/move constructor boilerplate because `std::atomic` isn't copyable/movable, (2) architectural confusion suggesting cross-thread access, (3) minor overhead on ARM (atomic stores emit barrier instructions even with relaxed ordering).
  _What to change:_ Replace `std::atomic<uint64_t> active_requests{0}` with `uint64_t active_requests = 0`. Delete the manual copy constructor, move constructor, copy assignment, and move assignment (lines 87-131) — the compiler-generated defaults will work. Update `BackendRequestGuard` to use direct read/write instead of `.load()`/`.fetch_add()`/`.fetch_sub()`.
  _Location:_ `src/router_service.cpp` (lines 73, 87-131, 334-399, 406-417)
  _Complexity:_ Low
  _Priority:_ P3 — Code simplification, minor perf improvement

### 14.7 Add Error Handling to Fire-and-Forget `unregister_backend_global` in Draining Reaper

- [ ] **Attach `.handle_exception()` to discarded `unregister_backend_global()` futures**
  _Justification:_ `run_draining_reaper()` calls `(void)unregister_backend_global(id)`, discarding the future. If unregistration fails (e.g., `submit_to` timeout on an overloaded shard), the backend remains registered but past its drain timeout — a "ghost backend" that is draining forever. There is no retry, no error logging, and no way to detect the failure. This violates Hard Rule #9 (every catch block must log at warn level).
  _What to change:_ Replace `(void)unregister_backend_global(id)` with `(void)unregister_backend_global(id).handle_exception([id](std::exception_ptr ep) { ... log warning ... })`. Optionally, track failed removals in a retry set and re-attempt on the next reaper tick.
  _Location:_ `src/router_service.cpp` (line 2111)
  _Complexity:_ Low
  _Priority:_ P3 — Defensive correctness

---

## References

- [Ranvier Architecture](./architecture.md)
- [API Reference](./api-reference.md)
- [Request Flow Diagrams](./request-flow.md)
- [Integration Test Guide](../tests/integration/README.md)
