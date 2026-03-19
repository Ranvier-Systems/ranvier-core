# Prefix-Affinity Routing

Prefix-affinity routing ensures that requests sharing the same token prefix are routed to the same backend, enabling vLLM's KV cache reuse across requests.

## Prerequisites

Prefix-affinity routing requires that backends have **prefix caching enabled**. Ranvier determines *which* backend should receive a request based on token prefix matching, but the backend itself must cache KV state for previously-seen prefixes. Without backend-side caching, routing to the "right" backend provides no benefit — there is no cached state to reuse.

| Backend | How to Enable |
|---------|---------------|
| **vLLM** | `--enable-prefix-caching` (Automatic Prefix Caching / APC) |
| **SGLang** | RadixAttention is enabled by default |
| **TensorRT-LLM** | KV cache reuse is enabled by default |

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

When using client-provided `prompt_token_ids` with `prefix_token_count`, the tokenization must match Ranvier's internal format. The internal format depends on the configured chat template:

- **No chat template (default)**: Messages are concatenated with `\n` separators: `{system_content}\n{user_content}\n...`
- **With chat template** (llama3, chatml, mistral): Messages are formatted using the template, matching vLLM's `apply_chat_template()` output

Ensure your `prefix_token_count` is computed against the same format Ranvier uses. When in doubt, use the automatic system message detection (Option 1) instead of client-provided boundaries.

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
# "prefix" enables prefix-affinity with ART, "hash" uses consistent hash only,
# "random" uses weighted random distribution (no affinity)
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

### System Message Boundary Detection

Ranvier determines the prefix boundary through a single-pass JSON extraction (`extract_text_with_boundary_info()` in `request_rewriter.hpp`) that builds one combined text string from all messages while tracking the character offset where system messages end. This combined text is formatted using the configured chat template (llama3, chatml, mistral, or plain newline-joining), so tokenization aligns with vLLM's internal representation.

The system prefix substring is then tokenized to produce the token-level boundary. Because the prefix is a substring of the same combined text that produces the full token sequence, the system tokens are guaranteed to be an exact prefix of the full tokenization — no special delimiter or trailing newline needed.

When system messages are interleaved with non-system messages (non-contiguous), Ranvier falls back to using the pre-extracted raw system text as an approximation.

### Cluster-Wide Hash Consistency

In multi-node deployments, each Ranvier node learns routes independently. Before routes are learned, the hash fallback must be deterministic across all nodes.

When a prefix boundary is available, Ranvier uses it (instead of `prefix_token_length`) as the hash input length. This ensures that requests sharing the same system message hash to the same backend across all nodes, even before routes are learned — regardless of differing user queries.

### Hash Function

The hash fallback uses FNV-1a over the raw token bytes, with the token count first rounded down to the nearest `block_alignment` boundary (defaulting to 16, matching vLLM's PagedAttention block size). This alignment ensures that two prefixes differing only in trailing partial-block tokens hash identically. See `hash_prefix()` in `router_service.cpp` for the implementation.

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

Uses a **relative threshold** based on the median load across all backends. This auto-adapts to any workload, model size, or cluster size — no per-model tuning needed.

```
1. Determine preferred backend (ART lookup or hash fallback)
2. If preferred load is 0, route to preferred (fast path)
3. Compute median load across all live backends (via nth_element, O(n))
4. Compute threshold = median * load_imbalance_factor + load_imbalance_floor
5. If preferred load > threshold:
   - Find least-loaded backend among candidates
   - Route to least-loaded (accepting cache miss)
6. Otherwise, route to preferred
```

See `apply_load_aware_selection()` in `router_service.cpp` for the implementation.

### Configuration

| Option | Default | Environment Variable | Description |
|--------|---------|---------------------|-------------|
| `load_aware_routing` | `true` | `RANVIER_LOAD_AWARE_ROUTING` | Enable load-aware backend selection |
| `load_imbalance_factor` | `2.0` | `RANVIER_LOAD_IMBALANCE_FACTOR` | Divert when preferred load > factor * median load |
| `load_imbalance_floor` | `2` | `RANVIER_LOAD_IMBALANCE_FLOOR` | Additive floor to prevent flapping at low load |

The threshold is computed as: `median_load * load_imbalance_factor + load_imbalance_floor`. This relative approach auto-adapts to any workload without per-model tuning.

#### YAML Configuration

```yaml
routing:
  load_aware_routing: true
  load_imbalance_factor: 2.0
  load_imbalance_floor: 2
```

### Load Tracking

In-flight requests are tracked per-backend using shard-local counters:

- **BackendRequestGuard**: Move-only RAII guard (`router_service.hpp`) that increments the backend's `active_requests` on construction and decrements on destruction, ensuring correct counting on any exit path (including exceptions and early co_await returns)
- **Lock-free**: Plain `uint64_t` in thread-local `ShardLocalState` — no atomics needed since each Seastar shard runs on a single reactor thread
- **Shard-local**: Each shard maintains its own counters; cross-shard load visibility requires the optional load sync feature (see `shard-load-balancing.md`)

### Metrics

| Metric | Description |
|--------|-------------|
| `router_load_aware_fallbacks_total` | Requests diverted due to backend overload |
| `backend_active_requests` | Current in-flight requests per backend (gauge, labeled by backend) |

### Trade-offs

| Aspect | Pro | Con |
|--------|-----|-----|
| **Tail Latency** | Reduces P95/P99 TTFT by >35% under heavy load | — |
| **Cache Efficiency** | — | May cause temporary cache misses when load spikes |
| **Complexity** | — | Additional per-request overhead (median computation over backends) |
| **Observability** | Metrics show load distribution | Debugging routing decisions requires trace logs |

### Recommendations

1. **Keep enabled by default**: The latency benefits outweigh the occasional cache miss
2. **Tune thresholds based on backend capacity**:
   - High-throughput backends: Increase `load_imbalance_factor` (e.g., 3.0-4.0) to tolerate more skew before diverting
   - Low-latency requirements: Decrease `load_imbalance_factor` for faster diversion
   - Increase `load_imbalance_floor` to prevent flapping when overall load is low
3. **Monitor metrics**: Watch `router_load_aware_fallbacks_total` to understand diversion frequency
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
