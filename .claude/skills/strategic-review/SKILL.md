---
name: strategic-review
description: Brutally honest external-CTO assessment of project direction versus the stated goal. Use when the user asks where the project stands, whether it has drifted, what to cut, or for a state-of-the-project evaluation.
argument-hint: [focus-area, optional]
allowed-tools: Read, Grep, Glob
---

I am requesting a STRATEGIC PROJECT ASSESSMENT. $ARGUMENTS

Act as an External CTO and Lead Architect. I need a "brutally honest" evaluation of where Ranvier Core stands today versus our stated goal: **a Layer 7+ LLM traffic controller with prefix-affinity routing that measurably reduces GPU KV-cache thrashing.**

> Ref: `.dev-context/claude-context.md` for architecture and the Hard Rules. Also consult `BACKLOG.md` (current work), `CHANGELOG.md` (trajectory), and `docs/internals/` (what's actually built).

---

## ASSESSMENT QUESTIONS

### 1. THE "GOAL ALIGNMENT" CHECK
- Based on the code implemented, are we actually building a Prefix Caching balancer, or have we drifted into a general-purpose proxy?
- Are the core "Prefix Logic" constraints actually reflected in the code, or are they just aspirations?
- Do the benchmark numbers (`tests/integration/benchmark-baseline.json`, `docs/benchmarks/`) still support the headline claim (~48% faster TTFT)?

### 2. THE "COMPLEXITY VS. VALUE" AUDIT
- Where is the highest concentration of complexity? Is that complexity buying us performance, or is it technical debt?
- Are any subsystems over-engineered relative to the current feature set? (Candidates to examine honestly: gossip/cluster sync, telemetry, the multiple routing-override layers now unified in `route_scorer.hpp`.)

### 3. THE "HIDDEN FRAGILITY" ASSESSMENT
- Identify the "Load-Bearing" files--if these fail, the whole system collapses. Are these files sufficiently guarded by our Hard Rules, tests, and fuzz harnesses?
- Based on BACKLOG.md, what is the "Next Big Risk" we are about to take on?

### 4. THE "STAFF ENGINEER" RECOMMENDATION
- If you were taking over this project today, what is the one thing you would delete, and the one thing you would refactor immediately to reach production readiness?

---

## OUTPUT FORMAT

Provide a "State of the Project" scorecard (A-F) for **Architecture**, **Reliability**, and **Progress-to-Goal**, followed by the prioritized analysis. Every claim cites evidence (file, benchmark number, BACKLOG item) — no vibes.

Actionable outcomes go to BACKLOG.md as `[STRATEGIC]` items; systemic code findings feed `/audit` or `/adversarial-audit`.
