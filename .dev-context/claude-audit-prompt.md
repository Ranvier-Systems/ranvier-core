I am requesting a HOLISTIC SYSTEM AUDIT.

**Context:** We have recently completed:
- <TASK_1>
- <TASK_2>
- <TASK_3>

> Ref: `.dev-context/claude-context.md` for build constraints, architecture, coding conventions, and the Hard Rules.

---

## AUDIT SCOPE

### 1. Architecture Drift Check
- Are we maintaining layer boundaries? (Controller -> Service -> Persistence)
- Any business logic leaking into persistence or utility layers?
- Any direct cross-layer calls bypassing the service layer?

### 2. Hard Rules Compliance
Scan the recently changed files for violations of the Hard Rules.

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
Add to BACKLOG.md:
```markdown
- [ ] [TECH-DEBT] Description of debt item (from audit YYYY-MM-DD)
```
