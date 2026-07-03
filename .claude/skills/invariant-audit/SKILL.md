---
name: invariant-audit
description: Semantic-correctness audit of hot-path code - enumerate the invariants a component must maintain, then hunt code paths that violate them (wrong answers, not crashes). Use after changing routing, eviction, accounting, or counter logic, before a release, or when metrics drift without any crash.
argument-hint: [scope, defaults to the hot-path files listed in invariants.md]
allowed-tools: Read, Grep, Glob, Write, Edit
---

I am requesting an INVARIANT AUDIT of: $ARGUMENTS (default: the components catalogued in `.dev-context/invariants.md`)

> Ref: `.dev-context/claude-context.md` for build constraints and the Hard Rules.

This is NOT `/adversarial-audit` (crashes, leaks, attack surface) and NOT `/audit`
(Hard Rules compliance). This audit hunts **wrong answers**: code that never crashes,
passes every rule, and quietly routes to the wrong backend, drifts a counter, or lets
a side-structure diverge from its primary. The worked example of this bug class is E1
in `adversarial-audit-2026-02-12.md` (route_count drift → cascading eviction); the
first full run is `invariant-audit-2026-07-03.md`.

---

## METHOD (invariants first, hunting second)

### 1. Load the catalog
Read `.dev-context/invariants.md` — the living invariant catalog this skill maintains.
For each in-scope component, take its invariant list as the checklist. Skim the prior
`invariant-audit-*.md` reports so resolved findings aren't re-reported and known
violations (⚠️ entries) are checked for regression or fix.

### 2. Extend the catalog for new scope
If the scope includes code without catalog entries, derive its invariants BEFORE
hunting: read the component's header contract and comments and write down what must
always be true. Productive invariant shapes in this codebase:

- **Counter symmetry:** every increment has a matching decrement on EVERY exit path —
  error, timeout, early return, move-assignment, re-registration. (Guards, budgets,
  `active_requests`, per-backend tallies.)
- **Primary/side-structure lockstep:** a cache, reverse index, LRU list, or tally
  updated when the primary structure adds MUST be updated on every path where the
  primary removes. Enumerate the primary's removal paths and check each one by name.
- **Identity/derivation consistency:** every producer and consumer of a derived key
  (hash, boundary, alignment, ordinal) computes it over the same inputs at the same
  depth. Grep for ALL call sites of the derivation function and diff their arguments.
- **Exactly-once transitions:** accumulators folded exactly once per state transition
  (segment accounting); snapshots that reset must be atomic per window.
- **Documented contracts:** trust ladders, tie-break orders, parity contracts
  ("default weights == old behavior"), monotonicity claims, "bounded by X" claims —
  each is an invariant; verify the code actually enforces it everywhere, not just at
  the site next to the comment.

### 3. Hunt violations
For each invariant, look where invariants die:
- error/timeout/early-return paths (the happy path is usually right);
- the *other* call sites — a contract enforced at one insert path and bypassed by a
  second (`insert` vs `insert_if_trusted` class of bug);
- capacity-hit and at-cap branches (drop/evict paths skip bookkeeping);
- cross-component seams where one file's comment claims what another file must do;
- defensive clamps (`count > removed ? … : 0`) — each one marks a spot where the
  author feared the invariant; treat it as a signpost, not a fix.

Every claim must cite `file:line`. Also record **what was checked and found sound** —
that negative space is what makes the next run cheaper.

### 4. Report
Write the report to `.dev-context/invariant-audit-YYYY-MM-DD.md` matching the
2026-07-03 structure: criticality score, findings table
(`| # | Severity | File:Line | Invariant | Issue | Recommendation |`), a
"checked and found sound" section, BACKLOG items, fix prompts for HIGH findings
(template: `.dev-context/audit-fix-prompts.md`), and **proposed unit tests as
Deferred Gates** — invariant findings convert to assertions more directly than crash
findings do; every HIGH/MEDIUM finding should name the test that would have caught it.

### 5. Close the flywheel
- Append newly derived invariants to `.dev-context/invariants.md` (with status marker
  ◻/⚠️/✅ and the finding link for ⚠️).
- Flip ⚠️ back to ◻/✅ in the catalog when re-checking shows a prior finding fixed —
  the catalog is living, unlike the dated reports.
- If a finding is systemic (same shape in 2+ places), run `/extract-pattern`.
- Add BACKLOG items under a dated section, matching the existing format.

---

For crash/leak/attack-surface hunting use `/adversarial-audit`; for Hard Rules
compliance on recent changes use `/review` or `/audit`.
