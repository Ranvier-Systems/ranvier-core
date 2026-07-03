---
name: debug-build
description: Triage build or runtime failures. Use when the user pastes a compile error, link error, runtime crash, test failure, or error log, or mentions a build failure.
argument-hint: [error-type]
---

I have a BUILD/RUNTIME FAILURE.

**Error type:** $ARGUMENTS

> Ref: `.dev-context/claude-context.md` for build constraints, architecture, coding conventions, and the Hard Rules.

---

## TRIAGE PROCESS

### Step 1: Classify the Error
- [ ] **Compile error** - Syntax, type mismatch, missing include
- [ ] **Link error** - Undefined reference, missing library
- [ ] **Runtime crash** - Segfault, assertion, unhandled exception
- [ ] **Logic error** - Wrong behavior, test failure

### Step 2: Isolate the Cause
For compile/link errors:
- Which file and line?
- Is it in new code or existing code affected by changes?
- Is it a Seastar API misuse?

For runtime errors:
- What's the stack trace?
- Which Seastar shard (check `seastar::this_shard_id()`)?
- Is it reproducible or intermittent?
- Does it happen under load or immediately?

### Step 3: Check Common Seastar Pitfalls
| Symptom | Likely Cause | Rule # |
|---------|--------------|--------|
| "Exceptional future ignored" / "future leaked" | Missing `co_await` or `.discard_result()` | #18 |
| Segfault in destructor | Cross-shard `shared_ptr` release | #0 |
| Reactor stall warning | Blocking call, mutex, unmarked `seastar::async`, or unyielding loop | #1, #12, #17 |
| Segfault in callback | Timer use-after-free (gate holder missing or wrongly scoped) | #5 |
| Segfault on NULL string | C API without null guard | #3 |
| OOM kill / unexplained RSS growth | Unbounded container; pinned `temporary_buffer::share()` | #4, #23 |
| `free(): invalid pointer`, garbage `si_addr` | Cross-shard heap move; FFI without local reallocation | #14, #15 |
| Crash at first real suspension (fine in tests) | Lambda coroutine passed to `.then()` unwrapped | #16 |
| Intermittent UAF under load, unrelated stack | `do_with` missing `auto&`; coroutine took `const&` param | #20, #21 |
| `.finally()`/`.handle_exception()` never runs | Function threw before returning a future | #22 |
| Waiters hang forever, semaphore units gone | Raw `wait()`/`signal()` leak on throw | #19 |
| Hang or UAF only at shutdown | thread_local destructor after reactor teardown; `stop()` ordering | #13, #6 |

For pitfalls beyond the Hard Rules (`when_all_succeed` exception drops, `foreign_ptr` destruction, `abort_source` lifetime, shutdown close-ordering races), check `.dev-context/seastar-pitfalls-reference.md`. To reproduce lifetime bugs deterministically, ask the developer for a `make sanitize-test` run (see `/validate`).

### Step 4: Verify Recent Changes
For each recently changed file:
- [ ] Does it compile in isolation?
- [ ] Are all includes correct?
- [ ] Any new async flows that need `co_await`?
- [ ] Any new cross-shard calls?

---

## OUTPUT FORMAT

### Diagnosis
```
Error Type: [compile/link/runtime/logic]
Root Cause: [description]
File:Line:  [location]
Rule Violated: [#N if applicable, or "N/A"]
```

### Fix
```cpp
// Before (problematic code)
[code]

// After (fixed code)
[code]
```

### Prevention
- [ ] Should this become a new anti-pattern in claude-context.md?
- [ ] Is there a missing test case that would have caught this?
