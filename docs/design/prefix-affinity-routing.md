# Prefix-Affinity Routing Implementation Plan

## Problem Statement

The current Ranvier routing implementation stores routes for **full request token sequences**, not shared prefixes. This results in ~50% cache hit rate with 2 backends (equivalent to random routing), because:

1. Request 1: `[System prompt + User question A]` → learns route for full sequence → Backend 1
2. Request 2: `[System prompt + User question B]` → different full sequence → cache miss → random backend

**Goal:** Route all requests with the same prefix (e.g., system prompt) to the same backend, enabling vLLM's KV cache to be reused across requests.

## Current Architecture

```
Request → Tokenize → RadixTree.lookup(full_tokens)
                            ↓
                      Cache Hit? → Use cached backend
                            ↓ No
                      Random backend → learn_route(full_tokens, backend)
```

**Files involved:**
- `src/router_service.hpp/cpp` - RouterService with lookup/learn_route methods
- `src/radix_tree.hpp` - Adaptive Radix Tree implementation
- `src/http_controller.cpp` - Request handling and routing decisions (lines 332-350)
- `src/config.hpp` - RoutingConfig with min_token_length, block_alignment

## Proposed Solution: Consistent Hashing on Prefix

### Design Overview

```
Request → Tokenize → Extract prefix tokens (first N tokens)
                            ↓
                      Hash(prefix) mod num_backends → Deterministic backend
                            ↓
                      (Optional) Store in RadixTree for faster subsequent lookups
```

### Key Changes

#### 1. Add Prefix Extraction Configuration

```cpp
// In config.hpp
struct RoutingConfig {
    // Existing
    size_t min_token_length = 10;
    uint32_t block_alignment = 16;

    // New: Prefix-affinity routing
    bool prefix_affinity_enabled = true;
    size_t prefix_token_length = 128;  // Use first N tokens as routing key
    // Alternative: prefix_char_length for character-based extraction
};
```

#### 2. Implement Prefix-Based Consistent Hashing

```cpp
// In router_service.hpp
class RouterService {
public:
    // New method: Get backend using consistent hashing on prefix
    std::optional<BackendId> get_backend_for_prefix(
        const std::vector<int32_t>& tokens,
        const std::string& request_id = "");

private:
    // Consistent hash ring for backend selection
    // Maps hash values to backend IDs
    std::vector<std::pair<uint64_t, BackendId>> _hash_ring;

    // Hash function for prefix tokens
    uint64_t hash_prefix(std::span<const int32_t> prefix_tokens);

    // Rebuild hash ring when backends change
    void rebuild_hash_ring();
};
```

#### 3. Modify HTTP Controller Routing Logic

```cpp
// In http_controller.cpp, handle_proxy() around line 332

// Extract prefix for routing (first N tokens)
size_t prefix_len = std::min(tokens.size(), _config.prefix_token_length);
auto prefix_tokens = std::span(tokens.data(), prefix_len);

BackendId target_id;

if (_config.prefix_affinity_enabled) {
    // Use consistent hashing on prefix
    auto affinity_backend = _router.get_backend_for_prefix(tokens, request_id);
    if (affinity_backend.has_value()) {
        target_id = affinity_backend.value();
        log_proxy.debug("[{}] Prefix affinity -> backend {}", request_id, target_id);
    } else {
        // Fallback to random if no backends available
        target_id = _router.get_random_backend().value_or(0);
    }
} else {
    // Legacy behavior: radix tree lookup + random fallback
    auto route_hit = _router.lookup(tokens, request_id);
    if (route_hit.has_value()) {
        target_id = route_hit.value();
    } else {
        target_id = _router.get_random_backend().value_or(0);
    }
}
```

#### 4. Consistent Hashing Implementation

```cpp
// In router_service.cpp

uint64_t RouterService::hash_prefix(std::span<const int32_t> prefix_tokens) {
    // Use xxHash or similar fast hash
    // Align to block_alignment boundary for vLLM compatibility
    size_t aligned_len = (prefix_tokens.size() / _config.block_alignment)
                         * _config.block_alignment;
    if (aligned_len == 0) aligned_len = prefix_tokens.size();

    return xxh64(prefix_tokens.data(), aligned_len * sizeof(int32_t), 0);
}

std::optional<BackendId> RouterService::get_backend_for_prefix(
    const std::vector<int32_t>& tokens,
    const std::string& request_id) {

    if (local_backend_ids.empty()) {
        return std::nullopt;
    }

    // Extract prefix
    size_t prefix_len = std::min(tokens.size(), _config.prefix_token_length);
    auto prefix = std::span(tokens.data(), prefix_len);

    // Hash prefix
    uint64_t prefix_hash = hash_prefix(prefix);

    // Consistent hashing: find backend in hash ring
    // Simple version: modulo (for production, use proper consistent hashing)
    std::vector<BackendId> live_backends;
    for (BackendId id : local_backend_ids) {
        if (!local_dead_backends.contains(id)) {
            auto it = local_backends.find(id);
            if (it != local_backends.end() && !it->second.is_draining) {
                live_backends.push_back(id);
            }
        }
    }

    if (live_backends.empty()) {
        return std::nullopt;
    }

    // Sort for deterministic ordering
    std::sort(live_backends.begin(), live_backends.end());

    size_t index = prefix_hash % live_backends.size();
    BackendId selected = live_backends[index];

    if (!request_id.empty()) {
        log_router.debug("[{}] Prefix hash {} -> backend {} (index {} of {})",
                         request_id, prefix_hash, selected, index, live_backends.size());
    }

    // Update metrics
    stats_cache_hits++;  // Prefix affinity is effectively always a "hit"
    if (g_metrics) {
        metrics().record_cache_hit();
    }

    return selected;
}
```

### Optional Enhancements

#### A. Hybrid Approach: Consistent Hashing + RadixTree Cache

Use consistent hashing for routing decisions, but cache results in RadixTree for faster subsequent lookups:

```cpp
std::optional<BackendId> RouterService::get_backend_for_prefix(...) {
    // First, check RadixTree for cached prefix mapping
    auto cached = lookup_prefix_only(prefix);
    if (cached.has_value()) {
        return cached;  // Fast path
    }

    // Compute via consistent hashing
    BackendId selected = compute_consistent_hash(prefix);

    // Cache for future lookups
    learn_prefix_route(prefix, selected);

    return selected;
}
```

#### B. Client-Provided Prefix Boundary

Allow clients to explicitly mark prefix boundaries:

```json
{
    "model": "llama",
    "messages": [...],
    "prefix_token_count": 256,  // First 256 tokens are the shared prefix
    // OR
    "prefix_hash": "abc123"     // Pre-computed prefix hash
}
```

#### C. Weighted Consistent Hashing

Respect backend weights in consistent hashing:

```cpp
void RouterService::rebuild_hash_ring() {
    _hash_ring.clear();

    for (const auto& [id, info] : local_backends) {
        if (info.is_draining) continue;

        // Add virtual nodes proportional to weight
        size_t vnodes = info.weight;  // More weight = more virtual nodes
        for (size_t i = 0; i < vnodes; i++) {
            uint64_t hash = xxh64(&id, sizeof(id), i);
            _hash_ring.emplace_back(hash, id);
        }
    }

    std::sort(_hash_ring.begin(), _hash_ring.end());
}
```

### Configuration

Add environment variables:

```bash
# Enable prefix-affinity routing (default: true)
RANVIER_PREFIX_AFFINITY_ENABLED=true

# Number of tokens to use for prefix routing (default: 128)
RANVIER_PREFIX_TOKEN_LENGTH=128

# Routing mode: "prefix" (new), "learned" (current), "round_robin" (baseline)
RANVIER_ROUTING_MODE=prefix
```

### Metrics

Add new metrics for prefix-affinity routing:

```cpp
// In metrics_service.hpp
void record_prefix_affinity_routing();
void record_prefix_hash(uint64_t hash, BackendId backend);

// Prometheus metrics
// ranvier_prefix_affinity_routes_total - Counter of prefix-based routing decisions
// ranvier_prefix_distribution{backend="1"} - Distribution of prefixes across backends
```

### Testing Strategy

#### Unit Tests

```cpp
// tests/unit/prefix_affinity_test.cpp

TEST(PrefixAffinity, SamePrefixSameBackend) {
    RouterService router(config);
    router.register_backend(1, addr1);
    router.register_backend(2, addr2);

    std::vector<int32_t> tokens1 = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    std::vector<int32_t> tokens2 = {1, 2, 3, 4, 5, 6, 7, 8, 99, 99, 99, 99};

    // Same prefix (first 8 tokens) should route to same backend
    auto backend1 = router.get_backend_for_prefix(tokens1);
    auto backend2 = router.get_backend_for_prefix(tokens2);

    EXPECT_EQ(backend1, backend2);
}

TEST(PrefixAffinity, DifferentPrefixDistribution) {
    // Test that different prefixes distribute across backends
    std::map<BackendId, int> distribution;

    for (int i = 0; i < 1000; i++) {
        std::vector<int32_t> tokens = generate_random_prefix();
        auto backend = router.get_backend_for_prefix(tokens);
        distribution[backend.value()]++;
    }

    // Should be roughly evenly distributed
    EXPECT_GT(distribution[1], 400);
    EXPECT_GT(distribution[2], 400);
}
```

#### Benchmark Validation

Update `locustfile_real.py` to track prefix affinity:

```python
def _generate_medium_prompt() -> Tuple[List[dict], str]:
    # Use consistent prefix hashing that matches Ranvier's logic
    category, user_prompt = random.choice(MEDIUM_PROMPTS)
    system_prompt = SYSTEM_PROMPTS[category]

    # Hash the system prompt (prefix) - should match Ranvier's routing
    prefix_hash = hash_tokens(tokenize(system_prompt)[:128])

    messages = [
        {"role": "system", "content": system_prompt},
        {"role": "user", "content": user_prompt},
    ]

    return messages, prefix_hash
```

Expected results with prefix-affinity:
- Cache hit rate: >90% (vs current ~50%)
- TTFT improvement: 30-60% for cache hits vs misses

### Migration Path

1. **Phase 1:** Add `prefix_affinity_enabled` config (default: false)
2. **Phase 2:** Implement consistent hashing, enable for testing
3. **Phase 3:** Benchmark and tune `prefix_token_length`
4. **Phase 4:** Enable by default, deprecate learned routing

### Dependencies

- xxHash library for fast hashing (or use existing hash in codebase)
- No new external dependencies required

### Estimated Effort

- Core implementation: 4-6 hours
- Unit tests: 2-3 hours
- Integration testing: 2-3 hours
- Benchmark validation: 2-3 hours

### References

- [Ray Serve PrefixCacheAffinityRouter](https://docs.ray.io/en/latest/serve/advanced-guides/llm-serving.html)
- [vLLM Prefix Caching](https://docs.vllm.ai/en/latest/automatic_prefix_caching/apc.html)
- [Consistent Hashing](https://en.wikipedia.org/wiki/Consistent_hashing)

---

## Quick Start Prompt for Implementation Session

```
Implement prefix-affinity routing for Ranvier. The goal is to route all requests
with the same prefix (first N tokens) to the same backend, enabling vLLM's KV
cache reuse.

Key changes:
1. Add get_backend_for_prefix() to RouterService using consistent hashing
2. Modify http_controller.cpp to use prefix-based routing
3. Add RANVIER_PREFIX_AFFINITY_ENABLED and RANVIER_PREFIX_TOKEN_LENGTH config
4. Add unit tests for prefix affinity behavior

See docs/design/prefix-affinity-routing.md for full design details.

Current routing (http_controller.cpp:332-350) uses RadixTree lookup on full
token sequence, falling back to random. New routing should hash the first
prefix_token_length tokens for deterministic backend selection.
```
