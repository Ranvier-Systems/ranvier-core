---
name: refactor
description: Refactor code with zero behavioral changes, preserving async semantics and shard affinity. Use when the user asks to restructure, extract, rename, move, or clean up code without changing what it does.
argument-hint: [refactor-description]
---

I am REFACTORING:
$ARGUMENTS

**Goal:** [clarity / modularity / performance / testability]

**Constraint:** NO behavioral changes. Existing tests must pass unchanged.

> Ref: `.dev-context/claude-context.md` for build constraints, architecture, coding conventions, and the Hard Rules.

---

## PRE-REFACTORING CHECKLIST

### Before Starting
- [ ] I have read and understand all affected code
- [ ] I have identified all callers/dependents of affected code (grep for every moved/renamed symbol)
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
- [ ] `co_await` points remain at same logical positions
- [ ] No new blocking calls introduced
- [ ] Future chain semantics identical (same error propagation)
- [ ] Converting `.then()` chains to coroutines is allowed (it's the preferred style) but is **High risk**: re-check Rules #16 (lambda coroutines), #21 (parameters by value), #22 (throws become failed futures) at every converted site

### Shard Affinity
- [ ] No new cross-shard data access without `smp::submit_to`
- [ ] Per-shard state remains shard-local
- [ ] No `std::shared_ptr` introduced for shard-local objects

### Lifetime Management
- [ ] Lambda captures reviewed (no new dangling `this`)
- [ ] Gate guards preserved for timer/callback patterns — and holder scope still covers the full async chain (Rule #5)
- [ ] RAII cleanup unchanged on all paths

### Comment Hygiene (see claude-context.md)
- [ ] No PR-time language added ("hoisted", "previously", "no longer")
- [ ] `Rule #N:` call-site annotations moved **with** the code they gate — never dropped

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
List each mechanical transformation.

### Step 3: Execute with Verification
After each transformation:
- [ ] Behavior unchanged (state why — mechanical transformation, or logic preserved because...)
- [ ] Signatures in `.hpp` still match `.cpp`

---

## OUTPUT FORMAT

### Changes Summary
| File | Change Type | Description |
|------|-------------|-------------|
| `src/foo.cpp` | Extract | Pull `validate_input()` into helper |
| `src/foo.hpp` | Rename | `process()` -> `process_request()` |

### Behavioral Equivalence Proof
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

### Deferred Gates
Compilation and tests cannot run in this sandbox. Provide the developer:
```
make test-unit                        # all tests must pass UNCHANGED
./scripts/lint-seastar-async.sh       # if any seastar::async sites were touched
```

Finish with `/review` before committing.
