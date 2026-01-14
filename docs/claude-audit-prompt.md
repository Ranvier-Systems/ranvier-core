1. Ref docs/claude-context.md for the "No Locks/Async Only" rules.
2. Run /compact if the conversation exceeds 4 turns.

---

I am requesting a HOLISTIC SYSTEM AUDIT.

**Context:** We have recently completed:
- <TASK_1>
- <TASK_2>
- <TASK_3>

---

## AUDIT SCOPE

### 1. Architecture Drift Check
- Are we maintaining layer boundaries? (Controller → Service → Persistence)
- Any business logic leaking into persistence or utility layers?
- Any direct cross-layer calls bypassing the service layer?

### 2. 12 Hard Rules Compliance
Scan the recently changed files for violations:

| Rule | Check | Status |
|------|-------|--------|
| #0 | No `std::shared_ptr` in Seastar code? | [ ] |
| #1 | No `std::mutex` in metrics/query methods? | [ ] |
| #2 | No sequential `co_await` in loops? | [ ] |
| #3 | All C string returns null-guarded? | [ ] |
| #4 | All containers have MAX_SIZE? | [ ] |
| #5 | Timer callbacks have gate guards? | [ ] |
| #6 | Metrics deregistered first in `stop()`? | [ ] |
| #7 | No validation in persistence layer? | [ ] |
| #8 | Per-shard state in `ShardLocalState`? | [ ] |
| #9 | All catch blocks log at warn+? | [ ] |
| #10 | String conversions use `from_chars`? | [ ] |
| #11 | Global state uses `call_once`/`atomic`? | [ ] |

### 3. Async Integrity
- Any hidden blocking calls?
- Any futures that aren't properly awaited or handled?
- Any potential reactor stalls?

---

## OUTPUT FORMAT

### Compliance Summary
```
[X] Compliant: Rules #1, #3, #7...
[!] Violations Found: Rules #4, #9...
[?] Not Applicable: Rules #8, #11...
```

### Violations Detail
For each violation:
- **File:Line:** `src/services/foo.cc:42`
- **Rule:** #4 (Unbounded Buffer)
- **Issue:** `_queue.push_back()` without size check
- **Fix:** Add `if (_queue.size() >= MAX_QUEUE_SIZE) { drop_oldest(); }`

### Technical Debt Items
Add to docs/TODO.md:
```markdown
- [ ] [TECH-DEBT] Description of debt item (from audit YYYY-MM-DD)
```

