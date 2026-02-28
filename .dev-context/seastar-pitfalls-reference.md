# Seastar Pitfalls Reference

Additional Seastar pitfalls derived from ScyllaDB production experience, Seastar GitHub issues, and the `seastar-dev` mailing list. These are **not** currently Hard Rules but are documented here for future audits and to prevent knowledge loss as the codebase evolves.

**When to promote:** If Ranvier adds the relevant feature (e.g., scheduling groups, `when_all_succeed` fan-outs, large cross-shard caches), promote the corresponding pitfall to a Hard Rule in `claude-context.md`.

---

## 1. Scheduling Group Escape

**Relevance:** Becomes critical if Ranvier adds scheduling groups for priority isolation.

**The Pattern:** Spawning background fibers, setting timers, or launching fire-and-forget work inside a `with_scheduling_group()` block without explicitly assigning the correct scheduling group.

**The Consequence:** Timer callbacks execute under whatever scheduling group was current when the callback was **created**, not when it **fires**. Background fibers spawned inside the block inherit the group implicitly but this is fragile. If 1,000 background fibers from a low-priority group are spawned but their continuations end up scheduled in the default group, they starve user-facing requests.

**The Fix:** Every "entry point" fiber (RPC handler, timer callback, signal handler, background task) must be explicitly wrapped in `with_scheduling_group()`. Never rely on implicit scheduling group inheritance for anything that outlives the current scope.

**Source:** [Seastar tutorial](https://github.com/scylladb/seastar/blob/master/doc/tutorial.md), [Seastar scheduling_group docs](https://docs.seastar.io/master/classseastar_1_1scheduling__group.html)

---

## 2. Exception Scalability Collapse Under Load

**Relevance:** Applies now -- Ranvier uses exceptions for some error paths.

**The Pattern:** Using C++ exceptions as the normal error reporting mechanism for expected failures (timeouts, rate limiting, overload rejection).

**The Consequence:** In libstdc++, throwing an exception acquires a **global mutex** that protects runtime data for dynamic library loading. On Seastar's thread-per-core design, one core throwing an exception blocks other cores that are also trying to throw. ScyllaDB measured this: at ~60k writes/second with timeouts, the exception-based path caused throughput collapse. The newer path using `boost::result` sustained 100k+ requests/second.

**The Fix:** Use `boost::result<T, E>`, error codes, or `std::optional<T>` for predictable failures. Reserve exceptions for truly exceptional conditions. Seastar provides `seastar::coroutine::as_future` to probe for exceptions without throw/catch overhead.

**Source:** [ScyllaDB Blog - Better Goodput Performance](https://www.scylladb.com/2024/05/14/better-goodput-performance-through-c-exception-handling/), [Seastar issue #73](https://github.com/scylladb/seastar/issues/73)

---

## 3. `when_all_succeed()` Silently Discards Extra Exceptions

**Relevance:** Becomes critical if Ranvier uses `when_all_succeed()` for multi-backend fan-out.

**The Pattern:** Using `when_all_succeed()` to fan out multiple operations, expecting to see all errors.

**The Consequence:** If two of three futures fail with different exceptions, `when_all_succeed()` propagates **one** exception (unspecified which) and **silently discards** the other. The same applies to `seastar::coroutine::all` and `parallel_for_each`.

**The Fix:** Use `when_all()` (not `when_all_succeed()`) and inspect each future individually. Or add per-operation error handling before the join point:
```cpp
auto safe_fetch = [](auto f) -> seastar::future<std::optional<Result>> {
    try {
        co_return co_await std::move(f);
    } catch (...) {
        log_error("fetch failed: {}", std::current_exception());
        co_return std::nullopt;
    }
};
```

**Source:** [Seastar tutorial - when_all](https://docs.seastar.io/master/split/12.html), [Seastar issue #135](https://github.com/scylladb/seastar/issues/135)

---

## 4. `finally()` Masking Original Exception with `nested_exception`

**Relevance:** Applies now -- Ranvier uses `.finally()` for cleanup.

**The Pattern:** Using `.finally()` for cleanup where the cleanup itself can fail.

**The Consequence:** If both the main operation and the `finally()` body throw, Seastar wraps both into a `seastar::nested_exception` with the `.finally()` exception on top. Downstream `handle_exception_type<OriginalError>()` will **fail to match** because the propagated type is `nested_exception`, not the original error.

**The Fix:** Protect `finally()` bodies against throwing:
```cpp
co_await do_work()
    .finally([] {
        return close_connection().handle_exception([](std::exception_ptr ep) {
            logger.warn("cleanup failed: {}", ep);
        });
    });
```

**Source:** [Seastar future class reference - finally()](https://docs.seastar.io/master/classseastar_1_1future.html)

---

## 5. `foreign_ptr` Destruction is Fire-and-Forget

**Relevance:** Becomes critical if Ranvier adds large cross-shard caches.

**The Pattern:** Destroying many `foreign_ptr` objects rapidly, expecting their destructors to complete before proceeding.

**The Consequence:** `foreign_ptr::~foreign_ptr()` sends the destruction back to the home shard asynchronously but **does not return a future**. There is no backpressure. Destroying thousands of foreign_ptrs in a tight loop floods inter-shard message queues.

**The Fix:** Batch destruction using `smp::submit_to()` with flow control:
```cpp
co_await seastar::max_concurrent_for_each(cache, 64, [](auto& fptr) {
    auto shard = fptr.get_owner_shard();
    auto raw = fptr.release();
    return seastar::smp::submit_to(shard, [raw = std::move(raw)] {});
});
```

**Source:** [seastar::foreign_ptr docs](https://docs.seastar.io/master/classseastar_1_1foreign__ptr.html)

---

## 6. Oversized Allocations Causing Latency Spikes and Fragmentation

**Relevance:** Applies now -- long-running server with per-shard allocator.

**The Pattern:** Allocating large contiguous memory blocks (>= 128KB) in steady-state operation.

**The Consequence:** Seastar's per-shard allocator is optimized for many small allocations. Large contiguous allocations cause fragmentation in long-running servers. ScyllaDB logs warnings like `seastar_memory - oversized allocation: 6033408 bytes.` The allocation itself may stall the reactor while the allocator searches for a contiguous region.

**The Fix:** Use chunked data structures (`seastar::chunked_fifo`, lists of fixed-size blocks). Set `seastar::memory::set_large_allocation_warning_threshold()` to catch violations early. For truly large buffers, allocate once at startup and reuse.

**Source:** [seastar_memory namespace reference](https://docs.seastar.io/master/namespaceseastar_1_1memory.html), [ScyllaDB issue #8300](https://github.com/scylladb/scylladb/issues/8300)

---

## 7. `sharded::stop()` Ordering

**Relevance:** Applies now -- Ranvier has multiple sharded services with dependencies.

**The Pattern:** Stopping sharded services in the wrong order, or forgetting to call `stop()`.

**The Consequence:** The `sharded<>` destructor asserts `_instances.empty()`. Forgetting `stop()` crashes. Wrong order causes service A's `stop()` to access already-destroyed service B (use-after-free). ScyllaDB experienced this: a `service_memory_limiter.stop()` was accidentally commented out.

**The Fix:** Use `seastar::defer()` with stack ordering for guaranteed reverse-order shutdown:
```cpp
co_await seastar::async([&] {
    seastar::sharded<ServiceB> b;
    auto stop_b = seastar::defer([&] { b.stop().get(); });
    b.start().get();

    seastar::sharded<ServiceA> a;
    auto stop_a = seastar::defer([&] { a.stop().get(); });
    a.start(std::ref(b)).get();
    // C++ destroys in reverse order: stop_a first, then stop_b
});
```

**Source:** [seastar::sharded docs](https://docs.seastar.io/master/classseastar_1_1sharded.html), [ScyllaDB issue #8421](https://github.com/scylladb/scylladb/issues/8421)

---

## 8. `abort_source` Subscription Lifetime

**Relevance:** Becomes critical if Ranvier adds cancellation support.

**The Pattern:** Letting the `abort_source` subscription object go out of scope before the operation completes, or forgetting to pass `abort_source` to blocking waits.

**The Consequence:** The subscription unlinks from the `abort_source` on destruction. If it goes out of scope early, the operation becomes uncancellable. Forgetting to wire `abort_source` to a semaphore wait or sleep creates an operation that cannot be cancelled, making shutdown hang indefinitely.

**The Fix:** Keep subscriptions alive for the full operation duration. Pass `abort_source` to every blocking primitive:
```cpp
co_await _sem.wait(1, as);
co_await seastar::sleep_abortable(1s, as);
```

**Source:** [seastar::abort_source docs](https://docs.seastar.io/master/classseastar_1_1abort__source.html)

---

## 9. Destructor Chain Reactor Stalls

**Relevance:** Applies now -- Ranvier uses `absl::flat_hash_map` and other large containers.

**The Pattern:** Destroying a large data structure (hash map, vector of vectors, tree) all at once.

**The Consequence:** Destructors run synchronously on the reactor thread. ScyllaDB observed a **1.3-second** reactor stall from `std::unordered_set::insert()` triggering rehash, and multi-second stalls from destroying large vectors. Hash map rehashing is particularly dangerous because it both allocates and copies in a tight loop with no preemption points.

**The Fix:** Implement incremental destruction for large containers:
```cpp
seastar::future<> destroy_incrementally(auto& container) {
    while (!container.empty()) {
        auto it = container.begin();
        container.erase(it);
        co_await seastar::coroutine::maybe_yield();
    }
}
```
Use `reserve()` upfront for hash maps that might grow large.

**Source:** [ScyllaDB issue #18173](https://github.com/scylladb/scylladb/issues/18173), [ScyllaDB issue #12999](https://github.com/scylladb/scylladb/issues/12999)

---

## 10. Shutdown Close Ordering Races

**Relevance:** Applies now -- Ranvier has TCP connections, gates, and connection pools.

**The Pattern:** Shutting down a server by closing connections and gates without coordinating with in-flight fibers.

**The Consequence:** A fiber handles a request and tries to send a response over a socket that was already shut down. TLS shutdown handshake races with timeout-based cleanup. TCP shutdown while a fiber is mid-write causes unexpected errors.

**The Fix:** Proper shutdown sequence: (1) Stop accepting new work (close gate, stop listener). (2) Signal in-flight fibers to stop via `abort_source` or `gate::check()`. (3) Wait for all in-flight work to complete. (4) Only then close sockets and destroy resources.

**Source:** [ScyllaDB issue #10217](https://github.com/scylladb/scylladb/issues/10217), [Seastar issue #727](https://github.com/scylladb/seastar/issues/727)

---

## 11. `do_with()` Background Work Outliving the Future

**Relevance:** Applies if any `do_with` blocks spawn fire-and-forget work.

**The Pattern:** Starting background operations inside `do_with()` that continue after the returned future resolves.

**The Consequence:** `do_with()` guarantees objects live until the **returned** future resolves -- not until all work using those objects completes. Any fire-and-forget future or timer callback using `do_with`'ed objects after the returned future resolves is use-after-free.

**The Fix:** Incorporate all background work into the returned future chain using `when_all()`. Or use coroutines where lifetimes are explicit.

**Source:** [Seastar tutorial - do_with](https://docs.seastar.io/master/tutorial.html)
