# Request Lifecycle: Crash Risk Assessment

Static-analysis audit of the main inference request path, mapped against the
phases documented in [request-lifecycle.md](../internals/request-lifecycle.md). This
assessment was produced without compiling or running the code; each finding
should be confirmed against the live source (file:line refs may drift) and
verified with a targeted test or sanitiser run before remediation.

> **Triage addendum (2026-05-08).** A second pass re-read the source around
> each HIGH and MED finding. Verdicts are summarised below; per-item
> dispositions are tracked in [BACKLOG.md §18](../../BACKLOG.md#18-request-lifecycle-crash-risk-audit-follow-ups-2026-05-08).
> The original findings are preserved verbatim for traceability.
>
> | Verdict | HIGHs | MEDs |
> |---------|-------|------|
> | CONFIRMED (fix) | H1, H2, H3, H5, H7, H9 | M5, M14 |
> | MITIGATED (close) | H4, H6, H8, H10 | M1, M2, M3, M4, M7, M8, M9, M10, M11, M12, M13, M15 |
> | HYPOTHETICAL (defensive only) | — | — |
> | INVESTIGATE FURTHER | — | — |
> | UPGRADED-BY-FUZZ (fixed) | — | M6 |
>
> **Fuzz update (2026-05-08):** the request-rewriter harness in
> `tests/fuzz/` triggered a stack overflow on adversarial deeply-nested
> JSON within ~10 minutes, contradicting the original triage of M6 as
> HYPOTHETICAL. RapidJSON's default recursive parser has no depth limit;
> Ranvier called it directly at nine sites in `request_rewriter.hpp`. M6
> is upgraded to CONFIRMED and fixed by passing `kParseIterativeFlag` to
> `Document::Parse(...)` at every call site (commit landed alongside this
> note). The iterative parser keeps state on the heap, can't
> stack-overflow regardless of nesting, and is bounded by the existing
> `max_request_body_bytes` cap. Reproducer artifact:
> `crash-9ebccad46252e559406b7dcff51ac746fb050996`.
>
> Two CONFIRMED items deserve a footnote: H1 and H7 are CONFIRMED by code
> shape (the dereference / arithmetic exists as flagged), but reachability
> in both cases is via "future caller refactor" or "operator
> misconfiguration" rather than network input. They are kept on the fix
> list because the cost of fixing each is trivial; do not treat them as
> security findings.
>
> Empirical follow-up: libFuzzer harnesses for the high-attack-surface
> boundaries land alongside this triage at `tests/fuzz/`. They cover
> H8/H10/M6/M11/M12 — anything that converts to "no crash after N hours"
> can be moved to MITIGATED with confidence.
>
> **Fuzz run results (2026-05-08, 30 min × harness, libFuzzer + ASan + UBSan, clang 17 on aarch64):**
>
> | Harness | Status | Execs | Notes |
> |---------|--------|-------|-------|
> | `radix_tree_fuzz` | clean | 4,959,251 | Seeded corpus (430 inputs). 1,175 new units. Validates H8, L9. |
> | `request_rewriter_fuzz` | clean (post-fix) | 5,552,208 | First long run crashed at ~564k execs (M6 upgrade); after fix, 30 min clean. 3,017 new units. Validates M6, L5. |
> | `stream_parser_fuzz` | not validated | 1 | Crashed in libFuzzer cleanup before any real input ran, due to Seastar's global `operator new`/`delete` override (Hard Rule #15) requiring an uninitialised reactor. Not a `StreamParser` bug. See `tests/fuzz/README.md`. H10, M11, M12 stay at static MITIGATED. |
>
> Findings moved to **MITIGATED-BY-FUZZ** by these runs: H8, L9, M6 (post-fix), L5.
>
> Findings staying at static **MITIGATED** (no fuzz validation possible
> until Seastar is rebuilt with `-DSeastar_USE_DEFAULT_ALLOCATOR=ON`):
> H10, M11, M12.
>
> **Investigation closure (2026-05-10):** M10 (fallback walker hard cap)
> resolved by code re-read. `HttpController::get_fallback_backend`
> (`src/http_controller.cpp:218-231`) iterates a value-copy returned from
> `_router.get_all_backend_ids()` and returns on the first allowed
> backend, so the walker is structurally bounded by the snapshot size
> taken at call time and cannot loop independently of the registry —
> the original "recommend dead backends in a loop" risk does not match
> the actual control flow. M10 → MITIGATED.
>
> **Investigation closure (2026-05-10):** P3 defensive sweep — M3, M9,
> M11 resolved as a single bundle (per BACKLOG §18 "skip if mitigation
> is more code than the latent risk").
>
> - **M3** (cross-shard `string_view` lifetime,
>   `src/tokenizer_service.cpp:213`) — pinned the invariant in code as a
>   block comment at the top of `TokenizerService::encode_cached_async`:
>   the caller's backing buffer must remain live until the synchronous
>   prelude copies into `text_copy` (cross-shard branch) and
>   `text_for_cache` (post-await), and no `co_await` may be inserted
>   before either copy. Signature change to `std::string&&` was
>   considered and rejected: the only call site
>   (`tokenizer_service.cpp:393`) forwards a `string_view` parameter of
>   its own coroutine and would have to be changed in lockstep, costing
>   more than the comment. M3 → MITIGATED.
> - **M9** (jump-hash `int64_t→int32_t` cast,
>   `src/router_service.cpp:1135`) — added a one-line cast-safety
>   comment citing `LocalDiscovery::MAX_KNOWN_BACKENDS` (= 64,
>   `local_discovery.hpp:81`), the hard upstream cap that bounds
>   `num_buckets` six orders of magnitude below the 2^31 ceiling. All
>   eight call sites pass `live_backends.size()` (or a local `n`
>   derived from it), and `live_backends` originates in the discovery
>   layer that enforces the cap, so the cast is provably lossless and
>   no runtime check / debug assert is warranted. M9 → MITIGATED.
> - **M11** (HTTP status snoop split-segment risk,
>   `src/stream_parser.cpp:152`) — re-read confirms this was already
>   mitigated by the `header_end = view.find("\r\n\r\n")` guard at
>   `parse_headers` (`stream_parser.cpp:141`): the function returns
>   0 / "need more data" when the four-byte header terminator is not
>   yet in the buffer, so by the time line 152 runs the *full* headers
>   (including the status line) are already accumulated. The "first
>   TCP segment splits inside `HTTP/1.1 20…`" shape cannot reach the
>   snoop. No code change. M11 → MITIGATED.
>
> **Investigation closure (2026-05-10):** M4 (`std::bad_alloc`
> propagation across `smp::submit_to`) resolved empirically by the
> diagnostic test at
> `tests/unit/cross_shard_exception_propagation_test.cpp`, which boots a
> 2-shard reactor and throws `std::bad_alloc` inside a lambda submitted
> via `seastar::smp::submit_to` (matching the cross-shard call shape in
> `TokenizerService::encode_cached_async`). The initiating shard's outer
> catch observed the exception with demangled typeid `std::bad_alloc`
> and `e.what() == "std::bad_alloc"` — propagated cleanly, not
> repackaged as `broken_promise` or any other type. The existing
> outer `catch (const std::exception&)` around `encode_threaded_async`
> in `HttpController::handle_proxy` (`src/http_controller.cpp:1340`)
> therefore catches `bad_alloc` correctly, so no in-lambda try/catch is
> required. M4 → MITIGATED.
>
> **Investigation closure (2026-05-10):** M7 (cross-shard
> `get_live_backends` race during reconfiguration) resolved by code
> re-read of all writers to the registry fields the reader touches.
> The reader (`ShardLocalState::get_live_backends`,
> `src/router_service.cpp:529-540`) is a synchronous member function
> with no `co_await` — it iterates `backend_ids`, then for each id
> probes `dead_backends.contains(id)`, `backends.find(id)`, and
> `it->second.is_draining`. Because Seastar's per-shard reactor runs
> one task to completion before scheduling the next, no other task on
> the same shard can interleave between those reads. Every production
> writer that mutates the same fields reaches the shard via
> `seastar::smp::parallel_for_each(... smp::submit_to(shard_id, ...))`,
> so the writer body executes as its own reactor task on the target
> shard and cannot preempt the synchronous reader:
>
> - `RouterService::register_backend_global`
>   (`src/router_service.cpp:3553-3586`) — `submit_to` lambda writes
>   `state.backends[id]` and conditionally `state.backend_ids.push_back(id)`.
> - `RouterService::unregister_backend_global`
>   (`src/router_service.cpp:3588-3616`) — `submit_to` lambda writes
>   `state.backends.erase`, `state.backend_ids.erase`, and
>   `state.dead_backends.erase`.
> - `RouterService::report_backend_health` broadcast
>   (`src/router_service.cpp:3712-3723`) — `submit_to` lambda writes
>   `state.dead_backends.insert/erase`.
> - `RouterService::drain_backend_global`
>   (`src/router_service.cpp:3764-3775`) — `submit_to` lambda writes
>   `it->second.is_draining = true` and `drain_start_time`.
> - `RouterService::handle_node_state_change`
>   (`src/router_service.cpp:3896-3926`) — `submit_to` lambda toggles
>   `it->second.is_draining` for both `DRAINING` and `ACTIVE`
>   transitions.
>
> The remaining direct mutators of these fields
> (`ShardLocalState::reset` at `src/router_service.cpp:478-513`, plus
> the `*_for_testing` helpers at `src/router_service.cpp:4135-4189`)
> are unit-test scaffolding only; they are not invoked from the
> request path or any cross-shard broadcast. Call sites of the reader
> (`get_backend_for_prefix` at `src/router_service.cpp:2220` and
> `get_backend_by_hash` at `src/router_service.cpp:2559`) consume the
> returned `std::vector<BackendId>` value-copy synchronously — once
> `get_live_backends()` returns, downstream indexing operates on a
> private copy that no writer can mutate. The "count observed > 0
> then drops to 0" shape that originally fed H6 cannot occur because
> the reader's empty-check and indexing both run against the same
> local snapshot. M7 → MITIGATED.

Scope: `POST /v1/chat/completions` happy path, Phases 1-9. Excludes gossip,
config loading, persistence internals, metrics scraping, and TLS / DTLS
contexts.

Findings are graded:

- **HIGH** — plausible crash, undefined behaviour, or use-after-free reachable
  from network input or normal shutdown.
- **MED** — narrow race, edge-case overflow, or correctness regression that
  could manifest as a crash under adversarial / saturated conditions.
- **LOW** — defensive cleanup, theoretical-only, or already-mitigated.

---

## Executive summary

| Phase | HIGH | MED | LOW |
|-------|------|-----|-----|
| 1. Ingress / admission | 2 | 2 | 3 |
| 2. Tokenization | 3 | 4 | 4 |
| 3-5. Routing / circuit breaker | 3 | 4 | 3 |
| 6-9. Connect / send / stream / cleanup | 2 | 5 | 2 |
| **Total** | **10** | **15** | **12** |

The most pressing themes:

1. **Lifetime of `this` and captured state across coroutine suspensions**
   during shutdown — multiple lambdas in `http_controller.cpp` and the
   tokenizer thread pool capture references that can outlive their owners if
   the gate / alien instance race is lost.
2. **Integer overflow in size / backoff / weight arithmetic** — Content-Length
   parsing, exponential backoff doubling, weighted random selection, and chunk
   trailer math all do unbounded multiplications or `uint64_t→size_t` casts
   without explicit saturation.
3. **Empty-container preconditions split from indexing** — `live_backends`,
   ART node arrays, and per-shard backend maps are checked for emptiness, then
   indexed many lines later, allowing the second access to crash if state has
   changed (or, on a fresh code path, was never re-checked).

---

## Phase 1: Ingress & Admission

Code under audit: `src/http_controller.cpp` (`register_routes`,
`handle_proxy`, the rate-limited handler wrapper, `ActiveRequestGuard`),
`src/rate_limiter.hpp`, `src/rate_limiter_core.hpp`.

### H1. Optional dereference in concurrency-slot acquisition — HIGH
- **Where:** `src/http_controller.cpp:1986` — `semaphore_units = std::move(*slot.units);`
- **Risk:** `slot.units` is `std::optional<seastar::semaphore_units<>>`. The
  dereference relies on the caller having checked `slot.rejected`, but a
  future caller path that returns `rejected=false, units=nullopt` (e.g. an
  early-exit success branch) crashes here.
- **Fix:** `assert(slot.units.has_value())` and use `slot.units.value()` to get
  a clear abort instead of a UB read; better, return a `variant`.

### H2. Content-Length truncation on uint64→size_t cast — HIGH
- **Where:** `src/http_controller.cpp:2742` (parse) and the size-limit check
  upstream (around line 1157).
- **Risk:** A `Content-Length` of e.g. `0x1_0000_0000` parses as a valid
  `uint64_t`, then is cast to `size_t`. On any host where this loses bits — or
  even on 64-bit hosts where the value passes the configured
  `max_request_body_bytes` only after truncation — the size limit is
  effectively bypassed. Subsequent buffer math is then driven by attacker
  input.
- **Fix:** Compare the parsed `uint64_t` against
  `max_request_body_bytes` *before* the cast; reject (`413`) on overflow.

### M1. `[this]` capture across shutdown — MED
- **Where:** `src/http_controller.cpp:1986` (streaming lambda),
  `register_routes()` (~303-308).
- **Risk:** Both the rate-limiter wrapper and the streaming lambda capture
  `[this]`. The gate holder + `_draining` block new requests, but coroutines
  already suspended in `co_await establish_backend_connection()` resume after
  the controller is being torn down if shutdown order ever changes.
- **Fix:** Capture a `seastar::shared_ptr<HttpController>` (or document the
  invariant explicitly and assert `_request_gate` waits cover all suspended
  coroutines — in particular, ensure the gate is closed *and awaited* before
  destructor runs).

### M2. Inconsistent optional pattern around `try_get_units` — MED
- **Where:** `src/http_controller.cpp:1119` — `early_units = std::move(*sem_check);`
- **Risk:** Currently guarded; flagged because it's the same pattern as H1 and
  is one refactor away from a crash. Worth normalising.

### L1. Rate-limiter fail-open at MAX_BUCKETS — LOW
- **Where:** `src/rate_limiter_core.hpp:73-76`. Documented as intentional
  (Hard Rule #4). Cleanup on a 60 s timer keeps growth bounded; no crash.

### L2. Active-request counter has both manual and RAII paths — LOW
- **Where:** `src/http_controller.cpp:1994, 2014, 2041, 2136, 2216`.
- **Risk:** Mixed manual `decrement_active_requests()` and `release()` on
  the guard. Currently correct (`release()` neutralises the guard's dtor) but
  brittle to refactor. Consolidate on RAII.

### L3. `req->_headers` accessed without null check — LOW
- Seastar always initialises this map; not exploitable in practice.

---

## Phase 2: Tokenization

Code: `src/tokenizer_service.{cpp,hpp}`,
`src/tokenizer_thread_pool.{cpp,hpp}`, `src/request_rewriter.hpp`,
`src/text_validator.hpp`.

### H3. Reactor-blocking FFI fallback with no timeout — HIGH
- **Where:** `src/http_controller.cpp:1318` calls
  `encode_threaded_async()` without `with_timeout(...)`. Path 4 ("local FFI
  last resort") runs the Rust BPE call on the reactor; nothing prevents an
  arbitrarily long stall from blocking the entire shard.
- **Fix:** Wrap with `with_timeout(deadline, ...)`. Make the local-FFI path
  emit a metric so the operator can see when the threaded path is failing
  through.

### H4. Promise lifecycle race in `TokenizerThreadPool` — HIGH
- **Where:** `src/tokenizer_thread_pool.cpp:310-349`, around the
  `_pending_jobs` map and the `alien::run_on` callback.
- **Risk:** If `stop()` clears `_pending_jobs` while a worker is mid-flight,
  the completion callback can run after the pending entry was destroyed,
  leaving `set_value()` to operate on a moved-from / freed promise.
- **Fix:** Hold the promise in a `shared_ptr` owned by the worker job, not in
  a map keyed by job id; or quiesce workers before clearing the map.

### H5. Alien instance captured by reference — HIGH
- **Where:** `src/tokenizer_thread_pool.cpp:66, 164` — worker lambda captures
  `alien_instance` by reference, then calls `alien::run_on(alien_instance, …)`
  on completion.
- **Risk:** If the `app_template` (and therefore the alien) is destroyed
  before the worker thread joins, the call dereferences a dangling reference.
  Proper shutdown order is critical and currently implicit.
- **Fix:** Either join workers before alien teardown (assert this in the
  destructor) or capture by value if the alien is movable / reference-counted.

### M3. Cross-shard `string_view` lifetime — MED
- **Where:** `src/tokenizer_service.cpp:243-312`.
- **Risk:** `encode_cached_async` takes a `string_view` and copies it into
  `text_copy` synchronously, but the function signature invites a
  `co_await` before the copy in future refactors. Document as
  "must-copy-immediately" or take `std::string&&`.

### M4. Unhandled `std::bad_alloc` for large prompts — MED
- **Where:** `src/tokenizer_service.cpp:249, 299-300, 306, 352, 390`.
- **Risk:** Multi-megabyte prompts can throw `bad_alloc` on string copy or
  vector growth. The cross-shard lambda at lines 254-291 has no `try/catch`;
  the outer caller in `http_controller.cpp:1340` does, but the exception
  type that crosses `smp::submit_to` may be repacked into a `broken_promise`
  on some failure modes.
- **Fix:** Either catch inside the submitted lambda and return
  `{{}, false}`, or set an explicit body-size cap before the tokenizer is
  invoked.

### M5. Thread-pool future has no timeout — MED
- **Where:** `src/tokenizer_thread_pool.cpp` `submit_async`, called from
  `tokenizer_service.cpp:344`. A wedged worker leaves the future pending
  forever. Pair this with H3 — both should be fixed by a single
  `with_timeout` policy.

### M6. RapidJSON nesting limit not surfaced — MED
- **Where:** `src/request_rewriter.hpp:342`.
- **Risk:** Deeply nested JSON returns a generic parse error, but downstream
  logging and metrics treat it the same as malformed UTF-8. Not a crash, but
  amplifies tokenizer cost on adversarial input. Consider an explicit nesting
  cap in `kParseFlags`.

### L4-L7. Cache iterator return, eviction-loop bound, vocab-id sign check,
RapidJSON exception path — LOW. All gated by per-shard ownership or are
defensive cases. See the underlying audit notes for details; recommend
adding `assert(max_token_id >= 0)` at config load to short-circuit any
signed-cast surprises.

---

## Phase 3-5: Routing & Circuit Breaker

Code: `src/router_service.{cpp,hpp}`, `src/radix_tree.hpp`,
`src/circuit_breaker.hpp`, `src/http_controller.cpp::get_fallback_backend`,
`src/backend_registry.hpp`.

### H6. Empty-vector indexing after early checks — HIGH
- **Where:** `src/router_service.cpp:2219, 2547`.
- **Risk:** `get_backend_for_prefix` and `get_backend_by_hash` short-circuit
  on `live_backends.empty()`, but a later branch with `prefix_len == 0`
  reaches `live_backends[0]` directly. If a future caller invokes the
  zero-token path before the empty check, this is an immediate crash.
- **Fix:** Move the empty check to immediately precede every indexing site,
  or wrap the vector access in a helper that returns `std::optional`.

### H7. Weighted random selection — total weight overflow — HIGH
- **Where:** `src/router_service.cpp:2144-2152`.
- **Risk:** `total_weight` is a `uint64_t` accumulated from per-backend
  weights without a saturation check. A configuration with very large
  weights (or a misconfiguration that copies an unsanitised value) can wrap
  to 0, then `uniform_int_distribution(0, total_weight - 1)` is undefined.
- **Fix:** Saturate (`if (total_weight > kMaxWeight) total_weight = kMaxWeight;`)
  and assert `> 0` before constructing the distribution.

### H8. ART `split_long_prefix` off-by-one — HIGH
- **Where:** `src/radix_tree.hpp:1098-1121`.
- **Risk:** When a prefix exceeds `MAX_PREFIX_LENGTH`, the split path reads
  `node->prefix[MAX_PREFIX_LENGTH]` — one past the buffer's valid range — to
  pick the edge byte. Adversarial keys aligned exactly on this boundary
  produce an out-of-bounds read; with ASLR / poisoned heap this is a fault.
  *Confirm against current source* — node prefix may be stored in a buffer
  one byte larger than `MAX_PREFIX_LENGTH` for exactly this reason; if so,
  downgrade to LOW with a comment.
- **Fix:** Index `prefix[MAX_PREFIX_LENGTH - 1]` for the edge or store the
  edge byte in a separate field.

### M7. `get_live_backends` race — MED
- **Where:** `src/router_service.cpp:529-540`, called from 2200, 2539.
- **Risk:** Two un-atomic reads (`backend_ids` vs `backends.find`). Within a
  shard this is sequential, so the only race is across `smp::invoke_on_all`
  reconfigurations. Unlikely to crash by itself, but feeds H6 if the count is
  observed > 0 then drops to 0.

### M8. Modulo-by-zero in MODULAR hash — MED
- **Where:** `src/router_service.cpp:2308-2309`. Same root cause as M7 —
  the empty check is separated from the modulo. Add a re-check immediately
  before the operation.

### M9. Jump-hash cast `int64_t → int32_t` — MED
- **Where:** `src/router_service.cpp:1145`. Truncation only matters with
  >2^31 buckets, but the unconditional cast hides the assumption.

### M10. Fail-open + zero failure_threshold — MED
- **Where:** `src/circuit_breaker.hpp:78-80` and
  `http_controller.cpp::get_fallback_backend` (around 217-230).
- **Risk:** `MAX_CIRCUITS` overflow makes `allow_request()` return `true`
  unconditionally; combined with a misconfigured `failure_threshold = 0`, the
  fallback walker will recommend dead backends in a loop. Bound the fallback
  retries (the doc says "up to 3" — verify in code).

### L8-L10. Use-after-free in LRU split (per-shard, not racy in practice),
recursive subspan logic, fallback iteration limit — LOW. The fallback limit
in particular is documented as "up to 3 attempts" in the lifecycle doc;
double-check the code enforces this hard cap.

---

## Phase 6-9: Connect / Send / Stream / Cleanup

Code: `src/connection_pool.hpp`, `src/http_controller.cpp`
(`establish_backend_connection`, `send_backend_request`,
`stream_backend_response`, `record_proxy_completion_metrics`),
`src/stream_parser.cpp`.

### H9. Backoff multiplier overflow — HIGH
- **Where:** `src/http_controller.cpp:755-757`.
- **Risk:** `current_backoff.count() * backoff_multiplier` is computed as a
  `double`-to-`int64_t` cast with no upper bound prior to `std::min` against
  the cap. After enough doublings (or a misconfigured multiplier > 2.0), the
  `int64_t` wraps negative; `seastar::sleep(negative duration)` is undefined.
- **Fix:** Compute the next backoff in `double`, clamp to `max_backoff_ms`
  *before* the cast: `int64_t next = static_cast<int64_t>(std::min(next_ms,
  max_backoff_ms));`.

### H10. Chunk trailer length overflow in `StreamParser` — HIGH
- **Where:** `src/stream_parser.cpp:328-345`.
- **Risk:** `size_t needed = _chunk_bytes_needed + 2;` has no overflow check.
  Although `max_chunk_size` is supposed to cap `_chunk_bytes_needed`, the
  invariant is enforced earlier; if an out-of-band path sets the field
  directly the addition can wrap. The signed cast at line 345 compounds the
  problem.
- **Fix:** Assert `_chunk_bytes_needed <= max_chunk_size` at the top of
  `parse_chunk_data`; reject the response otherwise.

### M11. HTTP status snoop assumes one-shot first chunk — MED
- **Where:** `src/stream_parser.cpp:152-157`.
- **Risk:** The substring search for `" 200 "` requires the first read to
  contain the whole status line. A backend that flushes after `HTTP/1.1` (12
  bytes split) will silently miss the 2xx; circuit breaker never records
  success, route learning is skipped, TTFB metric is wrong. Not a crash but a
  silent observability hole.
- **Fix:** Buffer until the first CRLF before classifying the status.

### M12. Content-Length parsed without upper bound — MED
- **Where:** `src/stream_parser.cpp:225-230, 249, 353`. Parses to `size_t`;
  later compares for accumulator size. A `Content-Length: SIZE_MAX` causes
  the comparator at 353 to underflow. Mirror H2's fix on the response path.

### M13. `bundle.is_valid` is plain `bool` — MED
- **Where:** `src/http_controller.cpp:2191-2200` (return-to-pool decision),
  set to false at 776, 831, 857, 912, 921, 933, 948.
- **Risk:** Within a single coroutine the read/write is sequenced, but if any
  fire-and-forget callback (e.g. route learning) reaches a path that touches
  the bundle after the parent decided to pool it, the pool gets a dead
  connection. Promote to `std::atomic<bool>` or, better, transition the
  validity check to a single point.

### M14. Fire-and-forget route learning captures `[this]` — MED
- **Where:** `src/http_controller.cpp:991-999`.
- **Risk:** `tokens` and `backend` captured by value; `this` is captured raw.
  If shutdown closes the gate between launch and `.then()` execution, the
  callback dereferences a dangling controller. Pair with M1's fix.

### M15. Non-EPIPE write exceptions rethrown after `break` — MED
- **Where:** `src/http_controller.cpp:1021-1037`.
- **Risk:** OOM during flush is rethrown after `stream_closed` is set,
  but the outer coroutine's exception path may not expect arbitrary
  exception types here. Verify by mapping the `throw` site to the
  enclosing coroutine's handler.

### L11. Histogram negative-duration risk — LOW
- **Where:** `src/http_controller.cpp:649-661`. Seastar uses
  `steady_clock`; negative durations should be impossible, but a guard
  costs nothing.

### L12. Connection-pool TOCTOU on idle vs. half-open — LOW
- Per-shard ownership makes this benign today; flag if any pool method
  starts being called from `smp::submit_to`.

---

## Cross-cutting recommendations

1. **`with_timeout` policy.** The reactor-blocking FFI path (H3), the thread
   pool future (M5), and several `co_await` sites lack timeouts.  Adopt a
   single helper (`co_await with_timeout(ctx->deadline, ...)`) and apply
   uniformly along the request path.
2. **Saturate before cast.** Adopt a `safe_cast<size_t>(uint64_t)` and a
   `saturating_mul<int64_t>(double)` and grep for every `static_cast<size_t>`
   / `static_cast<int64_t>` in the hot path. H2, H9, H10, M9, M12 all share
   the same pattern.
3. **Empty-collection helper.** Replace `vec[0]` accesses with
   `front_or_nullopt(vec)` or an early-return helper in the routing layer to
   close H6 / M7 / M8 by construction.
4. **Lifetime contract for shutdown.** Document — and assert in destructors —
   that (a) the request gate is *awaited* before `HttpController` destruction,
   (b) `TokenizerThreadPool` workers are joined before the alien instance
   goes away, and (c) all fire-and-forget futures spawned during a request
   are tracked by the gate. M1, M14, H4, and H5 all collapse to this single
   invariant.
5. **Fuzzing surface.** The most attractive crash candidates are reachable
   from the request body and backend response: Content-Length (H2, M12),
   chunk framing (H10), JSON nesting (M6), and ART key boundaries (H8).
   Targeted libFuzzer harnesses on `RequestRewriter::extract_*`,
   `StreamParser::push`, and `RadixTree::insert/lookup` would shake out most
   of these without needing a full integration environment.

---

## Audit method & limitations

- This assessment was produced via static reading of the named source
  files using parallel exploration agents. No build, test, or sanitiser
  output was consulted.
- Line numbers come from the audit run; some may have drifted by a few lines
  by the time of remediation. Confirm against `git blame` before opening
  fixes.
- A handful of findings (notably H8 and the connection-pool TOCTOU items)
  could be already-mitigated by an invariant the reviewer didn't see — they
  are listed as HIGH/MED so remediation gets a re-read rather than because
  exploitation is certain.
- The doc walks the *happy path*. Slow paths (cross-shard route learning
  retries, gossip-driven backend churn, persistence backpressure escalation)
  may have additional crash risks not enumerated here.
