# Shard-Aware Load Balancing

Ranvier uses a shard-aware load balancer to distribute incoming HTTP requests across Seastar shards (CPU cores) based on real-time load metrics. This prevents "hot shards" where one CPU core is at 100% utilization while others are idle.

## Overview

The load balancer uses the **Power of Two Choices (P2C)** algorithm, which provides near-optimal load distribution with O(1) overhead per request.

### How P2C Works

1. Randomly select 2 candidate shards (uniformly, with a deterministic bump if both draws collide)
2. Read each candidate's load score from the local snapshot cache
3. Pick the lower-scoring candidate
4. If the winner is a remote shard, only honor the choice when the relative improvement vs. the local shard exceeds `min_load_difference`; otherwise stay local

This avoids the O(n) cost of querying all shards while still achieving excellent load distribution. Research shows P2C reduces maximum load from O(log n / log log n) to O(log log n).

### Implementation Status

`ShardLoadBalancer::select_shard()` is fully implemented and wired into `HttpController::select_target_shard()`. **However, the HTTP request hot path does not yet act on the selection** — per the source comment in `http_controller.cpp`, the computed shard ID is currently advisory and requests are still processed on the receiving shard. The selection logic and metrics are exercised so the snapshot cache stays warm; actual `smp::submit_to` dispatch from the HTTP layer is a follow-up.

Cross-shard dispatch *is* used today by `TokenizerService` for offloading large tokenization work (round-robin shard selection, gated on `_load_balancer != nullptr`).

## Architecture

### Components

```
┌─────────────────────────────────────────────────────────────────┐
│                        HttpController                           │
│  ┌─────────────┐    ┌───────────────────┐    ┌───────────────┐  │
│  │  Incoming   │───>│  ShardLoadBalancer│───>│ Target Shard  │  │
│  │  Request    │    │  (P2C Selection)  │    │   (advisory)  │  │
│  └─────────────┘    └───────────────────┘    └───────────────┘  │
└─────────────────────────────────────────────────────────────────┘

Per-Shard Components (thread-local, lock-free):
┌─────────────────┐    ┌─────────────────────┐
│ ShardLoadMetrics│───>│ Snapshot Cache      │
│ (thread_local)  │    │ (vector<Snapshot>)  │
└─────────────────┘    └─────────────────────┘
```

### Key Files

| File | Purpose |
|------|---------|
| `shard_load_metrics.hpp` | Per-shard counters (active, queued, total) + RAII guards |
| `shard_load_balancer.hpp` | P2C algorithm, snapshot cache, Prometheus metrics |
| `cross_shard_request.hpp` | Move-only request context + `CrossShardDispatcher` for safe inter-shard transfer |

## Load Metrics

Each shard tracks three counters via `ShardLoadMetrics`:

- **Active Requests** (`active_requests`): Currently processing (CPU-bound)
- **Queued Requests** (`queued_requests`): Waiting on a semaphore (I/O-bound)
- **Total Requests** (`total_requests`): Cumulative count, recorded on completion

All counters are `uint64_t` with no atomics — safe because Seastar is shared-nothing
(one thread per shard) and cross-shard reads go through `submit_to()`, which
provides the necessary memory barrier. Use `ActiveRequestGuard` /
`QueuedRequestGuard` (RAII, non-copyable, non-movable) to increment/decrement.

### Load Score Calculation

```cpp
// shard_load_metrics.hpp
double load_score() const {
    return static_cast<double>(active_requests) * 2.0 +
           static_cast<double>(queued_requests);
}
```

Active requests are weighted 2x because they consume CPU; queued requests are
just waiting. `total_requests` is not part of the score.

## Configuration

`ShardLoadBalancerConfig` is constructed from `RanvierConfig::load_balancing`
in `application.cpp`. The defaults below come from `config_infra.hpp` /
`shard_load_balancer.hpp`:

| Option | Default | Description |
|--------|---------|-------------|
| `enabled` | `true` | Enable cross-shard load balancing (auto-disabled when `smp::count <= 1`) |
| `min_load_difference` | `0.2` | Minimum *relative* improvement (`(local - selected) / local`) required to honor a cross-shard pick |
| `local_processing_threshold` | `10` | Skip P2C entirely if local active requests is below this |
| `snapshot_refresh_interval_us` | `1000` | Intended snapshot cache refresh interval (μs). **Currently unused** — see "Snapshot Cache" below |
| `adaptive_mode` | `false` | Reserved for future latency-driven adaptation; no effect today |

`update_config()` allows hot-reloading these values per shard via
`HttpController::update_load_balancer_config()`.

> **Note on YAML loading:** `config_loader.cpp` does **not** currently parse a
> top-level `load_balancing:` section from `ranvier.yaml`, so YAML overrides for
> these fields are not picked up. The values reflect either the compiled-in
> defaults or programmatic overrides. Add a YAML section + parser before relying
> on YAML configuration of the load balancer.

## Prometheus Metrics

Registered under the `ranvier_load_balancer_*` group on every shard
(`ShardLoadBalancer::register_metrics()`):

| Metric | Type | Description |
|--------|------|-------------|
| `ranvier_load_balancer_local_dispatches` | Counter | Requests kept on the receiving shard |
| `ranvier_load_balancer_cross_shard_dispatches` | Counter | P2C selections that targeted a different shard with sufficient improvement |
| `ranvier_load_balancer_p2c_selections` | Counter | P2C selections that were *honored* (excludes the fast-path skip and the `min_load_difference` early-return) |
| `ranvier_load_balancer_enabled` | Gauge | `1.0` if load balancing is enabled, `0.0` otherwise |

Counter accounting in `select_shard()` / `select_shard_p2c()`:

- Fast path (`active < local_processing_threshold`): `++local_dispatches` only
- P2C picks local: `++local_dispatches`, `++p2c_selections`
- P2C picks remote but improvement `< min_load_difference`: `++local_dispatches` (no `p2c_selections` bump)
- P2C picks remote with sufficient improvement: `++cross_shard_dispatches`, `++p2c_selections`

So `local_dispatches + cross_shard_dispatches == total selections`, while
`p2c_selections` is strictly the count of selections where the P2C result was
ultimately respected.

### Example Queries

```promql
# Cross-shard dispatch ratio
sum(rate(ranvier_load_balancer_cross_shard_dispatches[5m])) /
sum(
  rate(ranvier_load_balancer_local_dispatches[5m]) +
  rate(ranvier_load_balancer_cross_shard_dispatches[5m])
)

# Fraction of P2C picks that were short-circuited by min_load_difference
1 - (
  sum(rate(ranvier_load_balancer_p2c_selections[5m])) /
  sum(
    rate(ranvier_load_balancer_local_dispatches[5m]) +
    rate(ranvier_load_balancer_cross_shard_dispatches[5m])
  )
)
```

There is currently no per-shard `active_requests` Prometheus gauge exposed by
this subsystem; if you need one, expose `ShardLoadMetrics::active_requests()`
explicitly.

## Performance Characteristics

- **Overhead**: O(1) per request — two PRNG draws + two cached `load_score()` reads
- **Latency Impact**: ~100 ns for shard selection
- **Memory**: `sizeof(ShardLoadSnapshot) * smp::count` per shard for the cache (40 bytes × shard count)

### When Load Balancing Helps

1. **Uneven Connection Distribution**: HTTP connections may cluster on certain shards
2. **Long-Running Requests**: LLM inference requests can run for seconds
3. **Burst Traffic**: Sudden spikes may overwhelm individual shards

### When to Disable

- Single-core deployments (the balancer auto-no-ops when `smp::count <= 1`)
- Very low traffic where overhead matters
- When requests are already evenly distributed

## Implementation Details

### Fast Path Optimization

For lightly loaded shards, the balancer skips P2C entirely:

```cpp
// shard_load_balancer.hpp
if (shard_load_metrics_initialized()) {
    auto local_active = shard_load_metrics().active_requests();
    if (local_active < _config.local_processing_threshold) {
        ++_local_dispatches;
        return seastar::this_shard_id();
    }
}
```

### Snapshot Cache

`_snapshot_cache` is a `std::vector<ShardLoadSnapshot>` of size `smp::count`.
It is **not** refreshed by a background timer today:

- The local entry is updated synchronously per request in
  `HttpController::select_target_shard()` via `update_local_snapshot()`.
- Remote entries are populated only when `select_shard_async()` or
  `refresh_all_snapshots()` is invoked — neither has a caller in the current
  codebase, so remote entries remain at default (zero) until something fetches
  them.

Practically, this means P2C currently compares the live local load against
mostly-empty remote snapshots, biasing toward staying local once warm. Wiring
`refresh_all_snapshots()` to a periodic timer governed by
`snapshot_refresh_interval_us` is the intended next step.

`fetch_shard_snapshot()` uses `seastar::smp::submit_to(shard_id, ...)`; if
metrics aren't initialized on the target shard it returns an empty snapshot
stamped with the current shard ID and time.

### Cross-Shard Safety

`CrossShardDispatcher::dispatch()` (in `cross_shard_request.hpp`) wraps the
context in `unique_ptr` then `seastar::foreign_ptr` for safe transfer:

```cpp
auto ctx_ptr = std::make_unique<CrossShardRequestContext>(std::move(ctx));
auto foreign = seastar::make_foreign(std::move(ctx_ptr));
return seastar::smp::submit_to(target_shard,
    [foreign = std::move(foreign), handler = std::forward<Handler>(handler)]() mutable {
        // CRITICAL: re-allocate std::string / std::vector members on the target
        // shard. The body uses temporary_buffer (Seastar-aware) and is moved as-is.
        CrossShardRequestContext local_ctx =
            std::move(*foreign).force_local_allocation();
        return handler(std::move(local_ctx));
    });
```

`force_local_allocation()` is mandatory: heap allocations made on the source
shard cannot be freed on a different shard under Seastar's per-shard allocator,
which would otherwise SIGSEGV at destruction time. `temporary_buffer<char>` is
Seastar-aware and is moved directly without re-copying — that's the zero-copy
path for the request body. `dispatch_with_foreign()` exists for callers that
already hold a `unique_ptr`.

Validated factory functions (`cross_shard::try_create_from_request`,
`try_create_from_buffer`, `try_create_with_tokens`) enforce the limits in
`CrossShardRequestLimits` (128 MB body, 128K tokens, 4 KB string fields,
8 KB path) before allocation.

### Thread-Local Metrics

Each shard has its own `ShardLoadMetrics` instance, lazily created on first access:

```cpp
// shard_load_metrics.hpp
inline thread_local std::unique_ptr<ShardLoadMetrics> g_shard_load_metrics;

inline ShardLoadMetrics& shard_load_metrics() {
    if (!g_shard_load_metrics) {
        g_shard_load_metrics = std::make_unique<ShardLoadMetrics>();
    }
    return *g_shard_load_metrics;
}
```

`init_shard_load_metrics()` is called per shard during application startup; the
lazy guard in `shard_load_metrics()` ensures correctness even if accessed before
explicit init.

## References

- Mitzenmacher, M. (2001). "The Power of Two Choices in Randomized Load Balancing"
- Lu, Y. et al. (2011). "Join-Idle-Queue: A novel load balancing algorithm for dynamically scalable web services"
