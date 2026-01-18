# Ranvier Core Integration Tests

This directory contains multi-node integration tests for validating Ranvier's distributed behavior.

## Overview

The integration tests use Docker Compose to spin up a 3-node Ranvier cluster with 2 mock backends, then validate:

1. **Cluster Formation**: All nodes discover and connect to their peers
2. **Backend Registration**: Backends can be registered via the admin API
3. **Route Learning**: Routes are learned when requests are processed
4. **Gossip Propagation**: Learned routes are propagated to other nodes via UDP gossip
5. **Peer Failure Detection**: When a node goes down, others detect it and prune routes
6. **Cluster Recovery**: When a node rejoins, the cluster recovers

## Architecture

```
                    ┌─────────────────────────────────────────────────┐
                    │           Docker Network (172.28.0.0/16)        │
                    │                                                  │
  ┌─────────────────┼──────────────────────────────────────────────────┼──────────┐
  │                 │                                                  │          │
  │   ┌─────────────▼─────────────┐    ┌────────────────────────────┐  │          │
  │   │       Mock Backends       │    │      Ranvier Cluster       │  │          │
  │   │                           │    │                            │  │          │
  │   │  ┌─────────┐ ┌─────────┐  │    │ ┌────────┐ ┌────────┐     │  │          │
  │   │  │Backend 1│ │Backend 2│  │    │ │ Node 1 │◄─►│ Node 2 │    │  │          │
  │   │  │:21434   │ │:21435   │  │    │ │ :8081  │ │ :8082  │     │  │          │
  │   │  │         │ │         │  │    │ └────┬───┘ └───┬────┘     │  │          │
  │   │  └─────────┘ └─────────┘  │    │      │  Gossip │          │  │          │
  │   │      172.28.1.10/11       │    │      │   UDP   │          │  │          │
  │   │                           │    │      │  :9190  │          │  │          │
  │   └───────────────────────────┘    │      └────┬────┘          │  │          │
  │                                    │           │               │  │          │
  │                                    │      ┌────▼────┐          │  │          │
  │                                    │      │ Node 3  │          │  │          │
  │                                    │      │ :8083   │          │  │          │
  │                                    │      └─────────┘          │  │          │
  │                                    │       172.28.2.1-3        │  │          │
  │                                    └────────────────────────────┘  │          │
  │                                                                    │          │
  └────────────────────────────────────────────────────────────────────┘          │
                                                                                   │
      External Ports:                                                              │
      - 8081-8083: Ranvier API                                                    │
      - 9181-9183: Prometheus Metrics                                             │
      - 21434-21435: Mock Backends                                                │
```

## Prerequisites

- Docker and Docker Compose
- Python 3.8+
- `requests` library (`pip install requests`)

## Running Tests

### Quick Start

```bash
# Run all integration tests
make test-integration

# Or directly with Python
python3 tests/integration/test_cluster.py
```

### Build Optimization

The test script automatically skips building if Docker images already exist:

```bash
# First run builds images (slow)
make test-integration

# Subsequent runs skip build (fast)
make test-integration

# Force rebuild
SKIP_BUILD=0 make test-integration

# Pre-build images only
make docker-build
```

### Manual Testing

For debugging or manual testing, you can start the cluster separately:

```bash
# Start the cluster
make integration-up

# View logs
make integration-logs

# Run tests manually
python3 tests/integration/test_cluster.py

# Clean up
make integration-down

# View live logs from cluster
docker-compose -f docker-compose.test.yml -p ranvier-integration-test logs -f

# Run a specific test
python3 -m pytest tests/integration/test_cluster.py::ClusterIntegrationTest::test_03_send_request_to_learn_route -v
```

### Example: Manual API Testing

Once the cluster is up, you can test manually:

```bash
# Register a backend on Node 1
curl -X POST "http://localhost:8081/admin/backends?id=1&ip=172.28.1.10&port=8000"

# Send a chat completion request
curl -X POST http://localhost:8081/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model": "test", "messages": [{"role": "user", "content": "Hello world"}]}'

# Check metrics on Node 2
curl http://localhost:9182/metrics | grep cluster_peers_alive

# Check gossip sync metrics
curl http://localhost:9182/metrics | grep router_cluster_sync
```

## Test Cases

| Test | Description |
|------|-------------|
| `test_01_cluster_peers_connected` | Verifies all nodes see 2 peers each |
| `test_02_register_backend_on_node1` | Registers mock backends on all nodes via admin API |
| `test_03_send_request_to_learn_route` | Sends request to learn a route prefix |
| `test_04_verify_route_propagation` | Verifies cluster health after route learning |
| `test_05_request_on_other_nodes` | Verifies other nodes can route requests |
| `test_06_stop_node_and_verify_peer_count` | Stops a node, checks peer count drops |
| `test_07_restart_node_and_verify_recovery` | Recreates node, verifies cluster recovery |

## Configuration

Test node configurations are in `tests/integration/configs/`:

- `node1.yaml` - Node 1 config (peers: node2, node3)
- `node2.yaml` - Node 2 config (peers: node1, node3)
- `node3.yaml` - Node 3 config (peers: node1, node2)

Key settings for testing:
- `gossip_heartbeat_interval_seconds: 2` - Faster heartbeats
- `gossip_peer_timeout_seconds: 6` - Quick failure detection
- `min_token_length: 2` - Learn routes with short prompts

## Mock Backend

The mock backend (`mock_backend.py`) simulates a vLLM-compatible endpoint:

- Handles `POST /v1/chat/completions`
- Returns streaming SSE responses
- Echoes the backend ID in responses
- Logs all requests for debugging

## Troubleshooting

### Tests fail with "No backends registered"

The mock backends might not have started. Check:
```bash
docker-compose -f docker-compose.test.yml -p ranvier-integration-test logs backend1 backend2
```

### Nodes don't see peers

Check gossip connectivity:
```bash
# Check if UDP port 9190 is accessible between containers
docker exec ranvier1 nc -zvu 172.28.2.2 9190
```

### Build fails

Ensure the production Dockerfile can build:
```bash
docker build -f Dockerfile.production -t ranvier:test .
```

## Files

```
tests/integration/
├── README.md                    # This file
├── test_cluster.py              # Main integration test script
├── mock_backend.py              # Mock vLLM backend
├── Dockerfile.mock-backend      # Dockerfile for mock backend
├── locustfile.py                # Locust load testing scenarios
├── Dockerfile.locust            # Dockerfile for Locust service
├── results_parser.py            # Unified parser for benchmark results and comparison
└── configs/
    ├── node1.yaml               # Node 1 configuration
    ├── node2.yaml               # Node 2 configuration
    └── node3.yaml               # Node 3 configuration
```

---

## Load Testing / Benchmarking

In addition to functional integration tests, this directory contains a Locust-based load testing framework for performance benchmarking.

### Overview

The load testing framework:
- Uses [Locust](https://locust.io/) to simulate concurrent users
- Measures **TTFT (Time To First Token)** - time from request start to first SSE chunk
- Validates P99 latency thresholds and cluster sync errors
- Supports A/B comparisons between configurations

### Quick Start

```bash
# Run a 5-minute benchmark (default settings)
make benchmark

# Run with a descriptive label (creates timestamped files)
make benchmark BENCHMARK_LABEL=baseline

# Skip rebuilding images for faster iteration
make benchmark BENCHMARK_BUILD=0 BENCHMARK_LABEL=quick_test

# Run with token forwarding enabled for A/B comparison
make benchmark BENCHMARK_LABEL=token_off BENCHMARK_TOKEN_FORWARDING=0
make benchmark BENCHMARK_LABEL=token_on BENCHMARK_TOKEN_FORWARDING=1 BENCHMARK_BUILD=0
```

### Configuration Options

| Variable | Default | Description |
|----------|---------|-------------|
| `BENCHMARK_USERS` | 10 | Number of concurrent simulated users |
| `BENCHMARK_SPAWN_RATE` | 2 | Users spawned per second |
| `BENCHMARK_DURATION` | 5m | Test duration (e.g., `1m`, `5m`, `1h`) |
| `BENCHMARK_LABEL` | _(empty)_ | Label prefix for output files |
| `BENCHMARK_BUILD` | 1 | Set to `0` to skip rebuilding Docker images |
| `BENCHMARK_TOKEN_FORWARDING` | 0 | Set to `1` to enable token forwarding |
| `P99_LATENCY_THRESHOLD_MS` | 100 | P99 TTFT threshold in milliseconds |
| `BENCHMARK_REPORT_DIR` | benchmark-reports | Output directory for results |

### Output Files

Each benchmark run creates timestamped files in `benchmark-reports/`:

```
benchmark-reports/
├── baseline_20251227_123456_output.log   # Raw Locust output
├── baseline_20251227_123456_stats.csv    # Parsed statistics
├── token_on_20251227_124500_output.log
├── token_on_20251227_124500_stats.csv
└── ...
```

### Comparing Results

Use the comparison script to analyze A/B test results:

```bash
python3 tests/integration/results_parser.py compare \
  benchmark-reports/token_off_20251227_123456/benchmark.log \
  benchmark-reports/token_on_20251227_124500/benchmark.log
```

Example output:
```
================================================================================
BENCHMARK COMPARISON
================================================================================
Baseline: benchmark-reports/token_off_20251227_123456_stats.csv
New:      benchmark-reports/token_on_20251227_124500_stats.csv

Metric                        Baseline          New                         Change
--------------------------------------------------------------------------------
P50 TTFT (ms)                    55.00        55.00             0.00 (0.0%) ~ SAME
P99 TTFT (ms)                    66.00        61.00         -5.00 (-7.6%) ✓ BETTER
Requests/sec                      6.76         7.29         +0.53 (+7.8%) ✓ BETTER
...

🎉 P99 TTFT improved by 7.6%
================================================================================
```

### Validation Criteria

The benchmark **fails** if:
- P99 TTFT exceeds the threshold (`P99_LATENCY_THRESHOLD_MS`, default 100ms)
- Any `router_cluster_sync_errors` occur during the run

### Interactive Mode

For debugging or exploratory testing, start the cluster with Locust's web UI:

```bash
# Start cluster with Locust web UI
make benchmark-up

# Open http://localhost:8089 in your browser
# Configure users, spawn rate, and start the test

# Stop when done
make benchmark-down
```

### What is TTFT?

**Time To First Token (TTFT)** measures the latency from when a client sends a request until it receives the first SSE `data:` chunk from the streaming response. This is a critical metric for LLM applications because:

- It determines how quickly users see the first response
- It includes router overhead, network latency, and backend startup time
- Lower TTFT = more responsive user experience

### Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    Docker Network (172.28.0.0/16)               │
│                                                                  │
│  ┌─────────────┐    ┌─────────────────────────────────────────┐ │
│  │   Locust    │    │           Ranvier Cluster               │ │
│  │  (Load Gen) │    │                                         │ │
│  │             │───►│  ┌────────┐ ┌────────┐ ┌────────┐      │ │
│  │ Simulates   │    │  │ Node 1 │ │ Node 2 │ │ Node 3 │      │ │
│  │ N users     │    │  └───┬────┘ └───┬────┘ └───┬────┘      │ │
│  └─────────────┘    │      │          │          │            │ │
│                      │      └──────────┼──────────┘            │ │
│                      │                 ▼                       │ │
│                      │  ┌──────────────────────────────────┐  │ │
│                      │  │         Mock Backends            │  │ │
│                      │  │   Backend 1      Backend 2       │  │ │
│                      │  └──────────────────────────────────┘  │ │
│                      └─────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### Performance Notes

The benchmark runs in a Docker environment, so absolute numbers will differ from production. Use it for:

- **Relative comparisons**: "Did this change improve or regress performance?"
- **Regression detection**: CI can fail if P99 exceeds threshold
- **A/B testing**: Compare configurations (e.g., token forwarding on/off)

For production performance testing, use dedicated infrastructure with:
- Native Linux (not Docker Desktop)
- Adequate CPU/memory resources
- Real vLLM backends instead of mock

---

## Real vLLM Backend Benchmarking

While mock benchmarks validate router overhead, **real vLLM benchmarks validate the core value proposition**: does prefix-aware routing actually improve performance by leveraging KV cache locality?

### Why Real Benchmarks Matter

Mock backends respond instantly, which is useful for measuring router latency but doesn't capture:
- **KV Cache Hits**: When a request hits a warm cache, TTFT is significantly faster
- **Prefix Locality**: Whether the router correctly routes similar prompts to the same backend
- **Real Token Generation**: Actual throughput in tokens per second
- **Scaling Effects**: How benefits change with request rate and prefix diversity

### Prerequisites

**Option 1: External vLLM Endpoints** (recommended for production testing)
- Two or more vLLM servers with `--enable-prefix-caching`
- Network connectivity between Docker and vLLM servers
- Same model loaded on all backends

**Option 2: Local vLLM** (for development/testing)
- NVIDIA GPU with 16GB+ VRAM
- NVIDIA Container Toolkit installed
- Docker with GPU support

### Quick Start

```bash
# Using the helper script (recommended)
./scripts/run-multi-gpu-benchmark.sh 129.213.118.109 123.45.67.89

# Single-GPU sanity check (recommended starting point)
HF_TOKEN=your_token make benchmark-single-gpu

# With external vLLM endpoints (2+ GPUs)
VLLM_ENDPOINT_1=http://gpu-server1:8000 \
VLLM_ENDPOINT_2=http://gpu-server2:8000 \
make benchmark-real

# With local vLLM (requires 2+ GPUs)
make benchmark-real-local

# A/B comparison: prefix-aware vs round-robin (requires 2+ GPUs)
VLLM_ENDPOINT_1=http://gpu1:8000 \
VLLM_ENDPOINT_2=http://gpu2:8000 \
make benchmark-comparison
```

### Known Limitations (Resolved)

**Multi-shard mode (FIXED)**: Previous segfault issues with `--smp 2` mode have been
resolved in PR #58. The fixes include:
- HttpController sharding with `sharded<HttpController>`
- Router `initialize_shards()` for cross-shard data access
- Gossip callback copy semantics to prevent cross-shard memory access
- MetricsService initialization on all shards

The benchmark infrastructure now runs with `--smp 2` by default to leverage
multi-core performance.

**Cache hit tracking**: Accurate cache hit tracking requires Ranvier to inject an
`X-Backend-ID` header into responses. Until this is implemented:
- Single-backend mode: Cache hits are tracked correctly (all requests go to backend 1)
- Multi-backend mode: Cache hits are approximated based on request distribution

**Single-GPU testing**: With only 1 GPU, you can run sanity checks but cannot
perform A/B comparisons between routing strategies (prefix-aware vs round-robin).
Use `make benchmark-single-gpu` for basic validation.

### Configuration

| Variable | Default | Description |
|----------|---------|-------------|
| `VLLM_ENDPOINT_1` | - | URL of first vLLM backend (required for external) |
| `VLLM_ENDPOINT_2` | - | URL of second vLLM backend (required for external) |
| `NUM_BACKENDS` | 2 | Number of vLLM backends |
| `BACKEND_BASE_IP` | - | Base IP for sequential ports (e.g., `172.17.0.1`) |
| `BACKEND_PORT_START` | 8000 | Starting port when using `BACKEND_BASE_IP` |
| `SKIP_BACKEND_REGISTRATION` | false | Skip auto-registration if backends already registered |
| `NUM_RANVIER_NODES` | 3 | Number of Ranvier router nodes |
| `RANVIER_BASE_IP` | - | Base IP for sequential ports (e.g., `localhost`) |
| `RANVIER_PORT_START` | 8080 | Starting port when using `RANVIER_BASE_IP` |
| `RANVIER_METRICS_PORT_START` | 9180 | Starting metrics port when using `RANVIER_BASE_IP` |
| `BENCHMARK_REAL_DURATION` | 5m | Test duration |
| `BENCHMARK_REAL_USERS` | 10 | Concurrent users |
| `BENCHMARK_REAL_SPAWN_RATE` | 2 | Users spawned per second |
| `RANVIER_ROUTING_MODE` | prefix | Routing mode: `prefix`, `hash`, or `random` |
| `PROMPT_DISTRIBUTION` | mixed | Prompt lengths: `short`, `medium`, `long`, `mixed` |
| `SHARED_PREFIX_RATIO` | 0.7 | Ratio of requests sharing a prefix (0.0-1.0) |
| `VLLM_MODEL` | Llama-3.2-1B | Model for local vLLM |
| `P99_LATENCY_THRESHOLD_MS` | 5000 | P99 TTFT threshold in ms |

### Prompt Distributions

The benchmark supports different prompt distributions to simulate various workload patterns:

**Short Prompts (< 100 tokens)**
- Simple questions, quick tasks
- Low prefix sharing opportunity
- Example: "Write a Python function to calculate factorial"

**Medium Prompts (100-500 tokens)**
- Include shared system prompts
- Moderate prefix sharing
- Example: Common system prompt + varying user questions

**Long Prompts (500+ tokens)**
- Detailed context with shared prefixes
- High prefix sharing opportunity
- Example: Microservices architecture prompt with different follow-ups

```bash
# Test with only long prompts (maximum cache benefit)
PROMPT_DISTRIBUTION=long make benchmark-real

# Test with high prefix sharing
SHARED_PREFIX_RATIO=0.9 make benchmark-real

# Test worst case (no shared prefixes)
SHARED_PREFIX_RATIO=0 make benchmark-real
```

### Metrics Collected

| Metric | Description |
|--------|-------------|
| **Cache Hit Rate** | Percentage of requests routed to backend with warm KV cache |
| **TTFT (Cache Hit)** | Time to first token when hitting warm cache |
| **TTFT (Cache Miss)** | Time to first token when cache is cold |
| **TTFT Improvement** | Percentage reduction in TTFT from cache hits |
| **Tokens/Second** | Token generation throughput |
| **Unique Prefixes** | Number of distinct prefix groups identified |

### A/B Comparison Workflow

The comparison script runs the same workload twice:

1. **Round-Robin Baseline**: Requests distributed evenly across backends (no prefix awareness)
2. **Prefix-Aware**: Requests with similar prefixes routed to the same backend

```bash
# Run full comparison
make benchmark-comparison

# With custom settings
BENCHMARK_REAL_DURATION=10m \
BENCHMARK_REAL_USERS=20 \
make benchmark-comparison
```

Output:
```
================================================================================
BENCHMARK COMPARISON: Prefix-Aware Routing vs Round-Robin
================================================================================

Metric                         Round-Robin     Prefix-Aware         Improvement
--------------------------------------------------------------------------------
P50 TTFT (ms)                      850.00           420.00       +50.6% faster
P99 TTFT (ms)                     1200.00           580.00       +51.7% faster
Cache Hit Rate (%)                   0.00            68.50              +68.5%
Tokens/Second                       45.20            89.30       +97.6% better
--------------------------------------------------------------------------------

RESULT: Prefix-aware routing improved P99 TTFT by 51.7%
================================================================================
```

### Interpreting Results

**Cache Hit Rate**
- Higher is better (target: >50% with `SHARED_PREFIX_RATIO=0.7`)
- Low rates may indicate prompts aren't sharing prefixes effectively
- Affected by prompt distribution and backend count

**TTFT Improvement**
- Measures latency reduction from cache hits
- Expected: 30-60% improvement for LLMs with prefix caching
- Varies by model and prompt length

**When is Prefix Routing Worth It?**

The benefit becomes significant when:
1. **Cache hit rate > 30%**: Enough requests share prefixes
2. **Request rate > 5 req/s**: Sufficient load to amortize routing overhead
3. **Prompt length > 200 tokens**: Longer prefixes = more cache benefit

### Hardware Requirements for Reproducible Benchmarks

**Minimum (Development Testing)**
- 1x NVIDIA GPU with 16GB VRAM (e.g., RTX 4080)
- 32GB system RAM
- 8+ CPU cores

**Recommended (Production Benchmarking)**
- 2x NVIDIA GPUs with 24GB+ VRAM (e.g., A10, RTX 3090)
- 64GB system RAM
- 16+ CPU cores
- Dedicated network between components

**Enterprise Setup**
- Multiple GPU servers with A100/H100
- 10Gbps+ networking
- Dedicated load generator machine

### Setting Up External GPU Instances (Lambda Labs)

For multi-GPU benchmarking with external cloud instances (e.g., Lambda Labs):

**Step 1: Launch GPU Instance**

1. Create a Lambda Labs account and launch a GPU instance (1x A10 or better)
2. SSH into the instance: `ssh ubuntu@<instance-ip>`
3. Install vLLM and start the server:

```bash
# On the Lambda Labs instance
pip install vllm

# Start vLLM server (adjust model based on GPU memory)
python -m vllm.entrypoints.openai.api_server \
  --model meta-llama/Llama-3.1-8B-Instruct \
  --host 0.0.0.0 --port 8000 \
  --enable-prefix-caching
```

**Step 2: Ensure Port Access**

Lambda Labs instances typically have open ports, but verify:
```bash
# From your local machine, test connectivity
curl http://<instance-ip>:8000/health
```

**Step 3: Run Multi-GPU Benchmark**

With two Lambda instances running vLLM:

```bash
# Using the helper script
./scripts/run-multi-gpu-benchmark.sh <gpu1-ip> <gpu2-ip>

# Or manually
VLLM_ENDPOINT_1=http://<gpu1-ip>:8000 \
VLLM_ENDPOINT_2=http://<gpu2-ip>:8000 \
BACKEND1_IP=<gpu1-ip> \
BACKEND2_IP=<gpu2-ip> \
VLLM_MODEL=meta-llama/Llama-3.1-8B-Instruct \
make benchmark-real
```

**Cost Optimization Tips**
- Use spot/on-demand instances for testing
- A10G instances offer good price/performance for 8B models
- Shut down instances when not in use
- Consider using the same instance for both backends in dev (not for A/B tests)

### Output Files

Each benchmark run creates files in `benchmark-reports/`:

```
benchmark-reports/
├── 20251227_123456_real_output.log      # Raw Locust output
├── 20251227_123456_real_stats.csv       # Parsed statistics
├── comparison_20251227_140000/
│   ├── round_robin_output.log
│   ├── round_robin_stats.csv
│   ├── prefix_aware_output.log
│   ├── prefix_aware_stats.csv
│   └── comparison_summary.txt
└── ...
```

### Troubleshooting

**vLLM connection refused**
```bash
# Verify vLLM is running
curl http://gpu-server:8000/health

# Check vLLM logs
docker logs vllm-backend1
```

**Low cache hit rate**
- Increase `SHARED_PREFIX_RATIO`
- Use longer prompts (`PROMPT_DISTRIBUTION=long`)
- Verify vLLM has `--enable-prefix-caching`

**High failure rate**
- Increase `P99_LATENCY_THRESHOLD_MS` for slower models
- Reduce `BENCHMARK_REAL_USERS` if backends are overloaded
- Check backend GPU memory usage

**GPU out of memory**
```bash
# Use smaller model
VLLM_MODEL=meta-llama/Llama-3.2-1B-Instruct make benchmark-real-local

# Reduce batch size
VLLM_MAX_MODEL_LEN=2048 make benchmark-real-local
```

### Files Reference

```
tests/integration/
├── locustfile_real.py           # Real vLLM benchmark Locust file
├── run_benchmark_comparison.py  # A/B comparison script
├── results_parser.py            # Unified results parser (parse, compare, export)
├── prometheus-benchmark.yml     # Prometheus config for metrics collection
└── ...

docker-compose.benchmark-real.yml  # Real vLLM benchmark infrastructure
```
