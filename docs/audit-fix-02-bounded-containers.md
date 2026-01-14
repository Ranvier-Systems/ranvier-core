# Audit Fix 02: Bounded Containers (HIGH)

**Focus:** Issues #5, #6, #8 - Unbounded maps that can cause OOM

---

1. Ref docs/claude-context.md for the "No Locks/Async Only" rules.
2. Run /compact if the conversation exceeds 4 turns.

## Build Constraints
1. **Static Analysis Only:** Do not attempt to run cmake or build.
2. **API Verification:** Verify syntax against Seastar documentation.
3. **Manual Verification:** I will build in Docker and provide logs if it fails.

---

## THE ISSUES

All three issues share the same root cause: **Rule #4 violation (unbounded containers)**.

| Issue | Severity | Location | Container |
|-------|----------|----------|-----------|
| #5 | HIGH | `src/metrics_service.hpp` | `_per_backend_metrics` map |
| #6 | HIGH | `src/circuit_breaker.hpp` | `_circuits` map |
| #8 | Medium | `src/gossip_service.hpp` | `_seen_messages` set |

### Common Anti-Pattern
```cpp
// ANTI-PATTERN: Grows without bound
std::unordered_map<std::string, Data> _map;

void add_entry(const std::string& key, Data data) {
    _map[key] = data;  // No size check!
}
```

### Why This Is Dangerous
- Malicious or buggy peers can flood entries
- Memory grows until OOM kills the process
- In a 12k LOC codebase, these grow sites are easy to miss
- Cascading failure when one shard OOMs

---

## STANDARD FIX PATTERN

Apply this pattern to all three issues:

```cpp
// 1. Define constant
static constexpr size_t MAX_ENTRIES = 10000;

// 2. Add overflow counter (for metrics)
std::atomic<uint64_t> _overflow_count{0};

// 3. Check before insert
void add_entry(const std::string& key, Data data) {
    if (_map.size() >= MAX_ENTRIES) {
        ++_overflow_count;
        log_warn("Container limit reached ({}), rejecting: {}", MAX_ENTRIES, key);
        return;  // Or throw, or evict oldest
    }
    _map[key] = data;
}
```

---

## ISSUE #5: Unbounded `_per_backend_metrics`

**Location:** `src/metrics_service.hpp` (or `src/router_service.hpp`)

### Problem
```cpp
std::unordered_map<std::string, BackendMetrics> _per_backend_metrics;

void record_metric(const std::string& backend_id, ...) {
    _per_backend_metrics[backend_id].update(...);
}
```

### Fix
```cpp
static constexpr size_t MAX_TRACKED_BACKENDS = 10000;
std::atomic<uint64_t> _backend_metrics_overflow{0};

void record_metric(const std::string& backend_id, ...) {
    auto it = _per_backend_metrics.find(backend_id);
    if (it != _per_backend_metrics.end()) {
        it->second.update(...);
        return;
    }

    // New entry - check bounds
    if (_per_backend_metrics.size() >= MAX_TRACKED_BACKENDS) {
        ++_backend_metrics_overflow;
        log_warn("Backend metrics limit reached, ignoring: {}", backend_id);
        return;
    }

    _per_backend_metrics[backend_id].update(...);
}
```

---

## ISSUE #6: Unbounded `_circuits` map

**Location:** `src/circuit_breaker.hpp`

### Problem
```cpp
std::unordered_map<std::string, CircuitState> _circuits;

CircuitState& get_circuit(const std::string& key) {
    return _circuits[key];  // Creates if missing!
}
```

### Fix
```cpp
static constexpr size_t MAX_CIRCUITS = 10000;
std::atomic<uint64_t> _circuits_overflow{0};

// Return optional instead of reference
std::optional<std::reference_wrapper<CircuitState>> get_circuit(const std::string& key) {
    auto it = _circuits.find(key);
    if (it != _circuits.end()) {
        return std::ref(it->second);
    }

    if (_circuits.size() >= MAX_CIRCUITS) {
        ++_circuits_overflow;
        log_warn("Circuit breaker limit reached, using default for: {}", key);
        return std::nullopt;  // Caller uses default CLOSED state
    }

    auto [new_it, _] = _circuits.emplace(key, CircuitState{});
    return std::ref(new_it->second);
}

// Alternative: Return default state instead of optional
CircuitState& get_or_default_circuit(const std::string& key) {
    static thread_local CircuitState default_closed{};

    auto it = _circuits.find(key);
    if (it != _circuits.end()) {
        return it->second;
    }

    if (_circuits.size() >= MAX_CIRCUITS) {
        ++_circuits_overflow;
        return default_closed;
    }

    return _circuits.emplace(key, CircuitState{}).first->second;
}
```

---

## ISSUE #8: Unbounded gossip dedup

**Location:** `src/gossip_service.hpp`

### Problem
```cpp
std::unordered_set<MessageId> _seen_messages;

bool is_duplicate(const MessageId& id) {
    if (_seen_messages.count(id)) return true;
    _seen_messages.insert(id);  // Never cleaned!
    return false;
}
```

### Fix (with TTL-based expiry)
```cpp
static constexpr size_t MAX_DEDUP_ENTRIES = 100000;
static constexpr auto DEDUP_TTL = std::chrono::minutes(5);

struct TimestampedMessageId {
    MessageId id;
    std::chrono::steady_clock::time_point timestamp;
};

std::deque<TimestampedMessageId> _seen_queue;      // For TTL ordering
std::unordered_set<MessageId> _seen_set;           // For O(1) lookup
std::atomic<uint64_t> _dedup_overflow{0};

bool is_duplicate(const MessageId& id) {
    auto now = std::chrono::steady_clock::now();

    // Cleanup expired entries (amortized)
    while (!_seen_queue.empty()) {
        const auto& oldest = _seen_queue.front();
        if (now - oldest.timestamp < DEDUP_TTL &&
            _seen_set.size() < MAX_DEDUP_ENTRIES) {
            break;
        }
        _seen_set.erase(oldest.id);
        _seen_queue.pop_front();
    }

    // Check duplicate
    if (_seen_set.count(id)) {
        return true;
    }

    // Bounds check for new entry
    if (_seen_set.size() >= MAX_DEDUP_ENTRIES) {
        ++_dedup_overflow;
        // Force evict oldest even if not expired
        if (!_seen_queue.empty()) {
            _seen_set.erase(_seen_queue.front().id);
            _seen_queue.pop_front();
        }
    }

    // Insert new
    _seen_set.insert(id);
    _seen_queue.push_back({id, now});
    return false;
}
```

---

## STAGED EXECUTION

### Pass 0: Identify All Affected Files
```
src/metrics_service.hpp   → Issue #5
src/circuit_breaker.hpp   → Issue #6
src/gossip_service.hpp    → Issue #8
```

### Pass 1: Apply Standard Pattern to Each
For each container:
1. Add `MAX_*` constant
2. Add `_*_overflow` atomic counter
3. Add size check before insert
4. Log at warn level when limit hit

### Pass 2: Expose Overflow Metrics
```cpp
// In metrics registration
_metrics.add_group("limits", {
    sm::make_counter("backend_metrics_overflow", _backend_metrics_overflow,
        sm::description("Times backend metrics limit was hit")),
    sm::make_counter("circuits_overflow", _circuits_overflow,
        sm::description("Times circuit breaker limit was hit")),
    sm::make_counter("dedup_overflow", _dedup_overflow,
        sm::description("Times gossip dedup limit was hit")),
});
```

### Pass 3: Verify Consistency
- [ ] All three use same pattern
- [ ] Constants are reasonable (10k-100k range)
- [ ] Overflow counters are atomic (for metrics access)
- [ ] Warn logs include context

---

## OUTPUT FORMAT

```
=== ISSUE #5: Unbounded _per_backend_metrics ===
File: src/metrics_service.hpp
Lines: X-Y
[Code changes]

=== ISSUE #6: Unbounded _circuits ===
File: src/circuit_breaker.hpp
Lines: X-Y
[Code changes]

=== ISSUE #8: Unbounded gossip dedup ===
File: src/gossip_service.hpp
Lines: X-Y
[Code changes]

=== NEW CONSTANTS (for reference) ===
MAX_TRACKED_BACKENDS = 10000
MAX_CIRCUITS = 10000
MAX_DEDUP_ENTRIES = 100000
DEDUP_TTL = 5 minutes
```

---

## PROMPT GUARD (add to context.md)

"Every push_back, emplace_back, insert, or operator[] that can create new map entries must have a corresponding size check against a MAX_* constant, with overflow logged at warn level."

---

## NEXT STEP

After these fixes are verified:
→ Proceed to `audit-fix-03-lifecycle.md`

