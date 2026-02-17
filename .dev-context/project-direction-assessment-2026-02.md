# Project Direction Assessment — February 2026

## Executive Summary

The core prefix-aware routing product is not viable long-term as a standalone cloud product. The competitive landscape has accelerated significantly since the initial assessment (early 2026), with the vLLM Semantic Router Iris release (January 2026), llm-d 0.5 (February 2026), and SGLang's 2026 Q1 roadmap all directly targeting Ranvier's cloud routing differentiators.

However, the VISION.md's heuristic intent classification (string matching) can be replaced with BERT-based classification using patterns already proven in the codebase (tokenizer FFI). And critically, vLLM-SR is **not tightly coupled to vLLM** — it routes OpenAI-compatible requests via Envoy — which both narrows Ranvier's "engine-agnostic" angle and validates the classification approach as portable.

The most defensible remaining niche is a **local developer proxy** with BERT-based intent classification, auto-discovery, and agent scheduling — a product that doesn't exist anywhere in the ecosystem. This should be built in Rust (using Candle for ML inference), not on Seastar.

---

## What Has Changed Since Initial Assessment

### 1. vLLM Semantic Router v0.1 "Iris" (January 2026)

The vLLM project now has its own intent classification system — the exact capability VISION.md Phase 1.4 identifies as the key differentiator. Iris uses a "Signal-Decision Driven Plugin Chain Architecture" with six signal types (domain, keyword, embedding, factual, feedback, preference), BERT-based classification, and multi-turn routing continuity. 50+ contributing engineers from Red Hat, IBM, AMD, and Hugging Face. Over 600 PRs merged since September 2025.

**Key architectural detail:** vLLM-SR is written in Go (44%), Python (21%), Rust (17%). ModernBERT runs internally within the router (not via vLLM). It operates as an Envoy External Processor routing **OpenAI-compatible requests** to any OpenAI-compatible endpoint. This means it is **not tightly coupled to vLLM** — it could theoretically route to Ollama, llama.cpp, or any OpenAI-compat backend. The "engine-agnostic" differentiator is therefore weaker than initially assessed.

**Sources:**
- https://blog.vllm.ai/2026/01/05/vllm-sr-iris.html
- https://blog.vllm.ai/2025/11/19/signal-decision.html
- https://github.com/vllm-project/semantic-router

### 2. llm-d 0.5 (February 4, 2026)

llm-d now has hierarchical KV offloading (GPU → CPU → filesystem tiers), LoRA-aware cache routing, resilient networking via UCCL, and scale-to-zero autoscaling. Integrated into Red Hat OpenShift AI 3.0 with full Prometheus/Grafana/OpenTelemetry observability. 87% cache hit rate and 88% faster TTFT for warm cache hits — comparable to Ranvier's numbers with the structural advantage of ground-truth cache introspection via KVEvents.

**Source:** https://llm-d.ai/blog/llm-d-v0.5-sustaining-performance-at-scale

### 3. SGLang 2026 Q1 Roadmap

SGLang is building priority-based traffic management, bucket-based routing (20-30% performance boost), and an autonomous gateway — all overlapping with VISION.md Phases 1-2. SGLang is deployed on 400,000+ GPUs worldwide.

**Source:** https://github.com/sgl-project/sglang/issues/12780

### 4. Enterprise AI Gateway Consolidation

Kong AI Gateway has PII sanitization across 20+ categories and 12 languages. F5 AI Gateway has real-time data classification. IBM Guardium AI Security and Databricks Mosaic AI Gateway offer comprehensive compliance. EU AI Act provisions took effect February 2026. The smaller players (Portkey $3M, LiteLLM $2.1M, Helicone $1.5M) have not raised new funding since 2023.

---

## Can Ranvier Adopt BERT-Based Classification?

### Yes — the hard part is already solved

The existing tokenizer FFI integration (`tokenizer_service.{hpp,cpp}`, `tokenizer_thread_pool.{hpp,cpp}`) demonstrates a battle-tested pattern for running external ML workloads from Seastar:

- **Cross-shard dispatch** via P2C to free the calling reactor during blocking FFI calls
- **Thread pool fallback** with lock-free SPSC queues and `seastar::alien::run_on()` for reactor callbacks
- **Memory reallocation at shard boundaries** (Rule #14/#15) to prevent `do_foreign_free` corruption
- **tikv-jemalloc patch** to give Rust its own allocator, bypassing Seastar's global malloc replacement

Adding BERT inference would follow the same architecture:

1. Add ONNX Runtime or Candle (HuggingFace Rust ML framework) via the same FFI pattern as tokenizers-cpp
2. Load ModernBERT-base (~80MB) or DistilBERT (~66MB)
3. Route inference through the existing thread pool path (1-10ms per classification on CPU)
4. Adopt Iris's signal type taxonomy: domain, keyword, embedding, factual, feedback, preference
5. Replace string-matching heuristics (`contains("suffix")`, `contains("diff")`) with proper semantic classification

This is incremental engineering, not architectural change. The tokenizer FFI proves the pattern works.

### But the strategic question remains

Even with BERT-based classification, the C++ cloud product faces the same competitive dynamics:

- **vLLM-SR already exists**, is open source, has 50+ contributors, and isn't vLLM-coupled
- **Reimplementing vLLM-SR's classification in C++** puts you in a permanent catch-up position
- **The classification is a feature, not a product** — what matters is the product experience around it

Where BERT classification *does* change the picture is for the **local developer proxy**. vLLM-SR requires Envoy and Kubernetes-style deployment. A single binary with embedded BERT classification, local discovery, and agent scheduling has no equivalent.

---

## Can Seastar Be Pivoted to a Different Use Case?

**Probably not in a way that justifies the complexity cost.**

Projects that successfully use Seastar are all **data infrastructure that genuinely needs extreme I/O performance**: ScyllaDB (database), Redpanda (streaming), Ceph Crimson (storage). These need millions of ops/sec where shared-nothing, thread-per-core provides an order-of-magnitude advantage.

For AI traffic proxying, the bottleneck is GPU inference (seconds), not routing decisions (microseconds). Ranvier's `< 50μs` radix tree lookup is impressive but irrelevant when users wait 2-10 seconds for model output. A Go proxy with a `100μs` routing decision delivers identical end-to-end latency.

### Alternative Pivots Considered

| Pivot | Why It Doesn't Work |
|-------|-------------------|
| High-perf API gateway | Envoy, NGINX, HAProxy are mature. No market gap. |
| Vector similarity search | Qdrant (Rust), Weaviate (Go), Milvus (C++) exist with large communities. |
| Semantic caching proxy | Being built into vLLM and SGLang directly. |
| SSE/WebSocket multiplexer | Real problem, but Rust or Go handles this fine. |
| Real-time feature store | Redis, DragonflyDB already serve this niche. |
| Token-level stream processor | Extremely niche; hard to build a business around. |

Seastar's strengths apply to **data plane** problems (moving bytes at massive scale). AI infrastructure in 2026 is mostly **control plane** problems (smart decisions, policy enforcement, intent classification). These don't need Seastar and are penalized by its complexity (the 16 Hard Rules exist because Seastar makes simple things hard).

---

## Recommendations (Updated)

### 1. Ranvier Local as a Rust proxy with BERT classification (most promising)

The **local developer experience** remains un-addressed by competitors. vLLM-SR requires Envoy. llm-d requires Kubernetes + vLLM. SGLang's gateway is SGLang-only. None target "developer with Ollama on a laptop."

**What to build:**
- Rust binary with Candle for embedded BERT inference (ModernBERT-base or DistilBERT)
- Adopt Iris's validated signal type taxonomy (domain, keyword, embedding at minimum)
- Auto-discover Ollama, LM Studio, llama.cpp via port scanning + semantic liveness checks
- Route between local (fast, free) and cloud (smart, paid) based on classified intent
- Agent scheduling: manage priority between Cursor, Claude Code, Cline
- Ship as `brew install ranvier`

**What transfers from the C++ codebase:**
- Radix tree prefix matching algorithm (good algorithm regardless of implementation language)
- Intent classification heuristics (as a starting point before BERT takes over)
- Agent scheduling design and priority queue logic
- The tokenizer FFI patterns (Candle uses the same HuggingFace tokenizer ecosystem)
- Operational knowledge: the 16 Hard Rules, anti-patterns, benchmark methodology

**What doesn't transfer (and shouldn't):**
- Seastar reactor model
- Gossip-based clustering
- Cross-shard dispatch complexity

### 2. Ship the C++ codebase as-is

The marginal cost of shipping is near zero. The blog post material exists, the code works, the benchmarks are real. Position honestly: "prefix-aware LLM routing for OpenAI-compatible backends, useful when you're not on vLLM/llm-d." The long tail of non-vLLM deployments (Ollama, llama.cpp, TGI, custom endpoints) won't get llm-d or Iris natively. That's a smaller market, but it's not zero.

Engineering content: "What I learned building a C++/Seastar LLM traffic controller" — the 16 Hard Rules, the gossip protocol, the radix tree, the Seastar FFI patterns. Position as engineering education, not product launch.

### 3. Contribute reusable components upstream

The Adaptive Radix Tree and gossip-based cluster sync are the most reusable components. Contributing to the Seastar ecosystem or packaging as standalone libraries has lasting impact.

### 4. Apply domain expertise elsewhere

Deep knowledge of KV cache dynamics, GPU scheduling, prefix-aware routing, Seastar internals, inference engine architecture, and now the BERT-based classification landscape. Valuable at companies building vLLM, SGLang, Anyscale, Red Hat, etc.

### What NOT to do

- Invest the 13-17 weeks described in VISION.md for the C++ cloud product
- Reimplement vLLM-SR's classification in C++/Seastar (permanent catch-up position)
- Pivot the Seastar codebase to a non-AI use case (too specialized)
- Compete on enterprise compliance/PII (Kong, F5, IBM have distribution advantages)

---

## Conclusion

The previous assessment was correct on the strategic direction, but too dismissive on the classification question. BERT-based intent classification is adoptable — the tokenizer FFI already proves the pattern — and should absolutely be used in any new product. The gap is not "can Ranvier do ML-based classification?" (yes) but "should it compete with vLLM-SR on cloud routing?" (no).

The local developer proxy with embedded BERT classification, auto-discovery, and agent scheduling remains the most defensible product. Build it in Rust with Candle. Ship the C++ codebase as a reference implementation. Use the domain expertise — not necessarily the Seastar code — as the foundation for what comes next.

---

*Assessment date: 2026-02-17*
*Revised: 2026-02-17 — Added vLLM-SR architecture analysis, BERT classification feasibility, corrected engine-agnostic assessment*
*Based on competitive landscape research through February 2026*
