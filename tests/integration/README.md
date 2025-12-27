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
├── parse_locust_output.py       # Parser for extracting stats from Locust output
├── compare_results.py           # Tool for comparing benchmark results
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
python3 tests/integration/compare_results.py \
  benchmark-reports/token_off_20251227_123456_stats.csv \
  benchmark-reports/token_on_20251227_124500_stats.csv
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
