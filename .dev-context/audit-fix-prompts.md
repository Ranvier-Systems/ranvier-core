# Audit Fix Prompts (2026-01-14)

Use these prompts to fix each issue from the adversarial audit. Copy the relevant section and use it as your task prompt.

---

## Issue 1: Blocking std::ifstream in K8s CA cert loading (CRITICAL)

```
Fix the blocking I/O violation in k8s_discovery_service.cpp.

PROBLEM:
- Location: src/k8s_discovery_service.cpp:234
- Code: `std::ifstream ca_file(_config.ca_cert_path)` performs blocking file I/O
- Impact: Stalls Seastar reactor thread during K8s discovery, blocking all network I/O

CONSTRAINTS (from docs/claude-context.md):
- Rule #2: "Async Only - All I/O must return seastar::future<>"
- Anti-pattern #12: "Never use std::ifstream/ofstream in Seastar code"

REFERENCE IMPLEMENTATION:
- See lines 204-207 in same file for correct pattern using seastar::open_file_dma()

FIX APPROACH:
1. Replace std::ifstream with seastar::open_file_dma() + seastar::make_file_input_stream()
2. Handle file-not-found gracefully (already has fallback to system trust)
3. Ensure proper stream cleanup via finally()
4. The init_tls() method is already a coroutine, so co_await is available

ACCEPTANCE CRITERIA:
- No std::ifstream or std::ofstream in the file
- CA cert loading uses Seastar async file I/O
- Existing fallback behavior preserved
- Unit tests pass
```

---

## Issue 2: Connection pool timer without gate guard (Medium)

```
Add RAII gate guard to connection pool reaper timer to prevent use-after-free.

PROBLEM:
- Location: src/connection_pool.hpp:99-109
- Code: `_reaper_timer([this] { reap_dead_connections(); })` captures `this`
- Impact: Race condition if timer callback executes after destructor begins

CONSTRAINTS (from docs/claude-context.md):
- Anti-pattern #5: "Timer callbacks need gate guards"
- Pattern: Callbacks acquire _timer_gate.hold() at entry; stop() closes gate first

REFERENCE IMPLEMENTATION:
- See src/async_persistence.hpp:294 for _timer_gate pattern
- See src/gossip_service.cpp:696-700 for callback guard pattern

FIX APPROACH:
1. Add `seastar::gate _timer_gate` member variable
2. In reap_dead_connections(), acquire gate holder at entry:
   ```cpp
   try {
       [[maybe_unused]] auto holder = _timer_gate.hold();
   } catch (const seastar::gate_closed_exception&) {
       return;
   }
   ```
3. Add stop() method that closes gate before canceling timer
4. Destructor should call stop() or be replaced with stop() pattern

ACCEPTANCE CRITERIA:
- Gate holder acquired at start of timer callback
- Gate closed before timer cancelled in destructor/stop
- No use-after-free possible in timer callback
```

---

## Issue 3: Null pointer dereference in metrics() accessor (Medium)

```
Add null check to metrics() accessor to prevent segfault if init_metrics() not called.

PROBLEM:
- Location: src/metrics_service.hpp:443-445
- Code: `return *g_metrics;` dereferences potentially null pointer
- Impact: Segfault if metrics() called before init_metrics()

CONSTRAINTS (from docs/claude-context.md):
- Defensive coding: All accessors should handle uninitialized state gracefully

FIX APPROACH (choose one):
Option A - Assertion (fail-fast, recommended for internal APIs):
```cpp
inline MetricsService& metrics() {
    assert(g_metrics && "init_metrics() must be called before metrics()");
    return *g_metrics;
}
```

Option B - Lazy initialization (safer but hides bugs):
```cpp
inline MetricsService& metrics() {
    if (!g_metrics) {
        init_metrics();
    }
    return *g_metrics;
}
```

ACCEPTANCE CRITERIA:
- metrics() cannot dereference null pointer
- Clear error message if called before initialization
- Document expected initialization order in header comment
```

---

## Issue 4: Thread-local MetricsService memory leak (HIGH)

```
Fix memory leak in thread-local MetricsService allocation.

PROBLEM:
- Location: src/metrics_service.hpp:436-440
- Code: `g_metrics = new MetricsService()` with no corresponding delete
- Impact: Memory leaked on process shutdown (one MetricsService per shard)

CONSTRAINTS (from docs/claude-context.md):
- Anti-pattern #13: "Never use raw 'new' with thread_local pointers"
- Prefer unique_ptr or explicit destroy function

FIX APPROACH:
Option A - Use unique_ptr (recommended):
```cpp
inline thread_local std::unique_ptr<MetricsService> g_metrics;

inline void init_metrics() {
    if (!g_metrics) {
        g_metrics = std::make_unique<MetricsService>();
    }
}

inline MetricsService& metrics() {
    assert(g_metrics && "init_metrics() must be called first");
    return *g_metrics;
}
```

Option B - Add explicit destroy function:
```cpp
inline void destroy_metrics() {
    delete g_metrics;
    g_metrics = nullptr;
}
```
Then call destroy_metrics() during shard shutdown in Application.

ACCEPTANCE CRITERIA:
- No raw `new` for thread_local pointer
- Memory properly freed on shutdown
- Valgrind reports no leaks from MetricsService
```

---

## Issue 5: Unbounded _per_backend_metrics map (HIGH)

```
Cap per-backend metrics map to prevent unbounded memory growth under attack.

PROBLEM:
- Location: src/metrics_service.hpp:397
- Code: `std::unordered_map<BackendId, BackendMetrics> _per_backend_metrics`
- Impact: Attacker can exhaust memory by spoofing unique backend IDs

CONSTRAINTS (from docs/claude-context.md):
- Anti-pattern #4: "Every growing container must have explicit MAX_SIZE"
- Anti-pattern #14: "Every map keyed by external IDs must have MAX_SIZE with eviction"

FIX APPROACH:
1. Add constant: `static constexpr size_t MAX_TRACKED_BACKENDS = 1000;`
2. In get_or_create_backend_metrics(), check size before inserting:
   ```cpp
   if (_per_backend_metrics.size() >= MAX_TRACKED_BACKENDS) {
       // Option A: Return reference to "overflow" bucket
       // Option B: Evict oldest entry (requires tracking access time)
       // Option C: Reject and return existing random entry
       ++_backend_metrics_overflow;  // Add overflow counter
       return _overflow_backend_metrics;  // Shared overflow bucket
   }
   ```
3. Add remove_backend_metrics(BackendId) method for cleanup when backends removed
4. Add overflow counter metric for monitoring

ACCEPTANCE CRITERIA:
- Map cannot grow beyond MAX_TRACKED_BACKENDS entries
- Overflow handled gracefully (metrics still collected, just aggregated)
- Overflow counter exposed as Prometheus metric
- Method to remove metrics when backend is deregistered
```

---

## Issue 6: Unbounded _circuits map in CircuitBreaker (HIGH)

```
Add circuit cleanup when backends are removed to prevent unbounded memory growth.

PROBLEM:
- Location: src/circuit_breaker.hpp:210
- Code: `std::unordered_map<BackendId, BackendCircuit> _circuits`
- Impact: Memory grows monotonically as backends come and go

CONSTRAINTS (from docs/claude-context.md):
- Anti-pattern #14: "Every map keyed by external IDs must have MAX_SIZE with eviction"

FIX APPROACH:
1. The reset(BackendId) method already exists (line 178) but is never called
2. Add call to circuit_breaker.reset(backend_id) when backend is removed
3. Find backend removal path in HttpController or RouterService
4. Alternative: Add periodic stale circuit reaper that removes circuits for backends not seen in N minutes

Integration point - find where backends are removed:
- Search for "remove_backend" or "deregister" in http_controller.cpp
- Add circuit_breaker.reset(id) after successful removal

ACCEPTANCE CRITERIA:
- Circuits removed when backends are deregistered
- No unbounded memory growth from circuit state
- Existing reset() and reset_all() methods work correctly
```

---

## Issue 7: Connection pool map entry leak (Medium)

```
Verify connection pool map entries are fully cleaned up when backends removed.

PROBLEM:
- Location: src/connection_pool.hpp:280-300
- Code: clear_pool() erases map entry, but need to verify it's called
- Impact: Empty deque entries remain in map if clear_pool() not called

INVESTIGATION:
1. Search codebase for calls to clear_pool()
2. Verify it's called from all backend removal paths
3. If not called, add call from backend removal handler

The clear_pool() method at line 280-300 already does `_pools.erase(it)` at line 294.
The issue is ensuring this method is called when backends are removed.

FIX APPROACH:
1. Find backend removal handler (likely in HttpController or RouterService)
2. Ensure clear_pool(backend_address) is called
3. The pool needs access to socket_address, so may need to store backend_id -> address mapping

ACCEPTANCE CRITERIA:
- When backend is deregistered, its pool entry is fully removed
- No empty deque entries accumulate in _pools map
- Add logging to confirm pool cleanup on backend removal
```

---

## Issue 8: Unbounded gossip dedup structures (Medium)

```
Add MAX_TRACKED_PEERS limit to gossip dedup structures.

PROBLEM:
- Location: src/gossip_service.cpp:1174-1195
- Code: _received_seq_sets and _received_seq_windows grow per peer address
- Impact: Memory grows with number of unique peer addresses seen

CONSTRAINTS (from docs/claude-context.md):
- Anti-pattern #14: "Every map keyed by external IDs must have MAX_SIZE"

NOTE: The sliding window already evicts old sequence numbers per peer (line 1188).
The issue is that the peer count itself can grow unbounded.

FIX APPROACH:
1. Add constant: `static constexpr size_t MAX_TRACKED_PEERS = 1000;`
2. Before adding new peer to dedup structures, check size
3. If at limit, either:
   - Reject (peer will retry, may get in later)
   - Evict oldest peer (requires tracking last-seen time per peer)
4. Existing refresh_peers() at line 1032-1056 already cleans up removed peers
5. Verify cleanup is working; add logging if not

Current cleanup in refresh_peers() (lines 1037-1040):
```cpp
_pending_acks.erase(addr);
_peer_seq_counters.erase(addr);
_received_seq_windows.erase(addr);
_received_seq_sets.erase(addr);
```

ACCEPTANCE CRITERIA:
- Dedup structures cannot grow beyond MAX_TRACKED_PEERS
- Existing peer cleanup in refresh_peers() confirmed working
- Add metrics for tracking peer count in dedup structures
```

---

## General Testing Checklist

After fixing each issue, verify:

1. [ ] Code compiles without warnings
2. [ ] Unit tests pass: `make test`
3. [ ] Integration tests pass: `make test-integration`
4. [ ] No new Seastar reactor stalls under load
5. [ ] Memory usage stable under sustained traffic
6. [ ] Valgrind reports no new leaks (for leak fixes)

## Commit Message Format

```
fix(<component>): <short description>

<detailed description of the fix>

Fixes audit issue: <issue title from TODO.md>
Location: <file:line>
Severity: <Critical|High|Medium>
```
