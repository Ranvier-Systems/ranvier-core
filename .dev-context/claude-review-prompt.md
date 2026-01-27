I have written code for:
<INSERT_TASK_NAME>

---

1. Ref .dev-context/claude-context.md for the "No Locks/Async Only" rules.
2. Run /compact if the conversation exceeds 4 turns.

Build Constraints:
1. Static Analysis Only: Do not attempt to run cmake or build. Seastar dependencies are too heavy for the sandbox.
2. API Verification: Verify syntax against Seastar documentation logic.
3. Manual Verification: I will build in my Docker container and provide logs if it fails.

---

## SELF-REVIEW (before commit)

Run through each of the Hard Rules listed in .dev-context/claude-context.md. For each rule, provide one of:
- `[N/A]` - Not applicable to this change
- `[OK]` - Compliant because: [brief reason]
- `[FIX]` - Violation found at [file:line]: [description]

---

## COMPLIANCE TABLE

Verify compliance with the list of Hard Rules outlined in .dev-context/claude-context.md

For example:
| # | Rule | Status | Notes |
|---|------|--------|-------|
| 0 | No `std::shared_ptr` in Seastar code | | |
| 1 | No `std::mutex` in metrics/query methods | | |
| 2 | No `co_await` inside loops (use `parallel_for_each`) | | |
| 3 | Null-guard all C string returns | | |
| 4 | Every container has MAX_SIZE | | |
| 5 | Timer callbacks have gate guards | | |
| 6 | Metrics deregistered first in `stop()` | | |
| 7 | No validation in persistence layer | | |
| 8 | Per-shard state in `ShardLocalState` struct | | |
| 9 | Every catch block logs at warn+ | | |
| 10 | String-to-number uses `std::from_chars` | | |
| 11 | Global state uses `call_once`/`atomic` | | |
| 12 | Use Seastar file I/O (no `std::ifstream`) | | |
| 13 | Thread-local raw new has destroy function | | |

---

## ADDITIONAL CHECKS

### Async Flow Integrity
- [ ] All futures are awaited or explicitly discarded
- [ ] No hidden blocking calls (file I/O, DNS, sleep)
- [ ] Cross-shard calls use `smp::submit_to`

### Error Handling
- [ ] All error paths are handled (no silent failures)
- [ ] Exceptions don't escape coroutines unexpectedly
- [ ] Resource cleanup happens on all paths (RAII)

### Memory Safety
- [ ] No dangling references in lambdas
- [ ] Captured `this` pointers are protected by gates
- [ ] No use-after-move

---

## OUTPUT FORMAT

### Compliance Summary
```
Compliant: #0, #1, #3, #7, #8, #10, #11
Not Applicable: #5, #6
Violations: #2, #4, #9
```

### Violations Requiring Fix
For each `[FIX]`:
```
Rule #2 - src/services/router.cc:156
  Issue: Sequential co_await in for loop over backends
  Fix: Replace with seastar::parallel_for_each
```

### Ready to Commit?
- `[YES]` - All rules pass, ready for commit
- `[NO]` - Fixes required (list above)

---

## POST-REVIEW ACTIONS

If violations found:
1. Fix all violations
2. Re-run this review
3. Only commit when all checks pass

If all checks pass:
1. Proceed to commit
2. Run `claude-doc-prompt.md` for tests and documentation

