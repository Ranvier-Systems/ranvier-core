1. Ref docs/claude-context.md for the "No Locks/Async Only" rules.
2. Run /compact if the conversation exceeds 4 turns.

Build Constraints:
1. Static Analysis Only: Do not attempt to run cmake or build. Seastar dependencies are too heavy for the sandbox.
2. API Verification: Verify syntax against Seastar documentation logic.
3. Manual Verification: I will build in my Docker container and provide logs if it fails.

---

## INCIDENT REPORT

**Severity:** [P0-Critical / P1-High / P2-Medium / P3-Low]
- P0: Complete service outage
- P1: Major feature broken, no workaround
- P2: Feature degraded, workaround exists
- P3: Minor issue, low impact

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

---

## INCIDENT RESPONSE PROTOCOL

### Phase 1: Immediate Assessment (< 5 min)
Answer these questions first:
1. Is the service completely down or degraded?
2. Is it affecting all users or a subset?
3. Is it getting worse, stable, or recovering?
4. Can we rollback to a known good state?

### Phase 2: Hypothesis Generation
Based on symptoms, generate hypotheses:

| # | Hypothesis | Supporting Evidence | Contradicting Evidence | Quick Test |
|---|------------|---------------------|------------------------|------------|
| 1 | [hypothesis] | [evidence] | [evidence] | [how to verify] |
| 2 | ... | ... | ... | ... |

### Phase 3: Seastar-Specific Failure Modes
Check these common Seastar failure patterns:

| Failure Mode | Symptoms | Check | Rule # |
|--------------|----------|-------|--------|
| Reactor stall | High latency, timeouts | `seastar::metrics::reactor_stalls` | #1, #2 |
| Memory exhaustion | OOM, growing RSS | Check unbounded queues | #4 |
| Cross-shard deadlock | Hanging requests | Check `smp::submit_to` patterns | #0 |
| Use-after-free | Random segfaults | Check timer/callback lifetimes | #5, #6 |
| Connection exhaustion | Connection refused | Check pool sizing, leaks | #4 |
| Future leak | "future leaked" logs | Check missing `co_await` | - |

### Phase 4: Evidence Collection
Gather:
- [ ] Error logs (with timestamps)
- [ ] Metrics at time of incident
- [ ] Stack traces if available
- [ ] Recent commit history
- [ ] Configuration changes

---

## OUTPUT FORMAT

### Diagnosis
```
Root Cause:       [description]
Affected Component: [file/service/function]
Failure Mode:     [which pattern from Phase 3]
Rule Violated:    [#N from the list of Hard Rules, or "New pattern"]
```

### Immediate Mitigation
**Goal:** Stop the bleeding. May be temporary.
```
Action: [what to do right now]
Command: [if applicable]
Expected Result: [what should improve]
```

### Permanent Fix
**Goal:** Proper solution preventing recurrence.
```cpp
// Location: [file:line]

// Before (problematic)
[code]

// After (fixed)
[code]
```

### Verification
1. [How to verify mitigation worked]
2. [How to verify permanent fix]
3. [How to confirm no regression]

---

## POST-INCIDENT ACTIONS

### Documentation
- [ ] Add to claude-context.md anti-patterns? → Use `claude-pattern-extractor-prompt.md`
- [ ] Update runbooks/playbooks?

### Monitoring
- [ ] Add alerting for this failure mode?
- [ ] Add metrics to detect early warning signs?

### Testing
- [ ] Add test case to prevent regression?
- [ ] Add chaos testing for this scenario?

### Follow-up
- [ ] Schedule post-mortem meeting?
- [ ] Create follow-up tickets for systemic fixes?

