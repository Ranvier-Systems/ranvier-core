1. Ref docs/claude-context.md for the "No Locks/Async Only" rules.
2. DO NOT read the full /docs or /assets folders.
3. Run /compact if the conversation exceeds 4 turns.

## Quick Reference (before every task)

Before writing any code, verify:
- [ ] Have I read the relevant file(s) I'm about to modify?
- [ ] Does this touch cross-shard communication? → Use `seastar::smp::submit_to`
- [ ] Is there a MAX_SIZE for any new container?
- [ ] Any new timer/callback? → Needs gate guard
- [ ] Any new metrics lambda capturing `this`? → Deregister in `stop()`
- [ ] Any C API string returns? → Null-guard required

## 12 Hard Rules (Quick Check)

| # | Check | If Yes → |
|---|-------|----------|
| 0 | New shared ownership? | Use `unique_ptr` or `lw_shared_ptr`, NOT `std::shared_ptr` |
| 1 | Metrics/status method? | Must be lock-free (no mutex) |
| 2 | Async loop over items? | Use `parallel_for_each`, NOT sequential `co_await` |
| 3 | C string from SQLite/etc? | Null-guard before use |
| 4 | Growing container? | Define MAX_SIZE + drop strategy |
| 5 | Timer with `this` capture? | Add gate guard |
| 6 | Metrics lambda with `this`? | Deregister first in `stop()` |
| 7 | Validation logic? | Keep in service layer, NOT persistence |
| 8 | Per-shard state? | Use `ShardLocalState` struct, NOT scattered `thread_local` |
| 9 | Exception handler? | Must log at warn level with context |
| 10 | String-to-number conversion? | Use `std::from_chars` with validation |
| 11 | Global state? | Use `std::call_once` or `std::atomic` |

---

I am starting a task:

