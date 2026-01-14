# Audit Fix 03: Lifecycle & Gate Guards

**Focus:** Issues #2, #4 - Timer/callback safety and metrics cleanup

---

1. Ref docs/claude-context.md for the "No Locks/Async Only" rules.
2. Run /compact if the conversation exceeds 4 turns.

## Build Constraints
1. **Static Analysis Only:** Do not attempt to run cmake or build.
2. **API Verification:** Verify syntax against Seastar documentation.
3. **Manual Verification:** I will build in Docker and provide logs if it fails.

---

## THE ISSUES

Both issues relate to object lifetime and safe shutdown:

| Issue | Severity | Rule | Location | Problem |
|-------|----------|------|----------|---------|
| #2 | Medium | #5 | `src/connection_pool.hpp` | Timer callback without gate guard |
| #4 | HIGH | #6 | `src/metrics_service.hpp` | Metrics lambdas not deregistered in stop() |

---

## ISSUE #2: Timer Without Gate Guard

**Rule Violated:** #5 (Timer callbacks need gate guards)

**Location:** `src/connection_pool.hpp`

### The Race Condition
```
Timeline:
1. Timer fires, callback queued on reactor
2. stop() called, cancels timer (but callback already queued)
3. stop() returns, destructor runs
4. Queued callback executes with dangling 'this' → USE-AFTER-FREE
```

### Problem Code
```cpp
class ConnectionPool {
    seastar::timer<> _cleanup_timer;

    void start() {
        _cleanup_timer.set_callback([this] {
            cleanup_idle_connections();  // Captures 'this' unsafely
        });
        _cleanup_timer.arm_periodic(std::chrono::seconds(30));
    }

    seastar::future<> stop() {
        _cleanup_timer.cancel();
        co_return;  // Callback may still be queued!
    }
};
```

### Fix
```cpp
class ConnectionPool {
    seastar::timer<> _cleanup_timer;
    seastar::gate _timer_gate;  // ADD: Gate for callback safety

    void start() {
        _cleanup_timer.set_callback([this] {
            // Guard: fail fast if shutting down
            auto holder = _timer_gate.try_hold();
            if (!holder) {
                return;  // Gate closed, skip callback
            }
            cleanup_idle_connections();
        });
        _cleanup_timer.arm_periodic(std::chrono::seconds(30));
    }

    seastar::future<> stop() {
        // FIRST: Close gate and wait for in-flight callbacks
        co_await _timer_gate.close();

        // THEN: Cancel timer (safe now, no callbacks can run)
        _cleanup_timer.cancel();
    }
};
```

### Key Points
1. `_timer_gate.close()` waits for any in-flight `hold()` to complete
2. After close, `try_hold()` returns empty optional
3. Cancel timer AFTER gate is closed, not before

---

## ISSUE #4: Metrics Lambda Outlives Service

**Rule Violated:** #6 (Deregister metrics in stop() before destruction)

**Location:** `src/metrics_service.hpp`

### The Problem
```cpp
class MetricsService {
    seastar::metrics::metric_groups _metrics;
    uint64_t _request_count{0};

    void register_metrics() {
        _metrics.add_group("svc", {
            seastar::metrics::make_counter("requests", [this] {
                return _request_count;  // Captures 'this'
            }),
        });
    }

    seastar::future<> stop() {
        // Missing: _metrics.clear()
        // Prometheus can still scrape after this returns!
        co_return;
    }
};
```

### The Race
```
Timeline:
1. stop() called, begins cleanup
2. Prometheus scrape arrives, calls metrics lambda
3. Lambda accesses _request_count on partially-destroyed object
4. USE-AFTER-FREE or garbage data
```

### Fix
```cpp
class MetricsService {
    seastar::metrics::metric_groups _metrics;
    uint64_t _request_count{0};

    void register_metrics() {
        _metrics.add_group("svc", {
            seastar::metrics::make_counter("requests", [this] {
                return _request_count;
            }),
        });
    }

    seastar::future<> stop() {
        // FIRST: Deregister all metrics before any other cleanup
        _metrics.clear();

        // Now safe to destroy members
        // ... other cleanup ...

        co_return;
    }
};
```

### Key Points
1. `_metrics.clear()` must be the FIRST action in stop()
2. This removes all lambdas from Prometheus scrape path
3. Only then is it safe to destroy captured state

---

## STAGED EXECUTION

### Pass 0: Identify All Timer and Metrics Patterns

Search for:
```bash
# Timers capturing 'this'
grep -rn "set_callback.*\[this\]" src/

# Metrics with lambdas
grep -rn "make_counter\|make_gauge" src/
```

### Pass 1: Fix Connection Pool Timer
```
src/connection_pool.hpp:
1. Add: seastar::gate _timer_gate;
2. Modify: callback to use try_hold()
3. Modify: stop() to close gate first
```

### Pass 2: Fix Metrics Service
```
src/metrics_service.hpp:
1. Add: _metrics.clear() as first line of stop()
2. Verify no other cleanup runs before clear()
```

### Pass 3: Check for Similar Patterns Elsewhere
Verify no other timers or metrics have the same issue:
- [ ] All timer callbacks use gate guards
- [ ] All metrics groups clear in stop()
- [ ] stop() order is: clear metrics → close gates → cancel timers → other cleanup

---

## COMPONENT DIAGRAM

```
┌────────────────────────────────────────────────────────────┐
│                     Service Lifecycle                       │
├────────────────────────────────────────────────────────────┤
│                                                            │
│   start()                          stop()                  │
│     │                                │                     │
│     ▼                                ▼                     │
│   register_metrics()          1. _metrics.clear()         │
│     │                                │                     │
│     ▼                                ▼                     │
│   arm_timer()                 2. co_await _gate.close()   │
│     │                                │                     │
│     ▼                                ▼                     │
│   [running]                   3. _timer.cancel()          │
│                                      │                     │
│                                      ▼                     │
│                               4. other cleanup            │
│                                                            │
└────────────────────────────────────────────────────────────┘
```

---

## OUTPUT FORMAT

```
=== ISSUE #2: Timer Gate Guard ===
File: src/connection_pool.hpp

Added member:
    seastar::gate _timer_gate;

Modified callback:
[code]

Modified stop():
[code]

=== ISSUE #4: Metrics Deregistration ===
File: src/metrics_service.hpp

Modified stop():
[code showing _metrics.clear() as first line]
```

---

## PROMPT GUARDS (add to context.md)

**For timers:**
"Any lambda capturing 'this' for a timer or async callback must acquire a gate holder at entry and the owning class must close that gate before cancelling the timer."

**For metrics:**
"Any metrics lambda capturing 'this' must be deregistered in stop() before any other cleanup."

---

## NEXT STEP

After these fixes are verified:
→ Proceed to `audit-fix-04-cleanup.md`

