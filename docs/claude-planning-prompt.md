1. Ref docs/claude-context.md for the "No Locks/Async Only" rules.
2. DO NOT read the full /docs or /assets folders.
3. Run /compact if the conversation exceeds 4 turns.

Build Constraints:
1. **Static Analysis Only:** Do not attempt to run cmake or build. Seastar dependencies are too heavy for the sandbox.
2. **API Verification:** Verify syntax against Seastar documentation logic.
3. **Manual Verification:** I will build in my Docker container and provide logs if it fails.

---

I am STARTING A PLANNING SESSION for the following feature:
<INSERT_FEATURE_DESCRIPTION>

---

## PLANNING CONSTRAINTS

- **Do not write implementation code.** Focus only on the roadmap.
- **Assume Seastar constraints apply.** Flag any step that involves cross-shard communication or async complexity.

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
  - src/services/foo.cc
  - src/services/foo.hh
  - tests/unit/foo_test.cc
```

### 3. Flag Architectural Concerns
For each step, check:
- [ ] Requires cross-shard communication? (Note: use `smp::submit_to`)
- [ ] Introduces new async flow? (Note: will need Mermaid diagram)
- [ ] Adds new container? (Note: needs MAX_SIZE)
- [ ] Adds timer/callback? (Note: needs gate guard)

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
- **Files:** src/x.cc, src/x.hh
- **Description:** [What this step accomplishes]
- **Concerns:** [Any rules that apply]
- **Status:** [ ] Pending

#### Step 2: [Title]
- **Depends on:** Step 1
- **Files:** ...
...

### Post-Implementation
- [ ] Run claude-review-prompt.md
- [ ] Run claude-doc-prompt.md
- [ ] Update TODO.md
```

Save output to docs/TODO.md under a new feature section.

