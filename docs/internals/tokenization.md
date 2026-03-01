# Tokenization Internals

The TokenizerService provides BPE tokenization for prefix-affinity routing, with multiple optimization layers to minimize reactor stalls during FFI calls to the Rust tokenizers library.

## Overview

The tokenizers-cpp library (Rust FFI) blocks the Seastar reactor for ~5-13ms per `Encode()` call. This is problematic for a high-throughput router where reactor stalls directly impact tail latency. The TokenizerService addresses this through three optimization layers:

1. **LRU Cache**: Eliminates redundant tokenization (80-90% hit rate for system messages)
2. **Cross-Shard Dispatch**: Offloads cache misses to other shards, freeing the calling reactor
3. **Dedicated Thread Pool**: Runs tokenization in OS threads outside Seastar's event loop entirely

## Architecture

```mermaid
flowchart TB
    subgraph "Per-Shard (Reactor Thread)"
        REQ[Request Handler]
        TS[TokenizerService]
        CACHE[LRU Cache<br/>O(1) lookup]
        TTP[TokenizerThreadPool]
    end

    subgraph "Cross-Shard (Other Reactor)"
        TS2[TokenizerService<br/>Target Shard]
        CACHE2[LRU Cache]
    end

    subgraph "Worker Thread (OS Thread)"
        WORKER[TokenizerWorker]
        TOK2[Dedicated Tokenizer<br/>Instance]
        QUEUE[SPSC Queue<br/>Lock-free]
    end

    REQ -->|encode_threaded_async| TS
    TS -->|1. lookup| CACHE
    CACHE -->|hit| TS
    TS -->|2. submit_async| TTP
    TTP --> QUEUE
    QUEUE --> WORKER
    WORKER --> TOK2
    WORKER -->|alien::run_on| TS

    TS -->|3. smp::submit_to| TS2
    TS2 --> CACHE2

    TS -->|4. local fallback| TOK[Local Tokenizer]

    classDef cache fill:#afa,stroke:#333
    classDef thread fill:#aff,stroke:#333
    classDef shard fill:#ffa,stroke:#333
    class CACHE,CACHE2 cache
    class WORKER,TOK2,QUEUE thread
    class TS2 shard
```

### Priority Order

`encode_threaded_async()` tries methods in order of reactor impact:

| Priority | Method | Reactor Blocked | Latency Overhead |
|----------|--------|-----------------|------------------|
| 1 | Cache hit | No | ~0 |
| 2 | Thread pool | No | ~10-50μs (queue + signal) |
| 3 | Cross-shard dispatch | Target shard only | ~1-10μs + copy |
| 4 | Local tokenization | Yes (5-13ms) | None |

## Layer 1: LRU Cache

The cache provides O(1) lookup for repeated texts, which is common for system messages and role tags.

### Configuration

```cpp
struct TokenizationCacheConfig {
    bool enabled = true;           // Enable/disable caching
    size_t max_entries = 1000;     // Maximum cache entries (Rule #4)
    size_t max_text_length = 8192; // Don't cache texts longer than this
};
```

### Expected Hit Rates

| Content Type | Hit Rate | Reason |
|--------------|----------|--------|
| System messages | 80-90% | Highly repetitive across requests |
| Role tags | 95%+ | e.g., `<|system|>\n` tokenized repeatedly |
| User queries | 10-30% | Depends on traffic patterns |

### Implementation

- **Data structure**: `absl::flat_hash_map` + `std::list` (LRU ordering)
- **Eviction**: Least-recently-used when at capacity
- **Thread safety**: Shard-local, no locks needed

## Layer 2: Cross-Shard Dispatch

On cache miss, dispatch tokenization to another shard's reactor. The calling shard's reactor is freed while waiting for the future.

### Configuration

```cpp
struct CrossShardTokenizationConfig {
    bool enabled = true;           // Enabled by default
    size_t min_text_length = 64;   // Skip for short texts (overhead > benefit)
    size_t max_text_length = 32768; // Skip for very long texts (copy overhead)
};
```

### Trade-offs

| Aspect | Impact |
|--------|--------|
| Calling reactor | **Freed** during tokenization |
| Target reactor | **Blocked** during FFI call |
| Latency | +1-10μs cross-shard overhead |
| Memory | String copied for transfer (Rule #14) |

### Shard Selection

Currently uses simple round-robin to the next shard:

```cpp
uint32_t select_tokenization_shard() const {
    uint32_t local = seastar::this_shard_id();
    return (local + 1) % seastar::smp::count;
}
```

## Layer 3: Thread Pool

For true non-blocking tokenization, dedicated OS threads run FFI calls outside Seastar's event loop entirely.

### Architecture

```
Reactor Thread (shard N)           Worker Thread N
─────────────────────────          ─────────────────
1. Create promise<Result>
2. Enqueue (job_id, text) ──────► 3. Dequeue job
3. Return future to caller        4. Tokenize (BLOCKING FFI)
   ↓                              5. alien::run_on(shard_N, complete)
[suspended]                          │
   ↓                                 │
6. complete(job_id, tokens) ◄────────┘
7. promise.set_value(tokens)
8. Future resolves
```

### Configuration

```cpp
struct ThreadPoolTokenizationConfig {
    bool enabled = false;          // Disabled by default (P3 priority)
    size_t max_queue_size = 256;   // Bounded queue (Rule #4)
    size_t min_text_length = 256;  // Skip for short texts
    size_t max_text_length = 65536; // Skip for very long texts
};
```

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `RANVIER_TOKENIZER_THREAD_POOL` | `false` | Enable thread pool |
| `RANVIER_TOKENIZER_THREAD_POOL_QUEUE_SIZE` | `256` | SPSC queue capacity |
| `RANVIER_TOKENIZER_THREAD_POOL_MIN_TEXT` | `256` | Minimum text length |
| `RANVIER_TOKENIZER_THREAD_POOL_MAX_TEXT` | `65536` | Maximum text length |

### YAML Configuration

```yaml
assets:
  tokenizer_thread_pool_enabled: true
  tokenizer_thread_pool_queue_size: 256
  tokenizer_thread_pool_min_text: 256
  tokenizer_thread_pool_max_text: 65536
```

### Key Components

| Component | Responsibility |
|-----------|----------------|
| `TokenizerWorker` | Owns worker thread + dedicated tokenizer instance |
| `TokenizerThreadPool` | Per-shard service managing lifecycle and promises |
| `TokenizationJob` | Job struct with owned text copy (Rule #14) |
| Thread-local callback | Completion signaling from worker to reactor |

### Thread Safety

| Component | Protection | Access Pattern |
|-----------|------------|----------------|
| Job queue | `boost::lockfree::spsc_queue` | Single producer, single consumer |
| Shutdown flag | `std::atomic<bool>` | Cross-thread visibility (Rule #11) |
| Statistics | `std::atomic<uint64_t>` | Lock-free increments |
| Pending jobs map | None (shard-local) | Reactor thread only |

### Backpressure

When the SPSC queue is full, `submit_async()` returns `std::nullopt` and the caller falls back to cross-shard dispatch or local tokenization:

```cpp
if (!_worker->submit(std::move(job))) {
    ++_jobs_fallback;
    return std::nullopt;  // Caller should try other methods
}
```

### Lifecycle

```mermaid
stateDiagram-v2
    [*] --> Created: constructor
    Created --> Configured: load_tokenizer(json)
    Configured --> Running: start_worker(alien)
    Running --> Running: submit_async()
    Running --> Stopping: stop_worker()
    Stopping --> Stopped: thread joined
    Stopped --> [*]: stop()
```

**Shutdown sequence** (in `application.cpp`):

1. `invoke_on_all([](pool) { pool.stop_worker(); })` — joins all worker threads
2. `_tokenizer_thread_pool.stop()` — resolves pending promises, clears metrics

## Monitoring

### Prometheus Metrics

#### Cache Metrics (TokenizerService)

| Metric | Type | Description |
|--------|------|-------------|
| `ranvier_tokenizer_cache_hits` | Counter | Cache hit count |
| `ranvier_tokenizer_cache_misses` | Counter | Cache miss count |
| `ranvier_tokenizer_cache_size` | Gauge | Current cache entries |

#### Cross-Shard Metrics (TokenizerService)

| Metric | Type | Description |
|--------|------|-------------|
| `ranvier_tokenizer_cross_shard_dispatches` | Counter | Cache misses sent to other shards |
| `ranvier_tokenizer_local_fallbacks` | Counter | Tokenization done locally |

#### Thread Pool Metrics (TokenizerThreadPool)

| Metric | Type | Description |
|--------|------|-------------|
| `ranvier_tokenizer_thread_pool_jobs_submitted` | Counter | Jobs submitted to worker |
| `ranvier_tokenizer_thread_pool_jobs_completed` | Counter | Jobs completed by worker |
| `ranvier_tokenizer_thread_pool_jobs_fallback` | Counter | Fallback due to queue full |
| `ranvier_tokenizer_thread_pool_pending_jobs` | Gauge | Currently pending jobs |
| `ranvier_tokenizer_thread_pool_worker_running` | Gauge | Worker thread status (1=yes) |
| `ranvier_tokenizer_thread_pool_enabled` | Gauge | Configuration status |

#### Latency Histograms (MetricsService)

| Metric | Buckets | Description |
|--------|---------|-------------|
| `ranvier_router_tokenization_latency_seconds` | 100μs–100ms | Total tokenization latency (end-to-end) |
| `ranvier_router_primary_tokenization_latency_seconds` | 100μs–100ms | Primary prompt tokenization (excludes boundary detection) |
| `ranvier_router_boundary_detection_latency_seconds` | 100μs–100ms | Prefix boundary detection (message tokenization for boundaries) |
| `ranvier_router_routing_latency_seconds` | 100μs–100ms | Routing decision latency (ART lookup + backend selection) |
| `ranvier_router_art_lookup_latency_seconds` | 100μs–100ms | ART radix tree lookup only |

The primary + boundary split allows operators to identify whether tokenization time is spent on the main prompt encode or on the boundary-detection pass that tokenizes individual messages to find prefix split points.

### Grafana Query Examples

```promql
# Cache hit ratio
sum(rate(ranvier_tokenizer_cache_hits[5m])) /
(sum(rate(ranvier_tokenizer_cache_hits[5m])) + sum(rate(ranvier_tokenizer_cache_misses[5m])))

# Thread pool utilization
sum(rate(ranvier_tokenizer_thread_pool_jobs_submitted[5m])) /
sum(rate(ranvier_tokenizer_cache_misses[5m]))

# Queue full fallback rate
sum(rate(ranvier_tokenizer_thread_pool_jobs_fallback[5m])) /
sum(rate(ranvier_tokenizer_thread_pool_jobs_submitted[5m]))

# Boundary detection fraction of total tokenization time
sum(rate(ranvier_router_boundary_detection_latency_seconds_sum[5m])) /
sum(rate(ranvier_router_tokenization_latency_seconds_sum[5m]))

# Average primary tokenization latency (p50 approximation)
sum(rate(ranvier_router_primary_tokenization_latency_seconds_sum[5m])) /
sum(rate(ranvier_router_primary_tokenization_latency_seconds_count[5m]))
```

## Performance Characteristics

| Method | Reactor Blocked | Per-Request Latency | Throughput Impact |
|--------|-----------------|---------------------|-------------------|
| Cache hit | No | ~0 | None |
| Thread pool | No | +10-50μs | None |
| Cross-shard | Target only | +1-10μs | Target shard reduced |
| Local | Yes (5-13ms) | +5-13ms | All traffic affected |

### When to Enable Thread Pool

Enable `tokenizer_thread_pool_enabled: true` when:

1. **Single-shard deployments**: Cross-shard dispatch provides no benefit
2. **High cache miss rate**: Many unique texts reduce cache effectiveness
3. **Latency-sensitive workloads**: Even target shard blocking is unacceptable
4. **Benchmark shows stalls**: Use Prometheus metrics to identify reactor stalls

### When to Keep Disabled (Default)

Keep disabled when:

1. **High cache hit rate (>80%)**: Most requests served from cache
2. **Multi-shard deployment**: Cross-shard dispatch already effective
3. **Memory constrained**: Each shard's worker has its own tokenizer instance

## Tuning Guidelines

### High Cache Hit Scenarios

```yaml
# Maximize cache, skip thread pool overhead
assets:
  tokenizer_cache_max_entries: 5000
  tokenizer_thread_pool_enabled: false
```

### Low Cache Hit / Single Shard

```yaml
# Enable thread pool for non-blocking tokenization
assets:
  tokenizer_thread_pool_enabled: true
  tokenizer_thread_pool_queue_size: 512
  tokenizer_thread_pool_min_text: 128  # Lower threshold
```

### High Throughput

```yaml
# Larger queue, aggressive caching
assets:
  tokenizer_cache_max_entries: 10000
  tokenizer_thread_pool_enabled: true
  tokenizer_thread_pool_queue_size: 1024
```

## Thread Pool Implementation Details

### SPSC Queue

Uses `boost::lockfree::spsc_queue` for lock-free, single-producer (reactor) single-consumer (worker) communication:

```cpp
boost::lockfree::spsc_queue<TokenizationJob> _job_queue;
```

### Worker Loop

The worker uses a spin-wait pattern to balance latency and CPU usage:

```cpp
constexpr auto SPIN_ITERATIONS = 1000;
constexpr auto SLEEP_DURATION = std::chrono::microseconds(100);

while (!_shutdown.load(std::memory_order_acquire)) {
    // Spin briefly for low latency
    for (int i = 0; i < SPIN_ITERATIONS; ++i) {
        if (_job_queue.pop(job)) { process_job(job); break; }
    }
    // Sleep if no job found
    std::this_thread::sleep_for(SLEEP_DURATION);
}
```

### Completion Signaling

Worker signals completion back to reactor via `seastar::alien::run_on()`:

```cpp
seastar::alien::run_on(alien_instance, target_shard,
    [job_id, tokens = std::move(tokens), success]() noexcept {
        if (auto* callback = get_thread_pool_completion_callback()) {
            (*callback)(job_id, std::move(tokens), success);
        }
    });
```

The callback is set per-shard via `static thread_local` to ensure lifetime (Rule #13).

## Memory Safety Across Thread Boundaries

Seastar replaces `malloc` globally with a per-shard allocator. Every allocation — even on worker threads — goes through this allocator and is tracked against a specific shard. This creates two hazards when data crosses the reactor/worker boundary:

1. **Foreign memory pinning**: Memory allocated on the reactor (shard N) that is held by the worker thread pins shard N's allocator resources on a foreign thread, preventing normal shard-local reclamation.
2. **`foreign_malloc` / `do_foreign_free` overhead**: Memory allocated on the worker thread is tracked as a `foreign_malloc`. When the reactor later frees it, Seastar must use `do_foreign_free`, which is slower and, when combined with Rust FFI, can cause memory corruption under stress (SIGSEGV with corrupted pointers).

### Bidirectional Reallocation Pattern

The thread pool avoids both hazards by copying data at each thread boundary crossing:

**Input: reactor → worker** (`tokenizer_thread_pool.cpp:148`)

```cpp
// Reallocate the string on the worker thread so the reactor shard's
// per-shard memory isn't held across the FFI call.
std::string local_text(job.text.data(), job.text.size());
```

The job already contains an owned `std::string` copy (Rule #14), but that copy was allocated on the reactor's shard. Re-copying on the worker thread ensures the FFI call operates on worker-allocated memory, freeing the reactor's allocation for immediate reclaim.

**Output: worker → reactor** (inside `alien::run_on()`, `tokenizer_thread_pool.cpp:171`)

```cpp
// Copy tokens into shard-local memory. The captured vector was
// allocated on the worker thread (foreign_malloc). Copying here
// keeps the hot path on the shard's own allocator and avoids
// do_foreign_free overhead on the reactor.
std::vector<int32_t> local_tokens(tokens.begin(), tokens.end());
```

The result vector from `Encode()` was allocated on the worker thread. The `alien::run_on()` lambda captures it by move, then immediately copies it into a shard-local vector before passing to the completion callback. This ensures the reactor only ever owns shard-local memory.

### Rust-Side Allocator Isolation (jemalloc)

Even with bidirectional reallocation on the C++ side, the Rust tokenizer library makes its own internal allocations during `Encode()`. Without intervention, these would go through Seastar's global malloc replacement, creating `foreign_malloc` tracking on worker threads and risking corruption when Rust's internal allocator interacts with `do_foreign_free`.

The solution is a build-time patch that gives Rust its own allocator entirely. In `CMakeLists.txt` (lines 173-228) and `Dockerfile.base` (lines 63-83), the build injects `tikv-jemallocator` into the tokenizers-cpp Rust crate:

```toml
# Cargo.toml (patched)
[dependencies]
tikv-jemallocator = "0.6"
```

```rust
// lib.rs (patched)
use tikv_jemallocator::Jemalloc;

#[global_allocator]
static GLOBAL: Jemalloc = Jemalloc;
```

This gives Rust a completely independent jemalloc instance. All Rust-internal allocations (`Vec`, `String`, `HashMap`, etc.) bypass Seastar's allocator entirely, eliminating any interaction between the two memory systems.

**Trade-offs:**

| Aspect | Impact |
|--------|--------|
| Binary size | +~300KB for jemalloc |
| Memory overhead | Two allocators in process (potential fragmentation) |
| Maintenance | Inline patching (no fork to maintain) |
| Safety | Complete memory isolation — no `foreign_malloc` corruption risk from Rust |

> With jemalloc patching in place, the C++ bidirectional reallocation is still maintained as defense-in-depth: it avoids pinning reactor memory on the worker thread and keeps the reactor's hot path on its own shard allocator.

## Local Fallback Semaphore

When both the thread pool and cross-shard dispatch are unavailable (queue full, text out of range, etc.), `encode_threaded_async()` falls through to Priority 4: local tokenization. This calls `tokenize_locally()`, which blocks the reactor for 5-13ms.

Without concurrency control, multiple concurrent requests hitting Priority 4 on the same shard would compound the stall — each blocking call adds another 5-13ms during which the reactor cannot process events. On a busy shard, this can cascade into hundreds of milliseconds of reactor stall.

### Semaphore Gating

A `seastar::semaphore` limits concurrent local tokenizations per shard (`tokenizer_service.hpp:195-198`):

```cpp
// Configure the shard-local semaphore that gates Priority 4 (local fallback)
// in encode_threaded_async(). Default is 1 concurrent local tokenization.
// Setting to 0 effectively disables the local fallback entirely.
void configure_local_fallback(size_t max_concurrent);
```

The implementation uses `try_wait()` for non-blocking acquisition (`tokenizer_service.cpp:377-383`):

```cpp
// Priority 4: Local tokenization (blocks reactor) — gated by semaphore
if (!_local_tokenize_sem.try_wait(1)) {
    ++_local_fallback_rejected;
    return seastar::make_ready_future<TokenizationResult>(TokenizationResult{});
}
// Scope guard: signal semaphore after tokenize_locally() returns (exception safety)
auto sem_guard = seastar::defer([this] { _local_tokenize_sem.signal(1); });
return seastar::make_ready_future<TokenizationResult>(tokenize_locally(text));
```

If the semaphore is already exhausted, the request gets an empty `TokenizationResult` and the caller falls back to hash or random routing (no prefix-affinity, but no reactor stall either).

### Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| `tokenizer_local_fallback_max_concurrent` | `1` | Max concurrent reactor-blocking tokenizations per shard |
| `RANVIER_TOKENIZER_LOCAL_FALLBACK_MAX_CONCURRENT` | — | Environment variable override |

```yaml
assets:
  tokenizer_local_fallback_max_concurrent: 1  # At most one blocking tokenization per shard
```

### Tuning Guidance

- **Default (1)**: Allows one blocking tokenization at a time. If a second request arrives while the first is blocking, it gets empty tokens and routes via hash/random. This is the safest setting — at most 5-13ms of reactor stall per event.
- **0**: Disables local fallback entirely. Every cache miss that can't use the thread pool or cross-shard dispatch returns empty tokens. Use this when reactor responsiveness is paramount.
- **2-3**: Allows limited compounding. Only appropriate if your workload has very low cache miss rates and you want to maximize prefix-affinity coverage at the cost of occasional 10-26ms stalls.

## References

- [Architecture Overview](../architecture/system-design.md)
- [Request Flow](../request-flow.md)
- [Performance Benchmarks](../deployment/performance.md)
- [Prefix Affinity Routing](./prefix-affinity-routing.md)
- [Seastar Alien API](https://docs.seastar.io/master/namespaceseastar_1_1alien.html)
- [Boost Lockfree SPSC Queue](https://www.boost.org/doc/libs/release/doc/html/lockfree/examples.html)
