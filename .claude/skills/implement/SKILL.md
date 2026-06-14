---
name: implement
description: Start a structured implementation task with staged execution. Use when the user wants to implement a new feature, add functionality, or begin a coding task.
argument-hint: [task-description]
---

I am STARTING AN IMPLEMENTATION TASK:
$ARGUMENTS

> Ref: `.dev-context/claude-context.md` for build constraints, architecture, coding conventions, and the Hard Rules.

---

## BEFORE WRITING CODE

### Step 1: List Affected Files
List every file you will modify or create in a numbered list:
```
1. src/services/foo.cc - Adding new method
2. src/services/foo.hh - Declaring new method
3. ...
```
Wait for my confirmation before proceeding.

### Step 2: Quick Rule Check
For this specific task, which of the Hard Rules apply?
- [ ] Rule #X applies because...
- [ ] Rule #Y applies because...

---

## STAGED EXECUTION

Each pass ends at a **Verification Gate**: a static, failable artifact an outside reader could reproduce — a `grep` result, a header/impl signature diff, the async diagram checked against the actual lines, or a cited Seastar API. "Looks correct" is not a gate. If you cannot produce the artifact, the pass is not done — say so and stop.

Some checks need a real compile or run, which this sandbox forbids (static analysis only). Do not fake them: record a **Deferred Gate** — the exact `make`/test command for the developer's Docker build, plus the output that would pass or fail.

### Pass 0: Visualize the Async Flow (REQUIRED)
Before any code, provide a Mermaid diagram showing:
- Entry point -> async boundaries -> shard crossings -> completion
- Mark any `co_await` points
- Mark any `smp::submit_to` calls

```mermaid
sequenceDiagram
    participant Client
    participant Shard0
    participant ShardN
    ...
```

- **Gate:** every async boundary and shard crossing in the diagram names the Seastar primitive that implements it (`co_await`, `smp::submit_to`, `parallel_for_each`), each checked against Seastar docs.

### Pass 1: Logic & Correctness
- Handle all edge cases and error paths
- Ensure robustness (null checks, bounds checks, error handling)
- Every `catch` block logs at warn level with context
- **Gate:** `grep` the changed files and cite lines proving Rule #9 (every `catch` logs at warn+), Rule #3 (C-string returns null-guarded), and Rule #4 (every new container has a MAX_SIZE). Zero unguarded cases = pass.

### Pass 2: Refactor for Clarity & Modularity
- Extract helper methods where appropriate
- Ensure single responsibility
- Add minimal necessary comments (only where logic isn't self-evident)
- **Gate:** diff each changed `.hpp` signature against its `.cpp` definition and the Step 1 file list — they match, and the refactor changes structure only (no behavioral delta).

### Pass 3: Optimize for Async Performance
- Replace any sequential `co_await` loops with `parallel_for_each`
- Verify no blocking calls on reactor thread
- Confirm gate guards for any timer/callback capturing `this`
- **Gate:** `grep` proves no sequential `co_await` remains inside a loop body (Rule #2) and any `this`-capturing timer/callback is gate-guarded (Rule #5). Cite the lines.

---

## OUTPUT FORMAT

For each modified file, provide:
```
=== FILE: path/to/file.cc ===
[Full content or diff]
```

### Architectural Trade-offs
Document any trade-offs made:
- Why this approach vs alternatives?
- Any technical debt introduced? (Add to BACKLOG.md if so)
