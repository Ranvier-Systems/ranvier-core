1. Ref docs/claude-context.md for the "No Locks/Async Only" rules.
2. DO NOT read the full /docs or /assets folders.

Build Constraints:
1. Static Analysis Only: Do not attempt to run cmake or build. Seastar dependencies are too heavy for the sandbox.
2. API Verification: Verify syntax against Seastar documentation logic.
3. Manual Verification: I will build in my Docker container and provide logs if it fails.

---

I have implemented the code for: <INSERT_TASK_NAME>

The final logic is verified. Now complete the maintenance tasks.

---

## PRE-DOCUMENTATION VERIFICATION

Before writing tests or docs, verify the implementation passes these checks:

### Code Quality Checklist
- [ ] No new `std::shared_ptr` (or justified use of `lw_shared_ptr`)
- [ ] No new `std::mutex` in reactor-callable code
- [ ] No sequential `co_await` in loops
- [ ] All C string returns null-guarded
- [ ] All new containers have MAX_SIZE defined
- [ ] All timers/callbacks with `this` capture have gate guards
- [ ] All metrics lambdas with `this` deregister in `stop()`
- [ ] No validation logic in persistence layer
- [ ] All catch blocks log at warn level with context
- [ ] String-to-number uses `std::from_chars` with validation

### If Any Check Fails
Stop and fix the implementation before proceeding to documentation.

---

## TASK 1: Generate/Update Unit Tests

Location: `tests/unit/`

Requirements:
- Match existing test style and naming conventions
- Cover happy path and error cases
- Test edge cases identified during implementation
- Mock external dependencies appropriately

```cpp
// Example test structure
SEASTAR_TEST_CASE(test_feature_happy_path) {
    // Arrange
    // Act
    // Assert
}

SEASTAR_TEST_CASE(test_feature_error_handling) {
    // Test that errors are handled gracefully
}
```

---

## TASK 2: Update Documentation

Location: `docs/` (excluding assets/)

Update relevant documentation:
- API changes → Update API docs
- New configuration → Update config docs
- Architecture changes → Update architecture docs
- New concepts → Add explanation

Maintain existing documentation tone and format.

---

## TASK 3: Update TODO.md

Provide the specific line to mark this task complete:

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

### TODO.md Changes
```
Line 42: Changed [ ] to [x] for "Implement feature X"
Line 43: Added new follow-up task
```

