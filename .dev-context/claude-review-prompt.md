I have written code for:
<INSERT_TASK_NAME>

> Ref: `.dev-context/claude-context.md` for build constraints, architecture, coding conventions, and the 24 Hard Rules.

---

## SELF-REVIEW (before commit)

Run through each of the Hard Rules. For each rule, provide one of:
- `[N/A]` - Not applicable to this change
- `[OK]` - Compliant because: [brief reason]
- `[FIX]` - Violation found at [file:line]: [description]

## COMPLIANCE TABLE

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
| 14 | Force local allocation for cross-shard data | | |
| 15 | Reallocate locally before FFI across boundaries | | |
| 16 | Wrap lambda coroutines with `seastar::coroutine::lambda()` | | |
| 17 | Insert preemption points in loops (>100 iterations) | | |
| 18 | Every future must be awaited or explicitly handled | | |
| 19 | Use `with_semaphore()`/`get_units()`, never raw `wait()`/`signal()` | | |
| 20 | Always use `auto&` in `do_with` lambdas; prefer coroutines | | |
| 21 | Coroutines take parameters by value, not reference | | |
| 22 | Wrap throws in `futurize_invoke()`; prefer coroutines | | |
| 23 | Clone `temporary_buffer` for long-lived data, don't `share()` | | |

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

If violations found, fix and re-run this review. Only commit when all checks pass.
Then run `claude-doc-prompt.md` for tests and documentation.
