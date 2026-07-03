---
name: audit
description: Holistic system audit for architecture drift, Hard Rules compliance, and async integrity across recently changed code. Use after completing a batch of tasks, before a release, or when the user asks for a health check or audit.
argument-hint: [recently-completed-tasks]
allowed-tools: Read, Grep, Glob
---

I am requesting a HOLISTIC SYSTEM AUDIT.

**Context:** We have recently completed:
$ARGUMENTS

> Ref: `.dev-context/claude-context.md` for build constraints, architecture, coding conventions, and the Hard Rules.

---

## AUDIT SCOPE

### 1. Architecture Drift Check
- Are we maintaining layer boundaries? (Controller -> Service -> Persistence)
- Any business logic leaking into persistence or utility layers?
- Any direct cross-layer calls bypassing the service layer?

### 2. Hard Rules Compliance
Scan the recently changed files for violations of the Hard Rules (all 24, #0–#23). Use `git log --oneline -20` and `git diff` to establish scope if not given.

### 3. Async Integrity
- Any hidden blocking calls?
- Any futures that aren't properly awaited or handled?
- Any potential reactor stalls?
- Any unmarked `seastar::async(` sites? (each needs a `// rule12-allow: <reason>` marker — `./scripts/lint-seastar-async.sh` is the enforcement)

### 4. Documentation Sync
- Do `docs/internals/` deep-dives still match the code in audited areas?
- Does the Source Code Layout in `.dev-context/claude-context.md` list all current `src/` files?

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
- **File:Line:** `src/foo.cpp:42`
- **Rule:** #4 (Unbounded Buffer)
- **Issue:** `_queue.push_back()` without size check
- **Fix:** Add `if (_queue.size() >= MAX_QUEUE_SIZE) { drop_oldest(); }`

### Technical Debt Items
Add to BACKLOG.md:
```markdown
- [ ] [TECH-DEBT] Description of debt item (from audit YYYY-MM-DD)
```

---

For a hostile, security-focused pass over the whole of `src/`, use `/adversarial-audit` instead. If any finding is systemic (same mistake in 2+ places), follow up with `/extract-pattern` to formalize it as a Hard Rule.
