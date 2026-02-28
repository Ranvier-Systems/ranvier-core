I have a BUILD/RUNTIME FAILURE.

**Error type:** [compile / link / runtime / test failure]

**Error log:**
```
<PASTE_ERROR_LOG>
```

> Ref: `.dev-context/claude-context.md` for build constraints, architecture, coding conventions, and the 24 Hard Rules.

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
| "future leaked" warning | Missing `co_await` or `.discard_result()` | - |
| Segfault in destructor | Cross-shard `shared_ptr` release | #0 |
| Reactor stall warning | Blocking call or mutex | #1 |
| Segfault in callback | Timer use-after-free | #5 |
| Segfault on NULL string | C API without null guard | #3 |
| OOM kill | Unbounded container | #4 |

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
