# Ranvier Core: Context & Constraints

## Project Essence
Layer 7+ load balancer for LLM inference using **Prefix Caching** to prevent KV-cache thrashing.

## Architecture Reference
- **Tech Stack:** C++20, Seastar (shared-nothing architecture), SQLite (WAL mode).
- **Data Flow:** HttpController → TokenizerService (GPT-2) → RouterService → RadixTree (ART) → ConnectionPool → GPU Backend.
- **Sharding:** Every CPU core has its own shard. RouterService broadcasts updates across shards.

## Critical Constraints for Coding
1. **NO LOCKS:** This is a Seastar project. Never use `std::mutex` or atomics. Use `seastar::smp::submit_to` for cross-core communication.
2. **Async Only:** All I/O and cross-shard calls must return `seastar::future<>`.
3. **Prefix Logic:** Routing is based on the Adaptive Radix Tree (ART) lookup of token sequences.
4. **Visualize the FutureChain** Since Seastar relies heavily on Future/Promise chains, "Visualize the Future Chain" in ASCII or Mermaid diagrams before writing the code to prevent "Future Leaks" or blocking the reactor.

## Documentation Map
- **API:** Admin on `:8080`, Data on `:8080`, Metrics on `:9180`.
- **Persistence:** SQLite tracks backends and routes.

## Anti-Patterns & Lessons Learned

### Security Audit Post-Mortem (2026-01-11)

The following patterns were identified during an adversarial security audit. Each represents a "seemingly reasonable" approach that violates our Seastar/shared-nothing architecture or creates latent bugs in a 12k LOC C++20 codebase.

---

#### 0. The Cross-Core shared_ptr Anti-Pattern

**THE PATTERN:** Using `std::shared_ptr<T>` for objects that live within a Seastar service, assuming "shared ownership" is always safe.

**THE CONSEQUENCE:** `std::shared_ptr` uses atomic reference counting. When the last reference is released on a different CPU core than where the object was allocated, the destructor runs on the "wrong" shard—violating Seastar's shared-nothing model. This causes subtle data races, memory corruption, or crashes when the destructor accesses shard-local state.

**THE LESSON:** *Hard Rule: For shard-local objects, prefer `std::unique_ptr`.* When shared ownership is truly needed within a single shard, use `seastar::lw_shared_ptr` (lightweight, non-atomic). Only use `seastar::shared_ptr` (with atomic refcount) when the pointer genuinely crosses shard boundaries via `seastar::foreign_ptr`.

**PROMPT GUARD:** "Never use std::shared_ptr in Seastar code—use unique_ptr for single ownership, seastar::lw_shared_ptr for shard-local sharing, or foreign_ptr<shared_ptr<T>> for cross-shard transfer."

---

#### 1. The Lock-in-Metrics Anti-Pattern

**THE PATTERN:** Using `std::lock_guard<std::mutex>` to provide thread-safe read access in a metrics/query method (e.g., `queue_depth()`).

**THE CONSEQUENCE:** Metrics collection runs on the Seastar reactor thread. A mutex lock—even briefly—blocks the entire event loop. With Prometheus scraping every 15s across 64 shards, you get 64 stalls per scrape cycle. Under load, this causes cascading latency spikes.

**THE LESSON:** *Hard Rule: Metrics accessors must be lock-free.* Use `std::atomic<T>` with relaxed memory ordering for counters/gauges. Maintain atomic shadow variables updated alongside the protected data structure.

**PROMPT GUARD:** "Never use std::mutex in any method that could be called from the reactor thread—especially metrics, health checks, or status queries."

---

#### 2. The Sequential-Await-Loop Anti-Pattern

**THE PATTERN:** Writing `for (auto& item : items) { co_await process(item); }` to iterate over a collection of async operations.

**THE CONSEQUENCE:** O(n) latency instead of O(1). If processing 100 K8s endpoints takes 10ms each, the loop takes 1000ms—blocking the reactor for the entire second. Seastar's event loop cannot multiplex; it waits for each await.

**THE LESSON:** *Hard Rule: Replace sequential awaits with `seastar::parallel_for_each` or `max_concurrent_for_each`.* Batch concurrent operations with a semaphore (e.g., 16 in-flight) to bound parallelism without serializing.

**PROMPT GUARD:** "Never co_await inside a loop over external resources; use parallel_for_each with a concurrency limit."

---

#### 3. The Null-Dereference-from-C-API Anti-Pattern

**THE PATTERN:** Directly casting C library return values: `record.ip = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));`

**THE CONSEQUENCE:** `sqlite3_column_text()` returns NULL for SQL NULL values. Constructing `std::string` from a null pointer is undefined behavior—typically a segfault. This only surfaces when real data has NULLs, often weeks after deployment.

**THE LESSON:** *Hard Rule: Wrap all C string returns in a null-guard helper.* Create `safe_column_text()` that returns empty string for NULL. Skip records with NULL in required fields.

**PROMPT GUARD:** "Never cast sqlite3_column_text (or any C function returning char*) without an explicit null check."

---

#### 4. The Unbounded-Buffer Anti-Pattern

**THE PATTERN:** Using `buffer.push_back(item)` without size validation, trusting that normal traffic patterns will keep the buffer bounded.

**THE CONSEQUENCE:** Malicious or buggy peers can flood the buffer (e.g., gossip messages). Without a cap, memory grows until OOM kills the process. In a 12k LOC codebase, these grow sites are easy to miss during review.

**THE LESSON:** *Hard Rule: Every growing container must have an explicit MAX_SIZE.* Implement a drop strategy (oldest-first batch drop, rejection with backpressure, or ring buffer). Add an overflow counter metric.

**PROMPT GUARD:** "Every push_back, emplace_back, or append must have a corresponding size check or the container must be bounded by design."

---

#### 5. The Timer-Captures-This Anti-Pattern

**THE PATTERN:** Scheduling a timer callback with `[this] { this->on_timer(); }` and cancelling in the destructor.

**THE CONSEQUENCE:** Race condition: (1) Timer fires, callback queued on reactor. (2) `stop()` cancels timer (but callback already queued). (3) `stop()` returns, destructor runs. (4) Queued callback executes with dangling `this` → use-after-free.

**THE LESSON:** *Hard Rule: Timer callbacks require RAII gate guards.* Add a `seastar::gate _timer_gate`. Callbacks acquire `_timer_gate.hold()` at entry—fails if closed. `stop()` closes the gate *first* (waiting for in-flight callbacks), *then* cancels timers.

**PROMPT GUARD:** "Any lambda capturing 'this' for a timer or async callback must acquire a gate holder at entry and the owning class must close that gate before cancelling the timer."

---

#### 6. The Metrics-Lambda-Outlives-Service Anti-Pattern

**THE PATTERN:** Registering metrics with lambdas that capture `this`: `metrics::make_gauge("foo", [this] { return _count; })`.

**THE CONSEQUENCE:** Prometheus scrapes continue after service shutdown begins. If the lambda executes after `this` is destroyed, you get use-after-free. Metrics libraries don't automatically deregister on object destruction.

**THE LESSON:** *Hard Rule: Deregister metrics in `stop()` before any member destruction.* Call `_metrics.clear()` or equivalent as the *first* action in `stop()`, ensuring no lambda can fire during teardown.

**PROMPT GUARD:** "Any metrics lambda capturing 'this' must be deregistered in stop() before any other cleanup."

---

#### 7. The Business-Logic-in-Persistence Anti-Pattern

**THE PATTERN:** Placing validation rules (e.g., "max token count", "valid port range") inside the persistence/storage layer because "it has access to the data."

**THE CONSEQUENCE:** Architecture drift—the persistence layer accumulates business rules. Testing requires database setup. Changes to validation require touching storage code. The "layered architecture" becomes a lie.

**THE LESSON:** *Hard Rule: Persistence layers only transform and store.* Validation belongs in the service/business layer. Persistence accepts already-validated data and returns raw data for the service layer to interpret.

**PROMPT GUARD:** "Never add validation, transformation, or business rules to persistence/storage code—only the service layer validates."

---

#### 8. The Scattered-ThreadLocal Anti-Pattern

**THE PATTERN:** Declaring 10+ `thread_local` variables at file scope for per-shard state: `thread_local RadixTree* g_tree; thread_local Stats g_stats; ...`

**THE CONSEQUENCE:** State management becomes fragile—no clear initialization order, no single point for reset/cleanup, difficult to test. In a 12k LOC codebase, scattered thread_locals are invisible coupling.

**THE LESSON:** *Hard Rule: Consolidate per-shard state into a single `ShardLocalState` struct.* Provides explicit lifecycle (`init()`, `reset()`), single point of truth, and `reset_for_testing()` capability.

**PROMPT GUARD:** "Never declare standalone thread_local variables—group all per-shard state into a single ShardLocalState struct with explicit lifecycle methods."

---

#### 9. The Silent-Catch-All Anti-Pattern

**THE PATTERN:** `catch (...) {}` or `catch (const std::exception&) {}` with no logging, treating exceptions as "expected noise."

**THE CONSEQUENCE:** Configuration errors, network failures, and actual bugs are silently swallowed. The system appears to work but operates in a degraded state. Debugging requires adding logging and redeploying.

**THE LESSON:** *Hard Rule: Every catch block must log at warn level minimum.* Include the exception message, context (what operation failed), and any relevant identifiers. Consider adding a counter metric for exception rate.

**PROMPT GUARD:** "Never write an empty catch block—always log the exception at warn level with context about what operation failed."

---

#### 10. The Unchecked-String-Conversion Anti-Pattern

**THE PATTERN:** Using `std::stoi()`/`std::stol()` on external input without try-catch or validation.

**THE CONSEQUENCE:** Non-numeric strings throw `std::invalid_argument`. Out-of-range values throw `std::out_of_range`. Network input or config typos crash the process.

**THE LESSON:** *Hard Rule: Use `std::from_chars` or wrap in a validating helper.* Create `parse_port()`, `parse_int()` helpers that return `std::optional<T>` or `expected<T, Error>`. Validate ranges explicitly.

**PROMPT GUARD:** "Never use std::stoi/stol/stof on external input—use std::from_chars with explicit error handling and range validation."

---

#### 11. The Global-Static-Init-Race Anti-Pattern

**THE PATTERN:** Using global statics for shared state: `static Tracer* g_tracer; static bool g_enabled;`

**THE CONSEQUENCE:** Concurrent initialization and shutdown from different threads (or Seastar shards) causes data races. The "singleton" pattern without synchronization is undefined behavior in C++.

**THE LESSON:** *Hard Rule: Use `std::call_once` for one-time initialization.* For boolean flags, use `std::atomic<bool>`. For complex state, either make it per-shard (thread_local) or protect with proper synchronization.

**PROMPT GUARD:** "Never use bare global statics for runtime state—use std::call_once for init, std::atomic for flags, or thread_local for per-shard state."

---

#### 12. The Blocking-ifstream-in-Coroutine Anti-Pattern

**THE PATTERN:** Using `std::ifstream` or `std::ofstream` inside a coroutine or Seastar method: `std::ifstream file(path); buffer << file.rdbuf();`

**THE CONSEQUENCE:** `std::ifstream` performs blocking I/O. In Seastar, this stalls the reactor thread—stopping all network I/O, timer callbacks, and request processing on that shard. A 10ms disk read becomes 10ms of zero throughput.

**THE LESSON:** *Hard Rule: Use Seastar file I/O APIs.* Use `seastar::open_file_dma()` + `seastar::make_file_input_stream()` for async file reads. For small files during startup only, document the blocking nature explicitly.

**PROMPT GUARD:** "Never use std::ifstream/ofstream in Seastar code—use seastar::open_file_dma and seastar::make_file_input_stream for async file I/O."

---

#### 13. The Thread-Local-Raw-New Anti-Pattern

**THE PATTERN:** Using `thread_local T* g_ptr = nullptr;` with `g_ptr = new T();` for per-shard state.

**THE CONSEQUENCE:** No corresponding `delete` call exists. Thread-local variables aren't destroyed by unique_ptr RAII. Memory leaks accumulate over the process lifetime. Tools like valgrind report leaks at exit.

**THE LESSON:** *Hard Rule: Use `thread_local std::unique_ptr<T>` or add explicit destroy function.* Alternatively, use Seastar's `seastar::sharded<T>` service pattern which handles per-shard lifecycle correctly.

**PROMPT GUARD:** "Never use raw 'new' with thread_local pointers—use unique_ptr or add an explicit destroy/cleanup function called during shutdown."

---

#### 14. The Cross-Shard Heap Memory Anti-Pattern

There are THREE distinct cross-shard memory bugs:

**BUG #1 - Cross-Shard FREE:** Moving heap-owning types across shards causes wrong-shard deallocation:
```cpp
auto tokens = get_tokens();  // Heap allocated on shard 0
smp::submit_to(shard_1, [tokens = std::move(tokens)] {
    // tokens' heap memory is still on shard 0!
    // When destructor runs here on shard 1, SIGSEGV or "free(): invalid pointer"
});
```

**BUG #2 - Cross-Shard READ:** Capturing references to shard-0 memory and reading from shard N is a race:
```cpp
do_with(std::move(data), [](auto& shared_data) {
    return parallel_for_each(shards, [&shared_data](unsigned shard_id) {
        return smp::submit_to(shard_id, [&shared_data] {  // BUG: &shared_data captured!
            // Reading shared_data from shard N while it lives on shard 0
            // This is a cross-shard memory access race condition
            auto copy = std::vector<T>(shared_data.begin(), shared_data.end());
        });
    });
});
```

**BUG #3 - Cross-Shard Callback State Access:** A callback captures `this` from shard X, then is broadcast to run on shard Y, accessing shard X's member variables:
```cpp
// On shard 0: set up callback that captures 'this'
service->set_callback([this](Data data) {
    _member_vector.push_back(data);  // Accesses shard 0's member!
});

// Later, in some broadcast code:
parallel_for_each(shards, [callback](unsigned shard_id) {
    return smp::submit_to(shard_id, [callback, data] {
        callback(data);  // BUG: Running on shard N, but callback accesses shard 0's state!
    });
});
```
This is particularly insidious because: (1) the callback looks innocent, (2) the broadcast looks correct, but (3) the combination causes multiple shards to simultaneously access shard 0's state without synchronization—a data race.

**THE CONSEQUENCE:** Bug #1 causes allocator corruption and delayed crashes. Bug #2 causes data corruption, null pointers, or SIGSEGV during reads. Bug #3 causes data races with corrupted containers, lost updates, or crashes. All three can manifest long after the bug occurs, making debugging extremely difficult.

**THE CORRECT PATTERN:** Use `foreign_ptr` to safely pass data across shards:
```cpp
do_with(std::move(data), [](auto& data) {
    return parallel_for_each(shards, [&data](unsigned shard_id) {
        // 1. Wrap copy in foreign_ptr (allocated on shard 0)
        auto ptr = std::make_unique<std::vector<T>>(data);
        auto foreign = seastar::make_foreign(std::move(ptr));

        return smp::submit_to(shard_id, [foreign = std::move(foreign)]() mutable {
            // 2. Read from foreign_ptr and create LOCAL allocation
            std::vector<T> local(foreign->begin(), foreign->end());
            // 3. Use local copy (properly allocated on THIS shard)
            process(local);
            // 4. foreign_ptr destructor returns to shard 0 for cleanup
        });
    });
});
```

**WHY `foreign_ptr` WORKS:**
- Wraps a `unique_ptr` for safe cross-shard transfer
- Destructor runs on the HOME shard (where it was created), not current shard
- Safe to READ from on any shard (data is guaranteed alive)
- Forces you to explicitly create local allocations

**RED FLAGS TO AUDIT:**
- `submit_to` lambdas capturing `&reference` to outer scope data
- `submit_to` lambdas capturing `vector`, `string`, `unique_ptr` by value (wrong-shard free)
- `parallel_for_each` broadcasting heap-owning data to all shards
- Raw pointers passed to `submit_to` lambdas (e.g., `[ptr = &data]`)
- `foreign_ptr` with `std::move(*foreign)` extracting data without local copy
- **Callbacks that capture `this` being broadcast to multiple shards** (Bug #3)
- `parallel_for_each` + `submit_to` patterns where the callback accesses member variables

**PROMPT GUARD:** "For cross-shard data: (1) Never capture references to outer-scope data in submit_to lambdas. (2) Never move heap-owning types directly—use foreign_ptr and create local allocations on the target shard. (3) Never broadcast a callback that captures `this` to multiple shards—either run the callback only on the owning shard, or ensure it only accesses shard-local state."

---

#### 15. The FFI Cross-Boundary Memory Anti-Pattern

**THE PATTERN:** Passing heap-allocated data (strings, vectors) across shard or thread boundaries to FFI code (Rust, C libraries):
```cpp
// Cross-shard FFI call
smp::submit_to(target_shard, [text = std::move(text)]() {
    rust_tokenizer->Encode(text);  // BUG: text's buffer allocated on calling shard
});

// Cross-thread FFI call (std::thread worker)
void worker_thread(Job& job) {
    rust_library->process(job.text);  // BUG: job.text allocated on reactor thread
}
```

**THE CONSEQUENCE:** While Seastar's `do_foreign_free` mechanism can handle cross-shard C++ allocations, **FFI allocators don't participate in Seastar's memory tracking**:
- Rust uses jemalloc (or system allocator) independently
- C libraries call `malloc`/`free` directly
- When FFI code operates on foreign-shard memory, it may reallocate, cache pointers, or interact with the buffer in ways that corrupt Seastar's per-shard allocator state

Real-world symptom: Under stress testing (100 users, 30 minutes), SIGSEGV crashes with corrupted pointers (`si_addr=0x5`) in the Rust tokenizer FFI.

**THE LESSON:** *Hard Rule: Always reallocate locally before passing data to FFI across shard/thread boundaries.*
```cpp
// Cross-shard: reallocate at start of submit_to lambda
smp::submit_to(target_shard, [text_copy = std::move(text)]() {
    std::string local_text(text_copy.data(), text_copy.size());  // Local allocation
    rust_tokenizer->Encode(local_text);  // FFI sees locally-allocated memory
});

// Cross-thread: reallocate before FFI call
void worker_thread(Job& job) {
    std::string local_text(job.text.data(), job.text.size());  // Local allocation
    rust_library->process(local_text);
}
```

**THIS APPLIES TO:**
- Rust libraries via C FFI (tokenizers, regex, simd-json, etc.)
- C libraries that may reallocate or cache buffers (SQLite, zlib, etc.)
- Any `std::thread` workers processing data from Seastar reactors
- Any external code outside Seastar's allocator control

**RED FLAGS TO AUDIT:**
- `smp::submit_to` lambdas that pass strings/vectors directly to FFI functions
- `seastar::alien::run_on` callbacks passing data to/from worker threads
- Worker threads (std::thread) calling FFI with reactor-allocated data
- Any FFI call in code that could run on a different shard than allocation

**PROMPT GUARD:** "Before passing strings, vectors, or any heap-allocated data to FFI code across shard or thread boundaries, always create a local copy: `std::string local(foreign.data(), foreign.size())`. FFI allocators don't participate in Seastar's cross-shard memory tracking."

---

### Quick Reference: The 16 Hard Rules

| # | Rule | Violation |
|---|------|-----------|
| 0 | Prefer `unique_ptr`; use `lw_shared_ptr` for shard-local sharing | `std::shared_ptr` in Seastar code |
| 1 | Metrics accessors must be lock-free | `std::mutex` in query method |
| 2 | No `co_await` inside loops over external resources | Sequential await in for-loop |
| 3 | Null-guard all C string returns | Direct cast of `sqlite3_column_text` |
| 4 | Every growing container needs MAX_SIZE | Unbounded `push_back` |
| 5 | Timer callbacks need gate guards | Lambda captures `this` without gate |
| 6 | Deregister metrics first in `stop()` | Lambda outlives service |
| 7 | Persistence only stores, never validates | Business logic in storage layer |
| 8 | Single `ShardLocalState` struct for per-shard state | Scattered `thread_local` variables |
| 9 | Every catch block logs at warn level | Silent `catch (...)` |
| 10 | Validating helpers for string-to-number | Bare `std::stoi()` on input |
| 11 | `std::call_once` or `std::atomic` for global state | Unprotected global statics |
| 12 | Use Seastar file I/O APIs | `std::ifstream/ofstream` in Seastar code |
| 13 | Thread-local raw new needs destroy function | `thread_local T* = new T()` without cleanup |
| 14 | Force local allocation for cross-shard data | Moving `vector`/`string` via `submit_to` |
| 15 | Reallocate locally before FFI across boundaries | Passing shard-allocated data to Rust/C FFI |
