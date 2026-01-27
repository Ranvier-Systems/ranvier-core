# Ranvier Intelligence Layer - Architecture & Vision

[![Status: Architecture Defined](https://img.shields.io/badge/Status-Architecture%20Defined-blue)](docs/ARCHITECTURE_AND_VISION.md)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](../LICENSE)

> **Goal**: Transform Ranvier from a "smart router" into a full "Intelligence Layer for Inference Infrastructure" with two product lines: Ranvier Cloud and Ranvier Local.

---

> **Note**: This document outlines the architectural vision for the Ranvier Intelligence Layer.
>
> **Ranvier Core** (routing, priority queues, prefix-based caching) is developed in the open under the Apache 2.0 license.
>
> **Advanced capabilities** (enterprise policy enforcement, team management, advanced analytics) may be developed as separate modules or plugins with different licensing. Features marked with 🔓 are Core/Open Source. Features marked with 🏢 are planned Enterprise capabilities.

---

## Executive Summary

| Phase | Focus | Duration | Outcome | License |
|-------|-------|----------|---------|---------|
| **Phase 1** | Foundation | 4-5 weeks | Request awareness, intent classification, priority infrastructure | 🔓 Core |
| **Phase 2** | Cloud Intelligence | 4-5 weeks | GPU-aware routing, HoL prevention | 🔓 Core |
| **Phase 3** | Local Product | 3-4 weeks | Ranvier Local MVP | 🔓 Core |
| **Phase 4** | Polish | 2-3 weeks | Integration, documentation, release | 🔓 Core |

**Total Estimated Effort**: 13-17 weeks for full vision

---

## The Core Value Proposition

> **"Why would I use Ranvier instead of just picking a model from a dropdown?"**

A dropdown (Cursor, etc.) sets a **global preference**. Ranvier makes **per-request decisions**:

| Scenario | Dropdown | Ranvier |
|----------|----------|---------|
| You type a character | Goes to your selected model | Routes to Local (0ms latency) |
| You hit "Apply Fix" | Goes to your selected model | Routes to Cloud/Smart (high intelligence) |
| Agent fires 5000 requests | All go to same model | Each routed by intent + cost |
| Dev pastes PII | Goes wherever | Blocked or routed to compliant provider |

**The "Extra Layer" pays rent by**:
- Lowering bills (cost arbitrage between providers)
- Lowering latency (localhost routing for autocomplete)
- Ensuring safety (PII filtering, audit logs)

---

## Phase 1: Foundation (Weeks 1-4) 🔓

> **License**: All Phase 1 components are Ranvier Core (Apache 2.0)

### 1.1 Request Size Estimation

**Why**: Enables Head-of-Line blocking prevention. Without knowing request "cost," we can't prioritize effectively.

**What to Build**:
- Parse incoming request body to extract prompt tokens
- Estimate output tokens from `max_tokens` field (or heuristic: prompt_len × 1.5)
- Calculate "request cost" = prefill_cost + estimated_decode_cost
- Store in `ProxyContext` for routing decisions

**Files to Modify**:
```
src/http_controller.hpp
  └── ProxyContext struct (line 64)
      + uint32_t estimated_input_tokens
      + uint32_t estimated_output_tokens
      + float estimated_cost

src/http_controller.cpp
  └── handle_proxy() (line 647)
      + Add estimate_request_cost() call after body parsing
      + Parse max_tokens from JSON body
      + Use tokenizer token count (already available)

src/config.hpp
  └── Add CostEstimationConfig struct
      + float prefill_cost_per_token (default: 1.0)
      + float decode_cost_per_token (default: 10.0)
      + uint32_t default_max_tokens (default: 256)
```

**Technical Approach**:
```cpp
// In http_controller.cpp
float estimate_request_cost(const ProxyContext& ctx) {
    uint32_t input_tokens = ctx.token_ids.size();
    uint32_t output_tokens = estimate_output_tokens(ctx);

    // Decode is ~10x more expensive than prefill per token
    return (input_tokens * _config.prefill_cost_per_token) +
           (output_tokens * _config.decode_cost_per_token);
}
```

> **CRITICAL: The "Max Tokens" Trap**
>
> Most agentic clients (Cursor, Cline) default `max_tokens` to 4096-8192 to avoid
> truncation, even when expecting ~50 lines of code. Naively using `max_tokens`
> will make Ranvier think every request is "expensive," causing aggressive
> throttling and GPU underutilization.

**Solution: Heuristic Decay**

```cpp
// Track actual output tokens per User-Agent
struct AgentOutputStats {
    std::string user_agent_pattern;
    double moving_avg_output_tokens;  // Exponential moving average
    uint64_t sample_count;
};

uint32_t estimate_output_tokens(const ProxyContext& ctx) {
    // 1. Check if we have historical data for this agent
    if (auto stats = _agent_stats.find(ctx.user_agent)) {
        // Use reality-based estimate with 20% safety margin
        return static_cast<uint32_t>(stats->moving_avg_output_tokens * 1.2);
    }

    // 2. Fall back to max_tokens for unknown agents (conservative)
    return ctx.max_tokens.value_or(_config.default_max_tokens);
}

// Called after each request completes
void record_actual_output(const ProxyContext& ctx, uint32_t actual_tokens) {
    auto& stats = _agent_stats[ctx.user_agent];
    constexpr double alpha = 0.1;  // EMA smoothing factor
    stats.moving_avg_output_tokens =
        alpha * actual_tokens + (1 - alpha) * stats.moving_avg_output_tokens;
    stats.sample_count++;
}
```

**Result**: Reserve resources based on reality, not safety limits. After ~50 requests
from an agent, estimates converge to actual behavior.

**Effort**: 1.5 weeks (increased for heuristic decay)
**Risk**: Low - additive change, no breaking modifications
**Dependencies**: None

---

### 1.2 Priority Tiers Infrastructure

**Why**: Foundation for both Local (agent vs interactive) and Cloud (SLA-based) priority routing.

**What to Build**:
- Priority extraction from request headers (`X-Ranvier-Priority`) or body
- Priority queue replacing simple semaphore-based backpressure
- Per-priority metrics and rate limits
- Priority inheritance for related requests (same conversation)

**Files to Modify**:
```
src/http_controller.hpp
  └── Add PriorityLevel enum
      CRITICAL = 0,  // Interactive typing, must not block
      HIGH = 1,      // Primary agent task
      NORMAL = 2,    // Background processing
      LOW = 3,       // Batch jobs, can wait

  └── Extend BackpressureSettings (line 46)
      + bool enable_priority_queue
      + std::array<uint32_t, 4> tier_capacity  // Per-tier limits

  └── Add PriorityQueue class
      + enqueue(ProxyContext, priority)
      + dequeue() -> highest priority waiting
      + size_by_priority() -> metrics

src/http_controller.cpp
  └── handle_proxy() (line 647)
      + Extract priority from X-Ranvier-Priority header
      + Extract priority from request body (agent metadata)
      + Enqueue with priority instead of direct semaphore acquire

  └── Add process_priority_queue() background task

src/metrics_service.hpp
  └── Add per-priority metrics
      + requests_by_priority[4]
      + queue_depth_by_priority[4]
      + latency_by_priority[4]
```

**Priority Extraction Logic**:
```cpp
PriorityLevel extract_priority(const http::request& req) {
    // 1. Check explicit header
    if (auto hdr = req.get_header("X-Ranvier-Priority")) {
        return parse_priority(*hdr);
    }

    // 2. Check for known agent signatures
    auto user_agent = req.get_header("User-Agent").value_or("");
    if (user_agent.contains("Cursor") || user_agent.contains("claude-code")) {
        // Cursor/Claude Code - likely interactive
        return is_streaming(req) ? CRITICAL : HIGH;
    }

    // 3. Check request body for agent metadata
    if (auto agent_id = extract_agent_id(req.body)) {
        return _agent_priorities.get(*agent_id, NORMAL);
    }

    return NORMAL;
}
```

**Effort**: 2 weeks
**Risk**: Medium - modifies critical path, needs careful testing
**Dependencies**: None (but enhances 1.1)

---

### 1.3 Configuration: Local Mode Flag

**Why**: Single configuration switch to disable cloud features for local development use.

**What to Build**:
- `local_mode` configuration section
- When enabled: disable gossip, single-shard, disable persistence, auto-discover local backends

**Files to Modify**:
```
src/config.hpp
  └── Add LocalModeConfig struct (around line 360)
      + bool enabled (default: false)
      + bool disable_clustering (default: true when local)
      + bool disable_persistence (default: true when local)
      + bool auto_discover_backends (default: true when local)
      + uint16_t discovery_port_start (default: 11434, Ollama)
      + uint16_t discovery_port_end (default: 11444)

  └── Extend RanvierConfig (line 373)
      + LocalModeConfig local_mode

  └── load() - Add YAML parsing for local_mode section
  └── validate() - Add local mode validation
  └── apply_env_overrides() - Add RANVIER_LOCAL_MODE=true support

src/application.cpp
  └── Modify initialization to check local_mode.enabled
      + Skip gossip service startup
      + Skip persistence manager startup
      + Force single shard
      + Start local backend discovery
```

**Example Config**:
```yaml
# ranvier-local.yaml
local_mode:
  enabled: true
  auto_discover_backends: true
  discovery_ports: [11434, 8080, 5000]  # Ollama, vLLM, custom

routing:
  mode: prefix  # Still use intelligent routing locally

# These are auto-disabled in local mode:
# clustering: ...
# gossip: ...
# persistence: ...
```

**Effort**: 1 week
**Risk**: Low - feature flag pattern, doesn't affect cloud path
**Dependencies**: None

---

### 1.4 Request Intent Classification (Protocol Fingerprinting)

**Why**: **Length ≠ Complexity.** A 50-token request could be a simple autocomplete (needs 10ms latency) or a complex "Apply Fix" instruction (needs GPT-4 intelligence). Heuristics based solely on token count will misroute critical "short but smart" tasks.

> **The Product Question This Solves**
>
> "Why would I use Ranvier instead of just picking a model from a dropdown?"
>
> Answer: A dropdown is a **global preference**. Ranvier makes **per-request decisions**
> based on the wire format. When you type a character, it routes to Local (0ms). When
> you hit "Apply Fix," it routes to Cloud (smart). A dropdown can't do millisecond-level
> intent-based switching.

**What to Build**:
- **Wire-Format Inspector**: Use simdjson to detect the "shape" of the request payload
- **Intent Detector**: Classify requests into `AUTOCOMPLETE` (FIM), `CHAT` (Interactive), or `EDIT` (Complex Instruction)
- **Routing Rules**: Map intents to backend tiers (e.g., `AUTOCOMPLETE` → Local/Groq, `EDIT` → Claude 3.5/GPT-4)

**Files to Modify**:
```
src/http_controller.hpp
  └── ProxyContext struct
      + enum class RequestIntent { AUTOCOMPLETE, CHAT, EDIT, UNKNOWN }
      + RequestIntent intent

src/router_service.hpp
  └── Add IntentRoutingConfig
      + backend_id autocomplete_backend  // Fast, local
      + backend_id chat_backend          // Balanced
      + backend_id edit_backend          // Smart, cloud

src/router_service.cpp
  └── Add classify_intent(const rapidjson::Document& json_body)
  └── Modify route_request() to consider intent
```

**The Protocol Fingerprints**:

| Intent | Wire Format Signal | Example |
|--------|-------------------|---------|
| **AUTOCOMPLETE** | `suffix`, `fim_prefix`, `fim_middle`, `<\|fim_hole\|>` tokens | Cursor typing completion |
| **EDIT** | System prompt contains "rewrite", "diff", "refactor"; XML tags like `<diff>` | "Apply Fix" button |
| **CHAT** | Generic messages array, no special fields | Normal conversation |

**Technical Approach**:
```cpp
// src/router_service.cpp - Zero-copy JSON inspection with simdjson

RequestIntent classify_intent(const ProxyContext& ctx) {
    // 1. Check for FIM (Autocomplete) - The "Typing" Case
    // Fast path: autocomplete requests have 'suffix' or specific FIM params
    if (ctx.json_body.contains("suffix") ||
        ctx.json_body.contains("fim_prefix") ||
        ctx.json_body.contains("fim_middle")) {
        return RequestIntent::AUTOCOMPLETE;  // Route to Local/Fast
    }

    // 2. Check for "Edit/Fix" Patterns - The "Apply Fix" Case
    // Look for keywords in system prompt indicating complex rewriting
    const auto system_prompt = ctx.get_system_prompt();
    if (system_prompt.contains("diff") ||
        system_prompt.contains("rewrite") ||
        system_prompt.contains("refactor") ||
        system_prompt.contains("You are an expert")) {
        return RequestIntent::EDIT;  // Route to Cloud/Smart
    }

    // 3. Check for edit-specific XML tags in any message
    for (const auto& msg : ctx.messages) {
        if (msg.content.contains("<diff>") ||
            msg.content.contains("<edit>") ||
            msg.content.contains("<code_change>")) {
            return RequestIntent::EDIT;
        }
    }

    // 4. Default to CHAT
    return RequestIntent::CHAT;
}

// Integration into routing decision
RouteResult route_request(const ProxyContext& ctx) {
    auto intent = classify_intent(ctx);

    switch (intent) {
        case RequestIntent::AUTOCOMPLETE:
            // Latency is king - use fastest available backend
            return route_to_fastest_backend(ctx);

        case RequestIntent::EDIT:
            // Intelligence is king - use smartest backend regardless of cost
            return route_to_smartest_backend(ctx);

        case RequestIntent::CHAT:
        default:
            // Use existing prefix-based routing with cost awareness
            return route_by_prefix(ctx);
    }
}
```

**Why This Matters for Each Product**:

| Product | Without Intent Classification | With Intent Classification |
|---------|------------------------------|---------------------------|
| **Ranvier Local** | All requests go to same local model | Typing → qwen-2.5-coder (fast), Apply Fix → Claude API (smart) |
| **Ranvier Cloud** | Token-count based routing only | Short edit requests get GPU priority, long chats can wait |
| **Enterprise** | No visibility into request types | Audit logs show: "34% autocomplete, 12% edits, 54% chat" |

**Metrics to Add**:
```cpp
// Per-intent counters for visibility
requests_by_intent[AUTOCOMPLETE]  // "How often are devs typing?"
requests_by_intent[EDIT]          // "How often are devs applying fixes?"
requests_by_intent[CHAT]          // "How often are devs chatting?"

latency_by_intent[intent]         // "Are we meeting latency targets per intent?"
cost_by_intent[intent]            // "What's our spend per intent type?"
```

**Effort**: 1 week
**Risk**: Low - pure logic change, highly testable with captured request payloads
**Dependencies**: Phase 1.1 (Request Parsing infrastructure)

---

## Phase 2: Cloud Intelligence (Weeks 5-9) 🔓

> **License**: Core routing intelligence is Apache 2.0. Advanced SLA enforcement and multi-tenant policies may be Enterprise.

### 2.1 vLLM Metrics Ingestion

**Why**: Can't manage GPU saturation without knowing GPU state. vLLM exposes `/metrics` endpoint.

**What to Build**:
- Periodic scraping of vLLM `/metrics` endpoint per backend
- Parse Prometheus format for key metrics
- Store in `BackendMetrics` for routing decisions
- Expose aggregated metrics on Ranvier's `/metrics`

**Files to Modify**:
```
src/health_service.hpp
  └── Extend HealthCheckResult
      + VLLMMetrics vllm_metrics

  └── Add VLLMMetrics struct
      + uint32_t num_requests_running
      + uint32_t num_requests_waiting
      + float gpu_memory_used_bytes
      + float gpu_memory_total_bytes
      + float gpu_cache_usage_percent
      + double tokens_per_second
      + uint64_t prompt_tokens_total
      + uint64_t generation_tokens_total

src/health_service.cpp
  └── check_backend()
      + Fetch /metrics endpoint (in addition to health)
      + Parse Prometheus text format
      + Extract vLLM-specific metrics

  └── Add parse_vllm_metrics(string_view body) -> VLLMMetrics

src/metrics_service.hpp
  └── Extend BackendMetrics (line 138)
      + VLLMMetrics latest_vllm_metrics
      + std::chrono::steady_clock::time_point metrics_updated_at

  └── Add record_backend_vllm_metrics(backend_id, VLLMMetrics)

src/router_service.hpp
  └── Add get_backend_load(backend_id) -> float
      // Returns 0.0 (idle) to 1.0 (saturated)
```

**vLLM Metrics to Scrape** (from vLLM's Prometheus endpoint):
```
# Key metrics for routing decisions:
vllm:num_requests_running        # Active requests on GPU
vllm:num_requests_waiting        # Queued in vLLM
vllm:gpu_cache_usage_perc        # KV cache saturation
vllm:avg_prompt_throughput       # Prefill speed
vllm:avg_generation_throughput   # Decode speed

# Memory metrics:
gpu_memory_used_bytes
gpu_memory_total_bytes
```

**Effort**: 2 weeks
**Risk**: Medium - depends on vLLM metric stability, need fallback for non-vLLM backends
**Dependencies**: Phase 1.2 (metrics infrastructure)

---

### 2.2 Load-Aware Backend Selection

**Why**: Current routing picks backend by prefix match, ignoring if that backend is overloaded. This causes HoL blocking.

**What to Build**:
- Query backend load before routing decision
- If preferred backend is overloaded, consider alternatives
- Implement "spillover" routing to less-loaded backends
- Track cache hit rate vs load balance tradeoff

**Files to Modify**:
```
src/router_service.hpp
  └── Extend RouteResult (line 104)
      + float backend_load_at_decision
      + bool was_load_redirect

  └── Add LoadAwareRoutingConfig
      + float max_backend_load (default: 0.8)
      + float load_redirect_threshold (default: 0.7)
      + bool prefer_cache_over_load (default: true)
      + uint32_t max_redirect_attempts (default: 3)

src/router_service.cpp
  └── get_backend_for_prefix()
      BEFORE:
        1. Radix lookup -> backend_id
        2. Return backend_id

      AFTER:
        1. Radix lookup -> preferred_backend_id
        2. Check load: get_backend_load(preferred_backend_id)
        3. If load > threshold:
           a. Find alternative with same prefix (if replicated)
           b. Or find least-loaded backend
           c. Record was_load_redirect = true
        4. Return selected_backend_id

  └── Add select_alternative_p2c(exclude_id) -> backend_id  // P2C selection
  └── Add find_alternative_for_prefix(tokens, exclude_id) -> optional<backend_id>
```

> **CRITICAL: Avoid the "Thundering Herd"**
>
> If Node A is overloaded and Node B is empty, a naive "find least loaded" approach
> will instantly switch ALL pending requests to Node B, overloading it in milliseconds.
> This causes oscillation where load ping-pongs between backends.

**Solution: Power of Two Choices (P2C)**

Don't scan all backends for the "best" one. Pick two random candidates and choose
the better of the two. This is mathematically proven to balance load without oscillation.

```cpp
backend_id select_alternative_p2c(backend_id exclude_id) {
    // Get two random candidates (excluding the overloaded one)
    auto candidates = get_healthy_backends();
    candidates.erase(exclude_id);

    if (candidates.size() < 2) {
        return candidates.empty() ? exclude_id : candidates[0];
    }

    // Pick two random backends
    std::uniform_int_distribution<size_t> dist(0, candidates.size() - 1);
    auto idx1 = dist(_rng);
    auto idx2 = dist(_rng);
    while (idx2 == idx1) idx2 = dist(_rng);

    auto b1 = candidates[idx1];
    auto b2 = candidates[idx2];

    // Return the one with lower load
    return get_backend_load(b1) < get_backend_load(b2) ? b1 : b2;
}
```

**Routing Decision Flow**:
```
Request arrives with tokens [t1, t2, ..., tn]
    │
    ▼
Radix Tree Lookup
    │
    ├─► Cache Hit: preferred_backend = B1
    │       │
    │       ▼
    │   Check Load(B1)
    │       │
    │       ├─► Load < 0.7: Route to B1 ✓
    │       │
    │       └─► Load >= 0.7:
    │               │
    │               ▼
    │           P2C Selection (pick 2 random, choose better)
    │               │
    │               └─► Route to winner of P2C
    │
    └─► Cache Miss: Consistent hash → P2C if overloaded → route
```

**Why P2C Works**: With N backends, naive "least loaded" has O(N) coordination cost
and causes herding. P2C has O(1) cost and provably achieves O(log log N) maximum load,
which is nearly optimal.

**Effort**: 2 weeks
**Risk**: Medium - changes core routing logic, needs A/B testing
**Dependencies**: 2.1 (vLLM metrics)

---

### 2.3 Request Cost-Based Routing

**Why**: True HoL prevention requires routing expensive requests away from backends serving cheap requests.

**What to Build**:
- Use estimated cost from Phase 1.1
- Track per-backend "cost budget" (requests in flight × estimated cost)
- Route to backend with available budget
- Implement "small request fast lane"

**Files to Modify**:
```
src/router_service.hpp
  └── Add CostBasedRoutingConfig
      + bool enabled (default: false)
      + float max_cost_per_backend (default: 10000.0)
      + float small_request_threshold (default: 500.0)
      + bool enable_fast_lane (default: true)

src/router_service.cpp
  └── Add per-backend cost tracking
      + std::atomic<float> current_cost_budget[backend_id]

  └── Modify route_request()
      + If cost-based enabled:
        - Check if request is "small" (cost < threshold)
        - If small + fast_lane: route to dedicated fast-lane backends
        - If large: route to backend with available budget
        - Reserve budget on route, release on completion

  └── Add reserve_cost_budget(backend_id, cost) -> bool
  └── Add release_cost_budget(backend_id, cost)

src/http_controller.cpp
  └── After response complete:
      + Call release_cost_budget() with actual cost if available
```

**Effort**: 1.5 weeks
**Risk**: Medium - requires accurate cost estimation, may need tuning
**Dependencies**: 1.1 (size estimation), 2.2 (load-aware routing)

---

## Phase 3: Ranvier Local Product (Weeks 10-13) 🔓

> **License**: Local discovery and agent scheduling are Apache 2.0. Team policy management may be Enterprise.

### 3.1 Local Backend Discovery

**Why**: Local users shouldn't need to configure backends manually. Auto-discover Ollama, LM Studio, llama.cpp.

**What to Build**:
- Port scanning for known LLM server ports
- **Semantic liveness checks** (not just TCP connect)
- Backend capability detection (model name, context length)
- Hot-add/remove as local servers start/stop

> **CRITICAL: The "Zombie Port" Problem**
>
> Local LLM servers (especially vLLM) often crash but leave the socket open
> (`TIME_WAIT`) or enter a zombie state where they accept TCP connections but
> hang on HTTP requests. A simple `connect()` will succeed, but requests will timeout.

**Solution: Semantic Liveness Check**

Don't just connect - send a minimal HTTP request and require a fast response.

**New Files**:
```
src/local_discovery.hpp
src/local_discovery.cpp
```

**Implementation**:
```cpp
class LocalDiscoveryService {
public:
    struct DiscoveredBackend {
        std::string address;
        uint16_t port;
        std::string server_type;  // "ollama", "vllm", "lmstudio", "llamacpp"
        std::vector<std::string> available_models;
        uint32_t max_context_length;
        std::chrono::steady_clock::time_point last_healthy;
    };

    // Probe known ports for LLM servers
    seastar::future<std::vector<DiscoveredBackend>> discover();

    // Start background discovery loop
    seastar::future<> start(std::chrono::seconds interval);

    // Callback when backends change
    void on_backends_changed(std::function<void(std::vector<DiscoveredBackend>)>);

private:
    // Known ports to probe
    static constexpr std::array<uint16_t, 6> KNOWN_PORTS = {
        11434,  // Ollama
        8080,   // vLLM default
        1234,   // LM Studio
        8000,   // llama.cpp server
        5000,   // Text Generation WebUI
        3000,   // LocalAI
    };

    // Semantic liveness: must respond to HTTP in <50ms
    static constexpr auto LIVENESS_TIMEOUT = std::chrono::milliseconds(50);

    seastar::future<std::optional<DiscoveredBackend>> probe_port(uint16_t port);
    std::string detect_server_type(const http::response& resp);
};

// Semantic probe - not just TCP connect
seastar::future<std::optional<DiscoveredBackend>>
LocalDiscoveryService::probe_port(uint16_t port) {
    auto start = std::chrono::steady_clock::now();

    try {
        // 1. TCP connect with short timeout
        auto conn = co_await connect_with_timeout(
            socket_address(ipv4_addr("127.0.0.1", port)),
            std::chrono::milliseconds(20)
        );

        // 2. Send actual HTTP request (semantic check)
        auto resp = co_await http_get_with_timeout(
            conn, "/v1/models", LIVENESS_TIMEOUT
        );

        auto elapsed = std::chrono::steady_clock::now() - start;

        // 3. Must respond in <50ms total to be considered healthy
        if (elapsed > LIVENESS_TIMEOUT) {
            co_return std::nullopt;  // Too slow = zombie
        }

        // 4. Parse response to detect server type and capabilities
        co_return parse_discovery_response(port, resp);

    } catch (...) {
        co_return std::nullopt;  // Connection failed or timeout
    }
}
```

**Why 50ms?** Local servers should respond to `/v1/models` in <5ms when healthy.
A 50ms timeout catches zombies quickly while allowing for brief CPU spikes.

**Integration Points**:
```
src/application.cpp
  └── If local_mode.enabled && auto_discover_backends:
      + Start LocalDiscoveryService
      + On discovery: call router_service.register_backend_global()
      + On backend disappear: call router_service.unregister_backend_global()
```

**Effort**: 1.5 weeks
**Risk**: Low - isolated new component
**Dependencies**: 1.3 (local mode config)

---

### 3.2 Agent-Aware Request Handling

**Why**: Local users run multiple agents (Cursor, Cline, Claude Code). Need to identify and prioritize.

**What to Build**:
- Agent identification from User-Agent, custom headers, or request patterns
- Agent registry with configurable priorities
- "Pause agent" API for explicit control
- Request attribution in logs/metrics

**Files to Modify**:
```
src/config.hpp
  └── Add AgentConfig struct
      + std::string pattern (regex for User-Agent)
      + std::string name
      + PriorityLevel default_priority
      + bool allow_pause

  └── Add to LocalModeConfig
      + std::vector<AgentConfig> known_agents
      + bool auto_detect_agents (default: true)

src/http_controller.hpp
  └── Add AgentRegistry class
      + register_agent(pattern, name, priority)
      + identify_agent(request) -> optional<AgentInfo>
      + pause_agent(agent_id)
      + resume_agent(agent_id)
      + list_agents() -> vector<AgentInfo>

src/http_controller.cpp
  └── handle_proxy()
      + Identify agent from request
      + Check if agent is paused -> return 503 with Retry-After
      + Apply agent priority to request
      + Record agent attribution in metrics
```

**Default Agent Patterns**:
```yaml
known_agents:
  - pattern: "Cursor/.*"
    name: "Cursor IDE"
    default_priority: critical

  - pattern: "claude-code/.*"
    name: "Claude Code"
    default_priority: critical

  - pattern: "cline/.*"
    name: "Cline"
    default_priority: high

  - pattern: "aider/.*"
    name: "Aider"
    default_priority: high

  - pattern: ".*"
    name: "Unknown"
    default_priority: normal
```

**Admin API for Agent Control**:
```
POST /admin/agents/{agent_id}/pause
POST /admin/agents/{agent_id}/resume
GET  /admin/agents                    # List all known agents
GET  /admin/agents/{agent_id}/stats   # Agent-specific metrics
```

**Effort**: 2 weeks
**Risk**: Medium - needs coordination with agent developers for proper identification
**Dependencies**: 1.2 (priority infrastructure)

---

### 3.3 Request Queuing with Pause/Resume

**Why**: The "OS scheduler for tokens" metaphor requires actual queuing, not just routing.

**What to Build**:
- Per-agent request queues (not just per-priority)
- Pause: stop dequeuing from agent's queue
- Resume: start dequeuing again
- **Queue-jumping** (not true preemption - see note below)
- Fair scheduling within same priority tier

> **Clarification: "Preemption" = Queue-Jumping**
>
> True preemption (stopping a running GPU request mid-generation) is **impossible**
> with current HTTP APIs. Once a request is sent to vLLM, it will complete.
>
> What we can do:
> - **Queue-jumping**: When a CRITICAL request arrives, it goes to the front of
>   Ranvier's queue, guaranteeing the next available GPU slot.
> - **Pause low-priority**: Stop sending LOW priority requests until CRITICAL clears.
>
> Future consideration (Phase 4+): For very long requests, we could potentially
> implement "chunked generation" where Ranvier requests smaller `max_tokens` batches
> and re-queues continuations, creating natural preemption points. This requires
> backend support for KV cache persistence.

**Files to Modify**:
```
src/http_controller.hpp
  └── Add RequestScheduler class
      + enqueue(ProxyContext, agent_id, priority)
      + dequeue() -> next request to process
      + pause_agent(agent_id)
      + resume_agent(agent_id)
      + get_queue_stats() -> per-agent queue depths

  └── Scheduling algorithm:
      1. Check for CRITICAL priority requests (always first)
      2. Round-robin among non-paused agents at highest active priority
      3. Apply fairness: agent that hasn't been served longest gets next slot

src/http_controller.cpp
  └── Replace direct semaphore with RequestScheduler
  └── Background task: scheduler.dequeue() -> process_request()
```

**Scheduling Algorithm**:
```cpp
std::optional<ProxyContext> RequestScheduler::dequeue() {
    // 1. Critical priority always wins
    if (auto req = _critical_queue.try_pop()) {
        return req;
    }

    // 2. Find highest active priority with non-paused agents
    for (auto priority : {HIGH, NORMAL, LOW}) {
        auto candidates = get_non_paused_agents_with_requests(priority);
        if (candidates.empty()) continue;

        // 3. Fair scheduling: pick agent waiting longest
        auto agent = std::min_element(candidates.begin(), candidates.end(),
            [](auto& a, auto& b) { return a.last_served < b.last_served; });

        if (auto req = agent->queue.try_pop()) {
            agent->last_served = now();
            return req;
        }
    }

    return std::nullopt;  // Nothing to process
}
```

**Effort**: 2 weeks
**Risk**: High - fundamental change to request flow, needs extensive testing
**Dependencies**: 1.2 (priority), 3.2 (agent awareness)

---

## Phase 4: Polish & Release (Weeks 14-16) 🔓

### 4.1 Single-Binary Local Distribution

**What to Build**:
- Static binary build for macOS (arm64, x86_64) and Linux
- Homebrew formula
- `ranvier local` CLI command that starts with sensible defaults
- First-run wizard / auto-configuration

**Effort**: 1 week

---

### 4.2 Local Dashboard UI

**What to Build**:
- Simple web UI on `localhost:9180/dashboard`
- Show: discovered backends, active agents, queue depths, request rates
- Controls: pause/resume agents, adjust priorities
- Built with vanilla JS (no build step, embedded in binary)

**Effort**: 1.5 weeks

---

### 4.3 Documentation & Examples

**What to Build**:
- "Getting Started with Ranvier Local" guide
- "Ranvier Cloud Deployment" guide
- Integration examples for Cursor, Cline, Claude Code
- Benchmark reproduction guide

**Effort**: 1 week

---

## Dependency Graph

```
Phase 1 (Foundation)
    ├── 1.1 Request Size Estimation ──────────────────┐
    │           │                                      │
    │           ▼                                      │
    ├── 1.4 Intent Classification ◄── 1.1 ────────────┼──┐
    │                                                  │  │
    ├── 1.2 Priority Infrastructure ──────────────────┼──┼──┐
    │           │                                      │  │  │
    │           │                                      │  │  │
    └── 1.3 Local Mode Config ────────────────────────┼──┼──┼──┐
                │                                      │  │  │  │
                ▼                                      │  │  │  │
Phase 2 (Cloud)                                        │  │  │  │
    ├── 2.1 vLLM Metrics ◄─────────────────────────────┘  │  │  │
    │           │                                         │  │  │
    │           ▼                                         │  │  │
    ├── 2.2 Load-Aware Routing ◄──────────────────────────┘  │  │
    │           │                                            │  │
    │           ▼                                            │  │
    └── 2.3 Cost-Based Routing ◄── 1.1, 1.4 ─────────────────┘  │
                                                                │
Phase 3 (Local)                                                 │
    ├── 3.1 Local Discovery ◄───────────────────────────────────┘
    │           │
    │           ▼
    ├── 3.2 Agent Awareness ◄── 1.2, 1.4
    │           │
    │           ▼
    └── 3.3 Request Scheduling ◄── 3.2

Phase 4 (Polish)
    └── 4.1, 4.2, 4.3 (parallel, after Phase 3)
```

**Key Insight**: Phase 1.4 (Intent Classification) feeds into both Cloud (2.3 Cost-Based Routing)
and Local (3.2 Agent Awareness) paths. It's the "Protocol Fingerprinting" that makes
per-request routing decisions possible.

---

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| vLLM metrics format changes | Medium | Medium | Abstract behind interface, support multiple versions |
| Request cost estimation inaccurate | High | Medium | Use actual costs from completed requests to train heuristic |
| Priority queue adds latency | Medium | High | Benchmark extensively, keep fast path for low-load scenarios |
| Agent identification unreliable | Medium | Low | Allow explicit header, fall back gracefully |
| Local discovery misses backends | Low | Low | Allow manual override, clear error messages |

---

## Success Metrics

### Cloud Product
- [ ] TTFT improvement maintained (>40%) with load-aware routing
- [ ] P99 latency reduction vs baseline (>30%)
- [ ] Zero HoL blocking incidents under mixed workload
- [ ] GPU utilization >80% under load

### Local Product
- [ ] <100ms cold start time
- [ ] Zero configuration required for Ollama users
- [ ] Interactive requests (typing) never blocked by background agents
- [ ] <1% overhead vs direct backend access

---

## Next Steps

### Immediate Priority: Validate the "Permission to Build"

Before writing code, **prove the value exists**:

1. **Run the "Tax vs Dividend" Benchmark** (Priority 0)
   - Execute the 10-30 concurrent user benchmark against real vLLM
   - Confirm the ~40% TTFT improvement still holds
   - Document the "7ms overhead → 40% gain" tradeoff as the core value proposition
   - **Why first?** This data point justifies the entire 16-week effort. If it doesn't hold, pivot.

2. **Phase 1.1 + 1.2 (Merged)** - Simplified First Implementation
   - Build `ProxyContext` with cost tracking
   - Build `PriorityQueue` infrastructure
   - **Simplification**: For MVP, use `input_tokens` (prefill) as the cost proxy
     - Prefill is the primary cause of HoL blocking in high-concurrency batches
     - Heuristic decay for output estimation can come in v1.1
   - **Deliverable**: Priority queue working, cost tracked, metrics exposed

3. **Phase 2.1: vLLM Metrics** - Enable Load-Awareness
   - Verify `/metrics` endpoint stability across vLLM versions
   - Implement scraping and storage
   - **Gate**: Must complete before 2.2 (load-aware routing)

4. **Benchmark Again** - Measure the "Intelligence Layer" Delta
   - Compare: baseline → prefix routing → prefix + priority + load-aware
   - Quantify each layer's contribution to TTFT improvement

### Revised Phase 1 Structure

```
Original:
  1.1 Size Estimation (1.5 weeks)
  1.2 Priority Infrastructure (2 weeks)
  1.3 Local Mode Config (1 week)

Revised:
  1.0 Benchmark validation (0.5 weeks) ← NEW: gates everything
  1.1 Cost + Priority (merged) (2.5 weeks) ← Simplified: input_tokens only
  1.2 Local Mode Config (1 week)
  1.4 Intent Classification (1 week) ← NEW: "Length ≠ Complexity"
```

---

## Future Considerations 🏢

The following capabilities are being explored for potential future development. These may be
developed as separate enterprise modules or offered as managed services:

| Capability | Description | Status |
|------------|-------------|--------|
| **Team Policy Enforcement** | Define routing policies per team (e.g., "Engineering → Claude", "Support → GPT-4") | Exploring |
| **PII/Secrets Detection** | Automatically detect and block requests containing sensitive data | Exploring |
| **Cost Attribution** | Track and report AI spend per user, team, or project | Exploring |
| **Compliance Logging** | Audit trail of all routing decisions for regulated industries | Exploring |
| **Multi-Tenant Isolation** | Hard isolation between tenants in shared infrastructure | Exploring |
| **SLA Guarantees** | Contractual latency/availability commitments with alerting | Exploring |

*Interested in these capabilities? Open an issue or reach out to discuss your use case.*

---

*Document created: 2026-01-23*
*Last updated: 2026-01-24*
*Revision: Restructured as Architecture & Vision document with Core/Enterprise distinction*
