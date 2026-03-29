---
layout: post
title: "24 Hard Rules for Writing Correct Async C++"
date: 2026-03-29
author: Minds Aspire
---

Async C++ will let you write a use-after-free that only manifests under load, on the third Tuesday of the month, in a stack frame that has nothing to do with the bug. The compiler won't warn you. Your tests will pass. Your sanitizers will shrug. And then production will teach you what you missed.

We maintain a ~50K LOC C++20 service built on Seastar. We catalogued every class of bug that burned us and codified them into 24 rules we enforce on every commit. Each rule exists because someone on the team lost at least a day to the bug it prevents. Here they are.

## How to Read This Post

Rules are numbered 0-23. Each one has **the anti-pattern** (what the bad code looks like), **why it breaks** (the subtle mechanism), and **the rule** (what to do instead). We cover the most interesting rules in full below. The rest are summarized at the end. Bookmark this—it's a reference, not a narrative.

## Memory That Isn't Yours Anymore

Lifetime bugs are the silent killers of async C++. In synchronous code, if the object exists, the scope that created it still exists. In async code, that guarantee vanishes. The object exists, but the scope is long gone.

**Rule 16 — Lambda coroutines in `.then()` are use-after-free.** This is the scariest bug on the list because it looks completely correct. You write a lambda that contains `co_await`, pass it to `.then()`, and everything compiles. Here's what actually happens: `.then()` moves the lambda into internal storage. The lambda's `operator()` is called, which creates a coroutine frame on the heap. The coroutine suspends at `co_await`. `.then()` is done with the lambda and destroys it. The coroutine resumes—into freed memory.

The fix is `seastar::coroutine::lambda()`, which ensures the coroutine frame owns its own state. The compiler will never warn you about this. We found it after three days of chasing a heap corruption that only appeared under sustained load.

**Rule 5 — Timer callbacks need gate guards.** A repeating timer fires after `stop()` has already begun destroying `this`. The callback dereferences member variables that no longer exist. The fix is `seastar::gate`—but the gate holder must outlive the *entire* async operation, not just the try block. We've seen code where the gate guard was scoped to the try body, which meant the catch path ran outside the gate. During shutdown, the catch path touched a destroyed logger.

**Rule 21 — Coroutine reference parameters dangle.** A coroutine that takes `const std::string&` looks correct, compiles fine, passes every unit test, and breaks under load. The caller's string goes out of scope. The coroutine suspends. When it resumes, the reference points to freed memory. The fix is to take parameters by value. Every time. The cost of a copy is nothing compared to the cost of debugging a dangling reference that only manifests at the 99.9th percentile.

**Rule 20 — Missing `&` in `do_with` lambdas.** `seastar::do_with` allocates objects on the heap and passes them by reference to your lambda. If you capture by value instead of by reference—forgetting a single `&`—you get a copy. That copy is destroyed when the lambda returns, but the future it spawned is still running, still holding a reference to the now-dead copy. Heap corruption that manifests in completely unrelated code, sometimes minutes later.

**Rule 23 — `share()` on `temporary_buffer` pins the whole allocation.** You call `.share()` to grab a 32-byte header from a 64KB network buffer. Both shared views now pin the same underlying allocation. You cache the header. The "temporary" buffer lives forever. Unexplained memory growth that doesn't correlate with logical data sizes. We spent a week on this one before we realized our header cache was silently pinning network buffers. The fix: copy the bytes you need into a new buffer, then release the shared view.

## The Reactor Is Not a Thread

Seastar is cooperative. There is no kernel to preempt you. Every microsecond you block is a microsecond of zero throughput on that core. Not "reduced throughput." Zero.

**Rule 2 — No `co_await` in unbounded loops over external resources.** The pattern `for (auto& item : items) { co_await process(item); }` is O(n) latency. 100 items at 10ms each means one full second where that core does nothing else. No other requests are served. No timers fire. No health checks respond. Use `seastar::parallel_for_each` or `seastar::max_concurrent_for_each` with bounded concurrency—process items in parallel, but cap the parallelism so you don't exhaust memory.

**Rule 12 — No `std::ifstream` in coroutines.** It compiles. It works in testing with SSDs. In production, one 10ms disk stall freezes the entire shard. Every connection on that core drops packets for 10ms. Use Seastar's file I/O, which goes through the reactor and yields properly. If you absolutely must call blocking I/O, isolate it in a `seastar::thread` and document it loudly.

**Rule 17 — Preemption points in hot loops.** A tight loop that runs for 500μs without yielding starves everything else on that core. Timers, health checks, other connections—all frozen. Insert `co_await seastar::coroutine::maybe_yield()` every ~100 iterations. The cost is a branch that's almost never taken. The cost of *not* doing it is a reactor stall warning in your logs and a mystery latency spike that disappears when you reduce load.

## Cross-Shard Is Cross-Universe

Seastar's shared-nothing architecture means each core has its own memory allocator. This isn't an implementation detail you can ignore. It's a load-bearing invariant, and violating it corrupts allocator state silently.

**Rule 0 — `std::shared_ptr` destructs on the wrong shard.** The refcount is atomic, so the decrement is "safe" from any core. But the destructor runs on whichever core decrements last. That destructor frees memory through the wrong core's allocator. Use `seastar::lw_shared_ptr` (non-atomic refcount, shard-local only) or wrap cross-shard pointers in `seastar::foreign_ptr`, which ensures the destructor runs on the owning shard. We made this Rule 0 because it was the first bug that burned us and the last one we expected.

**Rule 14 — Cross-shard heap data must be reallocated locally.** You `submit_to()` another shard with a `std::string`. The target shard reads memory allocated by the source shard's allocator. Maybe it works today. Maybe the allocator metadata is adjacent and you corrupt it on the next allocation. The rule: copy on receive. Allocate a new string on the target shard. It feels wasteful. It prevents silent corruption.

**Rule 15 — FFI across shard boundaries needs reallocation in both directions.** Passing Seastar-allocated memory to an FFI boundary (Rust, C libraries) means the foreign code may free or reallocate through a different allocator. Reallocate into standard `malloc` memory before calling FFI. Reallocate the result back into Seastar's allocator before returning to the reactor. Two copies, no corruption.

## Futures Are Not Exceptions

C++ has two error propagation systems now: exceptions and future chains. Code that mixes them has gaps where errors disappear.

**Rule 18 — Discarded futures silently swallow errors.** Calling an async function without `co_await` means the returned future is destroyed immediately. If that future eventually resolves with an exception, nobody sees it. Seastar logs a warning at runtime, but by then the damage is done—a write that didn't complete, a cleanup that never ran. The rule: every future must be `co_await`ed, returned, or explicitly discarded with a comment explaining why.

**Rule 22 — Throwing before returning a future bypasses `.finally()`.** If an exception is thrown synchronously before the function returns a future, it propagates as a regular C++ exception. Any `.finally()` attached to the expected return value never executes. Cleanup is skipped. Resources leak. The fix: use `seastar::futurize_invoke()` to wrap the call, which catches synchronous exceptions and converts them into failed futures. Or better—use coroutines, which handle this naturally.

**Rule 19 — Raw `semaphore::wait()/signal()` leaks units on throw.** You call `wait()`, do work, call `signal()` in a `.finally()`. But if the work throws synchronously before you attach `.finally()`, the units are never returned. The semaphore's available count decreases monotonically until everything deadlocks. Use `seastar::with_semaphore()`, which handles the lifecycle correctly regardless of how the operation fails.

## The Rules We Didn't Expect

Not every rule is about language mechanics. Some are about discipline.

**Rule 4 — Every growing container needs MAX_SIZE.** No unbounded buffers, ever. A single malicious peer sending oversized messages will OOM your process if nothing caps the queue. Every `std::vector`, every `std::deque`, every ring buffer gets a configured maximum. This isn't a performance optimization. It's a survival mechanism.

**Rule 9 — Every catch block logs at warn level.** A silent `catch(...)` is the number one cause of "it works but something is wrong" in production. If you're catching an exception, something unexpected happened. Log it. If it's too noisy, fix the root cause instead of silencing the symptom.

**Rule 7 — Persistence only stores, never validates.** This is a design rule, not a language rule. When the persistence layer also validates, you can't test business logic without spinning up storage. When it only stores, you can test validation in isolation and reason about correctness without thinking about I/O.

## The Remaining Rules

For completeness, here are the rules not covered in full above:

- **Rule 1** — Metrics accessors must be lock-free. Use `std::atomic<T>` with relaxed ordering, not `std::mutex`.
- **Rule 3** — Null-guard all C string returns. `sqlite3_column_text()` returns NULL for SQL NULL—constructing `std::string` from it is undefined behavior.
- **Rule 6** — Deregister metrics first in `stop()`. Prometheus scrapes continue during shutdown; lambdas capturing `this` become use-after-free.
- **Rule 8** — Consolidate per-shard state into a single `ShardLocalState` struct. Scattered `thread_local` variables have no clear init/cleanup order.
- **Rule 10** — Use validating helpers for string-to-number conversion. Bare `std::stoi()` on external input throws on non-numeric strings.
- **Rule 11** — Use `std::call_once` or `std::atomic` for global state. Unprotected global statics are data races during concurrent initialization.
- **Rule 13** — Thread-local raw `new` needs an explicit destroy function. `thread_local T*` with `new` has no corresponding `delete`.

## How We Enforce Them

These rules live in a shared document that every team member reviews before their first commit. They're enforced through code review, not tooling—the bugs they prevent are semantic, not syntactic. No linter can tell you that a lambda coroutine in `.then()` is a use-after-free.

In review comments, we reference rules by number. Writing "Rule 16" is faster than explaining the coroutine frame lifetime problem from scratch every time. New engineers learn the rules by seeing them cited in reviews and then reading the explanation.

The list started at Rule 0 and grew to 24. We add a rule only when a bug burns the team—never speculatively. If you're building something similar, start your own list. The specific rules matter less than the habit of writing them down.

## The Takeaway

Async C++ gives you performance that no garbage-collected language can match, but it replaces runtime safety nets with your discipline. The type system won't save you. The compiler won't save you. Your rules—written down, enforced in review, learned through pain—are the safety nets you build for yourself.

We're building [Ranvier](https://github.com/Ranvier-Systems/ranvier-core), a Layer 7 load balancer for LLM inference on Seastar. If this kind of systems work interests you, check out the [source](https://github.com/Ranvier-Systems/ranvier-core).

---

*Ranvier is a project of Minds Aspire, LLC.*
