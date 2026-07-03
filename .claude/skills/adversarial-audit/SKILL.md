---
name: adversarial-audit
description: Hostile security and robustness audit of src/ assuming malicious traffic and 100x load. Use before releases, after large feature merges, or when the user asks for a security audit, adversarial review, or "what breaks under attack".
argument-hint: [scope, defaults to all of src/]
allowed-tools: Read, Grep, Glob, Write
---

I am requesting an ADVERSARIAL SYSTEM AUDIT of: $ARGUMENTS (default: all source files under `src/`)

> Ref: `.dev-context/claude-context.md` for build constraints, architecture, coding conventions, and the Hard Rules.

---

## PERSONA

Act as a **cynical Staff Engineer and Security Auditor**. Do not look for "small issues"--look for **structural failures**. Assume the code will be attacked by malicious actors and stressed by 100x traffic. Every claim must cite `file:line` — a finding you cannot point to is not a finding.

Before starting, skim the prior reports (`.dev-context/adversarial-audit-*.md`) so you don't re-report resolved findings — and so you recognize regressions of previously-fixed ones.

---

## AUDIT LENSES

### 1. THE "ASYNC INTEGRITY" LENS
Scan for violations of Seastar's reactor model:
- Hidden blocking calls (file I/O, DNS, mutexes, unmarked `seastar::async`)
- `co_await` inside loops that should be `seastar::parallel_for_each`
- Synchronous wrappers around async operations that could deadlock
- Futures discarded without `.discard_result()` or proper handling
- Gate holders whose scope does NOT cover the full async chain (the classic finding: holder local to a lambda or `try` block while the work is a detached future — see A2/A3 in `adversarial-audit-2026-02-12.md`)
- Timer callbacks dereferencing `this` *before* acquiring the gate

**Flag:** Point to specific lines where reactor stalls or use-after-free could occur.

### 2. THE "EDGE-CASE CRASH" LENS
If external inputs fail, where does this code break?
- Network call returns error -> unhandled exception?
- SQLite returns NULL -> null pointer dereference?
- Input string is empty/malformed -> crash in parser? (untrusted surfaces: HTTP bodies, gossip packets, KV-event msgpack, Prometheus scrape text, EPP gRPC)
- External service timeout -> cascade failure?
- Counter/accounting drift: increments without matching decrements, "new item" counters bumped on updates (see E1 in the 2026-02-12 report — route_count drift caused cascading eviction)

**Flag:** Point to specific lines with unhandled failure modes.

### 3. THE "ARCHITECTURE DRIFT" LENS
Does this code respect our layered architecture?
- Controller calling persistence directly (bypassing service)?
- Business validation in persistence layer?
- Utility files accumulating business logic?

**Flag:** Identify any layer boundary violations.

### 4. THE "SCALE & LEAK" LENS
Will this hold up at 100x scale?
- Containers growing without bounds -> OOM risk? (check every `push_back`/`emplace`/`operator[]` insert against a MAX_SIZE)
- O(n) or O(n^2) algorithms on unbounded data?
- Event listeners/callbacks added without cleanup -> memory leak?
- Timers scheduled without cancellation on shutdown -> use-after-free?
- Shared `temporary_buffer` views pinning large allocations (Rule #23)

**Flag:** Identify resource exhaustion and leak vectors.

---

## OUTPUT FORMAT

Write the full report to `.dev-context/adversarial-audit-YYYY-MM-DD.md` (match the structure of the existing reports so verification passes can annotate them later).

### Criticality Score: X/10
- **1-3:** Minor issues, safe to ship
- **4-6:** Moderate issues, fix before next release
- **7-9:** Serious issues, fix before shipping
- **10:** Critical vulnerability, stop and fix now

### Findings by Lens

#### Async Integrity
| # | Severity | File:Line | Rule | Issue | Recommendation |
|---|----------|-----------|------|-------|----------------|
| A1 | HIGH | `src/foo.cpp:42` | #5 | Sequential await in loop | Use `parallel_for_each` |

(Repeat table per lens: E# Edge-Case, D# Drift, S# Scale & Leak.)

### Structural Fixes (for BACKLOG.md)
```markdown
- [ ] [CRITICAL] Fix: description (adversarial audit YYYY-MM-DD)
- [ ] [HIGH] Fix: description
- [ ] [MEDIUM] Fix: description
```

### Fix Prompts
For CRITICAL/HIGH findings, generate one self-contained fix prompt each (PROBLEM / CONSTRAINTS / REFERENCE IMPLEMENTATION in this codebase / FIX APPROACH / ACCEPTANCE CRITERIA — follow `.dev-context/audit-fix-prompts.md` as the template). These let cheaper follow-up sessions fix issues without re-deriving the analysis.

### Anti-Pattern Candidates
If any issues are systemic, run `/extract-pattern` to formalize them into the Hard Rules.
