# Ranvier Core

> **Saltatory Conduction for LLM Inference.**
>
> A standalone, Seastar-native router that reduces GPU cache thrashing by routing requests based on **Token Prefixes** rather than connection availability.

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)
[![Standard](https://img.shields.io/badge/C%2B%2B-20-purple.svg)](https://isocpp.org/)
[![Status](https://img.shields.io/badge/Status-Alpha-orange)]()

---

## ⚡ The Problem: "Blind" Routing
Standard load balancers (Nginx, HAProxy) route LLM requests based on *server availability* (Least Connections or Round Robin). They treat LLM requests as generic HTTP packets.

In the era of **KV-Caching**, this is inefficient.
* **Request A** loads a 4,000-token PDF into `GPU-1`.
* **Request B** (asking a question about that PDF) gets routed to `GPU-2` by Round Robin.
* **Result:** `GPU-2` must re-compute the entire 4,000-token prefill. Throughput collapses; latency spikes.

## 🧠 The Solution: Content-Aware Routing
**Ranvier** acts as a "Layer 7+" Load Balancer. It inspects the **semantic content** (token sequence) of the incoming request and routes it to the GPU that already holds the relevant KV Cache.

Just as the **Nodes of Ranvier** allow biological signals to "jump" gaps (Saltatory Conduction) to increase speed, Ranvier allows LLM inference to skip the prefill phase by jumping straight to the cached state.

### Key Architecture
* **Adaptive Radix Tree (ART):** Uses a cache-oblivious Radix Tree to map `TokenPrefix -> GPU_ID`. Lookups are $O(L)$ where $L$ is the prefix length, independent of total keys.
* **Seastar Framework:** Built on a shared-nothing, thread-per-core architecture. No locks, no atomics, massive concurrency.
* **Model Agnostic:** Uses HuggingFace `tokenizer.json` definitions to adapt to any model architecture (Llama 3, Mistral, GPT-4o) dynamically.

---

## 🚀 Performance Goals
* **Zero-Copy Hot Path:** Request bodies are parsed and hashed without unnecessary copying to userspace.
* **Microsecond Overhead:** Routing decisions occur in $< 50\mu s$.
* **Linear Scaling:** Throughput scales linearly with CPU cores due to Seastar's sharding.

---

## 🛠️ Configuration
Ranvier maps generic HTTP endpoints to specific Tokenizer/Model backends.

```yaml
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
```

```mermaid
graph TD
    User["User / Client"] -->|HTTP POST| Router["Ranvier Router"]

    subgraph "Ranvier Core (C++ Seastar)"
        Router -->|Parse| Tokenizer["GPT-2 Tokenizer"]
        Tokenizer -->|Tokens| Radix["Radix Tree"]
        Radix -->|Lookup| Cache{"Known Prefix?"}
        Cache -- Yes --> BackendID
        Cache -- No --> LB["Random Load Balancer"]
        LB --> BackendID
    end

    subgraph "Infrastructure"
        Sidecar["Python Sidecar"] -.->|Watch| DockerDaemon
        Sidecar -.->|Register| Router
    end

    Router == "Keep-Alive Connection" ==> GPU1["GPU 1 (Context A)"]
    Router == "Keep-Alive Connection" ==> GPU2["GPU 2 (Context B)"]

    style Router fill:#f9f,stroke:#333,stroke-width:4px
    style Radix fill:#ccf,stroke:#333
```

---

## 🐳 Deployment

### Docker

Pre-built multi-architecture images (amd64/arm64) are available on GitHub Container Registry:

```bash
# Pull the latest image
docker pull ghcr.io/ranvier-systems/ranvier:latest

# Pull a specific version
docker pull ghcr.io/ranvier-systems/ranvier:1.0.0

# Pull by commit SHA for traceability
docker pull ghcr.io/ranvier-systems/ranvier:sha-abc1234

# Run with required IPC_LOCK capability
docker run --cap-add=IPC_LOCK -p 8080:8080 -p 9180:9180 ghcr.io/ranvier-systems/ranvier:latest
```

Build from source (optional):

```bash
# Build production image locally (standalone, ~20 min)
docker build -f Dockerfile.production -t ranvier:latest .

# Or use the base image strategy for faster rebuilds (~2 min)
docker pull ghcr.io/ranvier-systems/ranvier-base:latest
docker build -f Dockerfile.production.fast -t ranvier:latest .

# Run with required IPC_LOCK capability
docker run --cap-add=IPC_LOCK -p 8080:8080 -p 9180:9180 ranvier:latest
```

---

## 🔧 Development Setup

### Prerequisites
- Docker with BuildKit enabled
- VS Code with Dev Containers extension (recommended)

### Quick Start

1. **Pull the base image** (pre-built from GitHub Container Registry):
   ```bash
   docker pull ghcr.io/ranvier-systems/ranvier-base:latest
   ```
   Or build locally if customizing:
   ```bash
   docker build -f Dockerfile.base -t ghcr.io/ranvier-systems/ranvier-base:latest .
   ```

2. **Open in VS Code:**
   - Open the project folder
   - Press `Ctrl+Shift+P` → "Dev Containers: Reopen in Container"
   - The dev container uses the base image for fast startup

3. **Build Ranvier:**
   ```bash
   mkdir build && cd build
   cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
   ninja
   ```

### Disk Management

After benchmarking or when disk usage grows:
```bash
./scripts/docker-cleanup.sh              # Keep 10GB cache (default)
./scripts/docker-cleanup.sh --keep 5GB   # Custom limit
./scripts/docker-cleanup.sh --aggressive # Remove everything unused
```

### Kubernetes (Helm)

Deploy a 3-node Ranvier cluster with gossip synchronization:

```bash
# Install with default values
helm install ranvier ./deploy/helm/ranvier \
  --namespace ranvier --create-namespace

# Production installation with backend discovery
helm install ranvier ./deploy/helm/ranvier \
  --namespace ranvier --create-namespace \
  --set "auth.apiKeys[0].name=admin" \
  --set "auth.apiKeys[0].key=rnv_prod_$(openssl rand -hex 24)" \
  --set "auth.apiKeys[0].roles={admin}" \
  --set backends.discovery.enabled=true \
  --set backends.discovery.serviceName=vllm-backends \
  --set serviceMonitor.enabled=true
```

See [Kubernetes Deployment Guide](docs/kubernetes.md) for detailed configuration options.

---

## 📖 Documentation

- [Architecture Overview](docs/architecture.md)
- [API Reference](docs/api-reference.md)
- [Request Flow](docs/request-flow.md)
- [Kubernetes Deployment](docs/kubernetes.md)
- [Performance Tuning](docs/performance.md)
- **Internals:**
  - [Gossip Protocol](docs/internals/gossip-protocol.md)
  - [Radix Tree](docs/internals/radix-tree.md)
  - [Prefix Affinity Routing](docs/internals/prefix-affinity-routing.md)
