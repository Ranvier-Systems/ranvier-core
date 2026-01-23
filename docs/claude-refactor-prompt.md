I am REFACTORING:
<DESCRIPTION>

**Goal:** [clarity / modularity / performance / testability]

**Constraint:** NO behavioral changes. Existing tests must pass unchanged.

---

1. Ref docs/claude-context.md for the "No Locks/Async Only" rules.
2. DO NOT read the full /docs or /assets folders.
3. Run /compact if the conversation exceeds 4 turns.

Build Constraints:
1. Static Analysis Only: Do not attempt to run cmake or build. Seastar dependencies are too heavy for the sandbox.
2. API Verification: Verify syntax against Seastar documentation logic.
3. Manual Verification: I will build in my Docker container and provide logs if it fails.

---

## PRE-REFACTORING CHECKLIST

### Before Starting
- [ ] All existing tests pass (baseline established)
- [ ] I have read and understand all affected code
- [ ] I have identified all callers/dependents of affected code
- [ ] The refactoring goal is clear and measurable

### Refactoring Type (select one)
| Type | Description | Risk Level |
|------|-------------|------------|
| **Extract** | Pull code into new function/class | Low |
| **Inline** | Collapse unnecessary abstraction | Low |
| **Rename** | Improve naming clarity | Low |
| **Move** | Relocate to better module | Medium |
| **Restructure** | Change internal organization | Medium |
| **Async Refactor** | Modify future chains | High |

---

## SEASTAR-SPECIFIC CONCERNS

### Async Boundary Preservation
When refactoring async code, verify:
- [ ] `co_await` points remain at same logical positions
- [ ] No new blocking calls introduced
- [ ] Future chain semantics identical (same error propagation)
- [ ] Coroutine vs callback style not mixed inappropriately

### Shard Affinity
- [ ] No new cross-shard data access without `smp::submit_to`
- [ ] Per-shard state remains shard-local
- [ ] No `std::shared_ptr` introduced for shard-local objects

### Lifetime Management
- [ ] Lambda captures reviewed (no new dangling `this`)
- [ ] Gate guards preserved for timer/callback patterns
- [ ] RAII cleanup unchanged on all paths

---

## REFACTORING PROCESS

### Step 1: Document Current Behavior
For each function being refactored:
```
Function: [name]
Purpose: [what it does]
Inputs: [parameters and preconditions]
Outputs: [return value and postconditions]
Side Effects: [state changes, I/O]
Async Behavior: [future semantics, shard interactions]
```

### Step 2: Plan the Changes
List each mechanical transformation:
1. [transformation 1]
2. [transformation 2]
...

### Step 3: Execute with Verification
After each transformation:
- [ ] Code compiles
- [ ] Behavior unchanged (reason why)

---

## OUTPUT FORMAT

### Changes Summary
| File | Change Type | Description |
|------|-------------|-------------|
| `src/foo.cc` | Extract | Pull `validate_input()` into helper |
| `src/foo.hh` | Rename | `process()` → `process_request()` |
| ... | ... | ... |

### Behavioral Equivalence Proof
For each changed function:
```
Function: [name]
Before: [behavior description]
After:  [identical behavior description]
Why Equivalent: [mechanical transformation / logic preserved because...]
```

### Risk Assessment
- [ ] **Low risk** - Mechanical transformation only
- [ ] **Medium risk** - Logic restructuring, needs careful review
- [ ] **High risk** - Async flow changes, needs thorough testing

### Hard Rules Check
Verify refactoring doesn't introduce violations:
- [ ] No new `std::shared_ptr`
- [ ] No new `std::mutex`
- [ ] No new sequential `co_await` loops
- [ ] All existing patterns preserved

