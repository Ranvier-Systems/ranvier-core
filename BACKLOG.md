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

- [ ] **[P1] Consolidate gossip debug metrics behind compile flag**
  _Description:_ Move 8 debugging-oriented gossip metrics behind `RANVIER_DEBUG_METRICS` compile flag. Keep ~15 operationally-relevant metrics always enabled.
  _Rationale:_ 27 metrics per gossip service adds scrape overhead. Debug metrics (`crypto_stalls_avoided`, `cert_reloads`, etc.) provide value during development, not production.
  _Files:_ `src/gossip_service.cpp`
  _Complexity:_ Low

- [ ] **[P2] Add inline Hard Rule documentation to radix_tree.hpp**
  _Description:_ Add explicit Hard Rule comments to load-bearing code paths in RadixTree (lookup, insert, eviction). Currently only 1 reference despite being most critical file.
  _Rationale:_ Knowledge preservation for future maintainers. Explicit documentation prevents accidental rule violations during optimization work.
  _Files:_ `src/radix_tree.hpp`
  _Complexity:_ Low

- [ ] **[P2] Add inline Hard Rule documentation to router_service.cpp**
  _Description:_ Add explicit Hard Rule comments to load-bearing code paths in RouterService (route_request, learn_route_global, get_backend_for_prefix). Currently only 1 reference despite being second most critical file.
  _Rationale:_ Knowledge preservation for future maintainers. RouterService orchestrates all routing decisions; undocumented constraints risk silent regressions.
  _Files:_ `src/router_service.cpp`
  _Complexity:_ Low

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

### 9.5 Tokenizer Performance

- [ ] **Rebuild tokenizers-cpp with statically-linked jemalloc allocator**
  _Justification:_ Cross-shard dispatch revealed memory allocation overhead in Rust tokenizer. jemalloc provides better performance for multi-threaded allocations and avoids glibc malloc contention.
  _Approach:_ Update tokenizers-cpp build to link jemalloc statically, benchmark allocation overhead reduction
  _Location:_ `tokenizers-cpp/CMakeLists.txt`
  _Complexity:_ Medium
  _Note:_ Added 2026-02-01 after debugging cross-shard memory crash

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
| **P2 - Medium** | DX | Benchmark regression CI | Medium | |
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
| **P1 - High** | DX | Consolidate gossip debug metrics behind compile flag | Low | |
| **P2 - Medium** | DX | Add Hard Rule documentation to radix_tree.hpp | Low | |
| **P2 - Medium** | DX | Add Hard Rule documentation to router_service.cpp | Low | |
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
| **P2 - Medium** | Performance | Tokenizer - jemalloc static linking | Medium | |

---

## Completed Items

_Move completed items here with completion date and PR reference._

| Date | Item | PR |
|------|------|----|
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

## References

- [Ranvier Architecture](./architecture.md)
- [API Reference](./api-reference.md)
- [Request Flow Diagrams](./request-flow.md)
- [Integration Test Guide](../tests/integration/README.md)
