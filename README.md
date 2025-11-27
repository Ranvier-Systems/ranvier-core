Ranvier Core
Saltatory Conduction for LLM Inference.

A standalone, Seastar-native router that reduces GPU cache thrashing by routing requests based on Token Prefixes rather than connection availability.

⚡ The Problem: "Blind" Routing
Standard load balancers (Nginx, HAProxy) route LLM requests based on server availability (Least Connections or Round Robin). They treat LLM requests as generic HTTP packets.

In the era of KV-Caching, this is inefficient.

Request A loads a 4,000-token PDF into GPU-1.

Request B (asking a question about that PDF) gets routed to GPU-2 by Round Robin.

Result: GPU-2 must re-compute the entire 4,000-token prefill. Throughput collapses; latency spikes.

🧠 The Solution: Content-Aware Routing
Ranvier acts as a "Layer 7+" Load Balancer. It inspects the semantic content (token sequence) of the incoming request and routes it to the GPU that already holds the relevant KV Cache.

Just as the Nodes of Ranvier allow biological signals to "jump" gaps (Saltatory Conduction) to increase speed, Ranvier allows LLM inference to skip the prefill phase by jumping straight to the cached state.

Key Architecture

Adaptive Radix Tree (ART): Uses a cache-oblivious Radix Tree to map TokenPrefix -> GPU_ID. Lookups are O(L) where L is the prefix length, independent of total keys.

Seastar Framework: Built on a shared-nothing, thread-per-core architecture. No locks, no atomics, massive concurrency.

Model Agnostic: Uses HuggingFace tokenizer.json definitions to adapt to any model architecture (Llama 3, Mistral, GPT-4o) dynamically.

🚀 Performance Goals
Zero-Copy Hot Path: Request bodies are parsed and hashed without unnecessary copying to userspace.

Microsecond Overhead: Routing decisions occur in <50μs.

Linear Scaling: Throughput scales linearly with CPU cores due to Seastar's sharding.

🛠️ Configuration
Ranvier maps generic HTTP endpoints to specific Tokenizer/Model backends.

YAML
# config.yaml
routes:
  - path: "/v1/chat/completions"
    model: "meta-llama/Meta-Llama-3-8B"
    backend_pool: "h100-cluster-a"
    # Ranvier uses this to tokenize the raw HTTP body
    tokenizer_config: "./tokenizers/llama-3.json"
    
    # Optimization settings
    min_prefix_length: 64   # Don't route on "Hello", wait for context
    block_alignment: 16     # Align with vLLM PagedAttention blocks
📦 Quick Start (Dev Container)
Ranvier is developed in a Strict Clean Room environment using Docker/Dev Containers to ensure reproducibility on Apple Silicon (M-Series) and Linux x86.

Clone:

Bash
git clone https://github.com/ranvier-systems/ranvier-core.git
cd ranvier-core
Open in VS Code:

Install the "Dev Containers" extension.

Run Dev Containers: Reopen in Container.

Build:

Bash
mkdir build && cd build
cmake .. -G Ninja
ninja
Run Demo:

Bash
./ranvier_server --config ../config.yaml
🔮 Roadmap
v0.1: Basic Prefix Routing (Static Radix Tree).

v0.2: Dynamic "Snooping" (Learn cache state from backend responses).

v0.3: DPDK Support (Kernel Bypass for HFT-grade latency).

v1.0: Production-ready HTTP/2 support and gRPC.

📜 License
Copyright © 2025 Ranvier Systems. Licensed under the Apache License, Version 2.0. See LICENSE for details.
