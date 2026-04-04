# Ranvier Cloud Deployment Guide

Deploy Ranvier as an intelligent load balancer in front of vLLM GPU backends. Ranvier adds prefix-aware routing, priority queuing, cost-based routing, and load-aware backend selection on top of standard HTTP proxying.

## Architecture

```
                    ┌──────────────────────────────┐
                    │         Clients / IDEs        │
                    └──────────┬───────────────────┘
                               │ HTTPS :443
                    ┌──────────▼───────────────────┐
                    │   Ranvier Cluster (3 nodes)   │
                    │   gossip sync :9190            │
                    │   metrics :9180                │
                    └──┬───────┬───────┬───────────┘
                       │       │       │
              ┌────────▼──┐ ┌─▼─────┐ ┌▼──────────┐
              │ vLLM GPU-1│ │GPU-2  │ │ GPU-3..N  │
              │ :8000     │ │:8000  │ │ :8000     │
              │ (A100)    │ │(A100) │ │ (A100)    │
              └───────────┘ └───────┘ └───────────┘
```

## Quick Start with Docker Compose

The integration test compose file is a working starting point:

```bash
# Start with mock backends (no GPUs needed)
docker compose -f docker-compose.test.yml up -d

# Verify
curl http://localhost:8081/admin/dump/backends
```

To replace mock backends with real vLLM servers, edit `docker-compose.test.yml`:

```yaml
services:
  backend1:
    image: vllm/vllm-openai:latest
    command: >
      --model meta-llama/Llama-3.1-8B
      --enable-prefix-caching
      --port 8000
    deploy:
      resources:
        reservations:
          devices:
            - driver: nvidia
              count: 1
              capabilities: [gpu]
```

Then register the backends:

```bash
curl -X POST "http://localhost:8081/admin/backends?id=1&ip=<vllm-host>&port=8000"
```

## Kubernetes Deployment

### Install with Helm

```bash
helm install ranvier deploy/helm/ranvier/ \
  --namespace ranvier --create-namespace
```

### Key Values

```yaml
# deploy/helm/ranvier/values.yaml (excerpts)

replicaCount: 3

backends:
  discovery:
    enabled: true
    serviceName: vllm-backends    # K8s service to watch

cluster:
  enabled: true
  gossip:
    port: 9190
    # Peers auto-discovered via DNS in K8s

tls:
  enabled: false                  # Enable for production
  certSecretName: ranvier-tls

auth:
  apiKeys:
    - name: admin
      key: "rnv_prod_<your-key>"
      roles: [admin]
```

### Production Install

```bash
helm install ranvier deploy/helm/ranvier/ \
  --namespace ranvier --create-namespace \
  -f deploy/helm/ranvier/values-production.yaml \
  --set "auth.apiKeys[0].name=admin" \
  --set "auth.apiKeys[0].key=rnv_prod_$(openssl rand -hex 24)" \
  --set "auth.apiKeys[0].roles={admin}" \
  --set backends.discovery.enabled=true \
  --set backends.discovery.serviceName=vllm-backends \
  --set serviceMonitor.enabled=true
```

See [`deploy/helm/ranvier/README.md`](../../deploy/helm/ranvier/README.md) for the full values reference.

## Register Backends

Three ways to register vLLM backends:

### Admin API (manual)

```bash
curl -X POST "http://ranvier:8080/admin/backends?id=1&ip=10.0.1.10&port=8000"
curl -X POST "http://ranvier:8080/admin/backends?id=2&ip=10.0.1.11&port=8000"

# Verify
curl http://ranvier:8080/admin/dump/backends
```

### K8s Service Discovery (automatic)

In your Helm values:

```yaml
backends:
  discovery:
    enabled: true
    serviceName: vllm-backends
    namespace: gpu-pool            # optional, defaults to same namespace
```

Ranvier watches the EndpointSlice for the named service and registers/deregisters backends as pods scale.

### Persistence (survives restarts)

Backends registered via the admin API are persisted to SQLite by default. On restart, Ranvier reloads them automatically. Set `RANVIER_DB_PATH` to control the database location.

## Enable Intelligence Features

All intelligence features can be enabled via environment variables or YAML config.

### Example `ranvier.yaml`

```yaml
server:
  api_port: 8080
  metrics_port: 9180

cost_estimation:
  enabled: true                    # Estimate prefill cost per request

priority_tier:
  enabled: true                    # Priority queue (CRITICAL > HIGH > NORMAL > LOW)
  respect_header: true             # Honor X-Ranvier-Priority header

intent_classification:
  enabled: true                    # Detect FIM/Chat/Edit from payload

agent_registry:
  enabled: true
  auto_detect_agents: true         # Identify agents from User-Agent header

health:
  enable_vllm_metrics: true        # Scrape /metrics from vLLM backends
  check_interval_seconds: 5

cost_routing:
  enabled: true                    # Route by cost budget per backend

routing:
  mode: prefix                     # Prefix-aware KV cache routing
  load_aware_routing: true         # P2C load balancing

backpressure:
  enable_priority_queue: true      # Priority-aware request queuing
```

### Environment Variable Equivalents

```bash
RANVIER_COST_ESTIMATION_ENABLED=true
RANVIER_PRIORITY_TIER_ENABLED=true
RANVIER_INTENT_CLASSIFICATION_ENABLED=true
RANVIER_AGENT_REGISTRY_ENABLED=true
RANVIER_AGENT_AUTO_DETECT=true
RANVIER_HEALTH_ENABLE_VLLM_METRICS=true
RANVIER_COST_ROUTING_ENABLED=true
RANVIER_BACKPRESSURE_ENABLE_PRIORITY_QUEUE=true
```

See [`ranvier.yaml.example`](../../ranvier.yaml.example) for the complete configuration reference.

## Monitoring

### Prometheus Endpoint

Ranvier exposes metrics on `:9180/metrics` in Prometheus text format.

### Key Metrics

Seastar adds a `seastar_` prefix to all metric names automatically.

| Metric | Description |
|--------|-------------|
| `seastar_ranvier_http_requests_total` | Total HTTP requests processed |
| `seastar_ranvier_http_requests_success` | Successful requests |
| `seastar_ranvier_router_cache_hits` | Prefix cache hits (ART lookup succeeded) |
| `seastar_ranvier_router_cache_misses` | Cache misses (hash fallback) |
| `seastar_ranvier_http_request_duration_seconds` | End-to-end request latency histogram |
| `seastar_ranvier_backend_response_duration_seconds` | Backend response latency histogram |
| `seastar_ranvier_router_gpu_load_redirects_total` | Requests rerouted due to GPU load |
| `seastar_ranvier_router_load_aware_fallbacks_total` | Total load-aware routing fallbacks |
| `seastar_ranvier_agents_tracked` | Number of detected agents |
| `seastar_ranvier_scheduler_queue_depth` | Priority queue depth |

### Grafana Dashboard Queries

```promql
# Cache hit rate (5m window)
rate(seastar_ranvier_router_cache_hits[5m]) / (rate(seastar_ranvier_router_cache_hits[5m]) + rate(seastar_ranvier_router_cache_misses[5m]))

# P99 request latency
histogram_quantile(0.99, rate(seastar_ranvier_http_request_duration_seconds_bucket[5m]))

# P99 backend response latency
histogram_quantile(0.99, rate(seastar_ranvier_backend_response_duration_seconds_bucket[5m]))

# Request rate
rate(seastar_ranvier_http_requests_total[1m])

# GPU load redirect rate
rate(seastar_ranvier_router_gpu_load_redirects_total[5m]) / rate(seastar_ranvier_http_requests_total[5m])
```

## Cluster Mode

### Gossip Configuration

Ranvier nodes synchronize routing state (which prefixes live on which backends) via a gossip protocol.

```yaml
cluster:
  enabled: true
  gossip:
    port: 9190
    peers:
      - "ranvier-1:9190"
      - "ranvier-2:9190"
      - "ranvier-3:9190"
    interval_ms: 1000
    heartbeat_interval: 3
    peer_timeout: 10
```

### DNS Discovery (Kubernetes)

Instead of listing peers explicitly, use DNS:

```yaml
cluster:
  gossip:
    dns_discovery:
      enabled: true
      hostname: ranvier-headless.ranvier.svc.cluster.local
      record_type: A              # A or SRV
```

The Helm chart configures this automatically via a headless service.

### TLS for Gossip (strongly recommended)

```yaml
cluster:
  gossip:
    tls:
      enabled: true
      cert_path: /etc/ranvier/gossip-cert.pem
      key_path: /etc/ranvier/gossip-key.pem
      ca_path: /etc/ranvier/gossip-ca.pem
```

In Kubernetes, mount a Secret with the certs and reference it in your Helm values.

## Benchmark Your Deployment

### Quick Smoke Test (mock backends)

```bash
docker compose -f docker-compose.test.yml up -d
./tests/integration/run-benchmark.sh
```

Expected: ~473 rps, <140ms P99 with mock backends.

### Real GPU Benchmark

For validated performance numbers on real vLLM backends, see:
- [Benchmark Guide (8x A100)](../benchmarks/benchmark-guide-8xA100.md) — full methodology
- [`scripts/bench.sh`](../../scripts/bench.sh) — automated benchmark runner
- [`tests/integration/locustfile_real.py`](../../tests/integration/locustfile_real.py) — Locust load test

Quick start:

```bash
# Configure vLLM backend addresses in bench.sh, then:
./scripts/bench.sh --duration 30m --users 20
```
