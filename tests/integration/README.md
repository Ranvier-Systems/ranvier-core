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
  │   │  │:11434   │ │:11435   │  │    │ │ :8081  │ │ :8082  │     │  │          │
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
      - 11434-11435: Mock Backends                                                │
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
| `test_02_register_backend_on_node1` | Registers mock backends via admin API |
| `test_03_send_request_to_learn_route` | Sends request to learn a route prefix |
| `test_04_verify_route_propagation` | Checks gossip metrics for propagation |
| `test_05_request_on_other_nodes` | Verifies other nodes can route requests |
| `test_06_stop_node_and_verify_peer_count` | Stops a node, checks peer count drops |
| `test_07_restart_node_and_verify_recovery` | Restarts node, verifies cluster recovery |

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
├── test_cluster.py             # Main test script
├── mock_backend.py             # Mock vLLM backend
├── Dockerfile.mock-backend     # Dockerfile for mock backend
└── configs/
    ├── node1.yaml              # Node 1 configuration
    ├── node2.yaml              # Node 2 configuration
    └── node3.yaml              # Node 3 configuration
```
