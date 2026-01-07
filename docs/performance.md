# Performance Benchmarks

This document provides benchmark results and performance expectations for Ranvier Core v1.0.

## Benchmark Environment

### 3-Node Gossip Cluster Configuration

| Component | Specification |
|-----------|---------------|
| Nodes | 3x Ranvier instances |
| Architecture | Seastar shared-nothing, thread-per-core |
| Cluster Protocol | UDP Gossip (port 8481) |
| Backend Routing | `absl::flat_hash_map` with SIMD-accelerated lookups |
| Radix Tree | Adaptive Radix Tree (ART) with Node4→16→48→256 |

---

## Baseline vs. Optimized Performance

### P99 Latency Improvement

After migrating to Abseil high-performance containers (`absl::flat_hash_map`), benchmark results show:

| Metric | Baseline | Optimized | Improvement |
|--------|----------|-----------|-------------|
| P99 TTFT | 71.4ms | 66ms | **7.6%** |
| P95 TTFT | 58.2ms | 54ms | 7.2% |
| P50 TTFT | 18ms | 17ms | 5.6% |

The improvement is attributed to:
1. **SIMD-accelerated lookups**: `absl::flat_hash_map` uses SSE2/AVX2 for parallel key comparisons
2. **Better cache locality**: Flat storage layout minimizes cache misses during routing decisions
3. **Reduced allocations**: Abseil containers have lower allocation overhead than `std::unordered_map`
4. **Slab allocation**: RadixTree nodes use a per-shard slab allocator (`NodeSlab`) with O(1) intrusive free list, eliminating malloc/free on hot paths

### Cache Hit vs. Miss Latency

| Scenario | Avg Latency | P99 Latency | Notes |
|----------|-------------|-------------|-------|
| Cache Hit (Prefix Match) | ~18ms | ~25ms | Route directly to backend with hot KV-cache |
| Cache Miss (Random) | ~500ms | ~520ms | Full prefill required on backend |

**Speedup Factor**: Cache hits achieve approximately **28x lower latency** compared to cache misses.

---

## Virtualized vs. Bare Metal

### Latency Expectations by Environment

| Environment | P99 TTFT | P95 TTFT | Notes |
|-------------|----------|----------|-------|
| **Bare Metal** | 45-55ms | 35-45ms | Direct NIC access, no virtualization overhead |
| **Docker (Linux)** | 60-70ms | 50-60ms | Standard bridge networking |
| **Docker Desktop (macOS)** | 80-100ms | 65-85ms | Additional VM layer (HyperKit/Virtualization.framework) |
| **Kubernetes** | 65-75ms | 55-65ms | CNI plugin overhead varies |

### Verified Cluster Benchmarks

A 3-node Gossip-based cluster achieved stable performance:

```
┌─────────────────────────────────────────────────────────┐
│  3-Node Cluster Benchmark Results                       │
├─────────────────────────────────────────────────────────┤
│  P99 TTFT:     66ms (stable)                            │
│  P95 TTFT:     54ms                                     │
│  P50 TTFT:     17ms                                     │
│  Cache Hit Rate: 89% (after warmup)                     │
│  Gossip Sync Lag: <50ms                                 │
└─────────────────────────────────────────────────────────┘
```

---

## Locust Load Test Results

### Test Configuration

```bash
locust -f tests/integration/locustfile.py \
    --headless -u 10 -r 2 \
    --host http://localhost:8080 \
    --run-time 30s
```

### Sample Results

```
Type     Name                    # reqs   # fails |   Avg    Min    Max    Med |  req/s
---------|----------------------|--------|--------|-------|------|------|------|-------
POST     Random_Noise_Miss           148   0(0.00%)|   506    501    521    510 |   4.95
POST     Viral_Context_Hit           497   0(0.00%)|    18     11    507     16 |  16.61
---------|----------------------|--------|--------|-------|------|------|------|-------
         Aggregated                  645   0(0.00%)|   130     11    521     17 |  21.56

Response time percentiles (approximated)
Type     Name                          50%    66%    75%    90%    95%    99%   100%
---------|-------------------------|------|------|------|------|------|------|------
POST     Random_Noise_Miss             510    510    510    510    510    520    520
POST     Viral_Context_Hit              16     17     17     19     20     31    510
```

### Key Observations

1. **First Request Penalty**: The max latency of 507ms on `Viral_Context_Hit` represents the initial cache miss. All subsequent requests hit the learned route.
2. **Consistent Cache Performance**: P99 for cache hits is 31ms, demonstrating stable routing performance under load.
3. **Zero Failures**: The proxy handles all requests without errors, even under concurrent load.

---

## Performance Tuning

### Recommended Settings

```yaml
# ranvier.yaml - Production tuning
router:
  min_token_length: 32          # Avoid routing on short prefixes
  enable_token_forwarding: true # Reduce backend CPU by 10-15%

connection_pool:
  max_connections_per_backend: 16
  connection_timeout_ms: 5000
  idle_timeout_seconds: 300

health:
  interval_seconds: 5
  failure_threshold: 3
  timeout_ms: 2000
```

### Shard Configuration

For optimal performance, match the number of Seastar shards to available CPU cores:

```bash
# Auto-detect cores (recommended)
./ranvier_server -c $(nproc)

# Explicit shard count
./ranvier_server -c 8
```

---

## Metrics for Monitoring

Key Prometheus metrics to track performance:

| Metric | Description | Target |
|--------|-------------|--------|
| `ranvier_router_cache_hits` | Total cache hit count | Increasing |
| `ranvier_router_cache_misses` | Total cache miss count | Low ratio |
| `ranvier_http_request_duration_seconds` | Request latency histogram | P99 < 100ms |
| `ranvier_pool_connections_reused` | Connection pool efficiency | High rate |
| `ranvier_cluster_peers_alive` | Healthy cluster nodes | = expected nodes |

### Grafana Query Examples

```promql
# Cache hit ratio
sum(rate(ranvier_router_cache_hits[5m])) /
(sum(rate(ranvier_router_cache_hits[5m])) + sum(rate(ranvier_router_cache_misses[5m])))

# P99 latency by shard
histogram_quantile(0.99, sum(rate(ranvier_http_request_duration_seconds_bucket[5m])) by (le, shard))
```

---

## References

- [Architecture Overview](architecture.md)
- [Request Flow Diagrams](request-flow.md)
- [API Reference](api-reference.md)
- [Locust Test Configuration](../tests/integration/README.md)
- [KV Cache Prefix-Routing Benchmark](benchmarks/kv-cache-prefix-routing-benchmark.md)

---

## Large-Prefix KV Cache Benchmark

For production LLM workloads with large context windows, see our [detailed benchmark](benchmarks/kv-cache-prefix-routing-benchmark.md) comparing routing modes on 8x A100 GPUs with Llama-3.1-8B-Instruct.

### Summary Results (2000-8000 token prefixes)

| Routing Mode | Cache Hit Rate | TTFT P50 (hit) | TTFT P99 (hit) |
|--------------|----------------|----------------|----------------|
| Round-Robin | 14.5% | 433.8ms | 925.1ms |
| **Prefix-Affinity** | **94.1%** | **433.5ms** | **487.4ms** |

### Key Findings

- **6.5x better cache hit rate** with prefix-affinity routing
- **47% lower P99 tail latency** for cache hits
- **37% TTFT improvement** for XLarge prefixes (4-8K tokens)
- **7% higher throughput** overall (847 vs 800 requests/5min)

### Routing Modes

Configure via `RANVIER_ROUTING_MODE`:

| Mode | Description | Use Case |
|------|-------------|----------|
| `prefix` | ART + consistent hashing | **Production LLM inference** |
| `radix` | ART + random fallback | Adaptive learning |
| `round_robin` | Random/weighted | Baseline testing |

```bash
# Enable prefix-affinity routing (recommended)
export RANVIER_ROUTING_MODE=prefix
```
