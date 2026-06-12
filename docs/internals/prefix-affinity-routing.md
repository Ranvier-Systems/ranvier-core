# Prefix-Affinity Routing

Prefix-affinity routing ensures that requests sharing the same token prefix are routed to the same backend, enabling vLLM's KV cache reuse across requests.

## Prerequisites

Prefix-affinity routing requires that backends have **prefix caching enabled**. Ranvier determines *which* backend should receive a request based on token prefix matching, but the backend itself must cache KV state for previously-seen prefixes. Without backend-side caching, routing to the "right" backend provides no benefit — there is no cached state to reuse.

| Backend | How to Enable |
|---------|---------------|
| **vLLM** | `--enable-prefix-caching` (Automatic Prefix Caching / APC) |
| **SGLang** | RadixAttention is enabled by default |
| **TensorRT-LLM** | KV cache reuse is enabled by default |

## Backend Type Applicability

Ranvier's `BackendType` enum (`src/types.hpp`) tags each registered backend. The type drives three independent gates: whether routes are learned into the ART, whether the vLLM-shaped `/metrics` endpoint is scraped, and whether `prompt_token_ids` is forwarded in the request body. The headline benefits in this document apply to types that pass the first gate; the rest stay live in the fleet for traffic-management value (rate limiting, circuit breaking, fair scheduling, auth header injection) but do **not** earn the cache-hit numbers below.

| `BackendType` | ART learning | vLLM `/metrics` scrape | `prompt_token_ids` forwarded | Notes |
|---|---|---|---|---|
| `vllm` | ✓ | ✓ | ✓ | The canonical path. Everything in this document applies. |
| `sglang` | ✓ | ✗ | ✓ | RadixAttention is the backend-side cache. Metrics scrape skipped — SGLang's `/metrics` schema differs from vLLM's. |
| `trt_llm` | ✓ | ✗ | ✓ | KV cache reuse is the backend-side cache. |
| `ollama` | ✓ | ✗ | ✗ | Has a KV cache; prefix affinity helps. No Prometheus endpoint. Does not accept `prompt_token_ids` — strip it at registration time. |
| `lm_studio` | ✓ | ✗ | ✗ | Same shape as Ollama. |
| `cerebras` | ✗ | ✗ | ✗ | Cerebras has a KV cache like every autoregressive transformer; on the WSE it lives in on-wafer SRAM (~44 GB, ~21 PB/s) rather than GPU HBM. The reason ART learning is off isn't the absence of a cache — it's that requests go to a single managed-API URL (`api.cerebras.ai`) whose internal scheduler chooses which physical instance handles each request. Ranvier can't pin a prefix to a specific wafer, so an ART entry pointing at this backend means "send prefix P to the same opaque load balancer we already send everything to" — i.e. it earns nothing. Cache reuse, where possible, is handled inside Cerebras's own scheduler. Auto-downgraded to `supports_token_ids=false` at registration. |
| `openai_compatible` | ✓ | ✗ | ✗ | Catch-all for self-hosted shims (e.g. SGLang behind an OpenAI-compatible API) where Ranvier IS the entity choosing which instance to hit. ART learning stays on for that reason; opt out per-deployment if your shim is an opaque managed API rather than a self-hosted instance. Auto-downgraded to `supports_token_ids=false`. |

The truth table above is enforced in three places:

- `RouterService::should_cache_routes_for()` (`src/router_service.cpp`) gates the ART-learn sites.
- `HealthService::scrape_one_backend()` (`src/health_service.cpp`) gates the `/metrics` scrape.
- `RouterService::register_backend_global()` (`src/router_service.cpp`) auto-downgrades `supports_token_ids` for `cerebras` and `openai_compatible`.

For non-vLLM backends, `get_backend_load_score()` always returns `0.0` (no scrape signal). Under `load_aware_routing: true` this makes them look idle and attract more traffic — usually acceptable for managed-API backends where the provider absorbs the implied skew, worth knowing for self-hosted backends that don't expose vLLM-shaped metrics. Ranvier emits a one-line `info` log at registration time for each non-VLLM backend under load-aware routing so this is visible to operators. See [Hybrid Fleets](../guides/hybrid-fleets.md) for the operator-facing walkthrough.

## Overview

When multiple requests share a common prefix (e.g., the same system prompt), routing them to the same backend allows that backend's KV cache to serve subsequent requests faster. Without prefix-affinity, requests are distributed randomly, resulting in ~50% cache hit rate with 2 backends.

**Benchmark Results:**

| Metric | Random Routing | Prefix-Affinity |
|--------|----------------|-----------------|
| Cache Hit Rate | ~49% | **81%** |
| Routing Overhead | - | 0.15ms |

These numbers measure what Ranvier's ART layer accomplishes when it can actually steer requests — i.e. when backends are individually addressable. Managed-API backends in a hybrid fleet (the `cerebras` row above) don't contribute to either the cache-hit or the TTFT-improvement headline: not because the backend has no cache, but because Ranvier can't influence which physical instance handles each request, so cache locality is the provider's concern, not ours.

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
   ├─ YES → preferred = learned backend (cache hit)
   │
   └─ NO ──┐
           ▼
┌─────────────────────────────────────────┐
│  2. Hash Fallback (Deterministic)       │
│     FNV-1a over block-aligned tokens →  │
│     configured hash strategy (default:  │
│     bounded-load + jump consistent hash)│
└─────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────┐
│  3. Unified Weighted Scoring            │
│     One ranked pass over the live       │
│     candidates (route_scorer.hpp):      │
│                                         │
│     PLACEMENT score (stable terms):     │
│       prefix affinity + hardware price  │
│       → original_selected (learned)     │
│                                         │
│     DISPATCH score (adds transients):   │
│       − load hinge over the strategy    │
│         allowance                       │
│       − cost-budget hinge               │
│       → final backend (this request)    │
│                                         │
│     Neutral default weights reproduce   │
│     the strategy/threshold rules        │
│     exactly (routing.scoring.* tunes).  │
└─────────────────────────────────────────┘
        │
        ▼
┌─────────────────────────────────────────┐
│  4. Learn Prefix (After Success)        │
│     Store the PLACEMENT winner (not the │
│     load/cost-budget dispatch target)   │
│     so cache affinity survives          │
│     transient load spikes. A hardware-  │
│     price divert moves placement, so it │
│     IS learned. Future requests get an  │
│     ART hit on the learned backend.     │
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

## Hardware-Cost-Aware Cache-Miss Placement

Clusters often mix GPU types with very different hourly cost (e.g. H100 next to A10G capacity). Operators can price each backend via two optional static-config fields (`backends:` array in YAML):

- `gpu_tier` — coarse informational hardware label (`"h100"`, `"a10g"`, …), surfaced in routing logs.
- `cost_per_hour` — the operator's hourly price for the backend. `0` (the default) means *unpriced*.

When a request **misses** the ART (step 2 above), the prefix must be recomputed wherever it lands — the hash choice has no cache advantage. If both the hash-selected backend and a cheaper live backend are priced, the selection is diverted to the cheapest priced candidate (the hardware-price term of the unified route score; `price_weight` in `routing.scoring`, default `1.0`, `0.0` disables). When a request **hits** the ART, price is ignored entirely: recomputing a warm prefix on a cold cheaper box costs more GPU-seconds than the hardware saving, so cache-warm traffic always stays on the warm backend.

Three properties bound the behavior:

- **Unpriced = untouched.** Backends with `cost_per_hour` unset are never preferred and never diverted from. A fleet with no priced backends routes exactly as before; a partially priced fleet only re-routes among priced backends. Equal prices never divert (the deterministic hash spread is preserved within a tier).
- **Overload guard.** A cheaper backend is skipped when its composite load exceeds `median · load_imbalance_factor + load_imbalance_floor` — the same "overloaded" definition the legacy median load threshold uses — so the preference cannot pile unbounded load onto the cheapest box. Cost ties break toward lower load.
- **The divert feeds route learning.** Unlike the transient load/cost-budget terms (which move only the dispatch target, never the learn target), the hardware-price term is part of the PLACEMENT score: `original_selected` follows it. The learned route therefore pins the prefix to the backend that actually computed it, and follow-up requests ART-hit warm on the cheap hardware. Since `cost_per_hour` is static operator config, this is a stable placement policy rather than a flapping redirect.

Observability: `router_hardware_cost_diverts_total` counts misses diverted to cheaper hardware; the per-divert debug log includes both backends' `gpu_tier` and price.

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

Multi-depth route storage is implemented by `RouterService::learn_route_global_multi()` in `router_service.cpp`, which inserts one ART entry per boundary. Per-message char offsets are pre-computed during JSON parsing (`message_char_ends` in `TextWithBoundaryInfo`) and translated to token boundaries by `boundary_detector.hpp` — either by re-tokenizing each formatted message or by proportional char-to-token estimation, depending on tokenizer availability.

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

This tells Ranvier to store routes at tokens 256, 306, and 406. The first boundary is also used as the hash-fallback prefix length so that hash routing matches the system-message depth.

Client-provided `prefix_boundaries` is gated on **both** `accept_client_prefix_boundary` AND `enable_multi_depth_routing` — if either is disabled, the array is ignored and Ranvier falls back to single-boundary detection (`prefix_token_count` or automatic system-message detection).

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
| `prefix_boundary_client` | Client-provided `prefix_token_count` or `prefix_boundaries` was used |
| `router_multi_depth_routes_stored` | Total routes stored via multi-depth learning |

## Configuration

### Environment Variables

```bash
# Legacy toggle: true=PREFIX mode, false=RANDOM mode
# Superseded by RANVIER_ROUTING_MODE — kept for backward compatibility
RANVIER_PREFIX_AFFINITY_ENABLED=true

# Number of tokens to use as routing key (default: 128)
RANVIER_PREFIX_TOKEN_LENGTH=128

# Routing mode: "prefix" (ART + hash), "hash" (hash only, no learning),
# or "random" (weighted random, no affinity). "round_robin" is accepted
# as a legacy alias for "random".
RANVIER_ROUTING_MODE=prefix

# Hash strategy used by both PREFIX-mode fallback and HASH mode.
# Values: "bounded_load" (default), "p2c", "jump", "modular"
RANVIER_HASH_STRATEGY=bounded_load
RANVIER_BOUNDED_LOAD_EPSILON=0.25   # capacity headroom for BOUNDED_LOAD
RANVIER_P2C_LOAD_BIAS=2             # primary affinity bias for P2C

# vLLM PagedAttention block alignment for the FNV-1a prefix hash (default: 16)
RANVIER_BLOCK_ALIGNMENT=16

# Prefix boundary detection (for multi-turn conversations)
RANVIER_ENABLE_PREFIX_BOUNDARY=true
RANVIER_MIN_PREFIX_BOUNDARY_TOKENS=4
RANVIER_ACCEPT_CLIENT_PREFIX_BOUNDARY=false
RANVIER_ENABLE_MULTI_DEPTH_ROUTING=false
```

### YAML Configuration

```yaml
routing:
  routing_mode: prefix          # "prefix", "hash", or "random"
  prefix_token_length: 128
  block_alignment: 16           # Align to vLLM's PagedAttention block size
  hash_strategy: bounded_load   # bounded_load | p2c | jump | modular
  bounded_load_epsilon: 0.25
  p2c_load_bias: 2
  enable_prefix_boundary: true  # Detect system message boundaries
  min_prefix_boundary_tokens: 4
  accept_client_prefix_boundary: false  # Accept prefix_token_count / prefix_boundaries
  enable_multi_depth_routing: false     # Store routes at multiple message depths
```

`prefix_affinity_enabled: true|false` is also accepted as a legacy alias that maps to `routing_mode: prefix` or `routing_mode: random` respectively.

### Configuration Options

| Option | Default | Description |
|--------|---------|-------------|
| `routing_mode` | `prefix` | `prefix` (ART + hash), `hash` (hash-only), or `random` |
| `prefix_token_length` | `128` | Number of tokens used as routing key when no boundary is available |
| `block_alignment` | `16` | Align prefix to vLLM block boundaries before hashing |
| `hash_strategy` | `bounded_load` | `bounded_load`, `p2c`, `jump`, or `modular` |
| `bounded_load_epsilon` | `0.25` | Per-backend capacity headroom = `ceil(avg_load * (1 + ε))` |
| `p2c_load_bias` | `2` | Switch to secondary only when `secondary + bias < primary` |
| `enable_prefix_boundary` | `true` | Detect system message boundaries for multi-turn |
| `min_prefix_boundary_tokens` | `4` | Minimum system message tokens to use as boundary |
| `accept_client_prefix_boundary` | `false` | Accept client-provided `prefix_token_count` / `prefix_boundaries` |
| `enable_multi_depth_routing` | `false` | Store routes at multiple message depths |

## Implementation Details

### Key Files

- `src/router_service.cpp` - `route_request()` (top-level dispatch), `get_backend_for_prefix()` (hybrid ART+hash + scoring pass), `bounded_load_select()`, `p2c_select()`, `jump_consistent_hash()`, `compute_load_allowance()`
- `src/route_scorer.hpp` - Pure unified-score decision core: term definitions, placement/dispatch argmaxes, tie-break order (reactor-free unit-testable)
- `src/router_service.cpp` - `learn_route_global()` (single boundary) and `learn_route_global_multi()` (multi-depth)
- `src/radix_tree.hpp` - Adaptive Radix Tree with longest prefix match
- `src/parse_utils.hpp` - `hash_prefix()` (FNV-1a + block alignment) shared by router, header injection, and cache-event eviction
- `src/http_controller.cpp` - Routing decision, prefix-boundary detection, `X-Backend-ID` header
- `src/request_rewriter.hpp` - `extract_text_with_boundary_info()`, `extract_prefix_token_count()`, `extract_prefix_boundaries()`
- `src/boundary_detector.hpp` - Translates char-level system-prefix boundary into token-level boundary
- `src/config_schema.hpp` - `RoutingConfig` struct (the `config.hpp` header is a thin facade)
- `src/config_loader.cpp` - YAML and environment-variable parsing

### System Message Boundary Detection

Ranvier determines the prefix boundary through a single-pass JSON extraction (`extract_text_with_boundary_info()` in `request_rewriter.hpp`) that builds one combined text string from all messages while tracking the character offset where system messages end. This combined text is formatted using the configured chat template (llama3, chatml, mistral, or plain newline-joining), so tokenization aligns with vLLM's internal representation.

The system prefix substring is then tokenized to produce the token-level boundary. Because the prefix is a substring of the same combined text that produces the full token sequence, the system tokens are guaranteed to be an exact prefix of the full tokenization — no special delimiter or trailing newline needed.

When system messages are interleaved with non-system messages (non-contiguous), Ranvier falls back to using the pre-extracted raw system text as an approximation.

### Cluster-Wide Hash Consistency

In multi-node deployments, each Ranvier node learns routes independently. Before routes are learned, the hash fallback must be deterministic across all nodes.

When a prefix boundary is available, Ranvier uses it (instead of `prefix_token_length`) as the hash input length. This ensures that requests sharing the same system message hash to the same backend across all nodes, even before routes are learned — regardless of differing user queries.

### Hash Function

The hash fallback uses FNV-1a over the raw token bytes, with the token count first rounded down to the nearest `block_alignment` boundary (defaulting to 16, matching vLLM's PagedAttention block size). This alignment ensures that two prefixes differing only in trailing partial-block tokens hash identically. When the available token count is smaller than `block_alignment`, all available tokens are hashed (no truncation). See `hash_prefix()` in `parse_utils.hpp` for the implementation; the same helper is reused for the `X-Ranvier-Prefix-Hash` header and cache-event eviction lookups so that all three views agree byte-for-byte.

### Hash Strategies

The 64-bit FNV-1a hash is fed into one of four bucket-selection strategies, configurable via `hash_strategy` (env: `RANVIER_HASH_STRATEGY`). The strategy controls the **hash-fallback path** in `prefix` mode and the entire selection in `hash` mode.

| Strategy | Behavior | Built-in load awareness |
|----------|----------|-------------------------|
| `bounded_load` (default) | Jump consistent hash + per-backend cap of `ceil(avg_load · (1 + ε))`. On overflow, sequentially probes adjacent buckets, then falls back to least-loaded. | Yes — the scorer's load term skips bounded-load hash anchors (load already vetted). |
| `p2c` | Power-of-two-choices: hashes to a primary and a secondary (XORed with a per-request salt). Sticks to primary unless secondary's load is lower by at least `p2c_load_bias`. | Yes — the scorer's load term skips P2C hash anchors (load already vetted). |
| `jump` | Plain Lamping/Veach jump consistent hash. Topology changes remap only ~1/n keys. | No — the scorer's load term applies the median allowance to both ART and hash anchors. |
| `modular` | `hash % num_backends`. **Benchmark only**: any topology change reshuffles all keys. | No — the scorer's load term applies the median allowance to both ART and hash anchors. |

`bounded_load_epsilon` (default `0.25`) and `p2c_load_bias` (default `2`) tune the corresponding strategies. All strategies honor the `load_aware_routing` toggle: when set to `false`, every strategy degrades to plain jump consistent hash with no diversion, giving operators a uniform escape hatch.

Both `bounded_load` and `p2c` use **capacity-adjusted load** (active requests blended with cache headroom and GPU load) when `capacity_headroom_weight > 0` and headroom data is available, so backends near KV-cache exhaustion are deprioritized for large requests.

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
| `router_hardware_cost_diverts_total` | Cache misses diverted to a backend with lower `cost_per_hour` |

## Comparison with Legacy Routing

| Aspect | Legacy (ART + Random) | Prefix-Affinity (ART + Hash) |
|--------|----------------------|------------------------------|
| Cache miss fallback | Random backend | Deterministic (hash) |
| Same prefix consistency | Only after learning | Immediate |
| First request | Random | Deterministic |
| Partial prefix match | Yes (ART) | Yes (ART) |

## Load-Aware Routing

When enabled (`load_aware_routing: true`, the default), the router considers backend queue depth before routing to the prefix-preferred backend. This prevents hot spots when a single backend becomes overloaded with requests.

How load awareness is applied depends on the active `hash_strategy`:

- **`bounded_load`** and **`p2c`** bake load awareness into hash selection itself (cap probing for bounded-load, secondary comparison for P2C). For ART hits, the unified score's **load term** hinges the anchor against the strategy's allowance (the cap for bounded-load, `least + bias` for P2C) and diverts when the anchor exceeds it.
- **`jump`** and **`modular`** are load-blind at selection, so the load term applies its **median allowance** (`median · load_imbalance_factor + load_imbalance_floor`) to both ART and hash anchors.

When `load_aware_routing: false`, the load term is disabled and all strategies skip diversion entirely.

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

### Algorithm (load term of the unified score)

The load term hinges every candidate's load against the active strategy's **allowance** — the load level a candidate may carry penalty-free:

```
allowance:
  bounded_load:   cap - 1, cap = max(1, ceil(avg_load * (1 + epsilon)))
  p2c:            least_load + p2c_load_bias
  jump / modular: median * load_imbalance_factor + load_imbalance_floor

dispatch score(b) = prefix_weight * affinity(b)
                  - load_weight   * max(0, load(b) - allowance)
                  - cost_weight   * cost_hinge(b)
                  [+ price terms on the miss path]
```

At the neutral defaults (`prefix_weight 0`, `load_weight 1`) the anchor keeps its seat exactly while at or under the allowance, and forfeits past it — the same keep/divert frontier as the per-strategy rules. Divert targets are encoded in the deterministic tie-break order: first-under-cap in jump-probe order for bounded-load, least-loaded (then lowest id) for the others. The median allowance auto-adapts to any workload, model size, or cluster size — no per-model tuning needed; `bounded_load`/`p2c` use capacity-adjusted load, `jump`/`modular` use composite load (`local_active_requests + gpu_load_score * gpu_load_weight`).

See `route_scorer.hpp` for the scorer and `compute_load_allowance()` in `router_service.cpp` for the allowance math; the miss-path hash strategies keep their own integrated load logic — see `bounded_load_select()` and `p2c_select()`.

Regardless of which candidate wins dispatch, route learning (Step 4 of the high-level flow) uses the **placement winner**, not the load-diversion target. This preserves long-term cache affinity across transient load spikes.

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

## Unified Route Scoring (`routing.scoring.*`)

Everything after the anchor — the residency-gated affinity, the load divert, the cost-budget divert, and the hardware-price preference — is a single weighted ranking over the live candidates (`src/route_scorer.hpp`), computed in one pass per request:

```
placement(b) = prefix_weight * affinity(b) + price_weight * price_advantage(b)
dispatch(b)  = placement(b)
             - load_weight * max(0, load(b) - allowance)
             - cost_weight * cost_hinge(b)
```

`placement` argmax → `original_selected` (the learn target, stable inputs only); `dispatch` argmax → the backend serving this request. Tie-breaks are deterministic: placement prefers the anchor then lower composite load then lower id; dispatch prefers a clean placement winner (no forfeited term), then jump-probe order, then lower cost pressure, then lower load, then lower id.

| Weight | Default | Env | Meaning |
|--------|---------|-----|---------|
| `prefix_weight` | `0.0` | `RANVIER_SCORING_PREFIX_WEIGHT` | Extra load units a cache-warm anchor may carry beyond the strategy allowance. `0` = stickiness governed entirely by the strategy parameters (legacy behavior). |
| `load_weight` | `1.0` | `RANVIER_SCORING_LOAD_WEIGHT` | Scale of the over-allowance penalty. `0` disables load diverts. |
| `residency_weight` | `0.0` | `RANVIER_SCORING_RESIDENCY_WEIGHT` | Discounts the affinity bonus by reported cache-cooling: `affinity *= 1 - w*(1-residency)`. `0` = hard gate at `cache_residency_threshold` only. |
| `cost_weight` | `1.0` | `RANVIER_SCORING_COST_WEIGHT` | Scale of the cost-budget hinge (inert unless `cost_routing.enabled`). |
| `price_weight` | `1.0` | `RANVIER_SCORING_PRICE_WEIGHT` | Hardware-price preference strength; `0` disables the cache-miss cheaper-hardware divert. |
| `slo_weight` | `0.0` | `RANVIER_SCORING_SLO_WEIGHT` | Reserved seam for a future latency/SLO term; parsed but not read. |

**Default-weight parity:** at the defaults above, every term reduces to its legacy rule — the per-step keys (`bounded_load_epsilon`, `p2c_load_bias`, `load_imbalance_factor/floor`, `cache_residency_threshold`, `cost_routing.*`, per-backend `cost_per_hour`) keep their authority and an un-tuned config routes exactly as the former sequential override pipeline did. The weights are the seam for BACKLOG §20.1: precise KV-event residency (P0.1) grades `affinity(b)` per backend instead of gating it, pool-role awareness (P0.3) adds a dimension to the same pass, and an SLO term plugs into the reserved weight.

## References

- [Ray Serve PrefixCacheAffinityRouter](https://docs.ray.io/en/latest/serve/advanced-guides/llm-serving.html) - Similar approach in Ray Serve
- [vLLM Automatic Prefix Caching](https://docs.vllm.ai/en/latest/automatic_prefix_caching/apc.html) - Backend KV cache reuse
- [Consistent Hashing](https://en.wikipedia.org/wiki/Consistent_hashing) - Karger et al., 1997
- [Adaptive Radix Trees](https://db.in.tum.de/~leis/papers/ART.pdf) - Leis et al., 2013
