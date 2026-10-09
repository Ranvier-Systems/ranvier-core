# Ranvier Core

> **Prefix-aware routing for self-hosted LLM fleets, in C++20 on Seastar.** Ranvier sends requests that share a prompt prefix to the backend that already holds it in KV cache, and diverts only when a backend is genuinely queued. On an 8×A100 fleet that raises vLLM's prefix-cache hit rate three to five times, cuts median time-to-first-token by 27–29% on CodeLlama-13B and serves 6–15% more requests; against round-robin it also cuts P99 by 27% on Llama-3.1-8B and 21–57% on CodeLlama-13B, though a live least-loaded router without affinity gets most of that tail reduction on its own. Every number, and the campaign that produced it: [Benchmark Results](#benchmark-results).
>
> *Named for the Nodes of Ranvier—enabling signals to jump gaps, just as Ranvier enables inference to skip redundant computation.*

A high-performance LLM traffic controller that reduces GPU cache thrashing by routing requests based on **Token Prefixes** rather than connection availability.

**Best for:** RAG with shared context, multi-turn chat with large system prompts, few-shot prompts with shared examples; any workload with 2K+ token shared prefixes. **Less benefit for:** short prompts, or a fleet whose KV cache cannot hold its share of the shared prefixes (both arms then run cache-cold).

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-purple.svg)](https://isocpp.org/)
[![Architecture](https://img.shields.io/badge/Architecture-Defined-blue)](docs/architecture/VISION.md)

---

## Quick Start

Run with Docker — no configuration needed:

```bash
docker run --cap-add=IPC_LOCK -p 8080:8080 -p 9180:9180 \
  ghcr.io/ranvier-systems/ranvier:2.2.0
```

`2.2.0` is the latest release (2026-10-06). The `:latest` tag tracks `main`, which carries
unreleased changes; read [CHANGELOG → Unreleased](CHANGELOG.md#unreleased) before using it.
Upgrading from 2.1.0 changes five routing defaults; see
[CHANGELOG → 2.2.0 → Upgrade notes](CHANGELOG.md#220---2026-10-06).

Point your client at `http://localhost:8080` and start sending requests.
For deployment options (Kubernetes, building from source), see [Deployment](#deployment) below.

---

## The Problem: "Blind" Routing

Standard load balancers (Nginx, HAProxy) route LLM requests based on *server availability* (Least Connections or Round Robin). They treat LLM requests as generic HTTP packets.

In the era of **KV-Caching**, this is inefficient.
* **Request A** loads a 4,000-token PDF into `GPU-1`.
* **Request B** (asking a question about that PDF) gets routed to `GPU-2` by Round Robin.
* **Result:** `GPU-2` must re-compute the entire 4,000-token prefill. Throughput collapses; latency spikes.

## The Solution: Content-Aware Routing

**Ranvier** acts as a "Layer 7+" Load Balancer. It inspects the **semantic content** (token sequence) of the incoming request and routes it to the GPU that already holds the relevant KV Cache.

Just as the **Nodes of Ranvier** allow biological signals to "jump" gaps (Saltatory Conduction) to increase speed, Ranvier allows LLM inference to skip the prefill phase by jumping straight to the cached state.

### Key Architecture
* **Adaptive Radix Tree (ART):** Uses a cache-oblivious Radix Tree to map `TokenPrefix -> GPU_ID`. Lookups are $O(L)$ where $L$ is the prefix length, independent of total keys.
* **Seastar Framework:** Built on a shared-nothing, thread-per-core architecture. No locks, no atomics, massive concurrency.
* **Model Agnostic:** Uses HuggingFace `tokenizer.json` definitions to adapt to any model architecture (Llama 3, Mistral, Qwen, Kimi) dynamically.

---

## Performance Characteristics

| Metric | Measured | Notes |
|--------|----------|-------|
| **Radix Tree Lookup** | < 50μs | Pure routing decision, O(L) in prefix length; component micro-benchmark (`make bench-hot-prefix`) |
| **Per-request Routing Decision** | ~0.2 ms P50, ~10 ms P99 | Router-side time including tokenization (the P99 is boundary-detection tokenization); Prometheus histogram estimates from the October 2026 13B fitted suite, the same at 10 and 20 users ([raw compare files](docs/benchmarks/results/2026-10-02-fitted/)) |
| **KV-cache hit rate vs round-robin** | 72% → 96% (8B); 7% → 26% (13B, 50-prefix set); 12% → 70% (13B, fitted set) | vLLM's own prefix-cache counter, token-level, median of three repeats |
| **P99 TTFT vs round-robin** | −27% (8B) · −21% (13B, 20 users) · −39% (13B, 10 users) · −57% (13B, 20 users, fitted set) · −42% and +15% throughput (13B, 30 users, fitted set) | Median of three repeats, both arm orders; every repeat of every row improved. Earlier campaigns regressed the 13B rows; the diagnosis and every intermediate run are in [Benchmark Results](#benchmark-results) |
| **vs a least-loaded router (no affinity)** | P50 −27 to −29% · throughput +6 to +9% · KV hit 3–4× · P99 −15% at 20 users, no reliable change at 30 users; **P99 +23%** where the fleet's cache cannot hold the prefix set (13B, 50-prefix set, 20 users), with P50 −9 to −16% and KV hit 3–5× | The no-affinity control (13B, three repeats each). Least-loaded alone removes most of the P99 tail against round-robin; what affinity adds is the prefill saved on every hit and the capacity it frees. In the eviction regime it loses the tail to a load balancer |

Figures from the superseded February and July 2026 campaigns (such as "58–98% cache hit rate", "~7 ms P50 overhead", "~2–16 ms overhead" and "+29% P99 at low load") are no longer quoted here; see [Benchmark Results](#benchmark-results) for why.

**Design Principles:**
* **Minimized Copying:** Uses `string_view` parsing with single network buffer copy; Radix lookups use `std::span` for zero-copy token access.
* **Shared-Nothing Architecture:** Thread-per-core via Seastar; no locks on the hot path. Each shard maintains its own routing tree.
* **Per-Core Independence:** Each shard owns its routing tree and in-flight counts; the only cross-shard traffic is the batched route flush (every 20 ms) and the in-flight load broadcast (every 100 ms).

---

## Benchmark Results

Four campaigns have been run on 8×A100 hardware. **The October 2026 results are the citable ones:** the current-defaults table for what 2.2.0 does, the October 1 re-baseline for the previous defaults it replaced. The July 2026 campaign was measured before a tooling audit found that its arms were not isolated (routing state carried across arms, different prefixes per arm) and its labels were wrong (event counts as req/s, route consistency as "cache hit"); only its 8B row reproduced on fixed tooling, and its 13B rows (including a +29% P99 regression at 10 users) did not. The February 2026 numbers came from a 5-prefix synthetic workload that the project's own methodology review found manufactures much of the headline win. Neither earlier campaign should be quoted as expected results.

### Current routing defaults (2.2.0, measured October 2026)

Under the previous defaults (next section) CodeLlama-13B regressed at 20 users in every campaign. The cause was the divert policy, not prefix affinity: bounded-load diversion read a 5-second-stale GPU score plus one shard's share of a node's in-flight count, and at ε 0.25 it diverted 25–30% of requests without reaching the tail. The defaults now read the node's live in-flight count, divert only at twice the mean (ε 1.0), and place new prefixes least-loaded with cluster-wide convergence. Same fleet and method as the re-baseline. The first table is the standard 50-prefix matrix (the same four rows as the re-baseline below) re-run at three repeats on the 2.2.0 image; the second is the fitted prefix set (16 prefixes × 2,000–4,000 tokens) from the diagnosis.

| Model, load (50-prefix set) | KV hit rate, round-robin → prefix | P99 TTFT vs round-robin | P50 TTFT | Throughput | Repeats |
|---|---|---|---|---|---|
| Llama-3.1-8B, 20 users | 72–76% → 93–97% | **−26.7%** median (−26.7, −17.7, −27.1) | −1% | +1–2% | 3, both arm orders |
| CodeLlama-13B, 30 users, 30 min (eviction regime) | 5–6% → 17–21% | **−14.1%** median (−14.1, −17.9, −13.1), P99 of completed requests; 1.3–1.8% timeouts in both arms | −18 to −22% | +5–6% | 3, both arm orders |
| CodeLlama-13B, 20 users | 6–8% → 23–29% | **−21.0%** median (−18.4, −27.9, −21.0) | −18 to −24% | +5–7% | 3, both arm orders |
| CodeLlama-13B, 10 users | 6–8% → 26–41% | **−38.8%** median (−38.8, −46.8, −36.8) | −7 to −11% | +1–4% | 3, both arm orders |

| Model, load (fitted set) | KV hit rate, round-robin → prefix | P99 TTFT vs round-robin | P50 TTFT | Throughput | Repeats |
|---|---|---|---|---|---|
| CodeLlama-13B, 10 users | 16–19% → 76–82% | **−28.6%** median (−28.6, −34.2, −23.6) | −29% | +7–9% | 3, both arm orders |
| CodeLlama-13B, 20 users | 12–15% → **67–72%** | **−56.6%** median (−56.6, −61.5, −55.8); the Oct 5 run on another instance gave −57.5% (−60.4, −57.5, −55.2) | −28% | +10–12% | 3 + 3, both arm orders |
| CodeLlama-13B, 30 users, 30 min | 10–11% → 46–50% | **−42.0%** median (−43.1, −42.0, −34.1) | −29% | **+14–15%** | 3, both arm orders |
| CodeLlama-13B, 20 users, hash placement (isolation) | 10–14% → 49–55% | −51.8% median (−48.4, −51.8, −53.5) | −28% | | 3 |

Zero incomplete requests in every fitted-set arm, so those P99 figures carry no timeout qualification. P50 is −29% at every load (the prefill a cache hit saves); P99 grows with the queue the divert policy removes, and throughput grows with saturation.

Twelve of twelve repeats of the standard matrix improved P99; the two rows that regressed under the previous defaults (13B at 20 and 10 users) improved by 21% and 39%. The 30-user row is the one place the prefix arm leaves slightly more requests incomplete (0.1–0.4 points), so its P50 and throughput are the robust figures and its P99 is qualified.

**Against a least-loaded router, not round-robin (October 8).** Round-robin is the weakest baseline there is, so the fitted 20- and 30-user rows were re-run with the baseline arm in Ranvier's own `least_loaded` mode: lowest live in-flight count, no affinity. That arm alone cuts P99 against round-robin by about 50% at 20 users and 41% at 30, with a nearly flat request distribution (Gini below 0.01), and it does nothing for P50. Prefix routing on top of it: P50 −27 to −29% and throughput +6 to +9% at both loads, KV hit rate 3–4× higher, P99 a further −14.6% (−14.6, −13.8, −20.8) at 20 users and no reliable change at 30 (−6.5, +6.6, +11.3), where the prefix arm's concentration (Gini 0.08–0.11) is itself the tail. So most of the P99 win over round-robin is load balancing; what affinity adds is the prefill a cache hit saves and the capacity that frees.

**A tighter divert cap does not recover the saturation tail (October 9).** The same 30-user row with the prefix arm diverting at 1.5× the mean (`bounded_load_epsilon` 0.5) instead of 2× gave −7.3, +6.0, +1.3% P99 against least-loaded, still mixed, with KV hits down to 42–44%; at 1.25× (ε 0.25, the pre-2.2.0 default) it was +11.1, +4.8, +25.4%, a consistent regression, with KV 32–37%. Each tightening diverts more, spreads flatter, gives back KV and P50, and moves the tail the wrong way: a diverted hit arrives on the least-loaded backend as a miss, and at saturation every backend is already within a request or two of the mean. The default stays 2×. **In the eviction regime prefix routing loses the tail to a load balancer.** On the 50-prefix set at 20 users, with vLLM's cache reset before each arm, prefix routing is +22.9, +24.9, +21.7% P99 against least-loaded (3.3–3.4 s → 4.1–4.2 s of completed requests) while keeping P50 −9 to −16%, the KV hit rate 3–5× (7% → 23–32%), the same throughput and fewer timeouts (1.5–1.7% vs 1.8–2.1%); against round-robin the same row is −21%. Where the fleet's cache cannot hold the shared prefixes, the hits queue behind each other on the few backends that hold one, and that queue is the tail.

The request distribution across backends stays uneven under prefix affinity (Gini 0.09–0.12 vs 0.02–0.03 for round-robin); the tail was queue depth, not request count, and a divert policy that sees the queue live removes it without giving back affinity (route consistency 50–53%). Full record, counters and every intermediate leg: [benchmark-results-current.md](docs/benchmarks/benchmark-results-current.md).

### Previous routing defaults: October 1 2026 re-baseline

Representative workload: 50 shared prefixes of 2,000–8,000 tokens; prefix-aware routing against round-robin on the same fleet; three repeats per configuration, alternating arm order; every repeat must agree for a verdict. KV hit rate is vLLM's own prefix-cache counter. Write-up with per-repeat numbers: [benchmark-results-current.md](docs/benchmarks/benchmark-results-current.md).

| Model, load | KV hit rate, round-robin → prefix | P99 TTFT vs round-robin | Verdict |
|---|---|---|---|
| Llama-3.1-8B, 20 users | 72% → 94% | **−17%** (all three repeats −6.5% or better) | consistent improvement |
| CodeLlama-13B, 30 users | 5% → 14% | −2.4%, −1.6%, +3.6% | no reliable effect |
| CodeLlama-13B, 20 users | 4% → 19% | **+11%** | consistent regression |
| CodeLlama-13B, 10 users | 6% → 26% | +12%, −11%, +6% | no reliable effect |

The deciding variable is whether a backend's KV cache can hold its share of the hot prefix set. On a 40 GB A100 an 8B backend holds about 142,000 tokens, so the 250,000-token set fits when split eight ways but not in one place: round-robin is already mostly cache-warm, affinity recovers the rest, and under a 20-user queue that is a sixth off the tail. A 13B backend holds about 11,600 tokens, less than its share of the set and, at 30 users, less than the in-flight requests alone; both arms run nearly cache-cold under constant preemption, and the routing policy's effect is small and unstable. The 13B regression at 20 users is consistent across repeats.

A follow-up "fitted" suite (October 2026) shrank the prefix set to 16 prefixes that fit the 13B cache. At 10 users prefix routing then improved P99 by 5.8% (median, all three repeats agreeing) with KV hits rising from 17% to 65%. At 20 users P99 still regressed by about 10% while P50 improved about 25%. With all load diversion switched off the regression stayed, which places the cause in prefix placement: hashing 16 prefixes onto 8 backends gave one backend four prefixes and another none, and the busiest backend's queue sets the tail. Least-loaded placement of new prefixes became part of the fix and is the default since 2.2.0; the rest of the diagnosis is in the current-defaults section above, and every run is in [benchmark-results-current.md](docs/benchmarks/benchmark-results-current.md).

**What this does and does not show.** Prefix routing reliably improves tail latency for prefix-heavy traffic on a busy fleet whose caches can hold the hot set, and under the current defaults (above) it did so on every repeat of every configuration measured, including the 13B rows that regressed here. Affinity does concentrate requests unevenly across backends (the per-backend request Gini is 3–5× round-robin's in every prefix arm); what decides the tail is whether the divert policy sees the resulting queue live and acts only on a real one. The eviction regime (13B at 30 users, 50 prefixes) is still measured with timeouts in both arms, so its number is directional; the same load on the fitted set completes every request and improves P99 by 42%. It has not been compared head-to-head with other prefix-aware routers; against a least-loaded policy without affinity, see the current-defaults section above (prefix keeps P50, KV and throughput at every load, a further −15% P99 at moderate load, nothing reliable at saturation, and +23% P99 in the eviction regime).

### February 2026 campaign (superseded)

5-prefix workload, 30-minute runs. Earlier release notes and posts cite these figures. Treat them as an upper bound from a favourable synthetic case, not as expected results.

| Model | Cache Hit Rate | TTFT Improvement | P99 Latency | Throughput |
|-------|----------------|------------------|-------------|------------|
| Llama-3.1-70B | 25% → 98% | 44% faster | ~same | ~same |
| CodeLlama-13b | 12% → 58-98% | 33% faster | -60% to -85% | +4% to +22% |
| Llama-3.1-8B | 12% → 68-98% | 40% faster | flat | ~same |

<sub>Hardware: 70B on 80GB A100s (TP=2, 4 backends); 13B/8B on 40GB A100s (8 backends).</sub>

See the [Benchmark Guide](docs/benchmarks/benchmark-guide-8xA100.md) for methodology and [Benchmark Reproduction](docs/guides/benchmark-reproduction.md) to run it yourself.

---

## Architecture & Capabilities

**Shipped in 2.2.0 (2026-10-06):**
- Routing defaults that read the node's live in-flight count, divert only at twice the mean, and place new prefixes least-loaded with cluster-wide convergence; measured in [Benchmark Results](#benchmark-results)
- A unified weighted route scorer replacing the sequential override chain, with per-signal weights
- Gateway API Inference Extension Endpoint-Picker mode (build-gated, off by default), with an integration test and an overhead microbenchmark
- Native vLLM KV-event subscriber: verified residency, route materialization and replay (opt-in, not yet exercised on GPU hardware)
- Disaggregated prefill/decode pool roles (opt-in, not yet exercised on GPU hardware)
- Embeddability seams: request-admission policy, usage-ledger sink, response-side usage accounting, OpenTelemetry GenAI semantic conventions
- Kimi (Moonshot) chat-template support with a tokenizer-parity harness (not yet exercised on GPU hardware)

**Shipped in 2.1.0 (2026-04-11):**
- Token-prefix routing via an Adaptive Radix Tree, with consistent-hash and random fallbacks, and passive route learning from backend responses
- Partial tokenization for routing: a byte-budgeted prefix is tokenized for the routing decision and full tokenization is deferred until it is needed
- Request intent classification (autocomplete, edit, chat), priority tiers, and a priority queue with fair per-agent scheduling
- vLLM metrics ingestion, GPU-aware load routing, and per-backend cost budgets
- Backend health checks with a circuit breaker; multi-node clustering over DTLS-encrypted gossip with cache-residency tracking
- Ranvier Local: discovery of local backends such as Ollama and LM Studio
- Kubernetes EndpointSlice discovery and a Helm chart

**Measurement status of 2.2.0:** the routing defaults, placement and scorer were measured on 8×A100 in October 2026 (three repeats for 13B/20 users, one confirmation repeat for the other rows). The opt-in features (Endpoint Picker mode, the KV-event subscriber, pool roles, Kimi templates) were off in those campaigns and have not been exercised on GPU hardware; they ship as experimental. See [CHANGELOG → 2.2.0 → Measurement status](CHANGELOG.md#220---2026-10-06).

The roadmap that produced 2.0.0 is in [VISION.md](docs/architecture/VISION.md).

---

## ⚠️ Backend Requirement: Prefix Caching

Ranvier routes requests to the backend that *should* have the relevant KV cache — but the backend must actually have prefix caching enabled for this to help. Without backend-side caching, Ranvier's routing decisions have no cache to hit.

For **vLLM**, enable Automatic Prefix Caching (APC):
```bash
# vLLM ≥0.4.0
python -m vllm.entrypoints.openai.api_server --enable-prefix-caching ...
```

Other backends with prefix/KV cache reuse (SGLang RadixAttention, TensorRT-LLM, etc.) also benefit. The key requirement is that the backend caches KV state for previously-seen token prefixes so that repeated prefixes skip the prefill phase.

---

## Configuration

A minimal config: where to listen, which tokenizer matches the served model, how to route, and the backends. Static backends as below, or Kubernetes EndpointSlice discovery instead (see [Deployment](#deployment)). Every key and its default is in `src/config_schema.hpp`; every key can also be set by a `RANVIER_*` environment variable.

```yaml
# config.yaml
server:
  api_port: 8080
  metrics_port: 9180

assets:
  tokenizer_path: ./assets/llama-3.json   # the served model's HuggingFace tokenizer.json

routing:
  routing_mode: prefix          # prefix | hash | random | least_loaded
  prefix_token_length: 128      # routing-key depth when no system-message boundary is found
  block_alignment: 16           # vLLM PagedAttention block size
  min_token_length: 32          # do not learn routes for very short prompts

backends:
  - { id: 1, host: 10.0.0.11, port: 8000 }
  - { id: 2, host: 10.0.0.12, port: 8000 }
```

```mermaid
graph TD
    User["User / Client"] -->|HTTP POST| Router["Ranvier Router"]

    subgraph "Ranvier Core (C++ Seastar)"
        Router -->|Parse| Tokenizer["Tokenizer"]
        Tokenizer -->|Tokens| Radix["Radix Tree (ART)"]
        Radix -->|Lookup| Cache{"Known Prefix?"}
        Cache -- Yes --> Route["Route to Cached Backend"]
        Cache -- No --> Place["Least-loaded placement (hash fallback)"]
        Place --> Route
        Route -->|Learn| Radix
    end

    subgraph "Backend Discovery"
        K8s["K8s EndpointSlice"] -.->|Watch| Router
        Config["YAML Config"] -.->|Load| Router
    end

    Route == "Keep-Alive" ==> GPU1["GPU 1 (vLLM)"]
    Route == "Keep-Alive" ==> GPU2["GPU 2 (vLLM)"]

    style Router fill:#dbeafe,stroke:#2563eb,stroke-width:2px
    style Radix fill:#d1fae5,stroke:#059669,stroke-width:2px
```

---

## Deployment

### Docker

Pre-built images are available on GitHub Container Registry (linux/amd64, linux/arm64):

```bash
# Pull the latest release
docker pull ghcr.io/ranvier-systems/ranvier:2.2.0

# Pull whatever is on main (unreleased; see CHANGELOG → Unreleased)
docker pull ghcr.io/ranvier-systems/ranvier:latest

# Pull by commit SHA for traceability
docker pull ghcr.io/ranvier-systems/ranvier:sha-abc1234

# Run with required IPC_LOCK capability
docker run --cap-add=IPC_LOCK -p 8080:8080 -p 9180:9180 ghcr.io/ranvier-systems/ranvier:2.2.0
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

## Development Setup

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

See [Kubernetes Deployment Guide](docs/deployment/kubernetes.md) for detailed configuration options.

---

## Project status

- **Latest release:** 2.2.0 (2026-10-06), on the [Releases page](https://github.com/Ranvier-Systems/ranvier-core/releases) and in [CHANGELOG.md](CHANGELOG.md). `main` carries unreleased work.
- **Maintainer:** one, part-time. Issues and pull requests are welcome; expect a response within a week rather than a day. See [CONTRIBUTING.md](CONTRIBUTING.md).
- **Security:** see [SECURITY.md](SECURITY.md) for how to report a vulnerability and for the known hardening gaps. In short: backend connections are not yet encrypted, so terminate TLS in front of Ranvier, and do not expose the metrics/admin port publicly.
- **Provenance:** a large share of this codebase was written with AI coding agents working under the maintainer's direction and review. Every change goes through the same CI (unit tests, sanitizers, fuzzers) and the Seastar rules in `.dev-context/claude-context.md`, and contributions are held to the same bar whether or not an agent helped write them.

---

## Documentation

### Guides
- [Getting Started with Ranvier Local](docs/guides/getting-started-local.md)
- [Cloud Deployment Guide](docs/guides/cloud-deployment.md)
- [IDE Integration (Cursor, Claude Code, Cline, Aider)](docs/guides/ide-integration.md)
- [Benchmark Reproduction](docs/guides/benchmark-reproduction.md)

### Reference
- [Architecture & Vision](docs/architecture/VISION.md)
- [Architecture Overview](docs/architecture/system-design.md)
- [API Reference](docs/api/reference.md)
- [Request Flow](docs/request-flow.md)
- [Benchmark Results (8x A100)](docs/benchmarks/benchmark-guide-8xA100.md)
- [Kubernetes Deployment](docs/deployment/kubernetes.md)
- [Performance Tuning](docs/deployment/performance.md)
- **Internals:**
  - [Gossip Protocol](docs/internals/gossip-protocol.md)
  - [Radix Tree](docs/internals/radix-tree.md)
  - [Prefix Affinity Routing](docs/internals/prefix-affinity-routing.md)
  - [Per-API-Key Attribution](docs/internals/per-api-key-attribution.md)
- [Changelog](CHANGELOG.md)

---

Ranvier is a project of Minds Aspire, LLC.
