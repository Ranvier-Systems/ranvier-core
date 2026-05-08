# Request Lifecycle: Code Path Reference

Developer reference for tracing an LLM inference request through Ranvier Core.
For visual sequence diagrams, see [docs/request-flow.md](../request-flow.md).

---

## Overview

```
Client POST /v1/chat/completions
  │
  ├─ 1. Ingress         HttpController::handle_proxy()
  │     ├─ Rate limit    RateLimiter::allow()
  │     ├─ Drain check   _draining flag
  │     ├─ Gate guard     _request_gate.hold()
  │     ├─ Concurrency    _request_semaphore (try_get_units)
  │     └─ Persistence    is_persistence_backpressured()
  │
  ├─ 2. Tokenize         TokenizerService::encode_threaded_async()
  │     ├─ Cache hit      TokenizationCache::lookup()
  │     ├─ Thread pool    TokenizerThreadPool::submit()
  │     ├─ Cross-shard    smp::submit_to(P2C target)
  │     └─ Local FFI      TokenizerService::encode()
  │
  ├─ 3. Boundary         RequestRewriter::extract_text_with_boundary_info()
  │
  ├─ 4. Route            RouterService::route_request()
  │     ├─ ART lookup     RadixTree::lookup()
  │     ├─ Hash fallback  consistent hash on prefix
  │     └─ Random         get_random_backend()
  │
  ├─ 5. Circuit breaker  CircuitBreaker::allow_request()
  │     └─ Fallback       HttpController::get_fallback_backend()
  │
  ├─ 6. Connect          ConnectionPool::get()
  │     └─ Retry loop     establish_backend_connection()
  │
  ├─ 7. Send             HttpController::send_backend_request()
  │
  ├─ 8. Stream           HttpController::stream_backend_response()
  │     ├─ Snoop 2xx      CircuitBreaker::record_success()
  │     ├─ Learn route    RouterService::learn_route_global()
  │     └─ Persist        AsyncPersistenceManager::queue_save_route()
  │
  └─ 9. Cleanup          record_proxy_completion_metrics()
```

---

## Phase 1: Ingress

**Entry:** `HttpController::register_routes()` in `src/http_controller.cpp`

Routes are registered during startup:

```
POST /v1/chat/completions  → make_rate_limited_handler(→ handle_proxy)
POST /v1/completions       → make_rate_limited_handler(→ handle_proxy)
GET  /health               → handle_health (no auth)
```

Every data-plane request passes through a rate-limit wrapper before reaching
`handle_proxy`. The wrapper calls `RateLimiter::allow(client_ip)` and returns
503 with `Retry-After: 1` on rejection.

**`HttpController::handle_proxy()`** is the main coroutine. It runs the
following admission checks in order:

| Check | Mechanism | Rejection reason | Status |
|-------|-----------|------------------|--------|
| Draining | `_draining` bool | Server shutting down | 503 |
| Gate | `_request_gate.hold()` | Gate closed during shutdown | 503 |
| Concurrency | `try_get_units(_request_semaphore, 1)` | Too many concurrent requests | 503 |
| Persistence | `is_persistence_backpressured()` | SQLite write queue > 80% full | 503 |
| Tokenizer | `_tokenizer.local().is_loaded()` | Tokenizer not initialized | 200 (error body) |

All guards (gate holder, semaphore units, `ActiveRequestGuard`) are RAII and
release automatically on any exit path.

---

## Phase 2: Tokenization

**Skipped entirely in RANDOM routing mode** — saves ~5-6ms per request since
token vectors are unused. `MetricsService::record_tokenization_skipped()` is
recorded instead.

For PREFIX or HASH mode, tokens are obtained in priority order:

### Path A: Client-provided tokens

When `accept_client_tokens` is enabled, `handle_proxy` calls
`RequestRewriter::extract_prompt_token_ids()` to parse `prompt_token_ids` from
the JSON body. Tokens are validated against `max_token_id`. Invalid tokens
produce a 400 response.

### Path B: Server-side tokenization

1. **Extract text** from JSON body via
   `RequestRewriter::extract_text_with_boundary_info()`. This performs a single
   JSON parse that also pre-computes system message boundary metadata (reused
   in Phase 3).

2. **Validate** via `TextValidator::validate_for_tokenizer()`. Checks UTF-8
   validity (skipped if JSON parser already validated) and length limits. On
   failure, tokens are cleared and routing falls back to round-robin.

3. **Tokenize** via `TokenizerService::encode_threaded_async()`, which tries
   each strategy in priority order:

   | Priority | Strategy | Latency | Blocking? |
   |----------|----------|---------|-----------|
   | 1 | LRU cache hit | ~0 | No |
   | 2 | Thread pool worker | 5-13ms | No (reactor freed) |
   | 3 | Cross-shard P2C dispatch | 5-13ms + ~1-10us | No (reactor freed) |
   | 4 | Local FFI | 5-13ms | Yes (last resort) |

   System messages have 80-90% cache hit rates. The thread pool
   (`TokenizerThreadPool`) runs the Rust FFI call on a dedicated worker thread
   so the Seastar reactor is never blocked.

---

## Phase 3: Prefix Boundary Detection

Determines where the "shared prefix" ends for route storage. This enables
requests sharing the same system prompt but different user queries to route to
the same backend for KV-cache reuse.

Sources checked in priority order:

1. **Client-provided** `prefix_boundaries` array (multi-depth) or
   `prefix_token_count` (single depth) — extracted via
   `RequestRewriter::extract_prefix_boundaries()` /
   `RequestRewriter::extract_prefix_token_count()`.

2. **Automatic multi-depth** (when `enable_multi_depth_routing` is on):
   tokenize each message separately using `encode_cached()`, compute
   cumulative token boundaries at each message break.

3. **Automatic single-depth**: tokenize system messages as a block, use that
   token count as the boundary. Uses the pre-extracted
   `system_text` from the Phase 2 JSON parse to avoid re-parsing.

---

## Phase 4: Route Lookup

**Entry:** `RouterService::route_request()` in `src/router_service.cpp`

### Fail-open check

If gossip quorum is lost and `fail_open_on_quorum_loss` is enabled, bypass
normal routing entirely and use random selection. This prioritizes availability
over cache affinity during split-brain.

### PREFIX mode (default)

`RouterService::get_backend_for_prefix()`:

1. **ART lookup** — `RadixTree::lookup()` performs O(k) longest-prefix-match
   through adaptive Node4/16/48/256 nodes. If the matched backend is live
   (checked against `get_live_backends()`), return it with `art_hit=true`.

2. **Hash fallback** — if no ART match (or matched backend is dead/draining),
   compute a consistent hash over the first `prefix_len` tokens. The prefix
   length is either the `prefix_boundary` (from Phase 3) or
   `config.prefix_token_length` (default 128). Hash selects deterministically
   from live backends.

The `RouteResult` returned to `handle_proxy` contains `backend_id`,
`routing_mode` ("prefix"/"hash"/"random"), `cache_hit`, and `error_message`.

### HASH mode

`RouterService::get_backend_by_hash()` — consistent hash only, no ART, no
learning. Used to benchmark baseline hash performance.

### RANDOM mode

`RouterService::get_random_backend()` — weighted random selection within the
highest-priority backend group.

---

## Phase 5: Circuit Breaker

**`CircuitBreaker::allow_request()`** in `src/circuit_breaker.hpp`

Per-backend, shard-local, lock-free state machine:

```
          failure_count >= threshold
CLOSED ──────────────────────────────► OPEN
  ▲                                      │
  │ success_count >= threshold           │ recovery_timeout elapsed
  │                                      ▼
  └──────────────────────────────── HALF_OPEN
          failure in half-open ──────► OPEN
```

| State | Behavior |
|-------|----------|
| CLOSED | All requests allowed; failures increment counter |
| OPEN | All requests blocked; transitions to HALF_OPEN after `recovery_timeout` |
| HALF_OPEN | Probe requests allowed; success closes circuit, failure reopens |

If the circuit is OPEN, `handle_proxy` calls
`HttpController::get_fallback_backend()` which iterates all registered backends
looking for one whose circuit allows requests. Up to 3 fallback attempts.

If all circuits are open, returns 200 with error body
`"All backends unavailable (circuit breaker open)"`.

---

## Phase 6: Connection Establishment

**`HttpController::establish_backend_connection()`** in `src/http_controller.cpp`

### ConnectionPool::get()

`ConnectionPool::get()` in `src/connection_pool.hpp` tries to reuse an idle
connection (LIFO pop from per-backend deque):

- **Skip expired** connections (idle > `idle_timeout`)
- **Skip max-age** connections (created > `max_connection_age` ago)
- **Skip half-open** connections (backend sent FIN — zombie detection)
- If valid connection found: `touch()` timestamp and return

If no pooled connection, create a new TCP connection via
`seastar::connect()`.

### Retry and fallback loop

On connection failure, `establish_backend_connection()` follows this logic:

```
while retry_attempt <= max_retries:
    if deadline exceeded → break (connection_failed)
    try connect with timeout
    if success → break

    record circuit breaker failure

    if fallback enabled and fallback_attempts < 3:
        try get_fallback_backend(current_backend)
        if found → switch target, continue (no retry increment)

    if retries remaining:
        sleep(current_backoff)  // exponential: 100ms → 200ms → ... → 5s cap
        retry_attempt++
    else:
        connection_failed = true
```

---

## Phase 7: Send Request

**`HttpController::send_backend_request()`** in `src/http_controller.cpp`

Writes raw HTTP/1.1 to the backend output stream:

```
POST /v1/chat/completions HTTP/1.1
Host: <backend_addr>
Content-Type: application/json
Content-Length: <body_size>
X-Request-ID: <request_id>
traceparent: <W3C trace context>    (if tracing enabled)
Connection: keep-alive

<body>
```

Headers and body are written separately to avoid doubling memory for large
payloads. Header values are sanitized to prevent CRLF injection.

Connection errors trigger `CircuitBreaker::record_failure()` and set
`ctx.connection_error`.

---

## Phase 8: Stream Response

**`HttpController::stream_backend_response()`** in `src/http_controller.cpp`

Main read loop:

```
while true:
    check request_deadline → timeout if exceeded
    read chunk from backend (with per-chunk 30s timeout)
    if EOF → break
    parse via StreamParser::push()

    if first 2xx snooped:
        circuit_breaker.record_success()
        record TTFB metrics

        if should_learn_routes and token_count >= min_token_length:
            (fire-and-forget) router.learn_route_global(tokens, backend)
            (fire-and-forget) persistence.queue_save_route(tokens, backend)

    write data to client output stream
    flush on first write (TTFB) and on stream completion
    if client disconnected (EPIPE) → break
```

### Route learning

`RouterService::learn_route_global()` in `src/router_service.cpp`:

1. Truncate tokens to `prefix_boundary` or `prefix_token_length`
2. Insert into ART on all shards via `smp::invoke_on_all`
3. Broadcast to cluster peers via gossip (if enabled)

Route learning is fire-and-forget — failures are logged but never fail the
request.

### StreamParser

`StreamParser::push()` in `src/stream_parser.cpp` handles chunked transfer
encoding and SSE event extraction. It snoops the HTTP status line from the
first chunk to detect 2xx success for circuit breaker feedback.

---

## Phase 9: Cleanup

After streaming completes:

1. **Metrics** — `record_proxy_completion_metrics()` records backend total
   latency and end-to-end request latency in histograms.
2. **Connection** — if `bundle.is_valid`, return to pool via
   `ConnectionPool::put()`. Otherwise close.
3. **RAII destructors** release (in reverse capture order):
   - `BackendRequestGuard` — decrements per-backend active request counter
   - Semaphore units — frees concurrency slot
   - Gate holder — allows shutdown to proceed
4. **Shard metrics** — decrement active request count for P2C load balancing

---

## Error Response Summary

| Condition | HTTP Status | Body |
|-----------|-------------|------|
| Rate limited | 503 | `Retry-After: 1` |
| Draining / gate closed | 503 | `Server is shutting down` |
| Concurrency limit | 503 | `Too many concurrent requests` |
| Persistence backpressure | 503 | `Persistence queue full` |
| Tokenizer not loaded | 200 | `Tokenizer not loaded` |
| Invalid client tokens | 400 | `Invalid prompt_token_ids` |
| No backends registered | 200 | `No backends registered` |
| All circuits open | 200 | `All backends unavailable` |
| Backend IP not found | 200 | `Backend IP not found` |
| Connection failed | SSE error | `Backend connection failed after retries` |
| Request timeout | SSE error | `Request timed out` |
| Backend parse error | SSE error | `Backend response parsing error` |

---

## Key Files

| File | Role in request path |
|------|---------------------|
| `src/http_controller.cpp` | Ingress, proxy orchestration, streaming |
| `src/router_service.cpp` | Route lookup, learning, backend management |
| `src/radix_tree.hpp` | ART data structure (O(k) prefix lookup) |
| `src/tokenizer_service.cpp` | BPE tokenization with LRU cache |
| `src/tokenizer_thread_pool.cpp` | Non-blocking tokenization workers |
| `src/connection_pool.hpp` | TCP connection reuse (LRU + TTL) |
| `src/circuit_breaker.hpp` | Per-backend health state machine |
| `src/stream_parser.cpp` | Chunked encoding / SSE parsing |
| `src/request_rewriter.hpp` | JSON body parsing, token injection |
| `src/async_persistence.cpp` | Fire-and-forget SQLite batching |
| `src/metrics_service.hpp` | Prometheus metric recording |
