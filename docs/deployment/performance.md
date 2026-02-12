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
5. **Zero-copy buffer management**: Request bodies use `seastar::temporary_buffer` with `string_view` access, avoiding copies in tokenization and routing paths

### Zero-Copy Buffer Management

The streaming response path uses optimized buffer management to minimize allocations:

| Component | Technique | Benefit |
|-----------|-----------|---------|
| `CrossShardRequestContext` | `temporary_buffer<char>` with move semantics | Zero-copy cross-shard transfer |
| `StreamParser` | Read-position offset tracking | Avoids O(n) `substr()` copies |
| `TokenizerService` | `string_view` input | No tokenization input copies |
| `logging.hpp` | `extract_*_view()` helpers | Zero-copy header extraction |

**StreamParser Optimizations:**
- Uses `_read_pos` offset instead of `substr()` to track consumed data
- Compacts buffer when >50% consumed (prevents unbounded growth)
- Size limits: 16KB headers, 1MB chunks (prevents OOM attacks)
- Error state handling for malformed chunked encoding

**CrossShardRequestContext Limits:**
- Max body size: 128MB
- Max tokens: 128K
- Max path length: 8KB
- Validated via `cross_shard::try_create_*` factory functions

### Cache Hit vs. Miss Latency

#### Mock Backend (Integration Tests)

| Scenario | Avg Latency | P99 Latency | Notes |
|----------|-------------|-------------|-------|
| Cache Hit (Prefix Match) | ~18ms | ~25ms | Mock backend with simulated delay |
| Cache Miss (Random) | ~500ms | ~520ms | Mock backend with 500ms delay |

> ⚠️ **Note:** These results are from mock backend tests used for integration testing. They demonstrate routing correctness, not real-world LLM performance.

#### Real vLLM Backend (8x A100, February 2026)

| Model | XLarge TTFT Improvement | P99 Latency | Throughput | Cache Hit Rate |
|-------|-------------------------|-------------|------------|----------------|
| **Llama-3.1-70B** (80GB, TP=2) | **44%** | ~same | ~same | 98% |
| **CodeLlama-13b** (40GB) | **33%** | **-85%** | **+22%** | 98% |
| **Llama-3.1-8B** (40GB) | **40%** | +6.5% | ~same | 98% |

*30-minute validated runs with load-aware routing + stale connection fix. See [Benchmark Guide](../benchmarks/benchmark-guide-8xA100.md) for full details.*

**Key insight**: Benefits scale with model size — larger models save more computation per cache hit. 13B is the sweet spot for aggregate metrics (-85% P99 tail latency, +22% throughput). 70B shows the highest per-request benefit (44% TTFT) but is compute-bound rather than queue-bound under sustained load.

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

### Radix Tree Performance Metrics

These metrics help operators tune the tokenizer and routing table for maximum throughput:

| Metric | Description | Interpretation |
|--------|-------------|----------------|
| `ranvier_radix_tree_lookup_hits_total` | Successful route lookups | Higher = better cache efficiency |
| `ranvier_radix_tree_lookup_misses_total` | Failed route lookups | Monitor miss rate for capacity planning |
| `ranvier_radix_tree_node_count{node_type="Node4\|16\|48\|256"}` | Node counts by type | High Node256 count may indicate suboptimal tokenization |
| `ranvier_radix_tree_slab_utilization_ratio` | Slab memory efficiency (0.0-1.0) | Target >0.7 for good memory utilization |
| `ranvier_radix_tree_average_prefix_skip_length` | Avg tokens skipped via path compression | Higher = better tree structure |

**Tuning Guidance:**

- **Low slab utilization (<0.5)**: Routes may have been evicted. Consider increasing `router.max_routes`.
- **Low average prefix skip (<2.0)**: Tree has poor path compression. Check if tokenizer produces diverse first tokens.
- **High Node256 ratio (>20%)**: May indicate tokenizer produces many unique first bytes. Consider adjusting `router.min_token_length`.

### Grafana Query Examples

```promql
# Cache hit ratio
sum(rate(ranvier_router_cache_hits[5m])) /
(sum(rate(ranvier_router_cache_hits[5m])) + sum(rate(ranvier_router_cache_misses[5m])))

# P99 latency by shard
histogram_quantile(0.99, sum(rate(ranvier_http_request_duration_seconds_bucket[5m])) by (le, shard))

# Radix tree lookup hit ratio
sum(rate(ranvier_radix_tree_lookup_hits_total[5m])) /
(sum(rate(ranvier_radix_tree_lookup_hits_total[5m])) + sum(rate(ranvier_radix_tree_lookup_misses_total[5m])))

# Node type distribution (per shard)
ranvier_radix_tree_node_count

# Slab memory utilization
avg(ranvier_radix_tree_slab_utilization_ratio)

# Path compression effectiveness
avg(ranvier_radix_tree_average_prefix_skip_length)
```

---

## References

- [Architecture Overview](../architecture/system-design.md)
- [Request Flow Diagrams](../request-flow.md)
- [API Reference](../api/reference.md)
- [Locust Test Configuration](../../tests/integration/README.md)
- [KV Cache Prefix-Routing Benchmark](../benchmarks/kv-cache-prefix-routing-benchmark.md)

---

## Large-Prefix KV Cache Benchmark

For production LLM workloads with large context windows, see our [detailed benchmark guide](../benchmarks/benchmark-guide-8xA100.md) comparing routing modes on 8x A100 GPUs with Llama-3.1-8B, CodeLlama-13b, and Llama-3.1-70B.

### Summary Results (8x A100, 30-minute validated, February 2026)

| Model | Cache Hit Rate | XLarge TTFT Improvement | P99 Latency | Throughput |
|-------|----------------|-------------------------|-------------|------------|
| **Llama-3.1-70B** | 25% → **98%** | **44%** faster | ~same | ~same |
| CodeLlama-13b | 12% → **98%** | **33%** faster | **-85%** | **+22%** |
| Llama-3.1-8B | 12% → **98%** | **40%** faster | +6.5% | ~same |

### Key Findings

- **4-8x better cache hit rate** with prefix-affinity routing (12-25% → 98%)
- **33-44% TTFT improvement** for XLarge prefixes (4K-8K tokens) across model sizes
- **Up to 85% lower P99 tail latency** — 13B is the sweet spot due to queue-bound behavior
- **Up to 22% higher throughput** — Load-aware routing prevents backend hotspots for 13B
- **Benefits scale with model size**: 70B shows 44% XLarge TTFT, 13B shows 33%, 8B shows 40%
- **0% incomplete rate** with stale connection retry (was 30-37% before fix)
- **Small prefix overhead**: Routing cost exceeds cache benefit for <500 token prefixes

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
