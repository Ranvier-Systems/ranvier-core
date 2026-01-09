# Shard-Aware Load Balancing

Ranvier uses a shard-aware load balancer to distribute incoming HTTP requests across Seastar shards (CPU cores) based on real-time load metrics. This prevents "hot shards" where one CPU core is at 100% utilization while others are idle.

## Overview

The load balancer uses the **Power of Two Choices (P2C)** algorithm, which provides near-optimal load distribution with O(1) overhead per request.

### How P2C Works

1. Randomly select 2 candidate shards
2. Query load metrics from both candidates (using cached snapshots)
3. Route to the shard with the lower load score

This simple approach avoids the O(n) cost of querying all shards while still achieving excellent load distribution. Research shows P2C reduces maximum load from O(log n / log log n) to O(log log n).

## Architecture

### Components

```
┌─────────────────────────────────────────────────────────────────┐
│                        HttpController                           │
│  ┌─────────────┐    ┌──────────────────┐    ┌───────────────┐  │
│  │  Incoming   │───>│  ShardLoadBalancer│───>│ Target Shard  │  │
│  │  Request    │    │  (P2C Selection)  │    │ (Processing)  │  │
│  └─────────────┘    └──────────────────┘    └───────────────┘  │
└─────────────────────────────────────────────────────────────────┘

Per-Shard Components:
┌─────────────────┐    ┌─────────────────────┐
│ ShardLoadMetrics│───>│ Snapshot Cache      │
│ (Thread-local)  │    │ (All shards' load)  │
└─────────────────┘    └─────────────────────┘
```

### Key Files

| File | Purpose |
|------|---------|
| `shard_load_metrics.hpp` | Per-shard load tracking (active requests, queue depth) |
| `shard_load_balancer.hpp` | P2C algorithm implementation |
| `cross_shard_request.hpp` | Zero-copy request context transfer |

## Load Metrics

Each shard tracks:

- **Active Requests**: Currently processing requests (CPU-bound)
- **Queued Requests**: Requests waiting for semaphore (I/O-bound)
- **Total Requests**: Cumulative count (for throughput calculation)

### Load Score Calculation

```cpp
load_score = active_requests * 2.0 + queued_requests * 1.0
```

Active requests are weighted higher because they consume CPU, while queued requests are just waiting for I/O.

## Configuration

Add to your `ranvier.yaml`:

```yaml
load_balancing:
  enabled: true                        # Enable cross-shard load balancing
  min_load_difference: 0.2             # Min 20% load difference to trigger dispatch
  local_processing_threshold: 10       # Process locally if < 10 active requests
  snapshot_refresh_interval_us: 1000   # Refresh snapshots every 1ms
```

### Configuration Options

| Option | Default | Description |
|--------|---------|-------------|
| `enabled` | `true` | Enable/disable cross-shard load balancing |
| `min_load_difference` | `0.2` | Minimum load difference (ratio) to trigger cross-shard dispatch |
| `local_processing_threshold` | `10` | Process locally if active requests below this threshold |
| `snapshot_refresh_interval_us` | `1000` | Snapshot cache refresh interval in microseconds |

## Prometheus Metrics

The load balancer exports the following metrics:

| Metric | Type | Description |
|--------|------|-------------|
| `ranvier_load_balancer_local_dispatches` | Counter | Requests processed on receiving shard |
| `ranvier_load_balancer_cross_shard_dispatches` | Counter | Requests dispatched to different shard |
| `ranvier_load_balancer_p2c_selections` | Counter | Total P2C algorithm invocations |
| `ranvier_load_balancer_enabled` | Gauge | Whether load balancing is enabled (1/0) |

### Example Queries

```promql
# Cross-shard dispatch ratio
sum(rate(ranvier_load_balancer_cross_shard_dispatches[5m])) /
sum(rate(ranvier_load_balancer_local_dispatches[5m]) + rate(ranvier_load_balancer_cross_shard_dispatches[5m]))

# Per-shard active requests
ranvier_shard_active_requests
```

## Performance Characteristics

- **Overhead**: O(1) per request (cached snapshot lookup + random selection)
- **Latency Impact**: Negligible (~100ns for shard selection)
- **Memory**: ~100 bytes per shard for snapshot cache

### When Load Balancing Helps

1. **Uneven Connection Distribution**: HTTP connections may cluster on certain shards
2. **Long-Running Requests**: LLM inference requests can run for seconds
3. **Burst Traffic**: Sudden spikes may overwhelm individual shards

### When to Disable

- Single-core deployments (`smp::count == 1`)
- Very low traffic where overhead matters
- When requests are already evenly distributed

## Implementation Details

### Fast Path Optimization

For lightly loaded shards, the balancer skips P2C entirely:

```cpp
if (active_requests < local_processing_threshold) {
    return local_shard;  // No cross-shard overhead
}
```

### Cross-Shard Safety

Request contexts are transferred using Seastar's `foreign_ptr` to ensure safe cross-shard memory access:

```cpp
auto foreign = seastar::make_foreign(std::make_unique<Context>(std::move(ctx)));
return smp::submit_to(target_shard, [foreign = std::move(foreign)] {
    return process(std::move(*foreign));
});
```

### Thread-Local Metrics

Each shard maintains its own metrics instance using thread-local storage:

```cpp
inline thread_local std::unique_ptr<ShardLoadMetrics> g_shard_load_metrics;
```

This enables lock-free metric updates on the hot path.

## References

- Mitzenmacher, M. (2001). "The Power of Two Choices in Randomized Load Balancing"
- Lu, Y. et al. (2011). "Join-Idle-Queue: A novel load balancing algorithm for dynamically scalable web services"
