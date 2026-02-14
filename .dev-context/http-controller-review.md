# HTTP Controller Code Review

Deep-dive review of `http_controller.hpp` and `http_controller.cpp` (~2500 lines)
with improvement suggestions along correctness, maintainability, and performance
dimensions.

## 1. P2C Shard Selection Is Dead Code (Correctness — High)

**Location:** `src/http_controller.cpp:725-731`, `src/http_controller.cpp:1839-1864`

`select_target_shard()` uses the Power-of-Two-Choices algorithm to pick a
target shard, but the result is only logged — the request is always processed on
the local shard:

```cpp
uint32_t target_shard = select_target_shard();  // line 725
uint32_t local_shard = seastar::this_shard_id();

// Log cross-shard dispatch decision for observability
if (target_shard != local_shard && _lb_config.enabled) {
    log_proxy.debug("[{}] P2C selected shard {} (local: {})", ...);
}
// ... continues processing locally, never calls smp::submit_to(target_shard, ...)
```

There is no `smp::submit_to(target_shard, ...)` anywhere in
`http_controller.cpp`. The `_requests_cross_shard_dispatch` counter increments,
but no actual cross-shard dispatch occurs. This means the entire P2C load
balancing feature — `LoadBalancingSettings`, `ShardLoadBalancer` integration,
and the cross-shard dispatch metric — is dead code that gives the appearance of
load balancing without providing it.

**Impact:** Load imbalance across shards under skewed traffic patterns. The
feature appears configured and operational (metrics increment) but does nothing.
Operators may believe load balancing is active when it is not.

**Suggestion:** Either implement the actual `smp::submit_to(target_shard, ...)`
dispatch path (requires `cross_shard_request.hpp` / `foreign_ptr` for safe data
transfer per Hard Rule #14), or remove the dead infrastructure to avoid
misleading operators.

---

## 2. Unescaped User-Controlled Strings in JSON Error Responses (Correctness — Medium)

**Location:** `src/http_controller.cpp:181, 803, 1071, 1448`

Multiple error responses interpolate external strings directly into JSON without
escaping:

```cpp
// line 181 — auth info can contain user-influenced content
std::string error_msg = "{\"error\": \"Unauthorized - " + info + "\"}";

// line 803 — token_result.error comes from parsing user input
rep->write_body("json", "{\"error\": \"Invalid prompt_token_ids: " + token_result.error + "\"}");

// line 1071 — route_result.error_message
rep->write_body("json", "{\"error\": \"" + route_result.error_message + "!\"}");

// line 1448 — validation.error from user input validation
rep->write_body("json", "{\"error\": \"Input validation failed: " + validation.error + "\"}");
```

If any of these strings contain `"`, `\`, or control characters, the response
becomes malformed JSON. The codebase already has an `escape_json_string` lambda
at line 1738, but it is local to `handle_keys_reload` rather than being a shared
utility.

**Suggestion:** Extract `escape_json_string` into a utility function (e.g., in
`parse_utils.hpp`) and use it for all JSON string interpolation, or use
RapidJSON (already a dependency) for response construction.

---

## 3. `std::atomic` Counters Unnecessary in Shared-Nothing Architecture (Correctness — Medium)

**Location:** `src/http_controller.hpp:259-264`

```cpp
std::atomic<uint64_t> _requests_rejected_concurrency{0};
std::atomic<uint64_t> _requests_rejected_persistence{0};
std::atomic<uint64_t> _requests_local_dispatch{0};
std::atomic<uint64_t> _requests_cross_shard_dispatch{0};
```

In Seastar's shared-nothing model, each `HttpController` instance lives on
exactly one shard. These atomics are only ever incremented from that shard's
reactor thread via `++_requests_rejected_concurrency` (a non-atomic increment on
an atomic type, line 689). No cross-shard code reads them either. Using
`std::atomic` here adds unnecessary memory fence overhead on every increment and
contradicts the project's Hard Rule #0/#1 philosophy of avoiding synchronization
primitives.

The `_draining` flag (line 251) has a slightly different profile — it is
conceivably set from a different context, though in practice it is also
shard-local via `start_draining()`.

**Suggestion:** Replace with plain `uint64_t`. If metrics scraping needs to read
them cross-shard, use `smp::submit_to` to gather them.

---

## 4. Raw HTTP Request Construction With Hardcoded Host and No CRLF Sanitization (Maintainability — High)

**Location:** `src/http_controller.cpp:420-432`

```cpp
sstring http_req =
    "POST " + sstring(ctx.endpoint) + " HTTP/1.1\r\n"
    "Host: localhost\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: " + to_sstring(ctx.forwarded_body.size()) + "\r\n"
    "X-Request-ID: " + ctx.request_id + "\r\n";
if (!ctx.backend_traceparent.empty()) {
    http_req += "traceparent: " + ctx.backend_traceparent + "\r\n";
}
http_req += "Connection: keep-alive\r\n\r\n" + ctx.forwarded_body;
```

Several issues:

- **Host header is hardcoded** to `localhost` instead of using the actual
  backend address. While backends are typically co-located in GPU clusters, this
  breaks if backends run on different hosts or use virtual hosting.
- **No header value sanitization.** `ctx.request_id` and
  `ctx.backend_traceparent` originate from client headers and are injected into
  the outgoing request without CRLF injection checks. A crafted
  `X-Request-ID` containing `\r\n` could inject arbitrary headers.
- **Body appended to headers string.** The entire request body is concatenated
  into the header string. For large LLM payloads (multi-MB), this doubles memory
  usage temporarily.

**Suggestion:** Sanitize header values (strip `\r\n`), derive the Host header
from the target address, and consider writing headers and body as two separate
`write()` calls to avoid the double-copy.

---

## 5. Per-Chunk `flush()` in Streaming Loop (Performance — Medium)

**Location:** `src/http_controller.cpp:602-603`

```cpp
co_await client_out.write(res.data);
co_await client_out.flush();
```

Every chunk received from the backend triggers a separate `flush()` syscall to
the client. For streaming LLM responses that produce many small SSE events
(often 10-50 bytes per token), this means one `write` + one `flush` per token.

For SSE streaming, this is partially intentional (low latency to first byte),
but a more nuanced approach would batch flushes when multiple chunks are
available in the same reactor tick, or flush on a timer (e.g., every 10ms)
rather than per-chunk.

**Suggestion:** Consider using `output_stream::write()` without immediate
`flush()` and let Seastar's internal batching handle it, or add a configurable
flush strategy (immediate for SSE `[DONE]` events, batched otherwise).

---

## 6. Missing `stop()` Method for Orderly Shutdown (Maintainability — Medium)

**Location:** `src/http_controller.hpp:147` (class declaration)

`HttpController` is used as `seastar::sharded<HttpController>` (per
`application.hpp:178`), which means Seastar calls `.stop()` on each shard during
shutdown. The class has no `stop()` method, so it relies on the default (which
returns a ready future).

However:

- The `_rate_limiter` timer is started in `register_routes()` (line 278) and
  only stopped in `wait_for_drain()`.
- If the application teardown sequence calls `_controller.stop()` without first
  calling `wait_for_drain()` on every shard, the rate limiter timer could fire
  after the object is destroyed.
- Per Hard Rule #6, any metrics lambdas capturing `this` should be deregistered
  in `stop()`.

**Suggestion:** Add an explicit `seastar::future<> stop()` method that calls
`_rate_limiter.stop()`, closes `_request_gate`, and ensures orderly cleanup
regardless of whether `wait_for_drain()` was called.

---

## 7. Unbounded Recursive JSON Serialization in Admin Endpoints (Maintainability — Medium)

**Location:** `src/http_controller.cpp:1871-1986`

The tree dump handler recursively serializes the entire radix tree into a
`std::ostringstream`:

```cpp
static std::string dump_node_to_json(const RadixTree::DumpNode& node, int indent_level = 0) {
    std::ostringstream ss;
    // ... recursive calls for each child
    ss << dump_node_to_json(node.children[i].second, indent_level + 2);
```

For a production system with tens of thousands of routes, this could produce
multi-MB JSON responses with no upper bound. The recursive
`dump_node_to_json` also risks stack overflow for deeply nested trees.
Additionally, `handle_dump_cluster` and `handle_dump_backends` (lines 2005-2077)
have similar unbounded output construction.

**Suggestion:** Add a `max_depth` parameter to `dump_node_to_json` to bound
recursion. Consider adding a `?limit=N` query parameter to cap the number of
nodes returned. For large responses, consider streaming the JSON output rather
than building the entire string in memory.
