# Adversarial System Audit Report

**Date:** 2026-02-12
**Auditor Posture:** Cynical Staff Engineer & Security Auditor
**Scope:** All 57 source files under `src/`
**Checked Against:** 16 Hard Rules (Rules 0-15), Seastar reactor model, layered architecture

---

## Criticality Score: 7/10

**Rationale:** Multiple CRITICAL async integrity violations (gate guard lifetime bugs, cross-shard `std::function` broadcast, mutex on hot path), a data structure corruption bug in the RadixTree route counter that causes cascading eviction failure, and numerous unbounded containers exploitable for OOM. The codebase shows strong discipline overall -- most Hard Rules are actively followed with explicit comments -- but the violations that exist are structural, not cosmetic.

---

## Findings by Lens

### 1. Async Integrity

| # | Severity | File:Line | Rule | Issue | Recommendation |
|---|----------|-----------|------|-------|----------------|
| A1 | **CRITICAL** | `gossip_protocol.cpp:201-203` | #5 | Heartbeat timer callback captures `this` but does NOT acquire gate holder. `broadcast_heartbeat()` acquires its own gate internally, but the `_transport` pointer deref happens before the gate hold. Contrast with retry timer (line 212) and discovery timer (line 237) which DO acquire gate holders. | Acquire `_timer_gate.hold()` at the start of the heartbeat callback lambda, before calling `broadcast_heartbeat()`. |
| A2 | **CRITICAL** | `gossip_protocol.cpp:237-254` | #5 | Discovery timer gate holder has **wrong lifetime**: `timer_holder` is a local variable that goes out of scope when the callback lambda returns -- but `refresh_peers()` is a detached future `(void)`. The gate holder does NOT outlive the async work. | Move the gate holder into the future chain: `(void)refresh_peers().finally([holder = std::move(timer_holder)] {});` |
| A3 | **CRITICAL** | `async_persistence.cpp:371-380` | #5 | `log_stats()` gate holder is scoped to the `try` block and **drops before member access**. After the `try` block, the function reads `_ops_processed`, `_ops_dropped`, `_batches_flushed`, and `queue_depth()` without gate protection. | Declare `timer_holder` at function scope and assign inside the `try`, matching the pattern in `on_flush_timer()` (line 261). |
| A4 | **CRITICAL** | `http_controller.cpp:1819-1827` | #1 | `is_persistence_backpressured()` calls `_persistence->queue_depth()` which acquires `std::mutex` -- on **every proxy request** (called at line 701). The comment acknowledges this violation but labels it "acceptable". Under high request rate with concurrent persistence writes, this mutex stalls the reactor. | Replace with `std::atomic<size_t>` shadow counter in `AsyncPersistenceManager`, updated alongside the queue. The comment says "future optimization" but this is a "fix now" for a production traffic controller. |
| A5 | **HIGH** | `gossip_transport.cpp:126-127` | #5 | DTLS session cleanup timer callback captures `this`. `stop()` closes the gate (line 51) then cancels timers (line 62-63). A timer that fires between gate close and cancel gets no holder (`hold()` throws), but `cleanup_dtls_sessions` dereferences `this` before trying to acquire the gate. | Cancel timers before closing the gate, or ensure gate close and timer cancel happen atomically relative to destruction. |
| A6 | **HIGH** | `k8s_discovery_service.cpp:910-912` | #5 | `watch_endpoints()` self-chains recursively (`_watch_future = watch_endpoints()`) without holding a gate guard. The `watch_endpoints()` function body does NOT hold `_gate.hold()`. If `stop()` calls `_gate.close()` between loop iterations, the watch body can execute during teardown. | Hold `_gate.hold()` at entry of `watch_endpoints()`. Replace recursive self-chaining with a while-loop inside a single coroutine. |
| A7 | **HIGH** | `router_service.cpp:1705` | #5 | `start_batch_flush_timer()` callback captures `this` but does NOT acquire gate holder. The gate acquisition happens inside `flush_route_batch()` (line 1746), but `_pending_remote_routes.empty()` is accessed at line 1752 before the gate hold. | Move gate acquisition to the timer callback itself. |
| A8 | **MEDIUM** | `application.cpp:841` | -- | Shutdown future chain discarded with `(void)`. The `.handle_exception_type` only catches `timed_out_error` -- any other exception type (e.g., `std::bad_alloc`) is unobserved. | Add a trailing `.handle_exception()` to catch non-timeout exceptions. |
| A9 | **MEDIUM** | `async_persistence.cpp:406-413` | #1 | `load_backends()`, `load_routes()`, `backend_count()`, and `route_count()` directly call through to `SqlitePersistence` which acquires `std::mutex` and does blocking I/O. Comments say "for startup only" but nothing enforces this at the type level. | Add runtime assertion that these are called before reactor starts, or rename to `blocking_load_backends()` to signal the blocking nature. |
| A10 | **MEDIUM** | `http_controller.hpp:251,259-264` | -- | 7 `std::atomic` members in `HttpController` on data that is shard-local only. The context doc says "Never use `std::mutex` or atomics". In Seastar's single-threaded-per-shard model, these are pure overhead. | Replace with plain `uint64_t`/`bool` for shard-local counters. |

### 2. Edge-Case Crashes

| # | Severity | File:Line | Rule | Issue | Recommendation |
|---|----------|-----------|------|-------|----------------|
| E1 | **CRITICAL** | `radix_tree.hpp:238-243` | -- | `insert()` increments `route_count_++` **unconditionally**, but `insert_recursive` at line 1259-1263 silently updates existing leaves without signaling "not new". Repeated inserts of the same prefix cause `route_count_` to drift above the actual number of routes. This triggers **cascading eviction**: the `while (route_count() >= max_routes)` loop in `router_service.cpp:1471` evicts all real routes, then the counter is still inflated, and subsequent inserts enter the eviction loop finding nothing to evict. Eventually ALL routes are evicted and the tree is permanently useless. | Have `insert_recursive` return a `std::pair<NodePtr, bool>` where the bool indicates whether a new route was added. Only increment `route_count_` for new routes. |
| E2 | **HIGH** | `k8s_discovery_service.cpp:200` | -- | `co_await stream.read_exactly(4096)` truncates K8s service account tokens longer than 4KB. Projected tokens with audience bounds can exceed 4KB. The service silently proceeds with a broken token. | Read file size first (like `load_ca_cert` at line 227), then `read_exactly(size)`. |
| E3 | **HIGH** | `k8s_discovery_service.cpp:42-51` | -- | `to_backend_id()` uses a weak `hash * 31 + c` hash truncated to 31 bits. Birthday problem: ~50% collision probability at ~46K UIDs. Two different endpoints with the same BackendId shadow each other with no detection. | Use a stronger hash (first 4 bytes of SHA-256, or `absl::Hash`) with collision detection at registration time. |
| E4 | **HIGH** | `tracing_service.cpp:437-456` | -- | Shutdown race: `g_tracer.reset()` at line 456 races with span construction at line 267 which reads `g_tracer` after checking `g_shutting_down`. TOCTOU between `g_enabled.exchange(false)` and `g_tracer.reset()`. | Do not call `g_tracer.reset()` or `g_provider.reset()`. Let them leak at process exit. |
| E5 | **HIGH** | `router_service.cpp:365-371` | -- | `BackendRequestGuard::~BackendRequestGuard()` performs non-atomic check-then-act on `std::atomic<uint64_t>`. If a backend is removed and re-registered between guard creation and destruction, the decrement is silently lost from the old entry. | Document that backend removal with in-flight requests causes counter drift (bounded, converges to 0), or use `compare_exchange_weak`. |
| E6 | **MEDIUM** | `k8s_discovery_service.cpp:521-522` | -- | HTTP status parsing uses `headers.find("200 OK")` which matches if "200 " appears anywhere in any header value. Also only accepts 200, not 201/204. | Parse HTTP status line properly: extract first line, split on space, parse numeric code. |
| E7 | **MEDIUM** | `k8s_discovery_service.cpp:844-846` | -- | On K8s watch "Status" event (e.g., 410 Gone), `_resource_version` is not reset. Reconnection reuses stale version, causing infinite reconnect loop. | Parse the Status event `code`. On 410, clear `_resource_version` and trigger `sync_endpoints()`. |
| E8 | **MEDIUM** | `router_service.cpp:1877-1878` | -- | `get_all_backend_states()` formats `socket_address` to string via `std::ostringstream` then manually parses back to extract IP/port. Fragile: depends on Seastar's `operator<<` format stability. | Use structured accessor methods instead of string round-tripping. |
| E9 | **LOW** | `config_schema.hpp:195` | -- | `std::gmtime()` returns pointer to static buffer, not thread-safe by C standard. Each Seastar shard is a thread, so concurrent calls from multiple shards race. | Use `gmtime_r()` (POSIX, thread-safe). |
| E10 | **LOW** | `config_schema.hpp:232-239` | -- | `secure_compare()` early-returns on length mismatch, leaking key length via timing. | Use `CRYPTO_memcmp()` from OpenSSL (already a dependency), or always iterate over max length. |
| E11 | **LOW** | `node_slab.cpp:43-54` | #9 | `SlabNodeDeleter` fallback path (slab is nullptr) does not log a warning. This canary for lifecycle ordering bugs is silent. | Add `warn` log when fallback path is taken. |

### 3. Architecture Drift

| # | Severity | File:Line | Rule | Issue | Recommendation |
|---|----------|-----------|------|-------|----------------|
| D1 | **CRITICAL** | `gossip_consensus.cpp:130-141` | #14 | `_route_prune_callback` (`std::function`) is captured by value and broadcast to every shard via `smp::submit_to`. If the lambda exceeds SBO (implementation-defined, typically 16-32 bytes), the heap allocation lives on shard 0 and is freed on shard N. This is Bug #3 from the anti-patterns: callback captures shard 0 state but runs on all shards. Appears **3 times** (lines 130, 180, 237). | Each shard should register its own local prune callback. Shard 0 should broadcast only the scalar `BackendId`. The code has a TODO acknowledging this. |
| D2 | **HIGH** | `gossip_transport.cpp:200-201,263,469` | #0 | Multiple uses of `std::make_shared` for data passed into `parallel_for_each` lambdas. Rule #0 is explicit: "Never use `std::shared_ptr` in Seastar code." | Use `seastar::do_with` or `seastar::lw_shared_ptr` for shard-local sharing. |
| D3 | **HIGH** | `tracing_service.cpp:139` | #0 | `static std::shared_ptr<sdk_trace::TracerProvider> g_provider` shared across shards. When last reference drops, destructor runs on wrong shard. | Use raw pointer or `unique_ptr` with explicit cleanup from shard 0 only. |
| D4 | **MEDIUM** | `application.cpp:1171` | #0 | `std::make_shared<RanvierConfig>` distributed to all shards via `invoke_on_all`. Atomic refcount overhead + wrong-shard deallocation. Risk mitigated by `RanvierConfig` being a plain data struct. | Use `seastar::foreign_ptr<std::unique_ptr<RanvierConfig>>` with per-shard copies, or `seastar::lw_shared_ptr`. |
| D5 | **MEDIUM** | `sqlite_persistence.cpp:325-329` | #7 | `load_routes()` validates `backend_id <= 0` -- business logic in persistence layer. Similarly, `load_backends()` at line 167 checks `record.ip.empty()`. | Move validation to the service layer. Persistence should return raw data. |
| D6 | **MEDIUM** | `metrics_service.hpp:518,530-534` | -- | When `MAX_TRACKED_BACKENDS` is hit, a `static thread_local BackendMetrics fallback_metrics` is returned. This shared mutable singleton accumulates histogram data from multiple backends into one incoherent object, leaking memory slowly. | Return a freshly default-constructed null-op `BackendMetrics`, or reject recording and just increment the overflow counter. |
| D7 | **LOW** | `sqlite_persistence.cpp:467-487` | #7 | `verify_integrity()` checks for orphaned routes -- cross-table consistency is business logic. The orphan count is computed but never exposed to the caller (dead code). | Move to service layer, or expose the orphan count, or remove the dead code. |

### 4. Scale & Leak

| # | Severity | File:Line | Rule | Issue | Recommendation |
|---|----------|-----------|------|-------|----------------|
| S1 | **HIGH** | `dtls_context.hpp:181` / `dtls_context.cpp:540` | #4 | `DtlsContext::_sessions` is an `unordered_map` with **no size bound**. `get_or_create_session()` creates a new SSL session for every unique peer address. A malicious actor spoofing source addresses can create thousands of sessions/sec. Cleanup only runs every 60s. Each session allocates OpenSSL SSL objects + BIO pairs. | Add `MAX_SESSIONS` constant. Reject new sessions when limit reached. Add overflow metric. |
| S2 | **HIGH** | `gossip_protocol.hpp:204-207` | #4 | `_peer_seq_counters` map has **NO bound**. `next_seq_num()` inserts unboundedly. Old entries never cleaned unless `cleanup_peer_state()` is called. Slow memory leak over cluster lifetime. | Bound to `MAX_DEDUP_PEERS`. Clean entries for peers no longer in peer table. |
| S3 | **HIGH** | `k8s_discovery_service.cpp:501-506` | #4 | Response accumulation in `k8s_get()` has **no size limit**. A compromised K8s API can send arbitrarily large response, exhausting shard 0 memory. | Add `MAX_RESPONSE_SIZE` (e.g., 16MB). Abort if exceeded. |
| S4 | **HIGH** | `k8s_discovery_service.cpp:1067-1084` | #4 | Watch stream `buffer` appends without bound. Data without newlines grows the buffer forever (Slowloris-style memory exhaustion). | Add `MAX_LINE_SIZE` (e.g., 1MB). Abort watch and reconnect if exceeded. |
| S5 | **HIGH** | `k8s_discovery_service.hpp:120` | #4 | `_endpoints` map has no MAX_SIZE. A misconfigured K8s selector matching all pods in a large cluster fills memory. | Add `MAX_ENDPOINTS` constant (e.g., 1000). |
| S6 | **HIGH** | `connection_pool.hpp:435` | #4 | `_pools` map limits per-host and total connections, but NOT the number of **unique backend addresses** (map keys). Thousands of unique addresses with 0 idle connections grow the map. | Add `MAX_BACKENDS = 1000`. Check `_pools.size()` before inserting. |
| S7 | **HIGH** | `dtls_context.cpp:433-438` | #12 | `get_file_mtime()` calls POSIX `stat()` -- blocking syscall on reactor. `SSL_CTX_use_certificate_file` at lines 403-410 does blocking file I/O for cert reload. | Offload cert reload to `seastar::async`. Use OpenSSL memory-based APIs (`SSL_CTX_use_certificate_ASN1`) after async file read. |
| S8 | **MEDIUM** | `request_rewriter.hpp:411-435` | #4 | `extract_prefix_boundaries()` processes a client JSON array with no size cap. Malicious client sends millions of entries -> OOM. | Add `MAX_PREFIX_BOUNDARIES` limit (e.g., 1000) early in the function. |
| S9 | **MEDIUM** | `cross_shard_request.hpp:326` | #4 | `CrossShardResult::headers` map has no MAX_SIZE. Backend with many headers causes memory pressure during cross-shard transfer. | Add `max_headers` limit to `CrossShardRequestLimits`. |
| S10 | **MEDIUM** | `stream_parser.cpp:126` | -- | `compact_if_needed()` calls `memmove` on up to ~500KB per `push()` call. While bounded by `max_accumulator_size`, the overhead is non-trivial on the reactor. | Consider ring buffer or chunked buffer design. |
| S11 | **LOW** | `config_schema.hpp:380` | #4 | `ClusterConfig::peers` and `AuthConfig::api_keys` vectors have no MAX_SIZE. Malicious YAML config can contain thousands of entries. | Cap peers at 256, api_keys at 100. |

---

## Silent Catch Violations (Rule #9 Systematic)

Multiple files violate Rule #9 (every catch block logs at warn level minimum):

| Severity | File:Line | Issue |
|----------|-----------|-------|
| HIGH | `connection_pool.hpp:69` | `.handle_exception([](auto) {})` -- silent swallow, zero logging |
| HIGH | `http_controller.cpp:1332-1333` | `catch (...) {}` -- empty catch, zero logging |
| MEDIUM | `http_controller.cpp:1285-1287,1379-1380,1384-1386,1393-1395` | `catch (...)` with `trace`-level logging (below `warn`) |
| MEDIUM | `gossip_service.cpp:224-226` | `catch (...) { throw; }` -- useless re-throw, no context logging |
| MEDIUM | `gossip_service.cpp:256-258` | `catch (...) { log_gossip.debug(...) }` -- debug, not warn |
| MEDIUM | `gossip_protocol.cpp:284-286,289-293` | `catch (...) { debug(...) }` -- debug, not warn |
| MEDIUM | `gossip_transport.cpp:42-44` | `catch (...) { throw; }` -- useless re-throw, no context |
| MEDIUM | `health_service.cpp:76` | `gate_closed_exception` caught at debug level |
| MEDIUM | `health_service.cpp:95-98` | `.handle_exception([](auto ep) { return false; })` -- exception discarded silently |

---

## Structural Fixes (for BACKLOG.md)

```markdown
## Adversarial Audit Backlog (2026-02-12)

### CRITICAL
- [ ] [CRITICAL] Fix: RadixTree::insert() unconditionally increments route_count_ on update -- causes cascading eviction (radix_tree.hpp:243)
- [ ] [CRITICAL] Fix: gossip_protocol.cpp discovery timer gate holder drops before async work completes (line 237-254)
- [ ] [CRITICAL] Fix: gossip_protocol.cpp heartbeat timer callback has no gate guard (line 201-203)
- [ ] [CRITICAL] Fix: async_persistence.cpp log_stats() gate holder drops at end of try block, not function scope (line 371-380)
- [ ] [CRITICAL] Fix: http_controller.cpp calls mutex-guarded queue_depth() on every proxy request (line 1819-1827)
- [ ] [CRITICAL] Fix: gossip_consensus.cpp broadcasts std::function across shards 3x -- cross-shard free + Bug #3 (lines 130,180,237)

### HIGH
- [ ] [HIGH] Fix: dtls_context _sessions map has no MAX_SIZE -- unbounded SSL session creation (dtls_context.cpp:540)
- [ ] [HIGH] Fix: k8s_discovery_service watch buffer has no MAX_LINE_SIZE -- Slowloris OOM (k8s_discovery_service.cpp:1067)
- [ ] [HIGH] Fix: k8s_discovery_service k8s_get() response has no MAX_RESPONSE_SIZE (k8s_discovery_service.cpp:501)
- [ ] [HIGH] Fix: k8s_discovery_service _endpoints map has no MAX_SIZE (k8s_discovery_service.hpp:120)
- [ ] [HIGH] Fix: k8s_discovery_service token read truncated at 4096 bytes (k8s_discovery_service.cpp:200)
- [ ] [HIGH] Fix: k8s_discovery_service to_backend_id() weak hash -- collision at ~46K UIDs (k8s_discovery_service.cpp:42)
- [ ] [HIGH] Fix: connection_pool _pools map has no MAX_BACKENDS (connection_pool.hpp:435)
- [ ] [HIGH] Fix: gossip_transport.cpp uses std::shared_ptr for shard-local data (lines 200,263,469)
- [ ] [HIGH] Fix: tracing_service.cpp uses std::shared_ptr for cross-shard provider (line 139)
- [ ] [HIGH] Fix: tracing_service.cpp shutdown race on g_tracer.reset() (line 437-456)
- [ ] [HIGH] Fix: gossip_protocol _peer_seq_counters map unbounded (gossip_protocol.hpp:204)
- [ ] [HIGH] Fix: dtls_context.cpp uses blocking stat() and SSL_CTX file I/O on reactor (line 433)
- [ ] [HIGH] Fix: gossip_transport.cpp DTLS cleanup timer gate/cancel ordering (line 126)
- [ ] [HIGH] Fix: k8s_discovery_service watch_endpoints() has no gate guard (line 910-912)
- [ ] [HIGH] Fix: router_service batch flush timer callback has no gate guard (line 1705)
- [ ] [HIGH] Fix: connection_pool.hpp silent exception swallow in close() (line 69)
- [ ] [HIGH] Fix: http_controller.cpp empty catch blocks in proxy cleanup (lines 1332-1333)

### MEDIUM
- [ ] [MEDIUM] Fix: application.cpp uses std::shared_ptr for config reload broadcast (line 1171)
- [ ] [MEDIUM] Fix: sqlite_persistence.cpp has business validation in persistence (line 325)
- [ ] [MEDIUM] Fix: metrics_service.hpp shared mutable fallback_metrics corrupts data (line 534)
- [ ] [MEDIUM] Fix: async_persistence.cpp read-through methods need startup-only enforcement (line 406)
- [ ] [MEDIUM] Fix: http_controller.hpp uses 7 std::atomic for shard-local data (lines 251,259-264)
- [ ] [MEDIUM] Fix: request_rewriter.hpp prefix_boundaries array no MAX_SIZE (line 411)
- [ ] [MEDIUM] Fix: cross_shard_request.hpp headers map no MAX_SIZE (line 326)
- [ ] [MEDIUM] Fix: k8s_discovery_service HTTP status parsing is brittle (line 521)
- [ ] [MEDIUM] Fix: k8s_discovery_service 410 Gone causes infinite reconnect loop (line 844)
- [ ] [MEDIUM] Fix: application.cpp shutdown future discards non-timeout exceptions (line 841)
- [ ] [MEDIUM] Fix: Promote all trace/debug catch blocks to warn level (Rule #9 systematic)
```

---

## Anti-Pattern Candidates

The following systemic issues should be formalized via `claude-pattern-extractor-prompt.md`:

### Candidate 1: "The Gate-Holder-Scoping Anti-Pattern"

**Pattern:** Acquiring a `seastar::gate::holder` in a `try` block or local scope, then launching async work that outlives the scope.

**Instances:**
- `async_persistence.cpp:371-380` -- gate holder in `try` block, member access after block ends
- `gossip_protocol.cpp:237-254` -- gate holder in callback, detached future outlives callback

**Proposed Rule:** "A gate holder must be scoped to the entire async operation it protects. If the operation is a detached future, move the holder into the future chain's `.finally()`. Never scope a gate holder to a `try` block when work continues after the block."

### Candidate 2: "The Unconditional-Counter-Increment Anti-Pattern"

**Pattern:** Incrementing a counter after an operation that may be a no-op (e.g., updating an existing entry rather than inserting a new one).

**Instance:** `radix_tree.hpp:243` -- `route_count_++` after `insert_recursive` which may update an existing leaf.

**Proposed Rule:** "Any counter that tracks the size of a data structure must only be incremented when the operation actually changes the size. Return a boolean or enum from mutation methods to indicate whether the operation was an insert vs. update."

### Candidate 3: "The Unbounded-K8s-Response Anti-Pattern"

**Pattern:** Accumulating HTTP response bodies or streaming data from external services without size limits.

**Instances:**
- `k8s_discovery_service.cpp:501-506` -- unbounded response
- `k8s_discovery_service.cpp:1067-1084` -- unbounded watch buffer

**Proposed Rule:** "All HTTP response body accumulation from external services must have a `MAX_RESPONSE_SIZE`. All line-oriented stream parsers must have a `MAX_LINE_SIZE`. Both must be enforced before the data reaches any JSON parser."

---

## Summary Statistics

| Category | CRITICAL | HIGH | MEDIUM | LOW | Total |
|----------|----------|------|--------|-----|-------|
| Async Integrity | 4 | 3 | 3 | 0 | 10 |
| Edge-Case Crash | 1 | 4 | 3 | 3 | 11 |
| Architecture Drift | 1 | 2 | 3 | 1 | 7 |
| Scale & Leak | 0 | 7 | 3 | 1 | 11 |
| Rule #9 (Systematic) | 0 | 2 | 7 | 0 | 9 |
| **Total** | **6** | **18** | **19** | **5** | **48** |

---

## Top 5 "Fix Before Shipping" Items

1. **RadixTree `route_count_` drift** (E1): This is a logic bomb. Under normal traffic with repeated prefix updates, the counter drifts high, triggering mass eviction. The routing table empties itself permanently. Straightforward fix: return a bool from `insert_recursive`.

2. **Discovery timer gate holder lifetime** (A2): The gate holder provides zero protection -- it dies when the callback returns, but the future runs for seconds. Any shutdown during `refresh_peers()` is a use-after-free.

3. **Cross-shard `std::function` broadcast** (D1): Three call sites broadcast a heap-allocating `std::function` to every shard. When the lambda exceeds SBO, this is a cross-shard free. The code has a TODO acknowledging the problem.

4. **Mutex on hot path** (A4): Every single proxy request calls `queue_depth()` through a `std::mutex`. Under high load, this serializes the entire shard on persistence queue access. Fix with an atomic shadow counter.

5. **Unbounded K8s containers** (S3, S4, S5): Three unbounded containers in the K8s discovery service are exploitable via a compromised or misconfigured API server. Each can independently cause OOM on shard 0.
