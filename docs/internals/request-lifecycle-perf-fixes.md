# Request Lifecycle Performance Fixes — Session Summaries

Each section below is a self-contained brief suitable for handing to a new
session to implement the fix. Sections are ordered by severity (CRITICAL first).

---

## Issue 1: Batch Locally-Learned Routes Instead of Per-Request Broadcast

**Severity:** CRITICAL
**Files:** `src/router_service.cpp`, `src/router_service.hpp`
**Symptom:** Every successful proxied request triggers `learn_route_global()`
which calls `smp::submit_to` to every shard plus shard 0 for gossip — that is
`O(shards + 1)` cross-shard messages, each allocating a `foreign_ptr<vector<int32_t>>`.
On an 8-core system this is 9 SMP messages and 9 heap allocations per request.
Under load this creates an SMP message storm that competes with actual request
processing.

**Existing pattern to reuse:** `flush_route_batch()` (line ~2037) already
implements batched cross-shard distribution for *remote* (gossip-received)
routes. It accumulates routes in `_pending_remote_routes` on shard 0 and
flushes periodically via `_batch_flush_timer` (see `RouteBatchConfig` in
`router_service.hpp`). The same pattern does not exist for locally-learned
routes.

**Task:**
1. Add a shard-local `_pending_local_routes` buffer (bounded, similar to
   `_pending_remote_routes` with `RouteBatchConfig::MAX_BUFFER_SIZE`).
2. Change `learn_route_global()` to push `{tokens, backend_id, prefix_boundary}`
   into the shard-local buffer instead of broadcasting immediately.
3. Add a shard-local flush timer (or piggyback on the existing timer if running
   on shard 0) that drains `_pending_local_routes` and does a single
   `parallel_for_each(all shards)` broadcast per batch.
4. Keep the gossip `submit_to(0)` in the flush (it needs shard 0).
5. Deduplicate within the batch before broadcasting (same prefix+backend → skip).
6. Preserve gate-holder safety (Rule #5) and foreign_ptr patterns (Rule #14).
7. Verify existing unit tests still pass; add a test for batch accumulation.

**Key constraint:** Each shard learns routes independently, so the buffer must
be shard-local (no cross-shard access). Only the flush broadcasts cross-shard.

---

## Issue 2: Cap Reactor-Blocking Tokenization Fallback

**Severity:** CRITICAL
**Files:** `src/tokenizer_service.cpp`, `src/tokenizer_service.hpp`
**Symptom:** When both the thread pool queue is full (Priority 1) and
cross-shard dispatch declines (Priority 2), `encode_threaded_async()` at line
352 falls back to `tokenize_locally()` — a synchronous Rust FFI call that
blocks the Seastar reactor for 5–13ms. All other requests on that shard stall.

**Current code (tokenizer_service.cpp:295-354):**
```
Priority 1: thread pool → submit_async()
Priority 2: cross-shard → encode_cached_async()
Priority 3: local (BLOCKS REACTOR) → tokenize_locally()
```

**Task:**
1. Add a shard-local `seastar::semaphore _local_tokenize_sem{1}` (or
   configurable, default 1) that gates Priority 3.
2. Before calling `tokenize_locally()`, attempt `try_wait(_local_tokenize_sem, 1)`.
   If it fails (another local tokenization is already in progress), return an
   empty `TokenizationResult` — the caller in `http_controller.cpp` will fall
   back to hash/random routing.
3. Add `_local_tokenize_sem.signal(1)` after `tokenize_locally()` returns
   (use RAII or scope guard for exception safety).
4. Add a Prometheus counter `ranvier_tokenizer_local_fallback_rejected` for
   when the semaphore is full. Expose `_cross_shard_local_fallbacks` as a
   counter if not already.
5. Document the config knob in `ranvier.yaml.example` if you make the
   semaphore size configurable.
6. Verify with existing tokenizer unit tests.

**Key constraint:** `tokenize_locally()` is synchronous (Rust FFI), so you
cannot use `seastar::get_units()` (async wait) — the caller must fail fast
with `try_wait()`.

---

## Issue 3: Replace `std::mutex` in Async Persistence with Lock-Free Queue

**Severity:** HIGH
**Files:** `src/async_persistence.cpp`, `src/async_persistence.hpp`
**Symptom:** `try_enqueue()` at line 202 acquires `std::lock_guard<std::mutex>`
on the Seastar reactor thread. The same mutex is contended by `extract_batch()`
(called from the persistence worker's `std::thread`). This violates Seastar's
shared-nothing model (Hard Rule #1 — no locks on the reactor).

**Current queue operations under `_queue_mutex`:**
- `try_enqueue()` — reactor thread, called from hot path
- `extract_batch()` — worker thread, periodic flush timer
- `drain_queue()` — worker thread, shutdown
- `queue_clear_all()` — reactor thread, admin endpoint

**Task:**
1. Replace `std::deque<PersistenceOp> _queue` + `std::mutex _queue_mutex` with
   a bounded SPSC (single-producer, single-consumer) ring buffer. The reactor
   thread is the sole producer; the persistence worker is the sole consumer.
   Options: implement a simple `std::array`-based ring buffer with atomic
   head/tail indices, or use a well-tested lock-free queue (e.g., from Folly
   or Boost.Lockfree if available).
2. `try_enqueue()` becomes a lock-free CAS on the tail index. Return false if
   full (existing drop behavior).
3. `extract_batch()` becomes a lock-free drain of up to `max_batch_size`
   elements from the head.
4. Handle `queue_clear_all()` carefully — this is an admin-only operation. It
   can set an atomic flag that the worker thread checks before processing.
5. Keep `_queue_size` atomic counter for metrics (already exists).
6. Remove `_queue_mutex` entirely.
7. Verify with existing persistence unit tests.

**Key constraint:** `PersistenceOp` is a `std::variant` of several op types.
The ring buffer must support move-only types. Pre-allocate the ring buffer to
`max_queue_depth` (config, default 10,000).

---

## Issue 4: Avoid Heap Copy of Text for Tokenizer Cache Insertion

**Severity:** HIGH
**Files:** `src/tokenizer_service.cpp`
**Symptom:** At line 325, the thread-pool tokenization path captures
`text_copy = std::string(text)` in the `.then()` continuation lambda solely
for cache insertion. This is a ~4KB heap allocation on every cache-miss
tokenization dispatched to the thread pool.

**Current code (tokenizer_service.cpp:324-339):**
```cpp
return std::move(*future_opt).then(
    [this, local_shard, text_copy = std::string(text)]  // <-- ALLOCATION
    (ThreadPoolTokenizationResult pool_result) {
        ...
        _cache.insert(text_copy, result.tokens);
        ...
    });
```

**Task:**
1. Examine the cache's `insert()` and `lookup()` signatures. If the cache uses
   hash-based lookup internally, compute the hash *before* dispatch (on the
   reactor, which already has the `string_view`) and pass only the hash + a
   pre-hashed key to the continuation.
2. If the cache API requires the full string for insertion (e.g., it stores
   the key), consider having the *thread pool worker* do the cache insertion
   (it already has a local copy of the text for FFI per Rule #15). The worker
   can insert into the cache before returning.
3. If approach 2 is used, ensure the thread pool worker's cache access is
   shard-safe (workers run on the same shard's thread pool, so shard-local
   cache is accessible).
4. Alternatively, move the text copy into the thread pool submission itself
   (it already copies for FFI safety) and return it alongside the tokens.
5. Verify with existing tokenizer tests.

**Key constraint:** Hard Rule #15 — FFI calls require locally-allocated text.
The thread pool already has a local copy. The goal is to avoid a *second* copy
in the continuation.

---

## Issue 5: Reduce `sstring` Reallocations in StreamParser

**Severity:** MEDIUM
**Files:** `src/stream_parser.cpp`, `src/stream_parser.hpp`
**Symptom:** `StreamParser::push()` at line 57 appends each network chunk to
`_accum` (a `seastar::sstring`). After exceeding the 15-byte SSO threshold
(virtually every response), each append may trigger a reallocation + copy.
A 100KB response in 50 chunks causes ~6–8 reallocations.

**Current code (stream_parser.cpp:57):**
```cpp
_accum.append(chunk.get(), chunk.size());
```

**Task:**
1. During `parse_headers()` (line 133), if a `Content-Length` header is present,
   extract it and pre-size `_accum` to at least that value. This eliminates all
   reallocations for non-streaming (non-chunked) responses.
2. For chunked responses (no Content-Length), pre-size `_accum` to
   `StreamParserConfig::initial_output_reserve` (4096) on first push to skip
   the sub-16-byte SSO range immediately.
3. Note: `seastar::sstring` does not have `reserve()`. You may need to switch
   `_accum` to `std::string` (which does have `reserve()`), or use a
   `seastar::temporary_buffer<char>` chain that avoids copying entirely.
4. If switching to a chain, the parse functions (`parse_headers`,
   `parse_chunk_size`, `parse_chunk_data`) need to work on the chain's
   `unread_view()` — likely a `std::string_view` over the current chunk.
5. Measure tradeoff: the chain avoids copies but complicates parsing when a
   token (e.g., `\r\n`) spans two chunks. The current contiguous buffer
   avoids this complexity.
6. Verify with existing stream parser unit tests.

**Key constraint:** `compact_if_needed()` at line 114 uses `memmove()` on the
contiguous buffer. If you keep the contiguous approach, pre-sizing is the
simplest win. If you switch to a chain, compaction is replaced by dropping
consumed chunks.

---

## Issue 6: Release Concurrency Semaphore During Connection Retry Backoff

**Severity:** MEDIUM
**Files:** `src/http_controller.cpp`
**Symptom:** At line 391, `co_await seastar::sleep(ctx.current_backoff)` pauses
the request during connection retry. The request holds a concurrency semaphore
unit (acquired at line 699 via `try_get_units(_request_semaphore, 1)`) for the
entire retry duration (up to 700ms with default config). With a concurrency
limit of 128, dead-backend retries can exhaust the semaphore, causing 503s for
requests targeting healthy backends.

**Current flow:**
```
handle_proxy():699 → try_get_units (acquire)
  → establish_backend_connection():391 → co_await sleep (holds unit)
  → ... rest of request processing ...
  → semaphore_units goes out of scope (release)
```

**Task:**
1. In `establish_backend_connection()`, release the semaphore unit before the
   retry sleep and re-acquire after waking. This requires passing the semaphore
   units into `establish_backend_connection()` or restructuring the retry loop.
2. Alternative approach: check the circuit breaker state before retrying. If
   `_circuit_breaker.state(ctx.current_backend) == CircuitState::OPEN` after
   `record_failure()`, skip remaining retries and fall back immediately. This
   short-circuits the retry loop without changing semaphore semantics.
3. Consider both approaches — the circuit breaker check is simpler and avoids
   the complexity of re-acquiring the semaphore (which could fail, requiring
   another 503 path).
4. Add a metric `ranvier_proxy_retry_semaphore_releases` to track how often
   this occurs.
5. Verify with existing proxy/connection tests.

**Key constraint:** Releasing the semaphore means another request could start
processing. The re-acquire must handle the case where the semaphore is now
full (return 503 rather than blocking).

---

## Issue 7: Deduplicate Route Learning Before Cross-Shard Broadcast

**Severity:** MEDIUM
**Files:** `src/router_service.cpp` (or `src/http_controller.cpp`)
**Symptom:** At `http_controller.cpp:582-601`, route learning fires on every
successful 2xx response. If 100 requests share the same system prompt (common
in RAG), the same prefix→backend route is broadcast 100 times. The ART
`insert()` is idempotent (overwrites), so 99 broadcasts produce no new state.

**Task:**
1. Before calling `learn_route_global()` (or in its implementation at
   `router_service.cpp:1619`), check if the route already exists with the same
   backend. `RadixTree::lookup()` is O(k) and shard-local.
2. Add a short-circuit at the top of `learn_route_global()`:
   ```cpp
   // Skip if route already points to this backend
   auto& tree = *shard_state().tree;
   auto existing = tree.lookup(tokens);
   if (existing.backend_id.has_value() && *existing.backend_id == backend) {
       return seastar::make_ready_future<>();  // Already known
   }
   ```
3. This must happen *after* the prefix truncation (line 1654) so the lookup
   uses the same effective prefix that would be inserted.
4. Also skip the `_persistence->queue_save_route()` call in
   `http_controller.cpp:600` when the route is already known — this avoids
   unnecessary persistence enqueues.
5. Add a counter `_routes_deduped` for observability.
6. Verify with existing router and route-learning tests.

**Key constraint:** The lookup must be on the *calling shard's* local ART
(shard-local, no SMP message). This is the same shard that would do the
insert, so the check is consistent. There is a benign TOCTOU race (another
shard could have evicted the route between check and insert), but redundant
inserts are safe — the goal is to eliminate the common case, not guarantee
zero duplication.

---

## Issue 8: Build Backend HTTP Headers Without Intermediate String Allocations

**Severity:** MEDIUM
**Files:** `src/http_controller.cpp`
**Symptom:** At lines 425–437, backend request headers are built via repeated
`sstring` concatenation (`+` operator), creating ~5 intermediate temporary
`sstring` allocations per proxied request.

**Current code:**
```cpp
sstring http_headers =
    "POST " + sstring(ctx.endpoint) + " HTTP/1.1\r\n"
    "Host: " + host_value + "\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: " + to_sstring(ctx.forwarded_body.size()) + "\r\n"
    "X-Request-ID: " + safe_request_id + "\r\n";
```

**Task:**
1. Replace with `fmt::format()` (Seastar includes fmt) or a single
   pre-reserved `sstring`/`std::string` with `append()` calls:
   ```cpp
   auto headers = fmt::format(
       "POST {} HTTP/1.1\r\nHost: {}\r\nContent-Type: application/json\r\n"
       "Content-Length: {}\r\nX-Request-ID: {}\r\n",
       ctx.endpoint, host_value, ctx.forwarded_body.size(), safe_request_id);
   ```
2. Handle the conditional `traceparent` header (line 433–435). Either include
   it in the format with a conditional, or append it separately.
3. Append `"Connection: keep-alive\r\n\r\n"` (line 437) in the same operation.
4. Ensure no behavior change — same header order, same values.
5. Verify with existing proxy tests.

**Key constraint:** `sstring` is `seastar::sstring`. `fmt::format` returns
`std::string` — you may need to convert to `sstring` for the `write()` call,
or change the write to accept `std::string_view`. Check what
`bundle.out.write()` accepts.

---

## Issue 9: Skip JSON Parse When Client Provides Tokens

**Severity:** LOW
**Files:** `src/http_controller.cpp`
**Symptom:** At line 833, `extract_text_with_boundary_info()` is called
unconditionally for all non-client-token requests. But at line 953–957, it is
called *again* if `text_extraction` was not populated (client-tokens path) and
prefix boundary detection is enabled. This means the client-tokens path still
does a JSON parse for boundary detection.

**Current code flow (http_controller.cpp:795-957):**
```
1. Check client tokens (line 799) → if found, set used_client_tokens=true
2. If !used_client_tokens → call extract_text_with_boundary_info (line 833)
3. ... tokenization ...
4. Boundary detection (line 953): if text_extraction not populated →
   call extract_text_with_boundary_info AGAIN (redundant if client tokens used
   but boundaries needed)
```

**Task:**
1. In the client-tokens path, if `_config.enable_prefix_boundary` is enabled,
   eagerly call `extract_text_with_boundary_info()` at the point where we know
   we'll need it (after determining `used_client_tokens=true`), and cache the
   result in `text_extraction`.
2. Alternatively, if boundaries are rarely used with client tokens, keep the
   lazy pattern but add a fast path: if the client also provides
   `prefix_token_count` (already checked at line 935), skip the JSON parse
   entirely.
3. The goal: when client provides both `prompt_token_ids` AND
   `prefix_token_count`, zero JSON parsing should occur.
4. Verify with existing proxy tests.

**Key constraint:** `extract_text_with_boundary_info()` costs ~500µs for a
typical 4KB JSON body. The savings are only meaningful at high request rates
with client-provided tokens — this is a low-severity optimization.

---

## Issue 10: Capture `lowres_clock::now()` Once Per Loop Iteration

**Severity:** LOW
**Files:** `src/http_controller.cpp`
**Symptom:** At lines 476 and 487 in `stream_backend_response()`,
`lowres_clock::now()` is called twice within 10 lines. The low-resolution clock
updates at ~100Hz (10ms granularity), so consecutive calls return the same
value. This repeats for every chunk in the streaming loop.

**Current code (http_controller.cpp:474-488):**
```cpp
while (true) {
    if (lowres_clock::now() >= ctx.request_deadline) { ... }       // Call 1
    auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
        ctx.request_deadline - lowres_clock::now());                // Call 2
```

**Task:**
1. Add `auto now = lowres_clock::now();` at the top of the while loop body
   (before line 476).
2. Replace both `lowres_clock::now()` calls (lines 476 and 487) with `now`.
3. No behavioral change — the values are identical at 100Hz resolution.
4. Verify with existing streaming tests.

**Key constraint:** Trivial change. Ensure `now` is used consistently for
both the deadline check and the remaining-time calculation.
