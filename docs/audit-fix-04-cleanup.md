# Audit Fix 04: Null Safety & Resource Cleanup

**Focus:** Issues #3, #7 - Null pointer safety and map entry cleanup

---

1. Ref docs/claude-context.md for the "No Locks/Async Only" rules.
2. Run /compact if the conversation exceeds 4 turns.

## Build Constraints
1. **Static Analysis Only:** Do not attempt to run cmake or build.
2. **API Verification:** Verify syntax against Seastar documentation.
3. **Manual Verification:** I will build in Docker and provide logs if it fails.

---

## THE ISSUES

| Issue | Severity | Rule | Location | Problem |
|-------|----------|------|----------|---------|
| #3 | Medium | #3 | `src/metrics_service.hpp` | Null pointer dereference in accessor |
| #7 | Medium | #4 | `src/connection_pool.hpp` | Map entries not erased on backend removal |

---

## ISSUE #3: Null Pointer Dereference in `metrics()` Accessor

**Rule Violated:** #3 (Null-guard all C string returns / pointer accessors)

**Location:** `src/metrics_service.hpp`

### Problem Code
```cpp
class MetricsService {
    MetricsData* _metrics_ptr = nullptr;

public:
    // ANTI-PATTERN: Returns raw pointer, caller may dereference null
    MetricsData* metrics() const { return _metrics_ptr; }
};

// Caller code:
auto value = svc.metrics()->request_count;  // CRASH if null!
```

### Why This Happens
- Pointer may be null before initialization
- Pointer may be null after cleanup/reset
- Caller assumes non-null without checking

### Fix Options

**Option A: Return `std::optional` (safest)**
```cpp
std::optional<std::reference_wrapper<const MetricsData>> metrics() const {
    if (!_metrics_ptr) {
        return std::nullopt;
    }
    return std::cref(*_metrics_ptr);
}

// Caller:
if (auto m = svc.metrics()) {
    auto value = m->get().request_count;
}
```

**Option B: Return reference with static default (convenient)**
```cpp
const MetricsData& metrics() const {
    static const MetricsData empty{};  // Zero-initialized default
    return _metrics_ptr ? *_metrics_ptr : empty;
}

// Caller (unchanged, but safe):
auto value = svc.metrics().request_count;  // Returns 0 if not initialized
```

**Option C: Assert non-null (if truly invariant)**
```cpp
const MetricsData& metrics() const {
    assert(_metrics_ptr && "metrics() called before initialization");
    return *_metrics_ptr;
}
```

### Recommendation
- Use **Option B** for metrics (zero is a safe default)
- Use **Option A** for data where null has meaning
- Use **Option C** only if null is truly a programming error

---

## ISSUE #7: Connection Pool Map Entry Leak

**Rule Violated:** #4 (Cleanup / bounded containers)

**Location:** `src/connection_pool.hpp`

### Problem Code
```cpp
class ConnectionPool {
    std::unordered_map<std::string, ConnectionList> _pools;

public:
    seastar::future<> remove_backend(const std::string& backend_id) {
        auto it = _pools.find(backend_id);
        if (it != _pools.end()) {
            co_await it->second.close_all();  // Closes connections...
            // BUG: Map entry still exists! Memory leak over time.
        }
    }
};
```

### Why This Is a Leak
- Backend is removed from routing
- Connections are closed
- But `_pools[backend_id]` entry remains forever
- Over time (backends added/removed), map grows unbounded

### Fix
```cpp
seastar::future<> remove_backend(const std::string& backend_id) {
    auto it = _pools.find(backend_id);
    if (it != _pools.end()) {
        // Close all connections first
        co_await it->second.close_all();

        // THEN erase the map entry
        _pools.erase(it);

        log_info("Removed backend pool: {}", backend_id);
    }
}
```

### Alternative: Extract then erase (if close_all might throw)
```cpp
seastar::future<> remove_backend(const std::string& backend_id) {
    auto it = _pools.find(backend_id);
    if (it == _pools.end()) {
        co_return;
    }

    // Move out of map first
    ConnectionList connections = std::move(it->second);
    _pools.erase(it);  // Entry removed immediately

    // Close connections (map entry already gone)
    try {
        co_await connections.close_all();
    } catch (const std::exception& e) {
        log_warn("Error closing connections for {}: {}", backend_id, e.what());
        // Connections will be cleaned up by destructor
    }
}
```

---

## STAGED EXECUTION

### Pass 1: Fix Null Pointer Issue
```
src/metrics_service.hpp:
1. Change return type of metrics() accessor
2. Choose Option A (optional) or Option B (default)
3. Update any callers if using Option A
```

### Pass 2: Fix Map Entry Leak
```
src/connection_pool.hpp:
1. Find remove_backend() or equivalent
2. Add _pools.erase(it) after close_all()
3. Consider move-then-erase pattern for exception safety
```

### Pass 3: Search for Similar Patterns
```bash
# Find other accessors returning raw pointers
grep -rn "return _.*ptr\|return _.*_\*" src/*.hpp

# Find map operations that clear but don't erase
grep -rn "\.clear()\|close_all()" src/*.cpp | grep -v erase
```

---

## VERIFICATION CHECKLIST

### For Issue #3 (Null safety)
- [ ] No raw pointer returned from public accessor
- [ ] Caller code updated if signature changed
- [ ] Default value is sensible (zero for metrics)

### For Issue #7 (Map cleanup)
- [ ] `erase()` called after closing/clearing entry
- [ ] Exception safety considered (move-then-erase)
- [ ] Log message confirms removal

---

## OUTPUT FORMAT

```
=== ISSUE #3: Null Pointer in metrics() ===
File: src/metrics_service.hpp
Lines: X-Y

Before:
    MetricsData* metrics() const { return _metrics_ptr; }

After:
    const MetricsData& metrics() const {
        static const MetricsData empty{};
        return _metrics_ptr ? *_metrics_ptr : empty;
    }

Callers updated: [list any, or "None - API compatible"]

=== ISSUE #7: Map Entry Leak ===
File: src/connection_pool.hpp
Lines: X-Y

Before:
    co_await it->second.close_all();
    // Missing erase

After:
    co_await it->second.close_all();
    _pools.erase(it);
    log_info("Removed backend pool: {}", backend_id);
```

---

## PROMPT GUARDS (add to context.md)

**For null safety:**
"Never return raw pointers from public accessors—use std::optional, references with static defaults, or require initialization as a precondition."

**For map cleanup:**
"When removing logical entities, ensure all associated map entries are erased, not just cleared or closed."

---

## COMPLETION

After this fix is verified, all 8 audit issues are addressed.

### Final Checklist
- [ ] Issue #1: Blocking ifstream → async I/O
- [ ] Issue #2: Timer gate guard added
- [ ] Issue #3: Null pointer accessor fixed
- [ ] Issue #4: Metrics deregistration in stop()
- [ ] Issue #5: _per_backend_metrics bounded
- [ ] Issue #6: _circuits map bounded
- [ ] Issue #7: Pool map entries erased
- [ ] Issue #8: Gossip dedup bounded + TTL

### Next Steps
1. Run `claude-review-prompt.md` for final compliance check
2. Run `claude-pattern-extractor-prompt.md` to formalize learnings
3. Commit with: `fix: address adversarial audit findings (#1-#8)`

