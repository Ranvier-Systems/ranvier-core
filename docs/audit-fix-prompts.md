# Adversarial Audit Fix Prompts

Generated from adversarial audit findings. Use with `claude-impl-prompt.md` workflow.

---

1. Ref docs/claude-context.md for the "No Locks/Async Only" rules.
2. DO NOT read the full /docs or /assets folders.
3. Run /compact if the conversation exceeds 4 turns.

## Build Constraints
1. **Static Analysis Only:** Do not attempt to run cmake or build. Seastar dependencies are too heavy for the sandbox.
2. **API Verification:** Verify syntax against Seastar documentation logic.
3. **Manual Verification:** I will build in my Docker container and provide logs if it fails.

---

## AUDIT FINDINGS TO FIX

### Progress Tracker

| # | Issue | Severity | Rule | Status |
|---|-------|----------|------|--------|
| 1 | Blocking `std::ifstream` in K8s CA cert loading | CRITICAL | #1 | [ ] Pending |
| 2 | Connection pool timer without gate guard | Medium | #5 | [ ] Pending |
| 3 | Null pointer dereference in `metrics()` accessor | Medium | #3 | [ ] Pending |
| 4 | Thread-local MetricsService memory leak | HIGH | #6 | [ ] Pending |
| 5 | Unbounded `_per_backend_metrics` map | HIGH | #4 | [ ] Pending |
| 6 | Unbounded `_circuits` map in CircuitBreaker | HIGH | #4 | [ ] Pending |
| 7 | Connection pool map entry leak | Medium | #4 | [ ] Pending |
| 8 | Unbounded gossip dedup structures | Medium | #4 | [ ] Pending |

---

## ISSUE DETAILS

### Issue 1: Blocking `std::ifstream` in K8s CA cert loading [CRITICAL]

**Rule Violated:** #1 (No blocking calls on reactor thread)

**Location:** `src/k8s_discovery_service.cpp` (verify exact line)

**Problem:**
```cpp
// ANTI-PATTERN: Blocks the Seastar reactor
std::ifstream ca_file(ca_cert_path);
std::string ca_cert((std::istreambuf_iterator<char>(ca_file)),
                     std::istreambuf_iterator<char>());
```

**Required Fix:** Replace with Seastar async file I/O:
```cpp
// Use seastar::open_file_dma + read
co_await seastar::with_file(
    co_await seastar::open_file_dma(ca_cert_path, seastar::open_flags::ro),
    [&ca_cert](seastar::file& f) -> seastar::future<> {
        auto size = co_await f.size();
        auto buf = co_await f.dma_read_bulk<char>(0, size);
        ca_cert = seastar::sstring(buf.get(), buf.size());
    }
);
```

**Prompt Guard:** "Never use std::ifstream, std::ofstream, or any synchronous file I/O—use Seastar's async file operations."

---

### Issue 2: Connection pool timer without gate guard [Medium]

**Rule Violated:** #5 (Timer callbacks need gate guards)

**Location:** `src/connection_pool.hpp` (verify exact line)

**Problem:**
```cpp
// ANTI-PATTERN: Timer callback captures 'this' without gate protection
_cleanup_timer.set_callback([this] {
    cleanup_idle_connections();
});
```

**Required Fix:**
```cpp
// Add gate to class members
seastar::gate _timer_gate;

// In callback
_cleanup_timer.set_callback([this] {
    if (_timer_gate.is_closed()) return;
    auto holder = _timer_gate.try_hold();
    if (!holder) return;
    cleanup_idle_connections();
});

// In stop()
co_await _timer_gate.close();  // FIRST
_cleanup_timer.cancel();        // THEN cancel
```

**Prompt Guard:** "Any lambda capturing 'this' for a timer or async callback must acquire a gate holder at entry and the owning class must close that gate before cancelling the timer."

---

### Issue 3: Null pointer dereference in `metrics()` accessor [Medium]

**Rule Violated:** #3 (Null-guard all C string returns)

**Location:** `src/metrics_service.hpp` (verify exact line)

**Problem:**
```cpp
// ANTI-PATTERN: May return null or access uninitialized pointer
const MetricsData* metrics() const { return _metrics_ptr; }
// Caller does: auto val = svc.metrics()->some_field;  // CRASH if null
```

**Required Fix:**
```cpp
// Option A: Return optional
std::optional<std::reference_wrapper<const MetricsData>> metrics() const {
    if (!_metrics_ptr) return std::nullopt;
    return std::cref(*_metrics_ptr);
}

// Option B: Return with default
const MetricsData& metrics() const {
    static const MetricsData empty{};
    return _metrics_ptr ? *_metrics_ptr : empty;
}
```

**Prompt Guard:** "Never return raw pointers from accessors—use std::optional, references with defaults, or explicit null checks at call sites."

---

### Issue 4: Thread-local MetricsService memory leak [HIGH]

**Rule Violated:** #6 (Deregister metrics in stop() before destruction)

**Location:** `src/metrics_service.hpp` (verify exact line)

**Problem:**
```cpp
// ANTI-PATTERN: Metrics lambdas capturing 'this' outlive the service
class MetricsService {
    seastar::metrics::metric_groups _metrics;

    void register_metrics() {
        _metrics.add_group("svc", {
            seastar::metrics::make_gauge("count", [this] { return _count; })
        });
    }
    // Missing: deregistration in stop()
};
```

**Required Fix:**
```cpp
seastar::future<> stop() {
    _metrics.clear();  // FIRST: deregister all metrics
    // ... then other cleanup
    co_return;
}
```

**Prompt Guard:** "Any metrics lambda capturing 'this' must be deregistered in stop() before any other cleanup."

---

### Issue 5: Unbounded `_per_backend_metrics` map [HIGH]

**Rule Violated:** #4 (Every growing container needs MAX_SIZE)

**Location:** `src/metrics_service.hpp` or `src/router_service.hpp` (verify)

**Problem:**
```cpp
// ANTI-PATTERN: Grows without bound as backends are added/removed
std::unordered_map<std::string, BackendMetrics> _per_backend_metrics;

void record_backend_metric(const std::string& backend_id, ...) {
    _per_backend_metrics[backend_id].update(...);  // Unbounded growth!
}
```

**Required Fix:**
```cpp
static constexpr size_t MAX_TRACKED_BACKENDS = 10000;

void record_backend_metric(const std::string& backend_id, ...) {
    if (_per_backend_metrics.size() >= MAX_TRACKED_BACKENDS) {
        // Option A: Reject new entries
        log_warn("Backend metrics limit reached, ignoring: {}", backend_id);
        ++_metrics_overflow_count;
        return;

        // Option B: LRU eviction (more complex)
    }
    _per_backend_metrics[backend_id].update(...);
}
```

**Prompt Guard:** "Every push_back, emplace_back, insert, or operator[] on a growing container must have a corresponding size check or the container must be bounded by design."

---

### Issue 6: Unbounded `_circuits` map in CircuitBreaker [HIGH]

**Rule Violated:** #4 (Every growing container needs MAX_SIZE)

**Location:** `src/circuit_breaker.hpp`

**Problem:**
```cpp
// ANTI-PATTERN: Circuit states accumulate forever
std::unordered_map<std::string, CircuitState> _circuits;

CircuitState& get_circuit(const std::string& key) {
    return _circuits[key];  // Creates entry if missing, unbounded!
}
```

**Required Fix:**
```cpp
static constexpr size_t MAX_CIRCUITS = 10000;
std::atomic<uint64_t> _circuits_overflow{0};  // For metrics

std::optional<std::reference_wrapper<CircuitState>> get_circuit(const std::string& key) {
    auto it = _circuits.find(key);
    if (it != _circuits.end()) {
        return std::ref(it->second);
    }

    if (_circuits.size() >= MAX_CIRCUITS) {
        ++_circuits_overflow;
        return std::nullopt;  // Or use default CLOSED state
    }

    return std::ref(_circuits.emplace(key, CircuitState{}).first->second);
}
```

**Prompt Guard:** "Every push_back, emplace_back, insert, or operator[] on a growing container must have a corresponding size check or the container must be bounded by design."

---

### Issue 7: Connection pool map entry leak [Medium]

**Rule Violated:** #4 (Bounded containers / cleanup)

**Location:** `src/connection_pool.hpp`

**Problem:**
```cpp
// ANTI-PATTERN: Pool entries created but never removed when backend disappears
std::unordered_map<std::string, ConnectionList> _pools;

void remove_backend(const std::string& backend_id) {
    // Connections closed but map entry remains
    _pools[backend_id].close_all();
    // Missing: _pools.erase(backend_id);
}
```

**Required Fix:**
```cpp
seastar::future<> remove_backend(const std::string& backend_id) {
    auto it = _pools.find(backend_id);
    if (it != _pools.end()) {
        co_await it->second.close_all();
        _pools.erase(it);  // Clean up the entry
    }
}
```

**Prompt Guard:** "When removing logical entities, ensure all associated map entries are erased, not just cleared."

---

### Issue 8: Unbounded gossip dedup structures [Medium]

**Rule Violated:** #4 (Every growing container needs MAX_SIZE)

**Location:** `src/gossip_service.hpp` / `src/gossip_service.cpp`

**Problem:**
```cpp
// ANTI-PATTERN: Dedup set grows forever
std::unordered_set<MessageId> _seen_messages;

bool is_duplicate(const MessageId& id) {
    if (_seen_messages.count(id)) return true;
    _seen_messages.insert(id);  // Never cleaned up!
    return false;
}
```

**Required Fix:**
```cpp
static constexpr size_t MAX_DEDUP_ENTRIES = 100000;
static constexpr size_t DEDUP_CLEANUP_BATCH = 10000;

// Option A: Time-based expiry with bounded size
struct TimestampedId {
    MessageId id;
    std::chrono::steady_clock::time_point seen_at;
};
std::deque<TimestampedId> _seen_messages_queue;
std::unordered_set<MessageId> _seen_messages_set;

bool is_duplicate(const MessageId& id) {
    // Cleanup old entries
    auto now = std::chrono::steady_clock::now();
    while (!_seen_messages_queue.empty() &&
           (now - _seen_messages_queue.front().seen_at > DEDUP_TTL ||
            _seen_messages_set.size() >= MAX_DEDUP_ENTRIES)) {
        _seen_messages_set.erase(_seen_messages_queue.front().id);
        _seen_messages_queue.pop_front();
    }

    if (_seen_messages_set.count(id)) return true;

    _seen_messages_set.insert(id);
    _seen_messages_queue.push_back({id, now});
    return false;
}

// Option B: Simple ring buffer (if ordering doesn't matter)
```

**Prompt Guard:** "Deduplication structures must have both a MAX_SIZE and a TTL-based cleanup mechanism."

---

## STAGED EXECUTION

### Pass 0: Visualize Affected Components

```
┌─────────────────────────────────────────────────────────────────┐
│                        Components Affected                       │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  k8s_discovery_service ──── Issue #1 (CRITICAL)                │
│         │                                                       │
│         ▼                                                       │
│  connection_pool ────────── Issues #2, #7                       │
│         │                                                       │
│         ▼                                                       │
│  metrics_service ────────── Issues #3, #4, #5                   │
│         │                                                       │
│         ▼                                                       │
│  circuit_breaker ────────── Issue #6                            │
│         │                                                       │
│         ▼                                                       │
│  gossip_service ─────────── Issue #8                            │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### Pass 1: Logic & Correctness
For each issue:
- Implement the fix as specified above
- Handle all edge cases (null, empty, overflow)
- Ensure error paths are logged at warn level

### Pass 2: Refactor for Clarity
- Extract constants (MAX_SIZE values) to header/config
- Add helper methods where logic is repeated
- Ensure consistent naming for overflow counters

### Pass 3: Optimize for Async Performance
- Verify no new blocking calls introduced
- Check that cleanup operations don't stall reactor
- Ensure bounded operations are O(1) amortized

---

## OUTPUT FORMAT

For each fixed issue, provide:

```
=== ISSUE #N: [Title] ===

Files Modified:
- path/to/file.hpp (lines X-Y)
- path/to/file.cpp (lines A-B)

Changes:
[Code diff or full replacement]

Verification:
- [ ] Compiles (static analysis)
- [ ] Rule #N compliance verified
- [ ] No new violations introduced
```

---

## POST-FIX ACTIONS

After all issues are fixed:

1. [ ] Run `claude-review-prompt.md` for compliance check
2. [ ] Update Progress Tracker above (change [ ] to [x])
3. [ ] Run `claude-pattern-extractor-prompt.md` if new patterns emerged
4. [ ] Commit with message: `fix: address adversarial audit findings (#1-#8)`

