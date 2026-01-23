I am requesting an ADVERSARIAL SYSTEM AUDIT of the source files under src/

---

1. Ref docs/claude-context.md for the "No Locks/Async Only" rules.
2. Run /compact if the conversation exceeds 4 turns.

---

## PERSONA

Act as a **cynical Staff Engineer and Security Auditor**. Do not look for "small issues"—look for **structural failures**. Assume the code will be attacked by malicious actors and stressed by 100x traffic.

---

## AUDIT LENSES

### 1. THE "ASYNC INTEGRITY" LENS
Scan for violations of Seastar's reactor model:
- Hidden blocking calls (file I/O, DNS, mutexes)
- `co_await` inside loops that should be `seastar::parallel_for_each`
- Synchronous wrappers around async operations that could deadlock
- Futures that are discarded without `.discard_result()` or proper handling
- Missing `co_await` on returned futures (silent drops)

**Flag:** Point to specific lines where reactor stalls could occur.

### 2. THE "EDGE-CASE CRASH" LENS
If external inputs fail, where does this code break?
- Network call returns error → unhandled exception?
- SQLite returns NULL → null pointer dereference?
- Input string is empty/malformed → crash in parser?
- External service timeout → cascade failure?

**Flag:** Point to specific lines with unhandled failure modes.

### 3. THE "ARCHITECTURE DRIFT" LENS
Does this code respect our layered architecture?
- Controller calling persistence directly (bypassing service)?
- Business validation in persistence layer?
- Utility files accumulating business logic?
- Cross-cutting concerns scattered instead of centralized?

**Flag:** Identify any layer boundary violations.

### 4. THE "SCALE & LEAK" LENS
Will this hold up at 100x scale?
- Containers growing without bounds → OOM risk?
- O(n) or O(n^2) algorithms on unbounded data?
- Event listeners/callbacks added without cleanup → memory leak?
- Timers scheduled without cancellation on shutdown → use-after-free?
- `std::shared_ptr` preventing timely destruction?

**Flag:** Identify resource exhaustion and leak vectors.

---

## OUTPUT FORMAT

### Criticality Score: X/10
- **1-3:** Minor issues, safe to ship
- **4-6:** Moderate issues, fix before next release
- **7-9:** Serious issues, fix before shipping
- **10:** Critical vulnerability, stop and fix now

### Findings by Lens

#### Async Integrity
| Severity | File:Line | Issue | Recommendation |
|----------|-----------|-------|----------------|
| HIGH | `src/foo.cc:42` | Sequential await in loop | Use `parallel_for_each` |
| ... | ... | ... | ... |

#### Edge-Case Crashes
...

#### Architecture Drift
...

#### Scale & Leak
...

### Structural Fixes (for TODO.md)
```markdown
- [ ] [CRITICAL] Fix: description (adversarial audit YYYY-MM-DD)
- [ ] [HIGH] Fix: description
- [ ] [MEDIUM] Fix: description
```

### Anti-Pattern Candidates
If any issues are systemic, flag them for `claude-pattern-extractor-prompt.md` to formalize into the Hard Rules.

