# Prefix-Affinity Routing

Prefix-affinity routing ensures that requests sharing the same token prefix are routed to the same backend, enabling vLLM's KV cache reuse across requests.

## Overview

When multiple requests share a common prefix (e.g., the same system prompt), routing them to the same backend allows that backend's KV cache to serve subsequent requests faster. Without prefix-affinity, requests are distributed randomly, resulting in ~50% cache hit rate with 2 backends.

**Benchmark Results:**

| Metric | Random Routing | Prefix-Affinity |
|--------|----------------|-----------------|
| Cache Hit Rate | ~49% | **81%** |
| Routing Overhead | - | 0.15ms |

## How It Works

Ranvier uses a hybrid approach combining the Adaptive Radix Tree (ART) with consistent hashing:

```
Request arrives with tokens
        │
        ▼
┌─────────────────────────────────────────┐
│  1. ART Lookup (Longest Prefix Match)   │
│     O(k) lookup for known prefixes      │
└─────────────────────────────────────────┘
        │
   Found in ART?
   ├─ YES → Route to learned backend (cache hit)
   │
   └─ NO ──┐
           ▼
┌─────────────────────────────────────────┐
│  2. Hash Fallback (Deterministic)       │
│     FNV-1a hash of first N tokens       │
│     hash % num_backends = backend       │
└─────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────┐
│  3. Learn Prefix (After Success)        │
│     Store first N tokens → backend      │
│     Future requests get ART hit         │
└─────────────────────────────────────────┘
```

### Why Hybrid?

Each component serves a specific purpose:

| Component | Purpose |
|-----------|---------|
| **ART (Radix Tree)** | O(k) longest prefix matching - finds known prefixes efficiently, even partial matches |
| **Consistent Hashing** | Deterministic fallback for new prefixes - no "cold start" problem |
| **Prefix Learning** | Store only first N tokens, so requests with same system prompt share one route |

The ART enables **partial prefix matching**: if request B shares a prefix with previously-seen request A, it routes to the same backend even if B is longer. Simple hashing alone cannot do this.

## Prefix Boundary Detection

### The Multi-Turn Problem

In multi-turn conversations, the tokenized request includes both shared content (system messages) and unique content (user queries). When using a fixed `prefix_token_length`, requests with the same system prompt but different user messages may hash to different backends:

```
Request 1: [system tokens...][user: "What is 2+2?"]  → Backend 1
Request 2: [system tokens...][user: "Hello!"]        → Backend 2  ❌ Cache miss!
```

Both requests share the same system prompt, but the user query difference causes different routing.

### Solution: Prefix Boundary

Ranvier detects the "shared prefix boundary" - the point where common content (system messages) ends and unique content (user queries) begins. Routes are stored at this boundary instead of `prefix_token_length`:

```
Request 1: [system tokens] | [user: "What is 2+2?"]  → Backend 1
Request 2: [system tokens] | [user: "Hello!"]        → Backend 1  ✓ Cache hit!
                          ↑
                   prefix boundary
```

### Detection Methods

Ranvier supports two methods for determining the prefix boundary:

#### 1. Automatic System Message Detection (Default)

Ranvier automatically extracts and tokenizes system messages from chat completion requests:

```json
{
  "messages": [
    {"role": "system", "content": "You are a helpful assistant."},
    {"role": "user", "content": "Hello!"}
  ]
}
```

The system message tokens become the prefix boundary. This works transparently without client changes.

#### 2. Client-Provided Prefix Boundary (Opt-in)

Clients can explicitly specify their prefix length using the `prefix_token_count` field:

```json
{
  "messages": [...],
  "prompt_token_ids": [1, 2, 3, ...],
  "prefix_token_count": 256
}
```

This is useful when:
- Clients pre-tokenize requests (know exact token counts)
- System message detection doesn't capture the full shared prefix
- Complex multi-part system prompts need custom boundaries

Client-provided boundaries take precedence over automatic detection.

**Important: Tokenization Format**

When using client-provided `prompt_token_ids` with `prefix_token_count`, the tokenization format must match Ranvier's internal format. Ranvier tokenizes the raw message content with newline separators:

```
{system_content}\n{user_content}\n...
```

**Do not** use chat template format (e.g., `<|system|>\n{content}`) when computing `prefix_token_count`. Use raw content concatenated with `\n` separators.

Example (Python):
```python
# Correct: raw content with newlines
system_text = system_content + "\n"
tokens = tokenizer.encode(system_text)
prefix_token_count = len(tokens)

# Incorrect: chat template format
# tokens = tokenizer.apply_chat_template(messages)  # Don't use this
```

### Multi-Depth Route Storage (Option C)

For optimal cache reuse in multi-turn conversations, Ranvier can store routes at multiple depths:

```
[system: 256 tokens][user1: 50 tokens][assistant1: 100 tokens][user2: 30 tokens]
       ↑                    ↑                    ↑                    ↑
   depth 1 (256)        depth 2 (306)       depth 3 (406)       depth 4 (436)
```

With multi-depth routing enabled:
- Routes are stored at each message boundary (not just the system message boundary)
- A request continuing an existing conversation can match at any previous depth
- Enables cache reuse for branching conversations and conversation continuations

#### Enabling Multi-Depth Routing

```bash
# Enable multi-depth route storage
RANVIER_ENABLE_MULTI_DEPTH_ROUTING=true
```

#### Client-Provided Boundaries

Clients can specify multiple boundaries using the `prefix_boundaries` array:

```json
{
  "prompt_token_ids": [...],
  "prefix_boundaries": [256, 306, 406]
}
```

This tells Ranvier to store routes at tokens 256, 306, and 406.

### Prefix Boundary Configuration

| Option | Default | Env Variable | Description |
|--------|---------|--------------|-------------|
| `enable_prefix_boundary` | `true` | `RANVIER_ENABLE_PREFIX_BOUNDARY` | Enable automatic system message detection |
| `min_prefix_boundary_tokens` | `4` | `RANVIER_MIN_PREFIX_BOUNDARY_TOKENS` | Minimum tokens for valid boundary |
| `accept_client_prefix_boundary` | `false` | `RANVIER_ACCEPT_CLIENT_PREFIX_BOUNDARY` | Accept client-provided `prefix_token_count` |
| `enable_multi_depth_routing` | `false` | `RANVIER_ENABLE_MULTI_DEPTH_ROUTING` | Store routes at multiple message depths |

### Metrics

| Metric | Description |
|--------|-------------|
| `prefix_boundary_used` | System message boundary detected and used |
| `prefix_boundary_skipped` | Boundary skipped (no system messages, too short, etc.) |
| `prefix_boundary_client` | Client-provided `prefix_token_count` was used |
| `multi_depth_routes_stored` | Total routes stored via multi-depth learning |

## Configuration

### Environment Variables

```bash
# Enable prefix-affinity routing (default: true)
RANVIER_PREFIX_AFFINITY_ENABLED=true

# Number of tokens to use as routing key (default: 128)
RANVIER_PREFIX_TOKEN_LENGTH=128

# Alternative: set routing mode directly
# "prefix" enables prefix-affinity, "round_robin" disables it
RANVIER_ROUTING_MODE=prefix

# Prefix boundary detection (for multi-turn conversations)
RANVIER_ENABLE_PREFIX_BOUNDARY=true
RANVIER_MIN_PREFIX_BOUNDARY_TOKENS=4
RANVIER_ACCEPT_CLIENT_PREFIX_BOUNDARY=false
RANVIER_ENABLE_MULTI_DEPTH_ROUTING=false
```

### YAML Configuration

```yaml
routing:
  prefix_affinity_enabled: true
  prefix_token_length: 128
  block_alignment: 16  # Align to vLLM's PagedAttention block size
  enable_prefix_boundary: true  # Detect system message boundaries
  min_prefix_boundary_tokens: 4
  accept_client_prefix_boundary: false  # Accept client-provided prefix_token_count
  enable_multi_depth_routing: false     # Store routes at multiple message depths
```

### Configuration Options

| Option | Default | Description |
|--------|---------|-------------|
| `prefix_affinity_enabled` | `true` | Enable hybrid ART + hash routing |
| `prefix_token_length` | `128` | Number of tokens to use as routing key |
| `block_alignment` | `16` | Align prefix to vLLM block boundaries |
| `enable_prefix_boundary` | `true` | Detect system message boundaries for multi-turn |
| `min_prefix_boundary_tokens` | `4` | Minimum system message tokens to use as boundary |
| `accept_client_prefix_boundary` | `false` | Accept client-provided `prefix_token_count` |
| `enable_multi_depth_routing` | `false` | Store routes at multiple message depths |

## Implementation Details

### Key Files

- `src/router_service.cpp` - `get_backend_for_prefix()` implements hybrid routing
- `src/router_service.cpp` - `learn_route_global()` stores prefix (truncated to N tokens)
- `src/radix_tree.hpp` - Adaptive Radix Tree with longest prefix match
- `src/http_controller.cpp` - Routing decision and `X-Backend-ID` header
- `src/config.hpp` - Configuration parsing

### BPE Tokenization Boundary Alignment

BPE (Byte Pair Encoding) tokenizers produce different tokens depending on context. For example, "helpful" may tokenize differently than "helpful\n" due to subword boundaries.

To ensure the system message tokens are an exact prefix of the full request tokens, Ranvier appends a trailing newline when tokenizing system messages:

```cpp
// Ensures BPE tokens match the prefix of full text tokenization
auto system_text = extract_system_messages(body) + "\n";
auto system_tokens = tokenizer.encode(system_text);
```

This aligns with how `extract_text()` formats messages internally (content separated by newlines).

### Cluster-Wide Hash Consistency

In multi-node deployments, each Ranvier node learns routes independently. Before routes are learned, the hash fallback must be deterministic across all nodes.

Ranvier uses `prefix_boundary` (not `prefix_token_length`) for hash computation when available:

```cpp
// Use prefix_boundary for hash to ensure cluster-wide consistency
size_t hash_len = (prefix_boundary > 0) ? prefix_boundary : prefix_token_length;
auto hash = hash_prefix(tokens.data(), hash_len, block_alignment);
backend_id = hash % num_backends;
```

This ensures that requests with the same system message (but different user queries) hash to the same backend across all nodes, even before routes are learned.

### Hash Function

Uses FNV-1a hash with block alignment for vLLM compatibility:

```cpp
inline uint64_t hash_prefix(const int32_t* tokens, size_t count, uint32_t block_alignment) {
    // Align to block boundary
    size_t aligned_len = (count / block_alignment) * block_alignment;
    if (aligned_len == 0) aligned_len = count;

    // FNV-1a hash
    uint64_t hash = 14695981039346656037ULL;  // FNV offset basis
    const uint8_t* data = reinterpret_cast<const uint8_t*>(tokens);
    for (size_t i = 0; i < aligned_len * sizeof(int32_t); ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;  // FNV prime
    }
    return hash;
}
```

### X-Backend-ID Header

Ranvier adds an `X-Backend-ID` response header containing the backend ID that served the request. This enables accurate cache hit tracking in benchmarks and monitoring.

```
HTTP/1.1 200 OK
X-Request-ID: req-12345
X-Backend-ID: 2
Content-Type: text/event-stream
```

## Metrics

| Metric | Description |
|--------|-------------|
| `router_prefix_affinity_routes` | Counter of prefix-affinity routing decisions |
| `router_cache_hits` | ART cache hits (known prefix) |
| `router_cache_misses` | Hash fallback (new prefix) |

## Comparison with Legacy Routing

| Aspect | Legacy (ART + Random) | Prefix-Affinity (ART + Hash) |
|--------|----------------------|------------------------------|
| Cache miss fallback | Random backend | Deterministic (hash) |
| Same prefix consistency | Only after learning | Immediate |
| First request | Random | Deterministic |
| Partial prefix match | Yes (ART) | Yes (ART) |

## Load-Aware Routing

When enabled (`load_aware_routing: true`), the router considers backend queue depth before routing to the prefix-preferred backend. This prevents hot spots when a single backend becomes overloaded with requests.

### Problem

Prefix-affinity routing maximizes KV cache reuse by sending requests with the same prefix to the same backend. However, under heavy concurrent load, this can create hot spots:

```
Backend 1: [||||||||||||] 12 in-flight (overloaded)
Backend 2: [||          ]  2 in-flight (underutilized)

Request with prefix P1 → Backend 1 (prefix affinity)
                       → Long queue wait → High TTFT
```

### Solution

Load-aware routing checks the preferred backend's in-flight request count before routing. If the backend is overloaded and a significantly less-loaded alternative exists, the request is diverted to reduce tail latency.

### Algorithm

```
1. Determine preferred backend (ART lookup or hash fallback)
2. Check preferred backend's in-flight request count
3. If count > `queue_depth_threshold`:
   - Find least-loaded backend among candidates
   - If load difference > `queue_diff_threshold`, route to least-loaded
   - Otherwise, route to preferred (marginal difference not worth cache miss)
```

```
┌─────────────────────────────────────────────────────────────────┐
│  Request arrives with prefix P1                                  │
│         │                                                        │
│         ▼                                                        │
│  ┌─────────────────────────────────────────┐                    │
│  │  1. Determine preferred backend         │                    │
│  │     (ART hit → backend 1)               │                    │
│  └─────────────────────────────────────────┘                    │
│         │                                                        │
│         ▼                                                        │
│  ┌─────────────────────────────────────────┐                    │
│  │  2. Check preferred load                │                    │
│  │     Backend 1: 8 in-flight              │                    │
│  └─────────────────────────────────────────┘                    │
│         │                                                        │
│    8 > threshold (4)?                                           │
│    ├─ NO → Route to preferred (backend 1)                       │
│    │                                                             │
│    └─ YES ──┐                                                   │
│             ▼                                                    │
│  ┌─────────────────────────────────────────┐                    │
│  │  3. Find least-loaded backend           │                    │
│  │     Backend 2: 1 in-flight              │                    │
│  └─────────────────────────────────────────┘                    │
│         │                                                        │
│    Difference (8-1=7) > diff_threshold (2)?                     │
│    ├─ NO → Route to preferred (cache affinity)                  │
│    │                                                             │
│    └─ YES → Route to least-loaded (backend 2)                   │
│             Increment cache_miss_due_to_load counter            │
└─────────────────────────────────────────────────────────────────┘
```

### Configuration

| Option | Default | Environment Variable | Description |
|--------|---------|---------------------|-------------|
| `load_aware_routing` | `true` | `RANVIER_LOAD_AWARE_ROUTING` | Enable load-aware backend selection |
| `queue_depth_threshold` | `4` | `RANVIER_QUEUE_DEPTH_THRESHOLD` | Max in-flight requests before considering alternatives |
| `queue_diff_threshold` | `2` | `RANVIER_QUEUE_DIFF_THRESHOLD` | Min load difference to justify cache miss |

#### YAML Configuration

```yaml
routing:
  load_aware_routing: true
  queue_depth_threshold: 4
  queue_diff_threshold: 2
```

### Load Tracking

In-flight requests are tracked per-backend using atomic counters:

- **BackendRequestGuard**: RAII guard that increments counter on construction and decrements on destruction
- **Lock-free**: Uses `std::atomic` with relaxed memory ordering
- **Shard-local**: Each Seastar shard maintains its own counters (no cross-shard synchronization)

```cpp
// Usage in request handling
auto guard = BackendRequestGuard(backend_id);
co_return co_await do_with(std::move(guard), [...](auto& g) {
    // Request processing
    // Counter automatically decremented on any exit path
});
```

### Metrics

| Metric | Description |
|--------|-------------|
| `router_load_aware_fallbacks` | Requests diverted due to backend overload |
| `router_cache_miss_due_to_load` | Same as above (alternative name) |
| `backend_active_requests{backend_id}` | Current in-flight requests per backend |

### Trade-offs

| Aspect | Pro | Con |
|--------|-----|-----|
| **Tail Latency** | Reduces P95/P99 TTFT by >35% under heavy load | — |
| **Cache Efficiency** | — | May cause temporary cache misses when load spikes |
| **Complexity** | — | Additional per-request overhead (atomic reads) |
| **Observability** | Metrics show load distribution | Debugging routing decisions requires trace logs |

### Recommendations

1. **Keep enabled by default**: The latency benefits outweigh the occasional cache miss
2. **Tune thresholds based on backend capacity**:
   - High-throughput backends: Increase `queue_depth_threshold` (e.g., 8-16)
   - Low-latency requirements: Decrease thresholds for faster response
3. **Monitor metrics**: Watch `cache_miss_due_to_load` to understand diversion frequency
4. **Combine with shard load balancing**: Load-aware routing handles backend imbalance; shard load balancing handles CPU core imbalance

### Benchmark Results

Under heavy load (30 concurrent users, 300 total requests):

| Metric | Without Load-Aware | With Load-Aware | Improvement |
|--------|-------------------|-----------------|-------------|
| TTFT P50 | 45ms | 42ms | 7% |
| TTFT P95 | 180ms | 115ms | **36%** |
| TTFT P99 | 320ms | 175ms | **45%** |
| Cache Hit Rate | 82% | 78% | -5% |

The small reduction in cache hit rate is offset by significantly improved tail latency.

## References

- [Ray Serve PrefixCacheAffinityRouter](https://docs.ray.io/en/latest/serve/advanced-guides/llm-serving.html) - Similar approach in Ray Serve
- [vLLM Automatic Prefix Caching](https://docs.vllm.ai/en/latest/automatic_prefix_caching/apc.html) - Backend KV cache reuse
- [Consistent Hashing](https://en.wikipedia.org/wiki/Consistent_hashing) - Karger et al., 1997
- [Adaptive Radix Trees](https://db.in.tum.de/~leis/papers/ART.pdf) - Leis et al., 2013
