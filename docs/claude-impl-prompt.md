1. Ref docs/claude-context.md for the "No Locks/Async Only" rules.
2. DO NOT read the full /docs or /assets folders.
3. Run /compact if the conversation exceeds 4 turns.

## Build Constraints
1. **Static Analysis Only:** Do not attempt to run cmake or build. Seastar dependencies are too heavy for the sandbox.
2. **API Verification:** Verify syntax against Seastar documentation logic.
3. **Manual Verification:** I will build in my Docker container and provide logs if it fails.

---

I am STARTING AN IMPLEMENTATION TASK:
<INSERT_TASK_FROM_TODO_LIST>

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
For this specific task, which of the 12 Hard Rules apply?
- [ ] Rule #X applies because...
- [ ] Rule #Y applies because...

---

## STAGED EXECUTION

### Pass 0: Visualize the Async Flow (REQUIRED)
Before any code, provide a Mermaid diagram showing:
- Entry point → async boundaries → shard crossings → completion
- Mark any `co_await` points
- Mark any `smp::submit_to` calls

```mermaid
sequenceDiagram
    participant Client
    participant Shard0
    participant ShardN
    ...
```

### Pass 1: Logic & Correctness
- Handle all edge cases and error paths
- Ensure robustness (null checks, bounds checks, error handling)
- Every `catch` block logs at warn level with context

### Pass 2: Refactor for Clarity & Modularity
- Extract helper methods where appropriate
- Ensure single responsibility
- Add minimal necessary comments (only where logic isn't self-evident)

### Pass 3: Optimize for Async Performance
- Replace any sequential `co_await` loops with `parallel_for_each`
- Verify no blocking calls on reactor thread
- Confirm gate guards for any timer/callback capturing `this`

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
- Any technical debt introduced? (Add to TODO.md if so)

