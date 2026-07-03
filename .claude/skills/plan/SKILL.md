---
name: plan
description: Decompose a feature into atomic, verifiable implementation steps before coding. Use when the user describes a feature that spans multiple files or sessions, or asks for a roadmap, breakdown, or plan.
argument-hint: [feature-description]
allowed-tools: Read, Grep, Glob
---

I am STARTING A PLANNING SESSION for the following feature:
$ARGUMENTS

> Ref: `.dev-context/claude-context.md` for build constraints, architecture, coding conventions, and the Hard Rules.

---

## PLANNING CONSTRAINTS

- **Do not write implementation code.** Focus only on the roadmap.
- **Assume Seastar constraints apply.** Flag any step that involves cross-shard communication or async complexity.
- **Read before planning.** Read every file the plan will touch. A plan that names the wrong file or a nonexistent function is worse than no plan.

---

## TASK: Decompose the Feature

### 1. Break into Atomic Steps
Each step should be:
- Independent (can be implemented/tested in isolation)
- Small (completable in one focused session)
- Verifiable (has clear "done" criteria)

### 2. Identify Affected Files
For each step, list the files that will be modified:
```
Step 1: [description]
  - src/foo.cpp
  - src/foo.hpp
  - tests/unit/foo_test.cpp
```

### 3. Flag Architectural Concerns
For each step, check:
- [ ] Requires cross-shard communication? (use `smp::submit_to`)
- [ ] Introduces new async flow? (will need Mermaid diagram in `/implement` Pass 0)
- [ ] Adds new container? (needs MAX_SIZE)
- [ ] Adds timer/callback? (needs gate guard)
- [ ] Adds a whole new service? (use `/new-service` for the scaffolding step)
- [ ] Changes an area with an authoritative deep-dive in `docs/internals/`? (doc update is part of the step, same PR)
- [ ] Touches `router_service.cpp`, `http_controller.cpp`, or `request_rewriter.hpp`? (check `.dev-context/claude-locust-sync-map.md` — the Locust files may need a matching change)

### 4. Identify Dependencies
Which steps depend on others? Create a dependency graph if complex.

---

## OUTPUT FORMAT

```markdown
## Feature: [Name]

### Prerequisites
- [ ] Any setup or refactoring needed first

### Implementation Steps

#### Step 1: [Title]
- **Files:** src/x.cpp, src/x.hpp
- **Description:** [What this step accomplishes]
- **Concerns:** [Any Hard Rules that apply]
- **Status:** [ ] Pending

#### Step 2: [Title]
- **Depends on:** Step 1
- **Files:** ...
...

### Post-Implementation
- [ ] Run `/review` (Hard Rules compliance)
- [ ] Run `/doc` (tests, docs, BACKLOG)
- [ ] Deferred Gates for the developer's Docker build (exact `make` commands — see `/validate`)
```

Save output to BACKLOG.md under a new feature section. Then implement step-by-step with `/implement`.
