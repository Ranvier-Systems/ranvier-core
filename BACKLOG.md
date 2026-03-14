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
15. [HTTP Controller Review (2026-02-14)](#15-http-controller-review-2026-02-14)
16. [Radix Tree Review (2026-02-14)](#16-radix-tree-review-2026-02-14)
17. [Router Service Review Pass 2 (2026-02-14)](#17-router-service-review-pass-2-2026-02-14)
18. [Connection Pool Review (2026-02-14)](#18-connection-pool-review-2026-02-14)
19. [Gossip Service Review (2026-02-14)](#19-gossip-service-review-2026-02-14)
20. [Hot-Path Performance Audit (2026-02-15)](#20-hot-path-performance-audit-2026-02-15)
21. [Request Lifecycle Performance Analysis (2026-02-20)](#21-request-lifecycle-performance-analysis-2026-02-20)
22. [Code Modularity (Low Priority)](#22-code-modularity-low-priority)
23. [Hard Rules Compliance Audit (2026-02-28)](#23-hard-rules-compliance-audit-2026-02-28)

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

- [ ] **Partial tokenization for routing decisions**
  _Justification:_ Tokenization accounts for ~10.6ms of ~10.62ms total routing decision time (99.9% of overhead). The ART lookup itself is <0.01ms. Currently the full prompt is tokenized, but routing only needs enough tokens to match against the prefix tree depth. Truncating the input to a byte budget before tokenizing could significantly reduce per-request tokenization cost.
  _Nuance:_ The system supports rewriting with tokenized output depending on the endpoint, so partial tokenization must not interfere with downstream token reuse. May require a two-phase approach: partial tokenization for routing, full tokenization deferred to the forwarding path only when needed.
  _Current mitigation:_ Tokenization is offloaded to a dedicated thread pool (not blocking the reactor), so the 10.6ms wall-clock time does not stall the event loop. Real overhead is thread pool queue contention + context switch, much less than 10ms.
  _Benchmark evidence:_ 30m run (b63c165, 2026-02-28) — CodeLlama-13b, 20 users, 8 GPUs. Routing decision P50: 10.62ms (tokenization: 10.61ms, ART: 0.01ms). Despite this overhead, P99 TTFT improved 78.2% (4500ms → 980ms) and cache hit rate improved from 12.5% to 73.9%.
  _Location:_ `src/tokenizer_service.hpp`, `src/tokenizer_service.cpp`, `src/http_controller.cpp`
  _Complexity:_ Medium
  _Priority:_ P3 — Optimization. Not urgent given thread pool offloading and dominant TTFT improvement.

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

- [x] **Add route table size and memory metrics** ✓
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

- [x] **Replace `prefix_hash % live_backends.size()` with jump consistent hash** ✓
  _Justification:_ The hash fallback in `get_backend_for_prefix()` and `get_backend_by_hash()` uses modular hashing, which reshuffles **all** keys when a backend is added/removed. True consistent hashing (e.g., jump hash) remaps only ~1/n keys, preserving KV-cache affinity during topology changes — which is the project's core value proposition.
  _Approach implemented:_ Added `jump_consistent_hash(uint64_t key, int32_t num_buckets)` (Lamping & Veach 2014) next to existing `hash_prefix()`. Replaced both `prefix_hash % live_backends.size()` call sites. Added two unit tests verifying minimal-remap property on backend addition and removal (≤40% remap threshold vs ~75% for modular hash).
  _Location:_ `src/router_service.cpp`, `tests/unit/router_service_test.cpp`
  _Complexity:_ Low
  _Completed:_ 2026-02-14

### 14.2 Fix `cache_hit` Misreporting in `route_request()`

- [x] **Return accurate `cache_hit` from `route_request()` for PREFIX mode** ✓
  _Justification:_ `route_request()` unconditionally sets `result.cache_hit = true` when `get_backend_for_prefix()` returns a backend, even when the backend was selected via hash fallback (an ART miss). This inflates `cache_hit` rates in monitoring dashboards and makes the metric unreliable for capacity planning.
  _Approach implemented:_ Added `PrefixRouteResult` struct with `backend_id` + `art_hit` flag. Changed `get_backend_for_prefix()` return type from `std::optional<BackendId>` to `PrefixRouteResult`. Each return path explicitly sets `art_hit` (true only for ART lookup hits). `route_request()` now sets `cache_hit = prefix_result.art_hit`. Updated `RouteResult::cache_hit` comment. Added `HashFallbackReportsCacheMiss` test and updated existing tests for new return type.
  _Location:_ `src/router_service.hpp`, `src/router_service.cpp`, `tests/unit/router_service_test.cpp`
  _Complexity:_ Low
  _Completed:_ 2026-02-14

### 14.3 Extract Shared Live-Backend Filtering Helper

- [x] **Deduplicate the live-backend collection pattern into `ShardLocalState::get_live_backends()`** ✓
  _Justification:_ The pattern of iterating `backend_ids`, checking `dead_backends.contains()`, looking up in `backends`, and checking `is_draining` is repeated identically in `get_random_backend()`, `get_backend_for_prefix()`, and `get_backend_by_hash()`. If filtering criteria change (e.g., adding weight=0 exclusion), all three sites must be updated in lockstep — a latent inconsistency risk.
  _Approach implemented:_ Added two helpers to `ShardLocalState`: (1) `get_live_backends()` returns a sorted `vector<BackendId>` — used by `get_backend_for_prefix()` and `get_backend_by_hash()`; (2) `get_live_backend_infos()` returns `vector<pair<BackendId, const BackendInfo*>>` — used by `get_random_backend()` for weight/priority access. Replaced all three inline loops. Net −30 lines.
  _Location:_ `src/router_service.cpp`
  _Complexity:_ Low
  _Completed:_ 2026-02-14

### 14.4 Eliminate `std::map` Allocation on Hot Path in `get_random_backend()`

- [x] **Replace `std::map` priority grouping with a single-pass min-priority scan** ✓
  _Justification:_ `get_random_backend()` constructs a `std::map<uint32_t, std::vector<...>>` on every call. `std::map` allocates a red-black tree node per entry. Since only the highest-priority group (lowest number) is used, a single pass tracking the minimum priority and accumulating candidates into a pre-allocated vector eliminates all heap allocations.
  _Approach implemented:_ Two linear passes over the `live_infos` vector returned by `get_live_backend_infos()`: (1) find `min_priority` and accumulate `total_weight` for that priority, (2) weighted random selection over matching candidates. Zero heap allocations — no `std::map`, no `std::vector` of candidates. Also removed now-unused `#include <map>`.
  _Location:_ `src/router_service.cpp`
  _Complexity:_ Low
  _Completed:_ 2026-02-14

### 14.5 Preserve Original Backend Weight Across DRAINING→ACTIVE Transitions

- [x] **Stop overwriting backend weight during DRAINING, or store original weight for restoration** ✓
  _Justification:_ `handle_node_state_change()` sets `weight = 0` on DRAINING and hardcodes `weight = 100` on ACTIVE restore. Backends registered with custom weights (e.g., 50 for smaller GPUs, 200 for larger ones) lose their weight permanently. Since `is_draining = true` already excludes backends from all routing paths (`get_random_backend`, `get_backend_for_prefix`, `get_backend_by_hash` all check `is_draining`), the `weight = 0` assignment is redundant.
  _Approach implemented:_ Option A — removed `weight = 0` from DRAINING handler and `weight = 100` from ACTIVE handler. The `is_draining` flag (checked by `get_live_backends()`/`get_live_backend_infos()`) is sufficient to exclude backends from all routing decisions. Original weight is now preserved through the full drain cycle. Added `clear_backend_draining_for_testing()` helper and `CustomWeightPreservedAcrossDrainCycle` unit test.
  _Location:_ `src/router_service.cpp`, `src/router_service.hpp`, `tests/unit/router_service_test.cpp`
  _Complexity:_ Low
  _Completed:_ 2026-02-14

### 14.6 Replace `std::atomic` with Plain `uint64_t` for Shard-Local Load Counter

- [x] **Remove unnecessary `std::atomic` from `BackendInfo::active_requests`** ✓
  _Justification:_ `BackendInfo` lives inside `thread_local ShardLocalState`. In Seastar's cooperative model, `active_requests` is only accessed from a single reactor thread (the `BackendRequestGuard` is documented as "shard-local operation only"). `std::atomic` is unnecessary and causes: (1) ~40 lines of manual copy/move constructor boilerplate because `std::atomic` isn't copyable/movable, (2) architectural confusion suggesting cross-thread access, (3) minor overhead on ARM (atomic stores emit barrier instructions even with relaxed ordering).
  _Approach implemented:_ Replaced `std::atomic<uint64_t>` with plain `uint64_t`. Deleted all four manual special members (copy/move ctor/assignment) — compiler defaults now work. Updated `BackendRequestGuard` to use `++`/`--`/direct reads. Updated `get_backend_load()` and `get_least_loaded_backend()`. Removed `#include <atomic>`. Net −51 lines.
  _Location:_ `src/router_service.cpp`
  _Complexity:_ Low
  _Completed:_ 2026-02-14

### 14.7 Add Error Handling to Fire-and-Forget `unregister_backend_global` in Draining Reaper

- [x] **Attach `.handle_exception()` to discarded `unregister_backend_global()` futures** ✓
  _Justification:_ `run_draining_reaper()` calls `(void)unregister_backend_global(id)`, discarding the future. If unregistration fails (e.g., `submit_to` timeout on an overloaded shard), the backend remains registered but past its drain timeout — a "ghost backend" that is draining forever. There is no retry, no error logging, and no way to detect the failure. This violates Hard Rule #9 (every catch block must log at warn level).
  _Approach implemented:_ Attached `.handle_exception()` with rethrow-and-log pattern (matching existing `flush_route_batch` style). Logs at `warn` level with backend ID and exception message.
  _Location:_ `src/router_service.cpp`
  _Complexity:_ Low
  _Completed:_ 2026-02-14

---

## 15. HTTP Controller Review (2026-02-14)

Deep-dive review of `http_controller.hpp` and `http_controller.cpp` (~2500 lines). Full analysis in `.dev-context/http-controller-review.md`.

### 15.1 Implement or Remove Dead P2C Cross-Shard Dispatch

- [x] **`select_target_shard()` computes a P2C target shard but never dispatches to it**
  _Justification:_ The P2C algorithm selects a target shard, but the result is only logged — the request is always processed on the local shard. There is no `smp::submit_to(target_shard, ...)` anywhere in `http_controller.cpp`. The `_requests_cross_shard_dispatch` counter increments, but no actual cross-shard dispatch occurs. The entire P2C load balancing feature (`LoadBalancingSettings`, `ShardLoadBalancer` integration, cross-shard dispatch metrics) is dead code that gives the appearance of load balancing without providing it.
  _What to change:_ **Option A (implement):** Add `smp::submit_to(target_shard, ...)` to actually dispatch requests cross-shard when P2C selects a remote shard. Requires `cross_shard_request.hpp` / `foreign_ptr` for safe data transfer per Hard Rule #14. **Option B (remove):** Delete `select_target_shard()`, `LoadBalancingSettings`, `_load_balancer`, `_lb_config`, and the cross-shard dispatch metrics to eliminate dead code.
  _Location:_ `src/http_controller.cpp` (lines 725-731, 1839-1864), `src/http_controller.hpp` (lines 55-60, 247-248)
  _Complexity:_ High (Option A), Low (Option B)
  _Priority:_ P1 — Dead code that misleads operators into believing load balancing is active
  _Completed:_ 2026-02-14 — Chose modified Option B: disabled by default, documented as advisory-only, removed misleading call from handle_proxy

### 15.2 Escape User-Controlled Strings in JSON Error Responses

- [x] **Extract `escape_json_string` into a shared utility and use it for all JSON string interpolation**
  _Justification:_ Multiple error responses interpolate external strings directly into JSON without escaping. If any of these strings contain `"`, `\`, or control characters, the response becomes malformed JSON. The codebase already has an `escape_json_string` lambda at line 1738, but it is local to `handle_keys_reload`. Affected lines: 181 (auth info), 803 (token error), 1071 (route error message), 1448 (validation error).
  _What to change:_ Move `escape_json_string` from the local lambda in `handle_keys_reload` to a utility function in `parse_utils.hpp`. Apply it to all JSON string interpolation sites. Alternatively, use RapidJSON (already a dependency) for response construction.
  _Location:_ `src/http_controller.cpp` (lines 181, 803, 1071, 1448, 1738), `src/parse_utils.hpp`
  _Complexity:_ Low
  _Priority:_ P2 — Correctness; malformed JSON responses on certain inputs
  _Completed:_ 2026-02-14 — Moved to parse_utils.hpp, applied to all 4 sites, removed local lambda

### 15.3 Replace `std::atomic` Counters With Plain `uint64_t`

- [x] **Remove unnecessary `std::atomic` from shard-local metric counters in `HttpController`**
  _Justification:_ In Seastar's shared-nothing model, each `HttpController` instance lives on exactly one shard. The four atomic counters (`_requests_rejected_concurrency`, `_requests_rejected_persistence`, `_requests_local_dispatch`, `_requests_cross_shard_dispatch`) are only ever incremented from that shard's reactor thread. No cross-shard code reads them. Using `std::atomic` adds unnecessary memory fence overhead and contradicts Hard Rule #0/#1.
  _What to change:_ Replace `std::atomic<uint64_t>` with plain `uint64_t` for all four counters. If metrics scraping needs cross-shard access, use `smp::submit_to` to gather them.
  _Location:_ `src/http_controller.hpp` (lines 259-264)
  _Complexity:_ Low
  _Priority:_ P3 — Code hygiene and minor performance improvement
  _Completed:_ 2026-02-14 — Also converted _draining from std::atomic<bool> to plain bool, removed <atomic> include

### 15.4 Sanitize Header Values and Fix Hardcoded Host in Backend Request Construction

- [x] **Sanitize outgoing header values and derive Host from target address**
  _Justification:_ `send_backend_request()` builds the HTTP request via raw string concatenation. The `Host` header is hardcoded to `localhost`. `ctx.request_id` and `ctx.backend_traceparent` originate from client headers and are injected without CRLF injection checks — a crafted `X-Request-ID` containing `\r\n` could inject arbitrary headers. The body is also concatenated into the header string, doubling memory for large payloads.
  _What to change:_ (1) Strip `\r` and `\n` from `request_id` and `backend_traceparent` before interpolation. (2) Derive the `Host` header from `ctx.current_addr`. (3) Write headers and body as two separate `write()` calls to avoid the double-copy for large payloads.
  _Location:_ `src/http_controller.cpp` (lines 420-432)
  _Complexity:_ Low
  _Priority:_ P2 — Header injection risk and correctness
  _Completed:_ 2026-02-14 — Added sanitize_header_value() to parse_utils.hpp, derived Host from ctx.current_addr, split write

### 15.5 Optimize Per-Chunk Flush in SSE Streaming Loop

- [ ] **Reduce flush frequency in `stream_backend_response()` via timer-based coalescing**
  _Justification:_ Every `bundle.in.read()` iteration triggers `write()` + `flush()` to the client. At typical LLM token rates (30-100 tok/s), each token arrives as a separate TCP segment, so this is effectively **one `sendmsg()` syscall per token** at steady state. Natural TCP batching only helps during bursts (multiple tokens arriving in one segment, which `StreamParser::push()` concatenates into a single `res.data`). At high concurrency (many concurrent streams), the per-token syscall overhead becomes measurable.

  **Previous attempt (2026-02-14, reverted in #280):** Flushed only on first write (TTFT) and `[DONE]`, relying on Seastar's 8KB `output_stream` buffer for intermediate events. This caused a **3.3x TTFT regression** because small SSE events (10-50 bytes each) never fill the 8KB buffer, stalling delivery for seconds until enough tokens accumulate. The regression was caught in production load testing and reverted.

  **Why "just skip flushes" fails:** Seastar's `output_stream` buffers 8KB by default. At 30 bytes/token, that's ~270 tokens (~9 seconds at 30 tok/s) before the buffer auto-flushes. SSE requires real-time delivery, so any approach that removes intermediate flushes without a bounded-latency fallback will break streaming.

  **Why "reactor tick coalescing" is not straightforward:** Seastar provides no primitive to check "is there more work pending in this reactor tick." `seastar::yield()` / `coroutine::maybe_yield()` are control-flow primitives, not introspection tools. You cannot peek at a future's readiness without consuming it, so "read-ahead to decide whether to flush" requires restructuring the entire loop.

  _What to change:_ Implement a **`seastar::timer<seastar::lowres_clock>`** flush strategy:
  1. On entering the streaming loop, arm a periodic timer (~10ms interval, matching `lowres_clock` resolution).
  2. In the read loop, call `co_await client_out.write(res.data)` **without** `flush()`.
  3. The timer callback flushes whatever has accumulated: `(void)client_out.flush().handle_exception(...)` with a `seastar::gate` holder per Rule #5.
  4. Flush **immediately** on `res.done` (terminal event) and on stream errors/cleanup.
  5. Disarm the timer on loop exit (before `client_out.close()`).

  _Design constraints:_
  - The timer is **per-request** (created/destroyed with each `stream_backend_response()` call). This is fine — Seastar's `timer_set` is optimized for frequent create/cancel cycles. See existing patterns: `router_service.cpp:2058` (batch flush), `connection_pool.hpp:500` (reaper timer).
  - The timer callback is synchronous but `flush()` is async. Use the fire-and-forget pattern with gate guard: `(void)flush().finally([holder = std::move(holder)] {})`.
  - Must handle the race between timer-initiated flush and loop-initiated close. The gate ensures the flush completes before stream teardown.
  - Must handle `flush()` failure in the timer callback (client disconnect). Log and set a flag the main loop checks, or let the next `write()` catch the broken pipe.
  - **10ms worst-case latency** is acceptable for SSE token streaming (human-imperceptible). This coalesces 1-3 tokens per flush at typical rates, reducing syscalls by ~2-3x.

  _Testing requirements (critical — the previous attempt was not adequately tested):_
  - **TTFT test:** Measure time from first backend token to client receipt. Must be <15ms above baseline (currently ~1ms with per-read flush).
  - **Stall test:** Stream 100+ tokens at 30 tok/s, verify no token is delayed >20ms from backend receipt to client delivery.
  - **Burst test:** Send 10 tokens in <1ms from backend, verify they arrive at client in one batch (not 10 separate deliveries).
  - **Disconnect test:** Client disconnects mid-stream, verify timer is cleaned up and no use-after-free.
  - **Shutdown test:** Server drains while streams are active, verify gate prevents timer callback after stream teardown.

  _Location:_ `src/http_controller.cpp` (`stream_backend_response()`, currently lines 605-620)
  _Complexity:_ Medium — per-request timer lifecycle, gate interaction, error propagation from timer callback
  _Priority:_ P3 — Performance optimization; most impactful at high concurrent stream count
  _Depends on:_ Stable TTFT measurement infrastructure to validate before/after

### 15.6 Add Explicit `stop()` Method for Orderly Shutdown

- [x] **Add `seastar::future<> stop()` to `HttpController` for clean lifecycle management**
  _Justification:_ `HttpController` is used as `seastar::sharded<HttpController>`, so Seastar calls `.stop()` on each shard during shutdown. The class has no `stop()` method and relies on the default (ready future). However, `_rate_limiter.start()` is called in `register_routes()` and only stopped in `wait_for_drain()`. If teardown skips `wait_for_drain()`, the rate limiter timer could fire after destruction. Per Hard Rule #5 (timer gate guards) and #6 (deregister metrics in stop).
  _What to change:_ Add `seastar::future<> stop()` that: (1) calls `_rate_limiter.stop()`, (2) closes `_request_gate` if not already closed, (3) ensures orderly cleanup regardless of whether `wait_for_drain()` was called.
  _Location:_ `src/http_controller.hpp` (class declaration), `src/http_controller.cpp` (new method)
  _Complexity:_ Low
  _Priority:_ P2 — Defensive correctness; prevents use-after-free on abnormal shutdown paths
  _Completed:_ 2026-02-14 — Added stop() with _stopped guard for double-stop safety (wait_for_drain + stop)

### 15.7 Bound Recursive JSON Serialization in Admin Endpoints

- [x] **Add depth and size limits to `dump_node_to_json` and admin dump handlers**
  _Justification:_ The tree dump handler recursively serializes the entire radix tree into a `std::ostringstream` with no upper bound. For production systems with tens of thousands of routes, this could produce multi-MB JSON responses. The recursive `dump_node_to_json` also risks stack overflow for deeply nested trees. `handle_dump_cluster` and `handle_dump_backends` have similar unbounded output.
  _What to change:_ (1) Add a `max_depth` parameter to `dump_node_to_json` (default: 32) to bound recursion. (2) Add a `?limit=N` query parameter to cap the number of nodes returned. (3) Consider streaming JSON output for large responses rather than building the entire string in memory.
  _Location:_ `src/http_controller.cpp` (lines 1871-1986, 2005-2077)
  _Complexity:_ Low
  _Priority:_ P3 — Defensive correctness; prevents OOM on large trees
  _Completed:_ 2026-02-14 — Added remaining_depth param (default 32), children_truncated field, ?max_depth=N query param

---

## 16. Radix Tree Review (2026-02-14)

Deep-dive review of `src/radix_tree.hpp` — the #1 load-bearing file in the codebase (per Strategic Assessment). Findings cover correctness bugs, maintainability debt, and performance opportunities.

### 16.1 Fix Node256 Collision Lookup Failure When Preferred Slot Is Vacated

- [x] **Prevent `find_child()` from silently missing displaced children in Node256**
  _Justification:_ `key_byte()` truncates `TokenId` (int32_t) to the lower 8 bits, so tokens like 0, 256, 512 all map to index 0. When a collision occurs during `add_child()`, the displaced child is placed in an arbitrary empty slot. However, `find_child()` at line 629 short-circuits the linear scan fallback with `if (n->keys[idx] != Node256::EMPTY_KEY)` — if the preferred slot is later vacated (by eviction or compaction), the condition becomes false and the displaced child becomes permanently invisible to lookups, even though it still exists in the tree. The same pattern affects `set_child()` and `extract_child()`.
  _What to change:_ Remove the `if (n->keys[idx] != Node256::EMPTY_KEY)` guard before the linear fallback in `find_child()`, `extract_child()`, and `set_child()` for Node256. Always fall through to linear scan when the preferred-slot key doesn't match. Alternatively, redesign Node256 to use a proper open-addressing hash table with a defined probe sequence so displaced entries are findable in O(1) amortized time.
  _Location:_ `src/radix_tree.hpp` lines 629-636 (`find_child`), 679-687 (`extract_child`), 740-747 (`set_child`)
  _Complexity:_ Low (fix) / Medium (redesign)
  _Priority:_ P1 — Correctness bug; causes silent data loss on lookup for colliding token IDs

### 16.2 Fix `compact_children()` Using Slot Index Instead of Key for Node256 Removal

- [x] **Use `n->keys[i]` instead of `static_cast<TokenId>(i)` when marking Node256 children for removal**
  _Justification:_ In `compact_children()` for the Node256 case (line 1547), empty children are marked for removal via `keys_to_remove.push_back(static_cast<TokenId>(i))`, where `i` is the array slot index (0–255). But `remove_child()` matches against the *stored key* (`n->keys[i]`), not the slot index. When collisions displace a child to a non-preferred slot, the slot index differs from the stored key. `remove_child()` will either remove the wrong child (if another key happens to equal the slot index) or fail to find and remove the target, leaking the empty node.
  _What to change:_ Replace `keys_to_remove.push_back(static_cast<TokenId>(i))` with `keys_to_remove.push_back(n->keys[i])` in the Node256 branch of `compact_children()`.
  _Location:_ `src/radix_tree.hpp` line 1547
  _Complexity:_ Low
  _Priority:_ P2 — Correctness bug; causes memory leaks during compaction with colliding token IDs

### 16.3 Fix `extract_child()` Leaving Stale Entries in Node4/Node16/Node48 Vectors

- [x] **Erase the key/child entry from vectors after extracting a child, or switch to swap-and-pop**
  _Justification:_ `extract_child()` for Node4/Node16 does `std::move(n->children[i])` but does not erase the entry from `keys` or `children` vectors. The vector retains its original size with a null `NodePtr` at position `i` and the key still present. Consequences: (1) `find_child()` will match the stale key and return nullptr, (2) `child_count()` returns an inflated count (it uses `keys.size()`), (3) `add_child()` may trigger premature growth (checking `keys.size() < 4`). The Node48 path has the same issue. `extract_child()` is called by `insert_recursive()` on every insert that traverses an existing child, making this a common code path.
  _What to change:_ After extracting, erase both `keys[i]` and `children[i]` from their respective vectors. For Node4/Node16, use swap-with-last + `pop_back()` for O(1) removal (order doesn't matter). For Node48, also update the index array (decrement indices > i, clear the removed key's index entry). Alternatively, refactor `insert_recursive()` to avoid extract+reinsert entirely — use a mutable reference to the child slot.
  _Location:_ `src/radix_tree.hpp` lines 643-691 (`extract_child`), called from line 1277 (`insert_recursive`)
  _Complexity:_ Medium
  _Priority:_ P2 — Correctness bug; causes wrong child counts, missed lookups, and premature node growth on every insert through existing paths

### 16.4 Replace `std::vector` with Small-Buffer-Optimized Container for Node Prefix and Small Node Keys

- [x] **Eliminate heap allocations for short prefixes and small node key/child arrays**
  _Justification:_ `Node::prefix` is `std::vector<TokenId>`, which always heap-allocates (even for 1-token prefixes). `Node4::keys` and `Node4::children` are also vectors with `reserve(4)`, causing two heap allocations per Node4 construction. In an ART, most prefixes are short (1-4 tokens) and most nodes are Node4 (the smallest type). Every `insert()` that creates a new leaf allocates a Node4 via slab, then does 2-3 additional heap allocations for vectors — undermining the slab allocator's goal of eliminating heap allocation. This also fragments memory and hurts cache locality since prefix data is not co-located with the node.
  _What to change:_ Replace `std::vector<TokenId>` prefix with a `SmallVector<TokenId, 8>` (inline storage for up to 8 tokens, ~32 bytes). Replace Node4 keys/children with `std::array<TokenId, 4>` + `std::array<NodePtr, 4>` + `uint8_t count`. Same for Node16. This keeps small-case data inline within the slab-allocated node, eliminating heap allocation entirely for the common case. Boost.Container `small_vector` or Abseil `InlinedVector` (already a dependency) are ready-made options.
  _Location:_ `src/radix_tree.hpp` lines 99 (`Node::prefix`), 109-110 (`Node4`), 119-120 (`Node16`)
  _Complexity:_ Medium
  _Priority:_ P2 — Performance; eliminates 2-3 heap allocations per node on the insert path, improves cache locality

### 16.5 Reduce Code Duplication via Unified Small-Node Template for Node4/Node16

- [x] **Consolidate duplicated switch-on-type code by templating Node4/Node16 operations**
  _Justification:_ Node4 and Node16 have identical structure (keys vector + children vector), yet every operation (`find_child`, `extract_child`, `set_child`, `add_child`, `remove_child`, `compact_children`, `visit_children`, `child_count`, `move_from_small_node`) duplicates the same logic in separate switch branches. This is ~300+ lines of pure duplication. If a bug is fixed in one branch but missed in the other, they silently diverge (as already happened with `extract_child` not erasing entries). The `move_from_small_node` template at line 987 shows this was partially recognized but not applied consistently.
  _What to change:_ Create a `SmallNode<N>` template (or a common base `KeyChildNode`) that Node4 and Node16 inherit from. Move shared logic into free functions or template methods operating on this base. This halves the switch-case branches for small nodes and makes it impossible to fix a bug in one without fixing the other. The Node48/Node256 cases remain separate since their storage differs.
  _Location:_ `src/radix_tree.hpp` — affects `find_child`, `extract_child`, `set_child`, `add_child`, `remove_child`, `compact_children`, `visit_children`, `child_count`
  _Complexity:_ Medium
  _Priority:_ P2 — Maintainability; reduces ~300 lines of duplication and prevents divergence bugs

### 16.6 Replace O(n) Full-Tree Scan in `evict_oldest()` with an LRU Index Structure

- [x] **Add an intrusive LRU list to make eviction O(1) instead of O(n)**
  _Justification:_ `evict_oldest()` calls `find_oldest_leaf()` which recursively walks the entire tree to find the leaf with the minimum `last_accessed` timestamp. This is O(n) where n is total nodes. Eviction is called in a loop (`while (route_count >= max_routes) evict_oldest()`), making bulk eviction O(n × k) where k is the number of routes to evict. Under memory pressure with thousands of routes, this becomes a latency spike on the insert path — potentially violating Hard Rule #1 (no blocking on hot path).
  _What to change:_ Maintain an intrusive doubly-linked list threaded through leaf nodes, ordered by `last_accessed`. On lookup hit, move the node to the head (O(1) with intrusive list). On eviction, pop from the tail (O(1)). Add `lru_prev`/`lru_next` pointers to `Node` (8 bytes each, acceptable overhead). This makes eviction O(1) and lookup LRU update O(1), at the cost of ~16 bytes per node and pointer maintenance on insert/remove.
  _Location:_ `src/radix_tree.hpp` lines 1404-1428 (`find_oldest_leaf`, `find_oldest_recursive`), lines 409-427 (`evict_oldest`, `evict_oldest_remote`)
  _Complexity:_ High
  _Priority:_ P3 — Performance; only impacts workloads at or near route capacity with frequent eviction

### 16.7 Add MAX_SIZE Bound to Node Prefix Length

- [x] **Enforce a maximum prefix length to prevent unbounded memory growth from long token sequences**
  _Justification:_ `insert_recursive()` creates new nodes with `prefix.assign(remaining.begin() + 1, remaining.end())` (line 1284). The prefix length is bounded only by the input token sequence length (after block alignment truncation). A very long input (e.g., a 128k-context request producing 100k+ tokens) could create a single node with a 100k-element prefix vector, allocating ~400KB of heap memory for one node. This violates Hard Rule #4 (every growing container needs explicit MAX_SIZE). While block_alignment truncation helps, typical alignment values (16) still allow prefixes up to `aligned_len - depth` tokens.
  _What to change:_ Add a `MAX_PREFIX_LENGTH` constant (e.g., 256 or 512 tokens). When a new node's prefix would exceed this, split it into a chain of nodes with bounded prefixes. This is analogous to how B-trees split pages — each node stores at most N prefix tokens, with overflow continuing in a child node. Add a check in `insert_recursive()` after constructing the prefix: `if (new_child->prefix.size() > MAX_PREFIX_LENGTH) split_long_prefix(new_child)`.
  _Location:_ `src/radix_tree.hpp` lines 1282-1285 (new node creation in `insert_recursive`), line 950 (`split_node` prefix assignment)
  _Complexity:_ Low
  _Priority:_ P3 — Defensive correctness; prevents pathological memory usage from long inputs

---

## 17. Router Service Review Pass 2 (2026-02-14)

Second review pass of `src/router_service.cpp` and `src/router_service.hpp` after completing all items from section 14. Focuses on correctness, stale comments, lifecycle safety, and minor code quality issues.

### 17.1 Stale "atomic" / "lock-free" Comments in Header File

- [x] **Update `BackendRequestGuard` class comment and `apply_load_aware_selection` comment in header**
  _Justification:_ Item 14.6 replaced `std::atomic` with plain `uint64_t`, but the header file `router_service.hpp` line 92 still says "Lock-free: uses atomic increment/decrement with relaxed ordering". The `apply_load_aware_selection` comment at line 442 in the .cpp also has "Atomic reads use relaxed ordering (no memory barriers)" which is stale.
  _What to change:_ In `router_service.hpp` line 92, change to "Shard-local: uses plain integer increment/decrement (no atomic needed)". In `router_service.cpp` line 442, remove the stale "Atomic reads use relaxed ordering" comment.
  _Location:_ `src/router_service.hpp:92`, `src/router_service.cpp:442`
  _Complexity:_ Trivial
  _Priority:_ P4 — Comment hygiene

### 17.2 `run_draining_reaper` Gate Holder Drops Before Fire-and-Forget Futures Complete

- [x] **Keep gate holder alive for the duration of `unregister_backend_global()` futures**
  _Justification:_ `run_draining_reaper()` acquires a `gate::holder` at line 2013, then launches fire-and-forget futures via `unregister_backend_global(id)` in a loop (lines 2046-2052). The gate holder is a local variable scoped to `run_draining_reaper()` — it drops when the function returns, which is immediately after launching the fire-and-forget futures. This means the gate is not actually protecting the async lifetime of `unregister_backend_global()`. If `stop()` closes `_timer_gate` while those futures are still in flight, the futures may access `this` (via `unregister_backend_global`) after destruction begins. Contrast with `run_ttl_cleanup()` at line 858 which correctly uses `do_with(std::move(holder), ...)` to extend the holder's lifetime.
  _What to change:_ Collect all `unregister_backend_global()` futures into a vector, then use `seastar::do_with(std::move(holder), ...)` to keep the holder alive until all futures complete. Apply `.handle_exception()` to the combined future (not individual ones). This also keeps the fire-and-forget pattern but with proper gate lifetime.
  _Location:_ `src/router_service.cpp:2009-2054`
  _Complexity:_ Medium
  _Priority:_ P2 — Latent use-after-free during shutdown

### 17.3 `run_ttl_cleanup` Fire-and-Forget Future Missing Error Handling

- [x] **Attach `.handle_exception()` to the discarded future in `run_ttl_cleanup()`**
  _Justification:_ `run_ttl_cleanup()` at line 858 launches `(void)seastar::do_with(...)` as fire-and-forget. If `parallel_for_each` or any `submit_to` inside fails, the exception is silently discarded. This violates Rule #9 (every catch must log at warn level). The same pattern was fixed in `run_draining_reaper` (item 14.7) and `flush_route_batch` (existing code).
  _What to change:_ Attach `.handle_exception([](std::exception_ptr ep) { try { std::rethrow_exception(ep); } catch (const std::exception& e) { log_main.warn("TTL cleanup failed: {}", e.what()); } })` to the `do_with` chain.
  _Location:_ `src/router_service.cpp:858`
  _Complexity:_ Low
  _Priority:_ P3 — Defensive correctness (Rule #9)

### 17.4 `get_all_backend_states()` Uses `std::ostringstream` on Hot-ish Path

- [x] **Replace `std::ostringstream` address formatting with `fmt::format`**
  _Justification:_ `get_all_backend_states()` (lines 1824-1826) creates a `std::ostringstream` per backend to format the socket address into a string, then manually parses the result to split address and port. `std::ostringstream` heap-allocates and is relatively expensive. While this is an admin API (not a true hot path), it runs on the reactor thread and could stall the event loop if there are many backends. Seastar's `socket_address` provides direct accessors for the address family, IPv4/IPv6 address, and port that avoid the stream round-trip entirely.
  _What to change:_ Use `seastar::net::inet_address` accessors and `addr.port()` directly instead of streaming to `ostringstream` and parsing back. This eliminates both the heap allocation and the string parsing.
  _Location:_ `src/router_service.cpp:1822-1854`
  _Complexity:_ Low
  _Priority:_ P4 — Code quality, minor perf improvement

### 17.5 `learn_route_global` Captures `this` in Cross-Shard Gossip Lambda

- [x] **Guard `this` captures in `learn_route_global()` gossip `submit_to` lambdas with gate**
  _Justification:_ At line 1368, `learn_route_global()` captures `this` in a `submit_to(0, ...)` lambda to access `_gossip->is_enabled()` and `_gossip->broadcast_route()`. This lambda runs on shard 0 asynchronously. The caller (`HttpController`) awaits the returned future, so normally `this` (the `RouterService`) is alive. However, if the caller drops or ignores the returned future (fire-and-forget pattern), the lambda could execute after `RouterService::stop()` has been called and `_gossip` has been destroyed. Currently `learn_route_global` is awaited by the caller, but this is a latent hazard if any future callsite uses fire-and-forget. The same pattern appears in `learn_route_global_multi()` at line 1518.
  _What to change:_ Add a `_timer_gate.hold()` at the start of `learn_route_global()` (with gate_closed_exception handling for early return), or document the contract that the returned future MUST be awaited. Both approaches are valid.
  _Location:_ `src/router_service.cpp:1368`, `src/router_service.cpp:1518`
  _Complexity:_ Low
  _Priority:_ P3 — Defensive correctness

### 17.6 `learn_route_global` Uses `_config` Instead of Shard-Local Config for `prefix_token_length`

- [x] **Use shard-local config for `prefix_token_length` in `learn_route_global()`**
  _Justification:_ At line 1325, `learn_route_global()` reads `_config.prefix_token_length` (the member variable) instead of `g_shard_state->config.prefix_token_length` (the shard-local copy). After a hot-reload via `update_routing_config()`, the shard-local config is updated on all shards (line 1943), and `_config` is also updated on shard 0 (line 1928). So on shard 0 they match. But `learn_route_global()` can be called from any shard via `HttpController`. If called from shard N (N > 0), `_config` is the RouterService member which lives on shard 0 — accessing it from shard N is a cross-shard read (Rule #14 violation). This hasn't caused issues because `learn_route_global` is likely always called on shard 0 via `submit_to`, but it's architecturally incorrect.
  _What to change:_ Replace `_config.prefix_token_length` with `g_shard_state->config.prefix_token_length` (with null guard). Same for `_config.max_route_tokens` at line 1338.
  _Location:_ `src/router_service.cpp:1325,1338`
  _Complexity:_ Low
  _Priority:_ P2 — Cross-shard data access correctness

### 17.7 `learn_route_remote` Uses `_config.max_route_tokens` (Member on Shard 0 Only)

- [x] **Use shard-local config for `max_route_tokens` in `learn_route_remote()`**
  _Justification:_ Same issue as 17.6. `learn_route_remote()` at line 1548 reads `_config.max_route_tokens`. This function is called from the GossipService callback registered at line 581-583, which runs on shard 0 (where GossipService lives), so in practice the access is safe. However, the method is public and could theoretically be called from other shards. Using `g_shard_state->config.max_route_tokens` would be architecturally correct and consistent with the hot-reload pattern.
  _What to change:_ Replace `_config.max_route_tokens` with `g_shard_state->config.max_route_tokens` (with null guard). Similarly for `learn_route_global_multi()` at line 1471.
  _Location:_ `src/router_service.cpp:1548,1471`
  _Complexity:_ Low
  _Priority:_ P3 — Consistency with shard-local config pattern

### 17.8 Duplicate Load-Tracking Stats: `cache_miss_due_to_load` and `load_aware_fallbacks`

- [x] **Remove duplicate stat `cache_miss_due_to_load` (identical to `load_aware_fallbacks`)**
  _Justification:_ In `apply_load_aware_selection()` at lines 473-474, both `cache_miss_due_to_load` and `load_aware_fallbacks` are incremented in exactly the same place, making them always identical. They also have separate Prometheus metrics (`router_cache_miss_due_to_load_total` and `router_load_aware_fallbacks_total`) that report the same value. The `cache_miss_due_to_load` comment at line 128 says "Routes diverted due to backend load (same as fallbacks)" — explicitly acknowledging the duplication.
  _What to change:_ Remove `cache_miss_due_to_load` from `Stats`, its `reset()` call, the Prometheus metric registration, and the increment in `apply_load_aware_selection`. Keep `load_aware_fallbacks` as the single source of truth.
  _Location:_ `src/router_service.cpp:128,145,473,743-745`
  _Complexity:_ Low
  _Priority:_ P4 — Code hygiene

---

## 18. Connection Pool Review (2026-02-14)

Deep-dive review of `src/connection_pool.hpp` across correctness, performance, and maintainability.

### 18.1 Silent Exception Swallowing in `BasicConnectionBundle::close()` (Rule #9)

- [x] **Log exceptions in `BasicConnectionBundle::close()` instead of silently discarding them**
  _Justification:_ Line 76 uses `.handle_exception([](auto) {})` which silently swallows all exceptions during stream close. This violates Hard Rule #9 (every catch block must log at warn level). A failed close could indicate a networking issue, resource leak, or Seastar internal error—all invisible today. The connection pool reaper and eviction paths funnel through `close()`, so a systematic close failure would be completely silent.
  _What to change:_ Replace the empty handler with one that logs at warn level: `.handle_exception([addr = this->addr](auto ep) { log_pool.warn("Failed to close connection to {}: {}", addr, ep); })`. Capture `addr` by value since the bundle may be destroyed by the time the handler runs.
  _Location:_ `src/connection_pool.hpp:76`
  _Complexity:_ Low
  _Priority:_ P2 — Correctness; violations of Rule #9 hide operational issues

### 18.2 Fragile `const_cast` in `evict_oldest_global()`

- [x] **Remove `const_cast` in `evict_oldest_global()` by storing the iterator or address by value**
  _Justification:_ Line 525 uses `const_cast<seastar::socket_address*>(&addr)` to capture a pointer to a map key during iteration, then dereferences it after the loop to index back into the map. While safe in single-threaded Seastar code today (no `co_await` between capture and use), this pattern is fragile: any future modification that inserts/erases map entries between the scan and the lookup would invalidate the pointer. `const_cast` on map keys is a well-known anti-pattern because it circumvents the const-correctness that protects key integrity.
  _What to change:_ Replace `seastar::socket_address* oldest_addr` with a `decltype(_pools)::iterator oldest_it = _pools.end()` and track the iterator directly. The lookup at line 530 becomes `auto& pool = oldest_it->second;` with no `const_cast` needed.
  _Location:_ `src/connection_pool.hpp:518-538`
  _Complexity:_ Low
  _Priority:_ P3 — Maintainability; eliminates a dangerous pattern before it becomes a real bug

### 18.3 Manual `_total_idle_connections` Counter is Drift-Prone

- [x] **Replace manual `_total_idle_connections` tracking with a derived or RAII-guarded counter**
  _Justification:_ The `_total_idle_connections` counter is manually incremented in `put()` (line 324) and decremented in four separate locations: `get()` (line 194), `put()` per-host eviction (line 308), `evict_oldest_global()` (line 534), `cleanup_expired()` (line 410), and `clear_pool()` (line 367). Any missed or double decrement silently corrupts capacity enforcement—the global eviction in `put()` uses this counter to decide when to evict (line 314), so drift means either premature eviction (lost connections) or unbounded growth (OOM). The scattered nature of these updates makes it easy to introduce drift during future modifications.
  _What to change:_ Option A (simple): Add a `debug_validate_counter()` method that walks all pools and asserts the sum matches `_total_idle_connections`; call it at the end of `put()`, `get()`, and `cleanup_expired()` in debug builds. Option B (structural): Compute the count on demand by summing `pool.size()` across all entries. This is O(B) where B is number of backends, but `evict_oldest_global()` already does an O(B) scan, so the marginal cost is low. Option C: Wrap the deque in a thin container that increments/decrements the shared counter on push/pop.
  _Location:_ `src/connection_pool.hpp` (all methods that modify `_total_idle_connections`)
  _Complexity:_ Medium
  _Priority:_ P2 — Correctness; silent counter drift causes incorrect capacity enforcement

### 18.4 `evict_oldest_global()` is O(B) on the `put()` Hot Path

- [x] **Avoid O(B) scan in `evict_oldest_global()` by maintaining a sorted eviction index**
  _Justification:_ When the pool is at `max_total_connections` capacity, every `put()` call triggers `evict_oldest_global()` (line 316), which iterates over all backend pools to find the one with the globally oldest front element (line 522-527). With `max_backends = 1000`, this is 1000 iterations per `put()` at capacity. In steady-state high-load scenarios (many backends, pool at capacity), this becomes a non-trivial reactor stall on the hot proxy path.
  _What to change:_ Maintain a `std::set` or min-heap of `(last_used, socket_address)` pairs updated on `put()` and `get()`. Eviction becomes O(log B) instead of O(B). Alternatively, accept the O(B) cost but document it as a known limitation, since the reaper timer (every 15s) keeps the pool below capacity in practice, making this path rare.
  _Location:_ `src/connection_pool.hpp:518-538`
  _Complexity:_ Medium
  _Priority:_ P3 — Performance; only matters at sustained high connection churn with many backends

### 18.5 Replace `std::unordered_map` with `absl::flat_hash_map`

- [x] **Switch `_pools` from `std::unordered_map` to `absl::flat_hash_map`**
  _Justification:_ The project already depends on Abseil (`absl::flat_hash_map` is used elsewhere). `std::unordered_map` uses node-based storage (one heap allocation per entry), causing poor cache locality during iteration. `absl::flat_hash_map` uses open addressing with flat storage, providing better cache behavior for both lookup (`get()`, `put()` call `find()`) and iteration (`evict_oldest_global()`, `cleanup_expired()`). The connection pool's `get()` and `put()` are on the hot proxy path, so even small lookup improvements compound.
  _What to change:_ Replace `std::unordered_map<seastar::socket_address, std::deque<Bundle>>` with `absl::flat_hash_map<seastar::socket_address, std::deque<Bundle>>`. Verify `seastar::socket_address` has an appropriate `AbslHashValue` overload or provide one. Update includes.
  _Location:_ `src/connection_pool.hpp:471`
  _Complexity:_ Low
  _Priority:_ P3 — Performance; incremental improvement on hot path

### 18.6 `BasicConnectionBundle::close()` Self-Referencing Continuation is an API Footgun

- [x] **Make `BasicConnectionBundle::close()` safe for direct callers or mark it private**
  _Justification:_ The `close()` method at line 74 chains `.then([this] { return in.close(); })`, capturing `this` in a Seastar continuation. This is only safe when the bundle is heap-allocated with a lifetime extending beyond the future (as `AsyncClosePolicy` does by moving to `unique_ptr` and capturing it in `.finally()`). If any caller invokes `close()` on a stack-local or member bundle that is destroyed before the continuation runs, the `[this]` capture becomes dangling—a use-after-free. The current code is safe because all close paths go through `close_bundle_async()` → `AsyncClosePolicy`, but the public `close()` method doesn't communicate this lifetime requirement.
  _What to change:_ Option A: Change `close()` to take a `seastar::lw_shared_ptr<BasicConnectionBundle>` and capture it in the continuation (self-preventing destruction). Option B: Remove `close()` from the public API entirely—make `AsyncClosePolicy` a friend and `close()` private, forcing all callers through `close_bundle_async()`. Option C: At minimum, add a prominent `/// @warning` doc comment stating the lifetime requirement.
  _Location:_ `src/connection_pool.hpp:71-77`
  _Complexity:_ Low (Option C) to Medium (Option A/B)
  _Priority:_ P3 — Maintainability; prevents future misuse as the codebase grows

### 18.7 Add `connections_created` and `connections_reused` Counters for Operational Visibility

- [x] **Track connection reuse ratio with `connections_created` and `connections_reused` stats**
  _Justification:_ The pool tracks `dead_connections_reaped`, `connections_reaped_max_age`, and `backends_rejected` but has no counter for total connections created (`create_connection()` calls) or successful pool reuses (the early-return path in `get()`). Without these, operators cannot determine the pool's hit rate—a critical metric for tuning `max_connections_per_host`, `idle_timeout`, and `max_connection_age`. A pool with <50% reuse rate suggests misconfigured timeouts or backend instability.
  _What to change:_ Add `size_t _connections_created = 0` (increment in `create_connection()`) and `size_t _connections_reused = 0` (increment in the reuse path of `get()`, line 237). Expose both in `Stats`. Optionally add `_connection_create_failures` for failed `seastar::connect()` calls.
  _Location:_ `src/connection_pool.hpp` (Stats struct, `create_connection()`, `get()`)
  _Complexity:_ Low
  _Priority:_ P2 — Observability; essential for production tuning and incident response

---

## 19. Gossip Service Review (2026-02-14)

Code review of `gossip_service.{hpp,cpp}` and its three sub-modules (`gossip_consensus`, `gossip_protocol`, `gossip_transport`). Findings span correctness, performance, and maintainability.

### 19.1 Fire-and-Forget Route Prune Futures Bypass the Gate (Rule #5)

- [x] **Gate-protect the route prune `parallel_for_each` futures instead of discarding them**
  _Justification:_ In three locations — `check_liveness()` (line 250), `remove_peer()` (line 139), and `update_peer_list()` (line 191) — the route pruning operation is dispatched as `(void)seastar::parallel_for_each(...)`, discarding the returned future. These fire-and-forget async operations are not covered by any gate, so they can continue executing after `GossipConsensus::stop()` returns and the object is destroyed. The `parallel_for_each` lambda captures `b_id` by value (safe), but the `.handle_exception` lambda calls `log_gossip_consensus()` which accesses a static logger (safe), so the immediate crash risk is limited. However, the `s_local_prune_callback` invoked on each shard could itself capture state from a service that has already been stopped. This is a latent use-after-free that will surface when callback implementations become more complex.
  _What to change:_ Acquire a `_timer_gate.hold()` at the start of each pruning dispatch and move the holder into `.finally()` on the outermost future. This ensures `stop()` (which closes `_timer_gate`) waits for all in-flight prune operations. Alternatively, collect the prune futures into a member `std::vector<seastar::future<>>` and drain them in `stop()`.
  _Location:_ `src/gossip_consensus.cpp:139-153`, `191-206`, `248-264`
  _Complexity:_ Low
  _Priority:_ P1 — Correctness; violates Rule #5 (gate guards for async lifetime)

### 19.2 Unbounded `_peer_table` Violates Rule #4

- [x] **Add a MAX_PEERS bound to the peer table and DNS discovery insertion paths**
  _Justification:_ `_peer_table` (`std::unordered_map<socket_address, PeerState>`) has no maximum size. The `add_peer()`, `update_peer_list()`, and initial population in `start()` all insert without a size check. A misconfigured or malicious DNS discovery response could return thousands of addresses, growing memory unbounded. Other gossip containers (`_received_seq_sets`, `_pending_acks`, `_peer_seq_counters`) already have `MAX_DEDUP_PEERS` and `MAX_PENDING_ACKS` bounds (Rule #4 compliance), but the authoritative peer table does not.
  _What to change:_ Add `static constexpr size_t MAX_PEERS = 1000;` (or configurable). Check size in `add_peer()` and `update_peer_list()` before inserting new entries. Log a warning and increment an overflow counter when the limit is reached. The existing `MAX_DEDUP_PEERS = 10000` in `GossipProtocol` should arguably be derived from `MAX_PEERS` rather than independently defined.
  _Location:_ `src/gossip_consensus.hpp:191`, `src/gossip_consensus.cpp:124-129`, `160-223`
  _Complexity:_ Low
  _Priority:_ P2 — Correctness; violates Rule #4 (bounded containers)

### 19.3 Dead `update_quorum_state()` Diverges From Active `check_quorum()`

- [x] **Remove dead `update_quorum_state()` or unify with `check_quorum()`**
  _Justification:_ `update_quorum_state()` (line 346) is declared private, defined at ~50 LOC, but never called anywhere in the codebase. It uses `_stats_cluster_peers_alive` (binary alive/dead) for quorum calculation, while the active `check_quorum()` uses `peers_recently_seen` (time-windowed). These two functions produce different quorum decisions for the same cluster state. If a future developer calls `update_quorum_state()` thinking it's interchangeable with `check_quorum()`, the cluster will exhibit inconsistent quorum behavior — one path could report HEALTHY while the other reports DEGRADED for the same peer table.
  _What to change:_ Option A (preferred): Delete `update_quorum_state()` entirely — it's dead code, and `check_quorum()` is the authoritative quorum path. Option B: If both alive-count and recently-seen semantics are needed, parameterize a single `evaluate_quorum(size_t alive_count)` method and call it from both sites with the appropriate count.
  _Location:_ `src/gossip_consensus.hpp:219`, `src/gossip_consensus.cpp:346-395`
  _Complexity:_ Low
  _Priority:_ P2 — Maintainability; dead code with divergent logic is a latent correctness risk

### 19.4 `process_retries()` Gate Holder Does Not Cover Fire-and-Forget Sends (Rule #5)

- [x] **Extend the gate holder lifetime in `process_retries()` to cover the send futures**
  _Justification:_ `process_retries()` (line 764) acquires a `_transport->timer_gate().hold()` at the top, then iterates pending ACKs and fires `(void)_transport->send(peer, pending.serialized_packet)` for each retry. The gate holder is scoped to the synchronous function body; it is destroyed when `process_retries()` returns. But the send futures are fire-and-forget — they execute asynchronously after the function returns. If `GossipTransport::stop()` closes the timer gate and destroys the channel while these sends are in-flight, the send futures may access a destroyed `_channel`. In practice Seastar's UDP send likely resolves with an exception on a closed channel, but this violates the gate pattern (Rule #5) and is unsafe if the transport's shutdown sequence changes.
  _What to change:_ Collect the retry send futures into a local vector and return `seastar::when_all_succeed(...)` from `process_retries()`, or move the gate holder into a `.finally()` on the aggregated future. This requires changing the retry timer callback to handle the returned future (e.g., `(void)process_retries().handle_exception(...)`).
  _Location:_ `src/gossip_protocol.cpp:764-832`
  _Complexity:_ Medium
  _Priority:_ P2 — Correctness; violates Rule #5 (gate guards must scope to full async lifetime)

### 19.5 O(N) Peer Address Lookup on Every Incoming Packet

- [x] **Replace `std::find()` on `_peer_addresses` vector with an `absl::flat_hash_set` for O(1) peer validation**
  _Justification:_ `handle_packet()` (line 430) validates the source address of every incoming UDP packet with `std::find(_peer_addresses->begin(), _peer_addresses->end(), src_addr)`. This is O(N) where N is the number of configured peers. While current cluster sizes are small (3-10 nodes), this is the hot path for packet reception — every heartbeat, route announcement, and ACK hits this linear scan. With DNS discovery enabled and larger clusters, this becomes a measurable cost per packet. The project already depends on Abseil.
  _What to change:_ Maintain a `absl::flat_hash_set<seastar::socket_address>` (or `std::unordered_set`) alongside the `_peer_addresses` vector. Update it in `refresh_peers()` when the peer list changes. Replace `std::find()` with a set lookup. The vector is still needed for iteration (heartbeat broadcast, etc.), so keep both.
  _Location:_ `src/gossip_protocol.cpp:430-434`
  _Complexity:_ Low
  _Priority:_ P3 — Performance; linear scan on hot path scales poorly with cluster size

### 19.6 Per-Peer Token Vector Copy in `broadcast_route()`

- [x] **Serialize the route packet once and reuse across peers, stamping only the per-peer sequence number**
  _Justification:_ `broadcast_route()` (line 335) uses `parallel_for_each` with a lambda that captures `tokens` by value. Inside the lambda, line 339 performs `pkt.tokens = std::vector<TokenId>(tokens.begin(), tokens.end())` — an explicit copy for each peer. For a cluster of N peers, this creates N+1 copies of the token vector (1 capture + N per-peer). With MAX_TOKENS=256 and sizeof(TokenId)=4, each copy is up to 1KB. The serialization at line 347 (`pkt.serialize()`) is also repeated N times. Since the packet differs only in `seq_num` (4 bytes at offset 2-5), the bulk of serialization is redundant work.
  _What to change:_ Serialize the packet once outside the loop with `seq_num=0`. Inside the per-peer lambda, copy the pre-serialized buffer and stamp the peer-specific `seq_num` at bytes 2-5. This eliminates N-1 token vector copies and N-1 serialization passes.
  _Location:_ `src/gossip_protocol.cpp:335-376`
  _Complexity:_ Low
  _Priority:_ P3 — Performance; redundant copies and serialization on broadcast path

### 19.7 Triplicated Route Prune Broadcast Code

- [x] **Extract route prune dispatch into a `broadcast_prune(BackendId)` helper method**
  _Justification:_ The identical ~12-line pattern of `parallel_for_each(irange(0, smp::count), [b_id](shard) { submit_to(shard, [b_id] { s_local_prune_callback(b_id); }); }).handle_exception(...)` appears verbatim in three methods: `check_liveness()`, `remove_peer()`, and `update_peer_list()`. This DRY violation means any fix to the prune dispatch (e.g., adding gate protection per 19.1, changing error handling, adding metrics) must be applied in three places. A missed update creates behavioral divergence.
  _What to change:_ Extract a private `seastar::future<> broadcast_prune(BackendId b_id)` method on `GossipConsensus`. Call it from all three sites. This also simplifies applying the gate fix from 19.1 in a single location.
  _Location:_ `src/gossip_consensus.cpp:139-153`, `191-206`, `248-264`
  _Complexity:_ Low
  _Priority:_ P3 — Maintainability; DRY violation across three call sites

### 19.8 No-Op `catch (...) { throw; }` Blocks Violate Rule #9

- [x] **Remove empty catch-rethrow blocks in `GossipService::start()` and `GossipTransport::start()`**
  _Justification:_ `GossipService::start()` (line 227-229) and `GossipTransport::start()` (line 45-47) both contain `catch (...) { throw; }` blocks that catch all exceptions and immediately rethrow without logging. These are no-ops — the exception would propagate identically without the try-catch. They add cognitive noise and violate Rule #9 (every catch block must log at warn level). If the intent was to add logging or cleanup later, the empty block is a "TODO" that was never completed.
  _What to change:_ Option A (preferred): Remove the try-catch blocks entirely — the exception propagates naturally. Option B: If cleanup is needed on failure (e.g., setting `_running = false`), add meaningful cleanup and a `log_gossip.error(...)` call before rethrowing.
  _Location:_ `src/gossip_service.cpp:227-229`, `src/gossip_transport.cpp:45-47`
  _Complexity:_ Low
  _Priority:_ P3 — Maintainability; dead code violating Rule #9

---

## 20. Hot-Path Performance Audit (2026-02-15)

Post-refactor benchmarks (February 15, 2026) show regression in prefix-routing gains compared to pre-refactor baselines (February 9, 2026). The 13B 30-user 30-minute apples-to-apples comparison dropped from -85% P99 / +22% throughput to -50% P99 / +7% throughput. Routing correctness is unchanged (98% cache hit rate, 0% incomplete). The regression is attributed to cumulative per-request overhead introduced by safety/audit fixes on the hot path (HTTP request → tokenize → ART lookup → backend select → forward). Estimated cumulative overhead: 160–800µs per request depending on prompt size.

### 20.1 UTF-8 Validation Scans Entire Request Body Per-Request

- [x] **Move UTF-8 validation to JSON parse boundary; remove redundant scan before tokenizer**
  _Justification:_ `TextValidator::validate_for_tokenizer()` is called at line 828 of `http_controller.cpp`, immediately before tokenization. It performs a byte-by-byte UTF-8 validation pass over the full text (`is_valid_utf8()` at `text_validator.hpp:68-112`). This is O(text_length) with high branch density — for a typical 50–100KB prompt, that's 50–100K branch-heavy iterations on every request. The text has already been parsed as valid JSON (which implies valid UTF-8 for string values), so this is a redundant validation. The null-byte scan (`text.find('\0')`) is also O(N) but cheaper due to memchr optimization.
  _What to change:_ Option A (preferred): Remove the `validate_for_tokenizer()` call entirely — JSON parsing already guarantees valid UTF-8 string content. Option B: If defense-in-depth is desired, validate once during JSON body parsing in the request handler and cache a `utf8_validated` flag on the request context, skipping re-validation at the tokenizer input. Option C: Replace the byte-by-byte loop with a SIMD UTF-8 validator (e.g., simdutf library) for ~10x throughput.
  _Location:_ `src/http_controller.cpp:828-829`, `src/text_validator.hpp:37-112`
  _Complexity:_ Low
  _Priority:_ P1 — Performance; O(N) per-byte scan on every request, redundant with JSON parsing

### 20.2 Prefix Boundary Detection Triggers Extra Tokenization Passes

- [x] **Pre-compute prefix boundary during initial tokenization instead of re-tokenizing**
  _Justification:_ The prefix boundary detection logic (`http_controller.cpp:886-1011`) calls `_tokenizer.local().encode_cached()` a second time (line 982) to tokenize the system message separately, after the full prompt was already tokenized at line 843. With multi-depth routing enabled, additional `encode_cached()` calls occur at line 939 for each message boundary. Even with tokenizer cache hits, each call involves a Rust FFI round-trip through tokenizers-cpp. The initial tokenization already processes the full text — the boundary could be computed from that result by tracking message offsets during JSON parsing.
  _What to change:_ During the initial `extract_text_for_tokenization()` call, also record the character offset where system messages end. After the single tokenization pass, use that offset to find the corresponding token boundary (binary search on token offsets or linear scan of token positions). This eliminates all secondary `encode_cached()` calls. The `RequestRewriter::extract_system_messages()` JSON parsing at line 973 can be folded into the existing body parsing.
  _Location:_ `src/http_controller.cpp:886-1011` (boundary detection), `843` (initial tokenization), `939` (multi-depth), `982` (system message)
  _Complexity:_ Medium
  _Priority:_ P1 — Performance; extra Rust FFI tokenization round-trips on every request with prefix boundary enabled

### 20.3 Load-Aware Backend Selection Iterates All Backends Twice Per Request

- [x] **Short-circuit load-aware selection and avoid duplicate calls in route_request()**
  _Justification:_ `apply_load_aware_selection()` (`router_service.cpp:440-481`) is called twice in `route_request()`: once after an ART hit (line 1202) and once after hash fallback (line 1232). Each invocation performs a `flat_hash_map` lookup for the preferred backend's load, then iterates all live backends via `get_least_loaded_backend()` to find the minimum. With 8 backends, that's 16+ map lookups and comparisons per request. The function also reads two config values and may record a metric (atomic increment). When load-aware routing is disabled, the early-return is cheap (config check + return), but the function is still called twice.
  _What to change:_ Option A: Call `apply_load_aware_selection()` once after the final backend is selected (whether from ART or hash), not in both branches. Option B: Cache the least-loaded backend per scheduling quantum (e.g., every 100µs or every N requests) rather than recomputing from scratch on every request — backend loads change slowly relative to request rate. Option C: Replace the O(N) linear scan with a pre-maintained min-heap or sorted structure updated on load change events.
  _Location:_ `src/router_service.cpp:440-481` (function), `1202-1203` (first call), `1232-1233` (second call)
  _Complexity:_ Low–Medium
  _Priority:_ P2 — Performance; O(num_backends) iteration twice per request, but individual iterations are cheap

### 20.4 Excessive Metrics Recording with Clock Reads on Hot Path

- [x] **Reduce per-request clock reads by batching latency measurements**
  _Justification:_ The `handle_proxy()` method in `http_controller.cpp` (starting at line 646) contains approximately 63 `metrics().record_*()` calls. Of these, 11 are latency-recording methods (`metrics_service.hpp:383-444`) that each call `steady_clock::now()` internally for histogram observation. Each clock read costs 10–100 CPU cycles depending on the platform's clock source (TSC vs HPET). With 5–8 latency recordings per request on the typical path (routing, tokenization, ART lookup, connect, response, backend total, first-byte, request total), the cumulative cost is 50–800 cycles per request. The non-latency metric recordings (atomic counter increments) are individually cheap (~5 cycles each) but 40+ per request adds up.
  _What to change:_ Option A: Take a single `steady_clock::now()` snapshot at each phase transition (request start, post-tokenize, post-route, post-connect, first-byte, request-end) and compute all latency deltas from these 5–6 snapshots. This replaces ~8 clock reads with ~6. Option B: Record latencies only on sampled requests (e.g., every 100th) for histogram accuracy with less overhead. Option C: Consolidate related counter increments into a single struct update per phase.
  _Location:_ `src/http_controller.cpp:646+` (handle_proxy), `src/metrics_service.hpp:383-444` (latency recording methods)
  _Complexity:_ Medium
  _Priority:_ P2 — Performance; cumulative clock reads and atomic increments on every request

### 20.5 Gate/Semaphore/Backpressure Checks Before Request Processing

- [x] **Reorder admission checks and evaluate whether all three are needed simultaneously**
  _Justification:_ Every request entering `handle_proxy()` performs three admission checks before any useful work: gate hold (atomic CAS, line 686), semaphore try_get_units (atomic CAS, line 699), and persistence backpressure check (line 713). The gate and semaphore are necessary for correctness (shutdown safety and concurrency limits). The persistence backpressure check queries whether the async persistence layer is behind; this is only relevant when persistence is enabled, which is a minority of deployments. All three checks execute unconditionally.
  _What to change:_ Option A: Guard the persistence backpressure check behind `if (_persistence_enabled)` to skip it in the common case. Option B: Combine gate + semaphore into a single admission operation if Seastar supports it (gate::hold already implies the service is running). Option C: Profile whether the atomic CAS operations are contended under load — if not, the overhead is negligible and this item is low priority.
  _Location:_ `src/http_controller.cpp:684-716`
  _Complexity:_ Low
  _Priority:_ P3 — Performance; three atomic operations per request, likely low individual impact

### 20.6 Shard Load Metrics Use Atomics on Shard-Local Data

- [x] **Replace atomic operations with plain integers for shard-local load tracking**
  _Justification:_ `shard_load_metrics.hpp` declares `_active_requests`, `_queued_requests`, and `_total_requests` as `std::atomic<uint64_t>` (lines 132–134). These are accessed via `increment_active()` / `decrement_active()` (lines 85–90) using `memory_order_relaxed` atomics. The shard load metrics object is shard-local (one per Seastar shard), so there is no cross-thread access — the atomics are unnecessary. Each `fetch_add`/`fetch_sub` is ~5 cycles on x86 even with relaxed ordering, and they execute at least twice per request (increment at line 732, decrement at lines 1202/1221/1246/1340/1411, plus `record_request_completed()` at line 1412). On a shared-nothing architecture, plain `uint64_t` with `++`/`--` would be sufficient and ~1 cycle each.
  _What to change:_ Replace `std::atomic<uint64_t>` with `uint64_t` for all three counters. Replace `fetch_add(1, memory_order_relaxed)` with `++` and `fetch_sub(1, memory_order_relaxed)` with `--`. Replace `load(memory_order_relaxed)` with direct reads. Verify that cross-shard reads (if any, e.g., from the load-aware selection in `router_service.cpp`) go through `submit_to()` which provides the necessary memory barrier.
  _Location:_ `src/shard_load_metrics.hpp:85-103` (methods), `132-134` (fields); `src/http_controller.cpp:732` (increment), `1202/1221/1246/1340/1411` (decrement)
  _Complexity:_ Low
  _Priority:_ P2 — Performance; unnecessary atomic operations on shard-local data, ~3–6 atomics per request

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

### 20.9 Thread Pool Burst-Routing Can Cause Transient Hot-Spotting

- [x] **Mitigate burst routing decisions after concurrent thread pool completions**
  _Status:_ **Done (option a).** Moved `BackendRequestGuard` creation from post-address-lookup to immediately after routing decision in `http_controller.cpp`. The guard's constructor increments `active_requests` speculatively, so the next routing decision in the same burst sees the updated load counter. RAII handles cleanup on error paths; circuit breaker fallback uses move-assignment to swap targets.
  _Changes:_ `src/http_controller.cpp` — guard creation moved ~40 lines earlier (from after address lookup to after `route_request()` returns). No API changes to `BackendRequestGuard`, `route_request()`, or load metrics.
  _Remaining options (b) and (c) are deferred — speculative increment addresses the root cause directly._

### 20.10 Pin tokenizers-cpp Dependency to Specific Commit

- [x] **Pin tokenizers-cpp to a known-good commit hash for reproducible builds**
  _Status:_ **Done.** Pinned to `34885cfd7b9ef27b859c28a41e71413dd31926f5` (2026-01-23, CMake compat + HF tokenizer 0.21.2).
  _Changes made:_
  (a) `CMakeLists.txt`: `GIT_TAG main` → `GIT_TAG 34885cfd7b9ef27b859c28a41e71413dd31926f5`
  (b) `Dockerfile.base`: Added `git checkout 34885cfd7b9ef27b859c28a41e71413dd31926f5` after clone
  (c) `Dockerfile.production.fast`: No clone — inherits from base image; added comment noting this
  (d) Update procedure documented below

  **Updating pinned tokenizers-cpp:**
  1. Check upstream: `git ls-remote https://github.com/mlc-ai/tokenizers-cpp HEAD`
  2. Review changes since pin: `https://github.com/mlc-ai/tokenizers-cpp/compare/34885cfd...main`
  3. Update the commit hash in both `CMakeLists.txt` (GIT_TAG) and `Dockerfile.base` (git checkout)
  4. Rebuild base image: `docker build -f Dockerfile.base -t ranvier-base:latest .`
  5. Run full test suite and benchmarks to validate

### 20.11 Cross-Shard Speculative Load Synchronization for Burst Routing

- [x] **Propagate speculative load increments across shards to prevent cross-shard burst-routing hot-spotting** ✓
  _Justification:_ The speculative load increment (20.9) increments `BackendInfo::active_requests` in `g_shard_state->backends` — a `thread_local` structure visible only to the current shard. When routing decisions happen near-simultaneously on different shards (e.g., client-side tokenization where routing overhead is 0.4ms, or high-concurrency bursts), each shard independently picks the "best" backend without seeing the other shards' speculative increments. This causes multiple shards to route to the same backend in the same flush interval, creating hot-spotting.
  _Evidence:_ On bb20555, server-tokenize 13B 30u 30m shows P99 -51.3% (excellent), but client-tokenize 13B 30u 10m shows P99 +18.3% (hot-spotting). The only difference is tokenization latency: server-side ~10-12ms provides natural stagger between routing decisions, while client-side 0.4ms causes near-simultaneous cross-shard routing. The batched local route learning (21.1) compounds this — per-request SMP broadcast was replaced with periodic flush, reducing inter-shard synchronization frequency.
  _Approach implemented:_ Option (b) — **Periodic global load snapshot**. Each shard periodically broadcasts its `active_requests` map to all other shards via `smp::submit_to`. Fixed SMP overhead regardless of request rate (O(shards^2) messages per interval, not per request). Implementation:
  (1) Added `cross_shard_load_sync` (bool, default true) and `cross_shard_load_sync_interval` (default 5ms) to `RoutingConfig` with YAML and env var support (`RANVIER_CROSS_SHARD_LOAD_SYNC`, `RANVIER_CROSS_SHARD_LOAD_SYNC_INTERVAL_MS`).
  (2) Added per-shard state in `ShardLocalState`: `shard_load_snapshots` (per-source-shard snapshot vector), `cross_shard_load` (aggregated map), `load_sync_timer`, `load_sync_gate`.
  (3) `broadcast_load_snapshot()`: Builds snapshot of local `active_requests` (only non-zero entries), wraps in `foreign_ptr` (Rule #14), broadcasts to all other shards via `parallel_for_each` + `submit_to`. Gate-holder safety (Rule #5).
  (4) `apply_load_snapshot()`: On receipt, stores snapshot indexed by source shard and recomputes `cross_shard_load` as sum of all other shards' loads.
  (5) `get_backend_load()`: Now returns `local_active_requests + cross_shard_load[id]` when sync is enabled, giving routing decisions a global view.
  (6) Lifecycle: `start_load_sync_timer()` called via `smp::invoke_on_all` in `initialize_shards()`. `stop_load_sync_timer()` called in `stop()` before member timer gate close. Skips startup if single-shard or sync disabled.
  _Metrics added:_ `router_load_sync_broadcasts_total`, `router_load_sync_snapshots_received_total`.
  _Location:_ `src/router_service.cpp`, `src/router_service.hpp`, `src/config_schema.hpp`, `src/config_loader.cpp`
  _Complexity:_ Medium
  _Priority:_ P2 — Performance; affects client-tokenize and high-concurrency burst scenarios. Server-side tokenization naturally mitigates via stagger.
  _Completed:_ 2026-02-21 (initial implementation, 575dd78)
  _Revised:_ 2026-02-22 (fix branch `claude/fix-shard-sync-performance`, 8e49187/c219fbd)

  **Performance regression and fix (Feb 22, 2026):**
  The initial implementation (575dd78, default 5ms interval) caused a **3x tokenization regression**
  (12ms → 40ms) from SMP reactor congestion. On an 8-shard system at 5ms interval, this generates
  ~24,000 SMP messages/sec — enough to congest the reactor and inflate `alien::run_on()` completion
  latency (tokenizer thread pool callback). The fix branch (8e49187) applied three optimizations:
  (1) Incremental `apply_load_snapshot()` — O(old + new entries) subtract-then-add instead of
      O(shards × backends) full recompute on every receive.
  (2) Serialize snapshots as `vector<pair>` instead of `flat_hash_map` — cheaper cross-shard copy.
  (3) Default interval increased to 100ms; feature disabled by default.

  **Benchmark results (Feb 22, c219fbd on Instance 6):**
  - Sync enabled @ 10ms flush: **-52.2% P99**, +11.2% throughput, **86.8% cache hits**, 20ms tokenization (2x)
  - Sync enabled @ 20ms flush: +6.1% P99, +12.4% throughput, 24.6% cache hits — **FAILED** (stale load data)
  - Sync disabled @ 10ms-50ms flush: -29% to -42% P99, 45-62% cache hits (high variance)

  **Remaining work:**
  - [ ] Decouple cross-shard sync interval from batch flush interval for independent tuning
  - [ ] Investigate reducing 2x tokenization overhead (20ms → target <15ms) when sync enabled
  - [ ] Consider making load signal a tiebreaker rather than primary routing signal

---

## 21. Request Lifecycle Performance Analysis (2026-02-20)

End-to-end trace of an LLM inference request through all phases documented in `docs/internals/request-lifecycle.md`. Identifies 10 potential performance issues ranked by severity, with mitigations. See `docs/internals/request-lifecycle-perf-analysis.md` for the full analysis.

### 21.1 Batch Locally-Learned Routes Instead of Per-Request Broadcast

- [x] **Accumulate learned routes in shard-local buffer and flush periodically instead of per-request cross-shard broadcast** ✓
  _Justification:_ Every successful proxied request triggers `learn_route_global()` which calls `smp::submit_to` to every shard plus shard 0 for gossip — `O(shards + 1)` cross-shard messages, each allocating a `foreign_ptr<vector<int32_t>>`. On an 8-core system this is 9 SMP messages and 9 heap allocations per request. Under load with low ART hit rates (cold start, new prompts), this creates an SMP message storm that competes with actual request processing. The `flush_route_batch()` pattern already exists for remote (gossip-received) routes — it accumulates routes in `_pending_remote_routes` on shard 0 and flushes periodically via `_batch_flush_timer` (see `RouteBatchConfig` in `router_service.hpp`). The same pattern does not exist for locally-learned routes.
  _Approach implemented:_ (a) Added `PendingLocalRoute` struct and shard-local `pending_local_routes` buffer in `ShardLocalState` (bounded by `RouteBatchConfig::MAX_BUFFER_SIZE` with batch-drop overflow strategy). (b) Changed `learn_route_global()` to validate and push into shard-local buffer via `buffer_local_route()` instead of broadcasting. Zero SMP messages on the hot path. (c) Added per-shard `local_flush_timer` in `ShardLocalState` that flushes via `flush_local_route_batch()` every 10ms (same interval as remote batch). Timer started via `smp::invoke_on_all` in `initialize_shards()`. (d) Gossip `submit_to(0)` preserved in flush — batch is sent to shard 0 where `GossipService` broadcasts each route via `parallel_for_each`. (e) Deduplication via FNV-1a hash of token bytes + backend ID using `absl::flat_hash_set` before broadcasting. (f) Gate-holder safety (Rule #5) via per-shard `local_flush_gate` in `ShardLocalState`; `flush_local_route_batch()` acquires holder at entry. `stop_local_batch_timer()` closes gate, cancels timer, drains remaining buffer. Cross-shard memory safety (Rule #14) uses same `foreign_ptr` pattern as remote batch flush.
  _Also modified:_ `learn_route_global_multi()` — pushes all boundary prefixes directly into shard-local buffer (O(1) per boundary, no futures), triggers flush only if buffer full.
  _Metrics added:_ `router_local_routes_batched_total`, `router_local_batch_flushes_total`, `router_local_routes_deduplicated_total`, `router_local_routes_dropped_overflow_total`.
  _Location:_ `src/router_service.cpp`, `src/router_service.hpp`
  _Complexity:_ Medium
  _Priority:_ P1 — Performance; O(shards) SMP messages per request on the hot path
  _Completed:_ 2026-02-20

### 21.2 Cap Reactor-Blocking Tokenization Fallback

- [x] **Gate local tokenization fallback with a shard-local semaphore to prevent reactor stalls**
  _Justification:_ When both the thread pool queue is full (Priority 1) and cross-shard dispatch declines (Priority 2), `encode_threaded_async()` at line 352 falls back to `tokenize_locally()` — a synchronous Rust FFI call that blocks the Seastar reactor for 5–13ms. All other requests on that shard stall: no reads, no writes, no timer callbacks. On a system handling 1000 req/s across 8 cores, a 10ms stall on one shard delays ~12 requests. Under load spikes (thread pool saturation + cross-shard backpressure), multiple shards can hit it simultaneously.
  _What to change:_ (a) Add a shard-local `seastar::semaphore _local_tokenize_sem{1}` (or configurable, default 1) that gates Priority 3. (b) Before calling `tokenize_locally()`, attempt `try_wait(_local_tokenize_sem, 1)`. If it fails (another local tokenization is already in progress), return an empty `TokenizationResult` — the caller in `http_controller.cpp` will fall back to hash/random routing. (c) Add `_local_tokenize_sem.signal(1)` after `tokenize_locally()` returns (use RAII or scope guard for exception safety). (d) Add a Prometheus counter `ranvier_tokenizer_local_fallback_rejected` for when the semaphore is full. Expose `_cross_shard_local_fallbacks` as a counter if not already. Note: `tokenize_locally()` is synchronous (Rust FFI), so you cannot use `seastar::get_units()` (async wait) — the caller must fail fast with `try_wait()`.
  _Location:_ `src/tokenizer_service.cpp:295-374`, `src/tokenizer_service.hpp`
  _Complexity:_ Low
  _Priority:_ P1 — Performance; 5–13ms reactor stall per shard on fallback path
  _Completed:_ 2026-02-21

### 21.3 Replace `std::mutex` in Async Persistence with Lock-Free Queue

- [ ] **Replace mutex-guarded deque in `AsyncPersistenceManager` with a lock-free SPSC ring buffer**
  _Justification:_ `try_enqueue()` at line 202 acquires `std::lock_guard<std::mutex>` on the Seastar reactor thread. The same mutex is contended by `extract_batch()` (called from the persistence worker's `std::thread`), creating cross-thread contention on the reactor. Even a brief lock hold on the reactor violates Seastar's shared-nothing model (Hard Rule #1 — no locks on the reactor). Current queue operations under `_queue_mutex`: `try_enqueue()` (reactor thread, hot path), `extract_batch()` (worker thread, periodic), `drain_queue()` (worker thread, shutdown), `queue_clear_all()` (reactor thread, admin endpoint).
  _What to change:_ (a) Replace `std::deque<PersistenceOp> _queue` + `std::mutex _queue_mutex` with a bounded SPSC ring buffer. The reactor thread is the sole producer; the persistence worker is the sole consumer. Options: implement a simple `std::array`-based ring buffer with atomic head/tail indices, or use a well-tested lock-free queue. (b) `try_enqueue()` becomes a lock-free CAS on the tail index. Return false if full (existing drop behavior). (c) `extract_batch()` becomes a lock-free drain of up to `max_batch_size` elements from the head. (d) Handle `queue_clear_all()` carefully — set an atomic flag that the worker thread checks before processing. (e) Keep `_queue_size` atomic counter for metrics (already exists). (f) Remove `_queue_mutex` entirely. Note: `PersistenceOp` is a `std::variant` of several op types. The ring buffer must support move-only types. Pre-allocate to `max_queue_depth` (config, default 10,000).
  _Location:_ `src/async_persistence.cpp:202-246`, `src/async_persistence.hpp`
  _Complexity:_ Medium
  _Priority:_ P2 — Performance; reactor-thread mutex contention violates shared-nothing model

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

### 21.6 Release Concurrency Semaphore During Connection Retry Backoff

- [ ] **Release semaphore unit before retry sleep to prevent dead-backend retries from exhausting concurrency**
  _Justification:_ At line 391, `co_await seastar::sleep(ctx.current_backoff)` pauses the request during connection retry. The request holds a concurrency semaphore unit (acquired at line 699 via `try_get_units(_request_semaphore, 1)`) for the entire retry duration. Under default config (max_retries=3, initial_backoff=100ms, multiplier=2x), a connection to a dead backend holds resources for up to 700ms. With a concurrency limit of 128, just 128 requests hitting a dead backend simultaneously exhaust the semaphore, causing all subsequent requests to get 503 — even requests targeting healthy backends.
  _What to change:_ (a) Release the semaphore unit before the retry sleep and re-acquire after waking. This requires passing the semaphore units into `establish_backend_connection()` or restructuring the retry loop. The re-acquire must handle the case where the semaphore is now full (return 503 rather than blocking). (b) Alternative simpler approach: check the circuit breaker state before retrying. If `_circuit_breaker.state(ctx.current_backend) == CircuitState::OPEN` after `record_failure()`, skip remaining retries and fall back immediately. This short-circuits the retry loop without changing semaphore semantics. (c) Add a metric `ranvier_proxy_retry_semaphore_releases` to track frequency.
  _Location:_ `src/http_controller.cpp:330-407` (establish_backend_connection), `699` (semaphore acquire)
  _Complexity:_ Medium
  _Priority:_ P2 — Reliability; dead-backend cascading 503 under concurrency pressure

### 21.7 Deduplicate Route Learning Before Cross-Shard Broadcast

- [ ] **Short-circuit route learning with shard-local ART lookup to skip redundant broadcasts**
  _Justification:_ Route learning at `http_controller.cpp:582-601` fires on every successful 2xx response. If 100 requests share the same system prompt (common in RAG workloads), the same prefix→backend route is broadcast 100 times: 100 cross-shard broadcasts + 100 persistence enqueues. The ART `insert()` is idempotent (overwrites), so 99 broadcasts produce no new state. Compounds with Issue 21.1 if route batching is not yet implemented.
  _What to change:_ (a) Add a short-circuit at the top of `learn_route_global()` (after prefix truncation at line 1654, so the lookup uses the same effective prefix): check if the route already exists with the same backend via `RadixTree::lookup()` (O(k), shard-local, no SMP message). If it matches, return early. (b) Also skip the `_persistence->queue_save_route()` call in `http_controller.cpp:600` when the route is already known. (c) Add a counter `_routes_deduped` for observability. Note: benign TOCTOU race exists (another shard could evict the route between check and insert), but redundant inserts are safe — the goal is to eliminate the common case.
  _Location:_ `src/router_service.cpp:1619-1670`, `src/http_controller.cpp:582-601`
  _Complexity:_ Low
  _Priority:_ P2 — Performance; eliminates ~99% redundant broadcasts for repeated prompts

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

## 22. Code Modularity (Low Priority)

Internal refactors that improve separation of concerns. No user-facing behavior change.

### 22.1 Extract BackendRegistry Interface from RouterService

- [ ] **Decouple HealthService and K8sDiscoveryService from RouterService**
  _Justification:_ HealthService and K8sDiscoveryService take `RouterService&` directly. They only use `get_all_backend_ids()`, `get_backend_address()`, and `set_backend_status()`. Extracting a `BackendRegistry` interface makes both services independently testable without constructing a full RouterService.
  _What to change:_ Define a `BackendRegistry` abstract class with the three methods above. Have RouterService implement it. Change HealthService and K8sDiscoveryService constructors to accept `BackendRegistry&`.
  _Location:_ `src/health_service.hpp`, `src/k8s_discovery_service.hpp`, `src/router_service.hpp`
  _Complexity:_ Low
  _Priority:_ P4 — Testability improvement

### 22.2 Split config_schema.hpp into Infrastructure and Product Configs

- [ ] **Separate generic infrastructure configs from routing-specific configs**
  _Justification:_ `config_schema.hpp` is 618 lines mixing infrastructure configs (ServerConfig, PoolConfig, HealthConfig, TlsConfig, AuthConfig, etc.) with Ranvier-specific configs (RoutingConfig, AssetsConfig). Splitting reduces cognitive load and makes infrastructure configs independently reusable.
  _What to change:_ Move infrastructure config structs to `config_infra.hpp`. Keep RoutingConfig and AssetsConfig in `config_schema.hpp`. RanvierConfig includes both headers.
  _Location:_ `src/config_schema.hpp`
  _Complexity:_ Low
  _Priority:_ P4 — Code organization

### 22.3 Split MetricsService into Helpers and Ranvier-Specific Counters

- [ ] **Extract generic histogram/counter patterns from Ranvier-specific metrics**
  _Justification:_ `metrics_service.hpp` is 620 lines mixing reusable patterns (MetricHistogram class, bucket definitions, per-backend metrics map) with Ranvier-specific counters (tokenization, ART, prefix boundary). Splitting makes the generic patterns reusable and reduces file size.
  _What to change:_ Move MetricHistogram, bucket helpers, and the bounded per-backend metrics pattern to `metrics_helpers.hpp`. Keep Ranvier-specific counters in `metrics_service.hpp`.
  _Location:_ `src/metrics_service.hpp`
  _Complexity:_ Low
  _Priority:_ P4 — Code organization

### 22.4 Template ShardedConfig on Config Type

- [ ] **Make ShardedConfig generic instead of hardcoded to RanvierConfig**
  _Justification:_ Trivial change (`ShardedConfig<T>` with backward-compatible alias `using ShardedConfig = BasicShardedConfig<RanvierConfig>`). Follows the existing pattern used by ConnectionPool, CircuitBreaker, and RateLimiter.
  _Location:_ `src/sharded_config.hpp`
  _Complexity:_ Trivial
  _Priority:_ P4 — Consistency with existing template patterns

---

## 23. Hard Rules Compliance Audit (2026-02-28)

Audit the codebase against Hard Rules 16-23 (added 2026-02-28 from ScyllaDB/Seastar production experience). Rules 0-15 were audited during previous reviews; rules 16-23 have never been checked against existing code.

**Approach:** Component-by-component, all rules per component. Each component is audited in a single session checking all 8 new rules, since rule violations interact within a component and fixes are more coherent with full component context.

**Phase 1 — Mechanical grep pass (all components, fast)**

- [ ] **Scan for lambda coroutines passed to `.then()` without `seastar::coroutine::lambda()` wrapper (Rule 16)**
  _Pattern:_ `.then([` near `co_await` inside the lambda body
  _Scope:_ All `src/**/*.{hpp,cpp}`
  _Priority:_ P1 — Use-after-free, ASAN may miss

- [ ] **Scan for loops without `maybe_yield()` preemption points (Rule 17)**
  _Pattern:_ `for (` loops without `maybe_yield` in body, particularly over containers that could exceed ~100 iterations
  _Scope:_ All `src/**/*.{hpp,cpp}`
  _Priority:_ P2 — Reactor stalls

- [ ] **Scan for raw `semaphore::wait()`/`signal()` pairs (Rule 19)**
  _Pattern:_ `_sem.wait(` or `.signal(` without corresponding `get_units` or `with_semaphore`
  _Scope:_ All `src/**/*.{hpp,cpp}`
  _Priority:_ P1 — Eventual deadlock

- [ ] **Scan for `do_with` lambdas missing `&` on parameters (Rule 20)**
  _Pattern:_ `do_with(` ... `](auto ` without `&`
  _Scope:_ All `src/**/*.{hpp,cpp}`
  _Priority:_ P1 — Use-after-free

- [ ] **Scan for coroutines taking reference parameters (Rule 21)**
  _Pattern:_ `future<>` functions with `const&` or `&` params
  _Scope:_ All `src/**/*.{hpp,cpp}`
  _Priority:_ P2 — Dangling reference on suspend

- [ ] **Scan for `temporary_buffer::share()` stored to member variables (Rule 23)**
  _Pattern:_ `.share(` assigned to `_member` or stored in containers
  _Scope:_ All `src/**/*.{hpp,cpp}`
  _Priority:_ P2 — Silent memory bloat

**Phase 2 — Component deep audit (all new rules per component)**

Rules 18 (discarded futures) and 22 (exception-before-future) require understanding async flow and cannot be reliably grepped. These are checked during the component audit along with a second pass on all other new rules.

- [ ] **Audit `http_controller.{hpp,cpp}`**
  _Justification:_ Highest traffic, most async complexity, most likely to have discarded futures and coroutine ref params
  _Priority:_ P1

- [ ] **Audit `application.{hpp,cpp}`**
  _Justification:_ Lifecycle orchestration, shutdown ordering, gate/timer patterns
  _Priority:_ P1

- [ ] **Audit `gossip_service.{hpp,cpp}`, `gossip_protocol.{hpp,cpp}`, `gossip_transport.{hpp,cpp}`, `gossip_consensus.{hpp,cpp}`**
  _Justification:_ Cross-shard communication, timers, background fibers
  _Priority:_ P1

- [ ] **Audit `tokenizer_service.{hpp,cpp}`, `tokenizer_thread_pool.{hpp,cpp}`**
  _Justification:_ FFI boundary, prior allocator corruption issues
  _Priority:_ P2

- [ ] **Audit `router_service.{hpp,cpp}`, `radix_tree.hpp`, `node_slab.{hpp,cpp}`**
  _Justification:_ CPU-bound potential (ART traversal), preemption relevance
  _Priority:_ P2

- [ ] **Audit remaining services (`sqlite_persistence`, `async_persistence`, `health_service`, `k8s_discovery_service`, `connection_pool`, `circuit_breaker`, `rate_limiter`, `stream_parser`, `shard_load_balancer`)**
  _Justification:_ Lower complexity but still need coverage
  _Priority:_ P3

---

## 23. Shard 0 Role Isolation Analysis (2026-03-06)

Investigation into whether shard 0 should be excluded from data plane traffic, given its exclusive control plane responsibilities (GossipService, GossipConsensus, K8sDiscoveryService, HealthService).

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

## References

- [Ranvier Architecture](./architecture.md)
- [API Reference](./api-reference.md)
- [Request Flow Diagrams](./request-flow.md)
- [Integration Test Guide](../tests/integration/README.md)
