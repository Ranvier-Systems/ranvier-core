---
name: doc
description: Post-implementation maintenance - write unit tests, sync documentation, and update BACKLOG.md. Use after code is implemented and reviewed, or when the user asks for tests or doc updates for a change.
argument-hint: [task-name]
---

I have implemented the code for: $ARGUMENTS

The final logic is verified. Now complete the maintenance tasks.

> Ref: `.dev-context/claude-context.md` for build constraints, architecture, coding conventions, and the Hard Rules.

---

## TASK 1: Generate/Update Unit Tests

Location: `tests/unit/<name>_test.cpp` (Google Test, `using namespace ranvier;`, `TEST_F` with fixtures). Mocks live in `tests/unit/mocks/`.

Requirements:
- Match existing test style and naming conventions — read a neighboring `_test.cpp` first.
- Cover happy path, error cases, and edge cases identified during implementation.
- **Reactor-free by default.** Seastar continuations (`.then()`, `.finally()`) segfault without a running reactor. If the code under test touches futures, it needs a policy seam:
  - **Clock injection:** template on `Clock`, test with `TestClock` (`tests/unit/test_clock.hpp`). Exemplars: `circuit_breaker.hpp`, `rate_limiter_core.hpp`, `connection_pool.hpp`.
  - **ClosePolicy injection:** production `AsyncClosePolicy`, test `SyncClosePolicy`. Exemplar: `connection_pool.hpp`.
  - **Pure core extraction:** the logic lives in a reactor-free header (`rate_limiter_core.hpp`, `proxy_retry_policy.hpp`, `route_scorer.hpp`, `gie_epp_plan.hpp` pattern) and the Seastar wrapper stays thin.
- Register the test in `CMakeLists.txt`. Seastar-dependent tests go under the `if(Seastar_FOUND)` block.
- You cannot run the tests in this sandbox. Record a **Deferred Gate**: the exact command (`make test-unit`, or `cd build && ctest -R <TestName> --output-on-failure`) and what passing output looks like.

## TASK 2: Update Documentation

- **Authoritative deep-dives:** if the change touches an area covered by `docs/internals/` (radix tree, prefix routing, tokenization, request lifecycle, shard load balancing, gossip, async persistence, cache residency, GIE EPP, per-API-key attribution), update that doc **in the same PR**. This is a standing project rule.
- API changes → API docs; new config keys → `ranvier.yaml.example` (and `ranvier-local.yaml.example` if relevant) with comments.
- **If you added, removed, or renamed files under `src/`**, update the Source Code Layout section in `.dev-context/claude-context.md`. Stale layout maps mislead every future session.
- **If you changed hashing, prefix extraction, or metrics names** consumed by the load tests, update the Python side per `.dev-context/claude-locust-sync-map.md` and bump its "Last verified" date.

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
tests/unit/foo_test.cpp - Added 3 test cases for new feature
CMakeLists.txt - Registered foo_test (under Seastar_FOUND: yes/no)
```

### Documentation Updated
```
docs/internals/foo.md - Updated section X for new behavior
.dev-context/claude-context.md - Added src/foo.hpp to layout
```

### BACKLOG.md Changes
```
Line 42: Changed [ ] to [x] for "Implement feature X"
```

### Deferred Gates (for the developer's Docker build)
```
make test-unit          # expect: 100% tests passed
```
