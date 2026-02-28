## PERFORMANCE INVESTIGATION

**Area:**
<COMPONENT_OR_FEATURE>

**Observation:**
- Current performance: <METRIC> (e.g., "500 req/s", "p99 = 50ms")
- Target performance: <METRIC>
- Gap: <DIFFERENCE>

**Suspected hot path:**
- `<FILE:FUNCTION>`

**Evidence:**
- [ ] Profiler data available
- [ ] Metrics showing bottleneck
- [ ] Anecdotal observation

> Ref: `.dev-context/claude-context.md` for build constraints, architecture, coding conventions, and the Hard Rules.

---

## PERFORMANCE ANALYSIS FRAMEWORK

### Step 1: Characterize the Bottleneck

| Type | Symptoms | Investigation |
|------|----------|---------------|
| **CPU-bound** | High CPU, low I/O wait | Profile hot functions |
| **I/O-bound** | Low CPU, high I/O wait | Check network/disk patterns |
| **Memory-bound** | Cache misses, allocation overhead | Check data structures |
| **Contention-bound** | Variable latency, cross-shard calls | Check shard distribution |

### Step 2: Measure Before Optimizing
```
Metric: [what we're measuring]
Current Value: [number]
Measurement Method: [how we measured]
```

### Step 3: Identify Optimization Opportunities

#### Seastar-Specific Optimizations

| Technique | When to Use | Trade-off | Gains |
|-----------|-------------|-----------|-------|
| `parallel_for_each` | Sequential `co_await` in loops | Memory up | Latency down |
| `max_concurrent_for_each` | Need bounded parallelism | Complexity up | Controlled concurrency |
| Batching | Many small I/O operations | Latency up | Throughput up |
| Shard-local caching | Repeated cross-shard reads | Memory up | Latency down |
| Object pooling | Frequent alloc/dealloc | Memory up | Allocation down |

#### Algorithmic Optimizations

| Current | Optimized | When Applicable |
|---------|-----------|-----------------|
| O(n) search | O(1) hash lookup | Known key space |
| O(n^2) nested loop | O(n log n) sort + scan | Sortable data |
| String copying | String views | Read-only access |
| Dynamic allocation | Stack allocation | Small, fixed size |

---

## ANTI-OPTIMIZATIONS (AVOID)

| Anti-Pattern | Why It Fails | Rule # |
|--------------|--------------|--------|
| Add `std::mutex` for "thread safety" | Blocks reactor | #1 |
| Over-parallelize (unbounded futures) | Memory exhaustion | #4 |
| Cache with `std::shared_ptr` | Cross-shard ref counting | #0 |
| Synchronous file I/O | Blocks reactor | #12 |

---

## OUTPUT FORMAT

### Bottleneck Analysis
```
Type:              [CPU/I/O/Memory/Contention]
Location:          [file:function:line]
Current Complexity: O([complexity])
Root Cause:        [why it's slow]
```

### Proposed Optimization
```cpp
// Before (showing why it's slow)
[code]

// After (with explanation)
[code]
```

### Expected Improvement
| Metric | Before | After (Expected) | Confidence |
|--------|--------|------------------|------------|
| Throughput | X req/s | Y req/s | [High/Medium/Low] |
| Latency p99 | X ms | Y ms | [High/Medium/Low] |

### Post-Optimization Checklist
- [ ] Baseline metrics documented
- [ ] Optimization doesn't violate Hard Rules
- [ ] Trade-offs are acceptable
- [ ] Tests still pass
