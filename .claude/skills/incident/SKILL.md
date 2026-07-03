---
name: incident
description: Structured incident response and root-cause analysis for production failures. Use when the user reports a production problem - crashes, latency spikes, OOM, hangs, wrong routing behavior - especially with logs, metrics, or benchmark evidence.
argument-hint: [one-line-description]
---

## INCIDENT REPORT

**Summary:** $ARGUMENTS

**Severity:** [P0-Critical / P1-High / P2-Medium / P3-Low]

**Symptoms / Timeline / Affected scope:** fill in from the user's report; ask for what's missing:
- First observed, recent changes (deployments, config), frequency (constant / intermittent / under load only)
- All shards or specific shard(s)? All requests or a specific pattern?

> Ref: `.dev-context/claude-context.md` for build constraints, architecture, coding conventions, and the Hard Rules.

---

## INCIDENT RESPONSE PROTOCOL

### Phase 1: Immediate Assessment (< 5 min)
1. Is the service completely down or degraded?
2. Is it affecting all users or a subset?
3. Is it getting worse, stable, or recovering?
4. Can we rollback to a known good state?

### Phase 2: Hypothesis Generation

| # | Hypothesis | Supporting Evidence | Quick Test |
|---|------------|---------------------|------------|
| 1 | [hypothesis] | [evidence] | [how to verify] |

**Discipline:** do not write a code fix until the evidence points to one hypothesis. If two hypotheses survive, design the discriminating experiment first (see the A/B methodology in `/benchmark` and `.dev-context/next-benchmark-checklist.md` for a worked example).

### Phase 3: Seastar-Specific Failure Modes

| Failure Mode | Symptoms | Check | Rule # |
|--------------|----------|-------|--------|
| Reactor stall | High latency, timeouts, "Reactor stalled for N ms" logs | Blocking calls, unmarked `seastar::async`, loops without `maybe_yield` | #1, #12, #17 |
| Memory exhaustion | OOM, growing RSS | Unbounded queues; pinned `temporary_buffer` shares | #4, #23 |
| Cross-shard deadlock | Hanging requests | `smp::submit_to` patterns, semaphore units vs capacity | #0, #19 |
| Use-after-free | Random segfaults, often under load only | Timer/callback lifetimes, gate holder scope, lambda coroutines, `do_with` copies | #5, #6, #16, #20, #21 |
| Allocator corruption | `free(): invalid pointer`, SIGSEGV with garbage addresses | Cross-shard heap moves; FFI without local reallocation | #14, #15 |
| Connection exhaustion | Connection refused | Pool sizing, leaks | #4 |
| Future leak | "Exceptional future ignored" logs | Missing `co_await` / discarded futures | #18 |
| Routing degradation | Cache-hit ratio drops, TTFT regression, affinity thrashing | Route learn/evict counters, load-aware + residency override interplay (`route_scorer.hpp`); see `.dev-context/investigation-289-routing-regression.md` and the May 22 follow-ups | — |

### Phase 4: Evidence Collection
- [ ] Error logs (with timestamps)
- [ ] Metrics at time of incident (`:9180` Prometheus; reactor stall counters, `ranvier_*` counters)
- [ ] Stack traces if available
- [ ] Recent commit history (`git log --oneline --since=...`)

---

## OUTPUT FORMAT

### Diagnosis
```
Root Cause:         [description]
Affected Component: [file/service/function]
Failure Mode:       [which pattern from Phase 3]
Rule Violated:      [#N or "New pattern"]
```

### Immediate Mitigation
```
Action: [what to do right now — config change, rollback, disable feature flag]
Expected Result: [what should improve]
```

### Permanent Fix
```cpp
// Before (problematic)
[code]

// After (fixed)
[code]
```

---

## POST-INCIDENT ACTIONS
- [ ] **Multi-session investigation?** Write it up in `.dev-context/investigations/<topic>.md`: symptoms, hypotheses tested with evidence, reproduction steps, current best explanation, next experiments. Follow `investigation-289-routing-regression.md` as the exemplar — these files are how the next session picks up where you stopped.
- [ ] New anti-pattern discovered? Run `/extract-pattern` to add it to the Hard Rules.
- [ ] Add alerting for this failure mode?
- [ ] Add test case to prevent regression? (`/doc`)
