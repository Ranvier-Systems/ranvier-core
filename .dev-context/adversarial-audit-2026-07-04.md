# Adversarial System Audit — 2026-07-04 (Pass A)

**Persona:** cynical Staff Engineer / Security Auditor. Assume malicious traffic and 100x load.
**Scope:** the untrusted-input surfaces with no prior fuzz coverage and no prior adversarial pass —
`src/kv_event_decoder.hpp` (msgpack from vLLM's ZMQ KV-event stream),
`src/kv_event_subscriber.{hpp,cpp}` (the ZMQ worker that feeds it), and the GIE
Endpoint-Picker gRPC surface (`src/gie_epp_plan.hpp`, `src/gie_epp_server.{hpp,cpp}`).
This is the last blind spot from the original audit strategy: the gossip
deserializers were cleared by the 2026-07-04 holistic audit, and the HTTP body
path is already fuzzed. **Static analysis only** — nothing executed; the A1 UB
below was confirmed by reading the cast and its input provenance, and is
reproducible the moment `kv_event_decoder_fuzz` runs under UBSan.

Prior reports skimmed for regressions: `adversarial-audit-2026-01-14.md`,
`adversarial-audit-2026-02-12.md`, `holistic-audit-2026-07-04.md`.

## Criticality Score: 4/10

The decoder is genuinely well-built for a hand-rolled msgpack parser: every read
is bounds-checked against `end`, every container and the skip recursion is capped
(Rule #4), and the total byte budget bounds the skip tree so the large per-level
caps can't blow up (each `read_header` consumes ≥1 byte from a ≤4 MB payload). The
subscriber's threading, gate discipline, and cross-thread reallocation (Rule #15)
are correct. One real defect (A1: undefined behavior on an attacker-controlled
float64 timestamp), one scale gap on the build-gated EPP path (S1), and two
low-severity notes. Nothing is remotely-exploitable for RCE; A1 is a
UBSan-tripping cast that in a release build silently produces a wrong timestamp.

---

## Findings by Lens

### Edge-Case Crash

| # | Severity | File:Line | Rule | Issue | Recommendation |
|---|----------|-----------|------|-------|----------------|
| A1 | MEDIUM | `src/kv_event_decoder.hpp:248-249` | #10 | `out.ts_ms = ts.fval > 0.0 ? static_cast<uint64_t>(ts.fval * 1000.0) : 0;` — `ts.fval` is an attacker-controlled IEEE-754 double read raw from a msgpack `0xcb` (`read_header` memcpy at :141-146). For `+inf` or any finite value ≥ 2^64/1000, `ts.fval * 1000.0` overflows the `uint64_t` range and the float→unsigned cast is **undefined behavior** (C++ [conv.fpint]). Under the fuzz/sanitizer build (`-fsanitize=undefined`) this aborts; in a release build it yields a platform "indefinite" value (x86-64 `cvttsd2si` → `0x8000…`), corrupting `ts_ms` — which feeds the conflict-resolution timestamp comparisons in the ledger and `cache_event_timestamps`. `NaN` is already handled (`NaN > 0.0` is false → 0); `+inf` and huge finite values are not. | Guard the cast: `if (std::isfinite(ts.fval) && ts.fval > 0.0) { double ms = ts.fval * 1000.0; out.ts_ms = ms < 18446744073709549568.0 ? static_cast<uint64_t>(ms) : UINT64_MAX; } else out.ts_ms = 0;` — or reject the batch on a non-finite/out-of-range timestamp. Pinned by `kv_event_decoder_fuzz` (added this pass). |
| A2 | LOW | `src/kv_event_decoder.hpp:251` | — | The INT timestamp path `out.ts_ms = ts.uval * 1000;` silently wraps for `ts.uval > 2^64/1000` (unsigned overflow — defined, but a wrong value). Same downstream effect as A1's release-mode behavior (bad conflict-resolution timestamp), without the UB. Low because vLLM never emits such a timestamp; a hostile publisher could. | Cap: `out.ts_ms = ts.uval > UINT64_MAX/1000 ? UINT64_MAX : ts.uval * 1000;`. Fold into the A1 fix. |

### Scale & Leak

| # | Severity | File:Line | Rule | Issue | Recommendation |
|---|----------|-----------|------|-------|----------------|
| S1 | MEDIUM | `src/gie_epp_server.cpp:173-193, 70-119` | #1-adjacent | The EPP `Process` RPC blocks its gRPC handler thread on `fut.get()` (:185) awaiting shard 0, with **no concurrency cap and no deadline**. gRPC's sync API grows its handler-thread pool with concurrent RPCs; under 100x load (or a slow-loris opening many `Process` streams) this spawns unbounded threads, each blocked on a `submit_to` to shard 0 — flooding shard 0's task queue and exhausting threads. The inline HTTP ingress has a concurrency cap (see data-flow in claude-context.md); this alternate ingress has none. Also: if shard 0 is wedged, `fut.get()` blocks the handler forever (no timeout). **Mitigation for deployed risk: `WITH_GIE_EPP` is build-gated OFF by default.** | Add an ingress semaphore bounding in-flight `decide()` calls (reject over-cap with an ImmediateResponse 503, the existing no-endpoint path), and a bounded wait on the future (`fut.wait_for(deadline)` → 503 on timeout) so a stalled reactor sheds load instead of parking threads. |
| S2 | LOW | `src/kv_event_decoder.hpp:207-228` | #4 | `read_u64_array`/`read_token_array` `reserve(out.size() + h.uval)` after checking only `h.uval <= max_n`, before confirming the bytes exist. A tiny payload claiming a `kKvMaxTokensPerEvent` (256K) array forces a ~1 MB reserve before the element loop fails. Bounded (one reserve per decode before the batch aborts; ~8× amplification for a fully-packed 4 MB batch of 1-byte fixints → ~32 MB decoded, one batch at a time on the worker), so not a DoS — noted for the record. | Optional: clamp the reserve to `min(h.uval, r.remaining() / min_element_bytes)`. Low priority. |

### Async Integrity

No findings. Recorded as checked (see below).

### Architecture Drift

No findings. The decoder is pure (types.hpp only); the plan core (`gie_epp_plan.hpp`)
is deliberately gRPC/Seastar-free; the subscriber confines all ZMQ to its TU and
worker thread; routing decisions go through `RouterService`, not around it.

---

## What Was Checked and Found Sound

- **Decoder bounds discipline (A-lens):** every `Reader` primitive checks `ok(n)`
  before advancing; `read_header` rejects ext/reserved bytes; `read_str`,
  `read_u64_array`, `read_token_array` validate kind + length before consuming;
  `decode_kv_event_batch` validates the outer array arity, event arity
  (`ev.uval < 1` rejected), and per-tag field counts (`remaining < 4/1`) before
  reading. Returns `nullopt` on every structural error; **never throws** (the
  contract the subscriber relies on). `NaN` timestamp handled.
- **Skip-tree DoS resistance:** `skip_value` is depth-capped (`kKvMaxSkipDepth=16`)
  AND element-capped per level; crucially, every `read_header` consumes ≥1 payload
  byte, so total skip work is bounded by the ≤4 MB payload regardless of the large
  per-level caps. No quadratic blowup.
- **Subscriber threading (async integrity):** ZMQ never touches the reactor; the
  worker is a dedicated OS thread (Rule #12). `on_shipment` reallocates the
  worker-allocated vector reactor-side before the broadcast (Rule #15, :360), runs
  under `with_gate(_apply_gate)`, and `stop()` joins the worker *then* closes the
  gate — so `this` and the ops outlive every in-flight apply. `ship()`'s
  bounded-in-flight throttle sleeps on the OS thread (legal off-reactor) instead of
  flooding shard 0. Command MPSC is bounded; drops are counted.
- **Replay path:** `attempt_replay` is deadline-bounded on BOTH the outer poll and
  an inner re-check inside the drain (:159-163) so a fast publisher can't hold the
  socket readable past the deadline; accepted batches capped at `kMaxReplayBatches`;
  contiguity/hole/short-buffer all fail closed to the reset path; mid-replay
  `flush_ops` keeps op accumulation bounded (Rule #4). DEALER socket choice is
  correct for the multi-message stream.
- **EPP reactor bridge:** `decide()` uses a plain (non-coroutine) `submit_to`
  lambda that copies the body reactor-side into a by-value named coroutine
  (`route_on_reactor`, Rule #21) — avoiding both the cross-thread body free and the
  Rule #16 lambda-coroutine trap. `EppDecision` is a trivially-copyable POD
  (socket_address union), so nothing shard-allocated is freed on a gRPC thread
  (Rules #14/#15). Body accumulation is bounded (`epp_append_bounded`, Rule #4).
  `stop()` drains gRPC on a dedicated thread and hops back to shard 0 to resolve —
  it does NOT freeze the reactor in `Wait()` (which would deadlock the in-flight
  handlers blocked on `submit_to`). `~Impl` joins the thread before `server`
  destructs (Rule #13).
- **EPP plan core:** `epp_format_endpoint` brackets IPv6, guards empty IP;
  `epp_plan` degrades to 503 on any missing input. Pure and total.

---

## Structural Fixes (for BACKLOG.md)

```markdown
- [ ] [MEDIUM] Fix: guard the float64 KV-event timestamp cast — inf/out-of-range double is UB in the uint64 conversion (adversarial audit 2026-07-04, A1); fold in the INT-path overflow cap (A2)
- [ ] [MEDIUM] Fix: bound EPP ext_proc in-flight Process RPCs (ingress semaphore + 503 shed) and add a deadline to the reactor-bridge fut.get() (adversarial audit 2026-07-04, S1)
- [ ] [LOW] Harden: clamp decoder array reserve() to remaining payload bytes (A2/S2)
```

## Fix Prompt — A1 (+ A2)

**PROBLEM:** `decode_kv_event_batch` (`src/kv_event_decoder.hpp:248-251`) converts
the batch timestamp to `ts_ms` two ways, both unsafe for a hostile publisher:
- float path (`0xcb` double): `static_cast<uint64_t>(ts.fval * 1000.0)` is
  **undefined behavior** when `ts.fval` is `+inf` or a finite value ≥ 2^64/1000
  (C++ float→unsigned out-of-range). `ts.fval` is memcpy'd raw from the wire
  (:141-146), so it is fully attacker-controlled. `NaN` is already handled.
- int path (`0xcf` etc.): `ts.uval * 1000` wraps silently (defined, but wrong).

Corrupt `ts_ms` propagates to the ledger's conflict-resolution timestamp
comparisons and `cache_event_timestamps`, so a wrong value is not cosmetic.

**CONSTRAINTS:** The decoder is PURE and its contract is total — never throws,
returns `nullopt` on any structural error (`kv_event_subscriber.cpp` treats
`nullopt` as a stream fault → reset). Keep it header-only, no new includes beyond
`<cmath>`. Do not change the happy-path result for any timestamp vLLM actually
emits (positive, well under 2^63 ms).

**REFERENCE IMPLEMENTATION:** The block_size read just below (:294-299) is the
house pattern for "bounds-check a wire integer before trusting it" — reject or
clamp before use. The `NaN` handling already present at :248 is the shape to extend.

**FIX APPROACH:**
1. float path: gate on `std::isfinite(ts.fval) && ts.fval > 0.0`; compute
   `double ms = ts.fval * 1000.0` and clamp to `UINT64_MAX` before the cast
   (`ms >= 18446744073709549568.0 ? UINT64_MAX : static_cast<uint64_t>(ms)`), else 0.
   (The literal is the largest double < 2^64; use it, not `2^64`, to keep the
   cast in-range.)
2. int path: `out.ts_ms = ts.uval > UINT64_MAX / 1000 ? UINT64_MAX : ts.uval * 1000;`.
3. Neither path rejects the batch — a clamped timestamp is still a usable ordering
   key; only a structurally broken header nullopts, unchanged.

**ACCEPTANCE CRITERIA:**
- `kv_event_decoder_fuzz` (added this pass) runs clean under `-fsanitize=undefined`
  for ≥ 10 min — before the fix it aborts within seconds on an inf timestamp.
- New unit cases in a `kv_event_decoder_test.cpp` (or the existing decoder test):
  a `0xcb` inf timestamp, a `0xcb` `1e300` timestamp, and a `0xcf` `UINT64_MAX`
  timestamp each decode to a finite `ts_ms` (0 or `UINT64_MAX`) with no UB and no
  `nullopt`; a normal timestamp is unchanged.
- Deferred Gates: `RANVIER_BUILD_FUZZERS=ON` build + `make fuzz-run-kv-decoder`;
  `make sanitize-test`.

## Anti-Pattern Candidates

None systemic to this pass. A1 is a single-site UB, not a repeated pattern — but
"float-from-wire cast to integer without an `isfinite` + range guard" is a
one-line grep worth running across `src/` if a second instance ever appears
(`response_usage_parser.hpp`, `prometheus_parser.hpp`, and the metrics scrapers
all parse numbers from external text — spot-checked clean here, but not the focus).

---

*Resolution note appended 2026-07-05 — post-fix record. Static analysis only; UBSan fuzz / sanitizer / gie-epp gates ran in the developer's Docker container. Lets the BACKLOG §24 entry collapse to a pointer (mirrors §20's closeout and the §22 verification pass).*

## Resolution (2026-07-05)

All findings fixed and merged (PRs #611 scope + #612); §24 fully closed.

| # | Disposition |
|---|-------------|
| A1 (MEDIUM) | FIXED — `decode_kv_event_batch` float path gates on `std::isfinite(ts.fval) && ts.fval > 0.0` and clamps `ts.fval * 1000.0` on the largest double `< 2^64` (`18446744073709549568.0`) → `UINT64_MAX` before the cast; no `[conv.fpint]` UB. Pinned by four `KvDecoder` unit cases and by `kv_event_decoder_fuzz` under UBSan (aborted within seconds pre-fix). |
| A2 (LOW) | FIXED — INT timestamp path caps `ts.uval > UINT64_MAX/1000` to `UINT64_MAX` instead of wrapping. |
| S1 (MEDIUM) | FIXED — EPP `decide()` admits at most `gie_epp.max_inflight_requests` (default 1024) concurrent bridges via an atomic counter, shedding over-cap down the existing 503 path; the reactor-bridge wait is now `fut.wait_for(gie_epp.bridge_deadline_ms)` (default 2000ms) so a wedged shard 0 sheds load instead of parking gRPC threads. Config plumbed (env + YAML + example); `docs/internals/gie-epp.md` "Ingress backpressure" section added. `WITH_GIE_EPP`-gated (default OFF). |
| S2 (LOW) | FIXED — decoder array `reserve()` clamped to remaining payload bytes. |

Headline artifact `tests/fuzz/kv_event_decoder_fuzz.cpp` is wired into `fuzz-ci` / `fuzz-run-all` and runs on every push — the decoder's permanent coverage.
