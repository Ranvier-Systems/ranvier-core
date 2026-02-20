# Request Lifecycle: Performance Analysis

Analysis of the top potential performance issues along the LLM inference request
path documented in [request-lifecycle.md](request-lifecycle.md).

Each finding references the lifecycle phase, source location, estimated severity,
and a recommended mitigation.

---

## 1. CRITICAL — `learn_route_global()` Broadcasts to All Shards on Every Cache Miss

**Phase:** 8 (Stream Response — route learning)
**File:** `src/router_service.cpp:1619-1750`

On every successful 2xx response where tokens meet the minimum length,
`learn_route_global()` performs:

1. Allocate `foreign_ptr<vector<int32_t>>` copy of the token vector
2. `smp::submit_to(0, ...)` — gossip broadcast to shard 0
3. `parallel_for_each(all shards)` — one `smp::submit_to` per shard, each
   allocating another `foreign_ptr` copy and inserting into the local ART

For an 8-core system with 128-token prefixes, that is:
- **9 cross-shard messages** per learned route (1 gossip + 8 shard inserts)
- **9 heap allocations** of ~512 bytes each (token vector copies)
- **9 `foreign_ptr` destructions** bouncing back to the originating shard

This runs inline in the streaming hot path (fire-and-forget, but the futures
are still scheduled on the reactor). Under high request volume with low ART
hit rates (cold start, new prompts), this creates an SMP message storm that
competes with real request processing.

**Mitigation:** Batch route learning. Accumulate learned routes in a shard-local
buffer and flush to all shards periodically (e.g., every 100ms or 64 routes),
reducing SMP messages from O(routes × shards) to O(batches × shards).
`flush_route_batch()` already exists for gossip-received routes — apply the
same pattern to locally-learned routes.

---

## 2. CRITICAL — Tokenization Fallback Blocks the Reactor for 5–13ms

**Phase:** 2 (Tokenization)
**File:** `src/tokenizer_service.cpp:352-374`

When both the thread pool queue is full and cross-shard dispatch is unavailable
(all remote shards busy), `encode_threaded_async()` falls back to
`tokenize_locally()`, which calls `_impl->Encode()` — a synchronous Rust FFI
call that blocks the reactor for 5–13ms.

During that time, **all** other requests on the same shard stall: no reads,
no writes, no timer callbacks. On a system handling 1000 req/s across 8 cores,
a 10ms stall on one shard delays ~12 requests.

The fallback is designed as a last resort, but under load spikes (thread pool
saturation + cross-shard backpressure), multiple shards can hit it
simultaneously.

**Mitigation:**
- Add a cap on local fallbacks (e.g., max 1 concurrent local tokenization per
  shard via a semaphore). When the cap is reached, return empty tokens and fall
  back to hash/random routing rather than blocking the reactor.
- Instrument `_cross_shard_local_fallbacks` as a Prometheus counter with alert
  threshold.

---

## 3. HIGH — `std::mutex` in Async Persistence Blocks the Reactor

**Phase:** 8 (Stream Response — persistence queue)
**File:** `src/async_persistence.cpp:202-213`

`try_enqueue()` acquires `std::lock_guard<std::mutex>` on every
`queue_save_route()` call. This runs on the reactor thread during the streaming
hot path (line 600 of `http_controller.cpp`).

```cpp
bool AsyncPersistenceManager::try_enqueue(PersistenceOp op) {
    std::lock_guard<std::mutex> lock(_queue_mutex);  // BLOCKS REACTOR
    if (_queue.size() >= _config.max_queue_depth) { ... }
    _queue.push_back(std::move(op));
    ...
}
```

The same mutex is contended by `extract_batch()` (called from the persistence
worker thread), creating cross-thread contention on the reactor. Even a brief
lock hold on the reactor violates Seastar's shared-nothing model (Hard Rule #1).

**Mitigation:** Replace the mutex-guarded `std::deque` with a lock-free
SPSC (single-producer, single-consumer) ring buffer. Each shard produces
exclusively; the persistence worker consumes. This eliminates all mutex
contention on the reactor thread. Alternatively, use a shard-local batch
buffer flushed via `seastar::alien::submit_to_reactor()` from the worker.

---

## 4. HIGH — Per-Request `std::string` Copies for Thread Pool Tokenization

**Phase:** 2 (Tokenization)
**File:** `src/tokenizer_service.cpp:325`

When tokenization is dispatched to the thread pool, a `std::string` copy of the
input text is captured in the continuation lambda for cache insertion:

```cpp
return std::move(*future_opt).then(
    [this, local_shard, text_copy = std::string(text)]  // HEAP ALLOC
    (ThreadPoolTokenizationResult pool_result) { ... });
```

For a typical 4KB request body, this is a guaranteed heap allocation on every
thread-pool-dispatched tokenization. With 80–90% cache hit rates for system
messages, this primarily affects user messages — but those are exactly the
requests that take the full 5–13ms tokenization path, adding allocation
pressure to an already-expensive operation.

**Mitigation:** Use the text hash as the cache key instead of the full string.
Compute the hash before dispatch, capture only the hash + tokens in the
continuation. The cache already uses hash-based lookup internally.

---

## 5. MEDIUM — `sstring` Accumulator in StreamParser Causes Repeated Reallocation

**Phase:** 8 (Stream Response)
**File:** `src/stream_parser.cpp:57`

The `StreamParser` accumulates response chunks into a `seastar::sstring`:

```cpp
_accum.append(chunk.get(), chunk.size());
```

`seastar::sstring` has a 15-byte small-string optimization, but once exceeded
(nearly every HTTP response), it heap-allocates. With chunked transfer encoding,
each chunk triggers an append that may reallocate the underlying buffer with a
growth factor. For a 100KB response arriving in 50 chunks of ~2KB, this can
cause 6–8 reallocations with O(n) copies each.

The compaction at line 114 adds another `memmove` when read position exceeds 50%
of the buffer, which is correct but doubles the copy work after each compaction
cycle.

**Mitigation:** Pre-size the accumulator based on `Content-Length` if present
in the response headers (many LLM backends provide it for non-streaming
responses). For streaming (chunked) responses, use a linked list of chunks
(`seastar::temporary_buffer` chain) instead of copying into a contiguous buffer,
parsing directly from the chain.

---

## 6. MEDIUM — Connection Retry Backoff Sleeps on the Reactor

**Phase:** 6 (Connection Establishment)
**File:** `src/http_controller.cpp:391`

When a backend connection fails and retries are needed:

```cpp
co_await seastar::sleep(ctx.current_backoff);
```

The backoff starts at 100ms and grows to 5s. While `seastar::sleep` is
non-blocking (timer-based), the **request** holds its concurrency semaphore unit
and gate holder for the entire retry duration. Under default config
(max_retries=3), a connection to a dead backend can hold resources for up to
100ms + 200ms + 400ms = 700ms before giving up.

With a concurrency limit of (e.g.) 128, just 128 requests hitting a dead
backend simultaneously exhaust the semaphore, causing all subsequent requests to
get 503 — even requests targeting healthy backends.

**Mitigation:**
- Release the semaphore unit before the retry sleep and re-acquire after.
  This prevents dead-backend retries from consuming concurrency slots.
- Alternatively, rely more aggressively on the circuit breaker: if
  `record_failure()` transitions to OPEN, skip remaining retries entirely
  and try fallback immediately.

---

## 7. MEDIUM — Route Learning Fires on Every Snooped 2xx (No Dedup)

**Phase:** 8 (Stream Response)
**File:** `src/http_controller.cpp:582-601`

Route learning is triggered on every successful response:

```cpp
if (_config.should_learn_routes() && ctx.tokens.size() >= _config.min_token_length) {
    (void)_router.learn_route_global(...);
    _persistence->queue_save_route(ctx.tokens, ctx.current_backend);
}
```

If 100 requests share the same system prompt (common in RAG workloads), the
same prefix→backend route is learned 100 times: 100 cross-shard broadcasts +
100 persistence enqueues. The ART `insert()` overwrites the existing entry
(idempotent), so 99 of those broadcasts produce no new state.

**Mitigation:** Check if the route already exists before broadcasting.
`RadixTree::lookup()` is O(k) and shard-local (no SMP message). Add a
short-circuit:

```cpp
if (tree->lookup(tokens).backend_id == backend) return;  // Already known
```

This converts O(requests × shards) SMP messages to O(unique_routes × shards).

---

## 8. MEDIUM — HTTP/1.1 Request Serialization via String Concatenation

**Phase:** 7 (Send Request)
**File:** `src/http_controller.cpp:425-437`

Backend request headers are built via `sstring` concatenation:

```cpp
sstring http_headers =
    "POST " + sstring(ctx.endpoint) + " HTTP/1.1\r\n"
    "Host: " + host_value + "\r\n"
    "Content-Type: application/json\r\n"
    ...
```

Each `+` operator allocates a new `sstring`, copies left and right operands,
and frees the old buffer. For 6 concatenation steps, that is 5 intermediate
allocations. This runs on every proxied request.

**Mitigation:** Use `fmt::format` or a pre-sized `sstring` with `reserve()`
and `append()` to build headers in a single allocation. Alternatively,
use `seastar::scattered_message` or `writev`-style I/O to avoid building
a contiguous header buffer at all.

---

## 9. LOW — JSON Parse in `extract_text_with_boundary_info()` on Every Request

**Phase:** 2–3 (Tokenization + Boundary Detection)
**File:** `src/request_rewriter.hpp` (called from `http_controller.cpp:833`)

Every non-RANDOM request parses the full JSON body via RapidJSON to extract
text for tokenization. For requests where the client provides
`prompt_token_ids`, the JSON parse for text extraction is wasted — the tokens
are already available.

**Mitigation:** Reorder the extraction logic: check for client-provided tokens
first (Path A). Only parse the JSON body if client tokens are absent (Path B).
The lifecycle doc shows this is already the intent, but the implementation
calls `extract_text_with_boundary_info()` before checking the extracted
boundaries, causing unnecessary work when client tokens are used but boundary
detection still needs system message text.

---

## 10. LOW — `lowres_clock::now()` Called Twice Per Chunk in Streaming Loop

**Phase:** 8 (Stream Response)
**File:** `src/http_controller.cpp:476-488`

```cpp
if (lowres_clock::now() >= ctx.request_deadline) { ... }       // Call 1
auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
    ctx.request_deadline - lowres_clock::now());                // Call 2
```

`lowres_clock` updates at ~100Hz (10ms resolution), so two calls 3 lines apart
return the same value. Minor, but the pattern repeats for every chunk in the
streaming loop (potentially hundreds of chunks per response).

**Mitigation:** Capture `auto now = lowres_clock::now()` once and reuse it.

---

## Summary

| # | Severity | Phase | Issue | Est. Impact |
|---|----------|-------|-------|-------------|
| 1 | CRITICAL | 8 | Per-route cross-shard broadcast storm | O(shards) SMP msgs/request |
| 2 | CRITICAL | 2 | Reactor-blocking tokenization fallback | 5–13ms stall per shard |
| 3 | HIGH | 8 | `std::mutex` in persistence enqueue | Reactor contention |
| 4 | HIGH | 2 | Heap alloc for tokenizer cache key | 4KB alloc/cache-miss |
| 5 | MEDIUM | 8 | sstring reallocation in StreamParser | 6–8 reallocs/response |
| 6 | MEDIUM | 6 | Retry sleep holds concurrency slot | Semaphore exhaustion risk |
| 7 | MEDIUM | 8 | No route-learning deduplication | 99% redundant broadcasts |
| 8 | MEDIUM | 7 | String concat for HTTP headers | 5 temp allocs/request |
| 9 | LOW | 2–3 | Unnecessary JSON parse with client tokens | ~500µs wasted |
| 10 | LOW | 8 | Duplicate `lowres_clock::now()` calls | Negligible |
