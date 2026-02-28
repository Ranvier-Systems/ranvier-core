I have implemented the code for:
<INSERT_TASK_NAME>

The final logic is verified. Now complete the maintenance tasks.

> Ref: `.dev-context/claude-context.md` for build constraints, architecture, coding conventions, and the 24 Hard Rules.

---

## TASK 1: Generate/Update Unit Tests

Location: `tests/unit/`

Requirements:
- Match existing test style and naming conventions
- Cover happy path and error cases
- Test edge cases identified during implementation
- Mock external dependencies appropriately

---

## TASK 2: Update Documentation

Location: `docs/` (excluding assets/)

Update relevant documentation:
- API changes -> Update API docs
- New configuration -> Update config docs
- Architecture changes -> Update architecture docs

---

## TASK 3: Update BACKLOG.md

Mark this task complete:
```markdown
- [x] Task description (completed YYYY-MM-DD)
```

If implementation revealed new work:
```markdown
- [ ] [FOLLOW-UP] New task discovered during implementation
```

---

## OUTPUT FORMAT

### Tests Added/Modified
```
tests/unit/foo_test.cc - Added 3 test cases for new feature
```

### Documentation Updated
```
docs/api.md - Added endpoint documentation for /new-route
```

### BACKLOG.md Changes
```
Line 42: Changed [ ] to [x] for "Implement feature X"
```
