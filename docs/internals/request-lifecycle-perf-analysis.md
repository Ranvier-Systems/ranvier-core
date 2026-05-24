# Request Lifecycle: Performance Analysis

Analysis of the top potential performance issues along the LLM inference request
path documented in [request-lifecycle.md](request-lifecycle.md).

Each finding references the lifecycle phase, source location, estimated severity,
and a recommended mitigation. Findings that have since been mitigated in `src/`
are marked **RESOLVED** with a pointer to the current implementation.

---

## 1. RESOLVED — `learn_route_global()` Per-Request Cross-Shard Broadcast Storm

**Phase:** 8 (Stream Response — route learning)
**File:** `src/router_service.cpp:2616-2690`

Originally, every successful 2xx with sufficient tokens triggered an immediate
`smp::submit_to(0, ...)` gossip broadcast plus a `parallel_for_each` over all
shards (each with its own `foreign_ptr<vector<int32_t>>` allocation). On an
8-core box this cost ~9 cross-shard messages and ~9 heap allocations per
learned route, contending with real request work in the streaming hot path.

**Current implementation:** `learn_route_global()` no longer broadcasts inline.
It validates the prefix, performs a shard-local ART dedup check (see Finding
#7), and then calls `buffer_local_route()` to append the route into a
shard-local `pending_local_routes` vector (`src/router_service.cpp:253`). A
per-shard periodic timer (`_batch_flush_timer`, armed in
`start_batch_flush_timer()` at `src/router_service.cpp:2883`) and a
buffer-full trigger drain the buffer via `flush_local_route_batch()`,
broadcasting in batches rather than per-request. SMP traffic drops from
O(routes × shards) to O(batches × shards).

---

## 2. RESOLVED — Tokenization Fallback Blocking the Reactor

**Phase:** 2 (Tokenization)
**File:** `src/tokenizer_service.cpp:370-381`

Originally, when the thread pool queue was full and cross-shard dispatch was
unavailable, `encode_threaded_async()` fell through to `tokenize_locally()`,
making a synchronous Rust FFI `Encode()` call on the reactor (5–13ms stall).
Multiple shards could hit this simultaneously under load spikes.

**Current implementation:** Local fallback is now gated by a per-shard
semaphore `_local_tokenize_sem` (`src/tokenizer_service.hpp:341`). The fast
path is:

```cpp
if (!_local_tokenize_sem.try_wait(1)) {
    ++_local_fallback_rejected;
    co_return TokenizationResult{};   // empty -> caller falls back to hash/random routing
}
auto sem_guard = seastar::defer([this] { _local_tokenize_sem.signal(1); });
co_return tokenize_locally(text);
```

The semaphore capacity is configured via `configure_local_fallback()`
(`src/tokenizer_service.cpp:158`). Both `_cross_shard_local_fallbacks` and
`_local_fallback_rejected` are exposed as Prometheus counters
(`local_fallbacks` and `local_fallback_rejected`, registered at
`src/tokenizer_service.cpp:168-171`).

---

## 3. RESOLVED — `std::mutex` in Async Persistence Enqueue

**Phase:** 8 (Stream Response — persistence queue)
**File:** `src/async_persistence.cpp:287-305`

Originally, `try_enqueue()` acquired `std::lock_guard<std::mutex>` on every
`queue_save_route()` call from the reactor, contending with the persistence
worker's `extract_batch()` and violating Hard Rule #1.

**Current implementation:** The mutex-guarded `std::deque` has been replaced
with an MPSC ring buffer (`_ring`) plus an atomic `_queue_size` reservation
counter:

```cpp
const auto prev = _queue_size.fetch_add(1, std::memory_order_relaxed);
if (prev >= _config.max_queue_depth) {
    _queue_size.fetch_sub(1, std::memory_order_relaxed);
    _ops_dropped++;
    return false;
}
if (!_ring->try_push(std::move(op))) { ... }
```

The reactor never blocks. `extract_batch()` (`src/async_persistence.cpp:307`)
and `drain_queue()` are single-consumer drains called only from the worker
thread.

---

## 4. HIGH — Per-Request `std::string` Copy for Tokenizer Cache Insert

**Phase:** 2 (Tokenization)
**File:** `src/tokenizer_service.cpp:351-355`

After a thread-pool tokenization completes, the result is inserted into the
local cache keyed by a heap-allocated copy of the input text:

```cpp
std::string text_copy(text);
if (!result.tokens.empty()) {
    _cache.insert(text_copy, result.tokens);
}
```

For a typical 4KB request body, this is a guaranteed allocation per
thread-pool-dispatched tokenization. With 80–90% cache hit rates for system
messages, this primarily affects user messages — but those are exactly the
requests that already paid the full 5–13ms tokenization cost, adding
allocation pressure on top of an already-expensive operation. The same
pattern recurs in `encode_cached_async()` at `src/tokenizer_service.cpp:306`.

**Mitigation:** Use the text hash as the cache key instead of the full string.
Compute the hash once before dispatch and pass only the hash + tokens through
the continuation. The cache already uses hash-based lookup internally, so the
external string key is redundant.

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

The compaction at `src/stream_parser.cpp:118` adds another `memmove` when read
position exceeds the configured threshold, which is correct but doubles the
copy work after each compaction cycle. (Note: `sstring` does not expose
`reserve()`, so up-front sizing is not currently available — see comment at
`src/stream_parser.cpp:59`.)

**Mitigation:** Pre-size the accumulator based on `Content-Length` if present
in the response headers (many LLM backends provide it for non-streaming
responses). For streaming (chunked) responses, use a linked list of chunks
(`seastar::temporary_buffer` chain) instead of copying into a contiguous buffer,
parsing directly from the chain.

---

## 6. MEDIUM — Connection Retry Backoff Sleeps on the Reactor

**Phase:** 6 (Connection Establishment)
**File:** `src/http_controller.cpp:746-757`

When a backend connection fails and retries are needed:

```cpp
co_await seastar::sleep(ctx->current_backoff);
```

The backoff starts at 100ms and grows to 5s. While `seastar::sleep` is
non-blocking (timer-based), the **request** holds its concurrency semaphore
unit and gate holder for the entire retry duration. With a concurrency limit
of (e.g.) 128, just 128 requests hitting a dead backend simultaneously
exhaust the semaphore.

**Partial mitigation already in place:** `establish_backend_connection()`
short-circuits the retry loop when the circuit breaker for the current
backend has transitioned to `OPEN`, breaking out before the next sleep
(`src/http_controller.cpp:706-713`):

```cpp
if (_circuit_breaker.get_state(ctx->current_backend) == CircuitState::OPEN) {
    ++_retries_skipped_circuit_open;
    metrics().record_retry_skipped_circuit_open();
    ctx->connection_failed = true;
    break;
}
```

This eliminates the worst case (sustained retries against a confirmed-dead
backend), but the first failure still pays the initial backoff sleep before
the breaker opens, and a half-open breaker can still incur retries.

**Remaining mitigation:** Release the semaphore unit before the retry sleep
and re-acquire after, so retry-bound requests do not hold concurrency slots
that healthy traffic could use.

---

## 7. RESOLVED — Route Learning Fired on Every Snooped 2xx (No Dedup)

**Phase:** 8 (Stream Response)
**File:** `src/router_service.cpp:2659-2675`, `src/http_controller.cpp:969-1001`

Originally, route learning fired on every successful response, so 100 requests
sharing the same system prompt produced 100 cross-shard broadcasts and 100
persistence enqueues even though the ART entry was idempotent.

**Current implementation:** `learn_route_global()` performs a shard-local ART
dedup check before buffering:

```cpp
RadixTree* tree = state.tree.get();
if (tree) {
    std::span<const TokenId> token_span(tokens.data(), tokens.size());
    auto existing = tree->lookup(token_span);
    if (existing.has_value() && existing.value() == backend) {
        state.stats.routes_deduplicated_pre_buffer++;
        return seastar::make_ready_future<bool>(false);
    }
}
```

The lookup is O(k), shard-local, and returns `false` when the route is a
duplicate. The single-depth call site in `http_controller.cpp` propagates this
return value and only enqueues persistence when the route is genuinely new:

```cpp
(void)_router.learn_route_global(...)
    .then([this, tokens = ctx->tokens, backend = learn_backend](bool is_new_route) {
        if (is_new_route && _persistence) {
            _persistence->queue_save_route(tokens, backend);
        }
    })
```

For multi-depth routing, dedup is handled per-boundary inside
`learn_route_global_multi()` and `push_local_route()`.

---

## 8. MEDIUM — HTTP/1.1 Request Serialization via String Concatenation

**Phase:** 7 (Send Request)
**File:** `src/http_controller.cpp:786-814`

Backend request headers are built via `sstring` concatenation:

```cpp
sstring http_headers =
    "POST " + sstring(ctx->endpoint) + " HTTP/1.1\r\n"
    "Host: " + host_value + "\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: " + to_sstring(ctx->forwarded_body.size()) + "\r\n"
    "X-Request-ID: " + safe_request_id + "\r\n";
// followed by conditional traceparent, X-Ranvier-Prefix-Hash,
// and a trailing "Connection: keep-alive\r\n\r\n" via +=.
```

Each `+` allocates a new `sstring`, copies left and right operands, and
frees the old buffer. The base block alone produces 5 intermediate
allocations on every proxied request, with additional `+=` appends for
optional headers. The body is already written separately via
`bundle->out.write(ctx->forwarded_body)` (`src/http_controller.cpp:823`),
so the issue is contained to the header buffer, but it still runs on every
request.

**Mitigation:** Use `fmt::format` or a pre-sized `sstring` with `reserve()`
and `append()` to build headers in a single allocation. Alternatively,
use `seastar::scattered_message` or `writev`-style I/O to avoid building
a contiguous header buffer at all.

---

## 9. RESOLVED — Unconditional JSON Parse for Text Extraction

**Phase:** 2–3 (Tokenization + Boundary Detection)
**File:** `src/http_controller.cpp:1233-1271`

Originally, `extract_text_with_boundary_info()` ran on every non-RANDOM
request, parsing the full JSON body via RapidJSON even when the client had
already supplied `prompt_token_ids`.

**Current implementation:** The tokenization phase now checks for client
tokens first and only parses the body when local tokenization is required:

```cpp
// First, check if client provided pre-tokenized prompt_token_ids
if (_config.accept_client_tokens) {
    auto token_result = RequestRewriter::extract_prompt_token_ids(body_view, _config.max_token_id);
    if (token_result.found) { ... tokens = std::move(token_result.tokens); used_client_tokens = true; }
}

// If no client tokens, tokenize locally
if (!used_client_tokens) {
    text_extraction = RequestRewriter::extract_text_with_boundary_info(
        body_view, _config.enable_multi_depth_routing, _config.chat_template);
    ...
}
```

When the client provides tokens, the JSON body is no longer re-parsed for
text extraction. Boundary info is still computed in the same single
`extract_text_with_boundary_info()` call when local tokenization runs, so the
boundary-detection phase no longer re-parses either.

---

## 10. LOW — `lowres_clock::now()` Called Multiple Times Per Streaming Iteration

**Phase:** 8 (Stream Response)
**File:** `src/http_controller.cpp:853-866`

```cpp
if (lowres_clock::now() >= ctx->request_deadline) { ... }                 // Call 1
auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
    ctx->request_deadline - lowres_clock::now());                          // Call 2
auto read_deadline = lowres_clock::now() + read_timeout;                   // Call 3
```

`lowres_clock` updates at ~10ms resolution, so all three calls within a few
lines return the same value. Minor, but the pattern repeats for every read
iteration of the streaming loop (potentially hundreds of chunks per response).

**Mitigation:** Capture `auto now = lowres_clock::now()` once at the top of the
loop iteration and reuse it.

---

## Summary

| #  | Status   | Phase | Issue                                              | Notes                                  |
|----|----------|-------|----------------------------------------------------|----------------------------------------|
| 1  | RESOLVED | 8     | Per-route cross-shard broadcast storm              | Batched via `pending_local_routes` + flush timer |
| 2  | RESOLVED | 2     | Reactor-blocking tokenization fallback             | Gated by `_local_tokenize_sem`         |
| 3  | RESOLVED | 8     | `std::mutex` in persistence enqueue                | Replaced with MPSC ring buffer         |
| 4  | HIGH     | 2     | Heap alloc for tokenizer cache key                 | ~4KB alloc per cache-miss              |
| 5  | MEDIUM   | 8     | sstring reallocation in StreamParser               | 6–8 reallocs per response              |
| 6  | MEDIUM   | 6     | Retry sleep holds concurrency slot                 | Circuit-breaker short-circuit added; semaphore release still pending |
| 7  | RESOLVED | 8     | No route-learning deduplication                    | ART dedup check in `learn_route_global` |
| 8  | MEDIUM   | 7     | String concat for HTTP headers                     | 5+ temp allocs per request             |
| 9  | RESOLVED | 2–3   | Unnecessary JSON parse with client tokens          | Client-token check now precedes parse  |
| 10 | LOW      | 8     | Duplicate `lowres_clock::now()` calls              | 3 calls per streaming iteration        |
