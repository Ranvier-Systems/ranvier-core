1. Ref docs/claude-context.md for the "No Locks/Async Only" rules.
2. DO NOT read the full /docs or /assets folders.
3. Run /compact if the conversation exceeds 4 turns.

---

## PERFORMANCE INVESTIGATION

**Area:** <COMPONENT_OR_FEATURE>

**Observation:**
- Current performance: <METRIC> (e.g., "500 req/s", "p99 = 50ms")
- Target performance: <METRIC>
- Gap: <DIFFERENCE>

**Suspected hot path:**
- `<FILE:FUNCTION>`
- `<FILE:FUNCTION>`

**Evidence:**
- [ ] Profiler data available
- [ ] Metrics showing bottleneck
- [ ] Anecdotal observation

---

## PERFORMANCE ANALYSIS FRAMEWORK

### Step 1: Characterize the Bottleneck

| Type | Symptoms | Investigation |
|------|----------|---------------|
| **CPU-bound** | High CPU, low I/O wait | Profile hot functions |
| **I/O-bound** | Low CPU, high I/O wait | Check network/disk patterns |
| **Memory-bound** | Cache misses, allocation overhead | Check data structures |
| **Contention-bound** | Variable latency, cross-shard calls | Check shard distribution |

Current bottleneck type: [ ]

### Step 2: Measure Before Optimizing
Establish baseline metrics:
```
Metric: [what we're measuring]
Current Value: [number]
Measurement Method: [how we measured]
```

### Step 3: Identify Optimization Opportunities

#### Seastar-Specific Optimizations

| Technique | When to Use | Trade-off | Gains |
|-----------|-------------|-----------|-------|
| `parallel_for_each` | Sequential `co_await` in loops | Memory ↑ | Latency ↓↓ |
| `max_concurrent_for_each` | Need bounded parallelism | Complexity ↑ | Controlled concurrency |
| Batching | Many small I/O operations | Latency ↑ | Throughput ↑↑ |
| Shard-local caching | Repeated cross-shard reads | Memory ↑ | Latency ↓↓ |
| `lw_shared_ptr` | Frequent intra-shard sharing | Complexity ↑ | Allocation ↓ |
| Object pooling | Frequent alloc/dealloc | Memory ↑ | Allocation ↓↓ |
| Continuation passing | Deep coroutine stacks | Readability ↓ | Stack usage ↓ |

#### Algorithmic Optimizations

| Current | Optimized | When Applicable |
|---------|-----------|-----------------|
| O(n) search | O(1) hash lookup | Known key space |
| O(n²) nested loop | O(n log n) sort + scan | Sortable data |
| String copying | String views | Read-only access |
| Dynamic allocation | Stack allocation | Small, fixed size |

---

## ANTI-OPTIMIZATIONS (AVOID)

These "optimizations" violate Seastar principles:

| Anti-Pattern | Why It Fails | Rule # |
|--------------|--------------|--------|
| Add `std::mutex` for "thread safety" | Blocks reactor | #1 |
| Use `std::atomic` everywhere | Often unnecessary in single-shard | - |
| Over-parallelize (unbounded futures) | Memory exhaustion | #4 |
| Cache with `std::shared_ptr` | Cross-shard ref counting | #0 |
| Busy-wait polling loops | Blocks reactor | #1 |
| Synchronous file I/O | Blocks reactor | #1 |

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
// Complexity: O(n) per request, blocks on each await
for (auto& item : items) {
    co_await process(item);  // Sequential!
}

// After (with explanation)
// Complexity: O(1) wall-clock, parallel execution
co_await seastar::parallel_for_each(items, [](auto& item) {
    return process(item);
});
```

### Expected Improvement
| Metric | Before | After (Expected) | Confidence |
|--------|--------|------------------|------------|
| Throughput | X req/s | Y req/s | [High/Medium/Low] |
| Latency p50 | X ms | Y ms | [High/Medium/Low] |
| Latency p99 | X ms | Y ms | [High/Medium/Low] |

### Trade-offs
| Gain | Cost |
|------|------|
| [what improves] | [what we give up] |

### Verification Plan
1. [How to measure improvement]
2. [What load test to run]
3. [How to detect regressions]

---

## POST-OPTIMIZATION CHECKLIST

- [ ] Baseline metrics documented
- [ ] Optimization doesn't violate 12 Hard Rules
- [ ] Trade-offs are acceptable
- [ ] Improvement is measurable
- [ ] No new failure modes introduced
- [ ] Tests still pass
- [ ] Consider adding performance regression test

