## INCIDENT REPORT

**Severity:** [P0-Critical / P1-High / P2-Medium / P3-Low]

**Summary:** <ONE_LINE_DESCRIPTION>

**Symptoms:**
- <SYMPTOM_1>
- <SYMPTOM_2>

**Timeline:**
- **First observed:** [timestamp]
- **Recent changes:** [deployments, config changes]
- **Frequency:** [constant / intermittent / under load only]

**Affected scope:**
- [ ] All shards
- [ ] Specific shard(s): [which]
- [ ] All requests
- [ ] Specific request pattern: [which]

> Ref: `.dev-context/claude-context.md` for build constraints, architecture, coding conventions, and the 16 Hard Rules.

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

### Phase 3: Seastar-Specific Failure Modes

| Failure Mode | Symptoms | Check | Rule # |
|--------------|----------|-------|--------|
| Reactor stall | High latency, timeouts | `seastar::metrics::reactor_stalls` | #1, #2 |
| Memory exhaustion | OOM, growing RSS | Check unbounded queues | #4 |
| Cross-shard deadlock | Hanging requests | Check `smp::submit_to` patterns | #0 |
| Use-after-free | Random segfaults | Check timer/callback lifetimes | #5, #6 |
| Connection exhaustion | Connection refused | Check pool sizing, leaks | #4 |
| Future leak | "future leaked" logs | Check missing `co_await` | - |

### Phase 4: Evidence Collection
- [ ] Error logs (with timestamps)
- [ ] Metrics at time of incident
- [ ] Stack traces if available
- [ ] Recent commit history

---

## OUTPUT FORMAT

### Diagnosis
```
Root Cause:       [description]
Affected Component: [file/service/function]
Failure Mode:     [which pattern from Phase 3]
Rule Violated:    [#N or "New pattern"]
```

### Immediate Mitigation
```
Action: [what to do right now]
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
- [ ] Add to claude-context.md anti-patterns? -> Use `claude-pattern-extractor-prompt.md`
- [ ] Add alerting for this failure mode?
- [ ] Add test case to prevent regression?
