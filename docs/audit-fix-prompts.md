# Adversarial Audit Fix Tracker

Master index for fixing 8 audit findings. Use individual prompt files in order.

---

## Progress Tracker

| # | Issue | Severity | Prompt File | Status |
|---|-------|----------|-------------|--------|
| 1 | Blocking `std::ifstream` in K8s CA cert | CRITICAL | `audit-fix-01-async-io.md` | [ ] |
| 5 | Unbounded `_per_backend_metrics` map | HIGH | `audit-fix-02-bounded-containers.md` | [ ] |
| 6 | Unbounded `_circuits` map | HIGH | `audit-fix-02-bounded-containers.md` | [ ] |
| 8 | Unbounded gossip dedup structures | Medium | `audit-fix-02-bounded-containers.md` | [ ] |
| 4 | MetricsService memory leak | HIGH | `audit-fix-03-lifecycle.md` | [ ] |
| 2 | Timer without gate guard | Medium | `audit-fix-03-lifecycle.md` | [ ] |
| 3 | Null pointer in `metrics()` accessor | Medium | `audit-fix-04-cleanup.md` | [ ] |
| 7 | Connection pool map entry leak | Medium | `audit-fix-04-cleanup.md` | [ ] |

---

## Execution Order

```
┌─────────────────────────────────────────────────────────────────┐
│  1. audit-fix-01-async-io.md                                    │
│     └── Issue #1 (CRITICAL) - Blocking file I/O                 │
│         ↓ verify build                                          │
├─────────────────────────────────────────────────────────────────┤
│  2. audit-fix-02-bounded-containers.md                          │
│     └── Issues #5, #6, #8 (HIGH/Medium) - Unbounded maps        │
│         ↓ verify build                                          │
├─────────────────────────────────────────────────────────────────┤
│  3. audit-fix-03-lifecycle.md                                   │
│     └── Issues #2, #4 (HIGH/Medium) - Timer & metrics safety    │
│         ↓ verify build                                          │
├─────────────────────────────────────────────────────────────────┤
│  4. audit-fix-04-cleanup.md                                     │
│     └── Issues #3, #7 (Medium) - Null safety & map cleanup      │
│         ↓ verify build                                          │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
                    All 8 issues complete
```

---

## Quick Reference

| Prompt File | Issues | Focus | Est. Complexity |
|-------------|--------|-------|-----------------|
| `audit-fix-01-async-io.md` | #1 | Async file I/O | High (API change) |
| `audit-fix-02-bounded-containers.md` | #5, #6, #8 | MAX_SIZE + overflow | Medium (same pattern x3) |
| `audit-fix-03-lifecycle.md` | #2, #4 | Gates + metrics cleanup | Medium |
| `audit-fix-04-cleanup.md` | #3, #7 | Null safety + erase | Low |

---

## Rules Violated Summary

| Rule | Description | Issues |
|------|-------------|--------|
| #1 | No blocking calls on reactor | #1 |
| #3 | Null-guard all pointer returns | #3 |
| #4 | Every container needs MAX_SIZE | #5, #6, #7, #8 |
| #5 | Timer callbacks need gate guards | #2 |
| #6 | Deregister metrics in stop() | #4 |

---

## Files to Modify

| File | Issues | Prompt |
|------|--------|--------|
| `src/k8s_discovery_service.cpp` | #1 | 01 |
| `src/metrics_service.hpp` | #3, #4, #5 | 02, 03, 04 |
| `src/circuit_breaker.hpp` | #6 | 02 |
| `src/gossip_service.hpp` | #8 | 02 |
| `src/connection_pool.hpp` | #2, #7 | 03, 04 |

---

## How to Use

1. **Start a new conversation** for each prompt file
2. **Copy the entire prompt** from the file
3. **Paste as your message** to Claude
4. **Verify build** in Docker after each batch
5. **Update tracker** above when complete

---

## Post-Fix Checklist

After all issues are fixed:

- [ ] Run `claude-review-prompt.md` - Verify 12 Hard Rules compliance
- [ ] Run `claude-pattern-extractor-prompt.md` - Document new learnings
- [ ] Update this tracker - Mark all issues [x]
- [ ] Commit: `fix: address adversarial audit findings (#1-#8)`
- [ ] Update `claude-context.md` with any new prompt guards

---

## New Prompt Guards (to add to context.md after fixes)

```markdown
#### 12. The Synchronous-File-IO Anti-Pattern
**PROMPT GUARD:** "Never use std::ifstream/ofstream in Seastar—use async file operations."

#### 13. The Dangling-Dedup Anti-Pattern
**PROMPT GUARD:** "Deduplication structures must have both MAX_SIZE and TTL-based cleanup."

#### 14. The Orphaned-Map-Entry Anti-Pattern
**PROMPT GUARD:** "When removing entities, erase map entries—don't just clear their contents."
```

