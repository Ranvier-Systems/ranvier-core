# Ranvier Core Helm Chart

A Helm chart for deploying [Ranvier Core](https://github.com/ranvier-systems/ranvier-core) - a distributed LLM router with gossip synchronization - to Kubernetes.

## Overview

Ranvier Core is a high-performance, distributed router for Large Language Model (LLM) inference backends. It provides:

- **Prefix-based routing**: Routes requests to backends with cached KV state
- **Gossip synchronization**: Shares routing state across cluster nodes
- **Kubernetes-native discovery**: Automatically discovers GPU backends via EndpointSlices
- **OpenAI-compatible API**: Drop-in replacement for vLLM/TGI endpoints
- **Prometheus metrics**: Comprehensive observability with ServiceMonitor support

## Prerequisites

- Kubernetes 1.23+
- Helm 3.8+
- PV provisioner (for persistent storage)
- [Optional] Prometheus Operator (for ServiceMonitor)
- [Optional] GPU backends (vLLM, TGI, etc.)

## Installation

### Quick Start

```bash
# Add the Ranvier Helm repository (if published)
helm repo add ranvier https://charts.ranvier.io
helm repo update

# Or install from local directory
helm install ranvier ./deploy/helm/ranvier \
  --namespace ranvier \
  --create-namespace
```

### Production Installation

```bash
helm install ranvier ./deploy/helm/ranvier \
  --namespace ranvier \
  --create-namespace \
  --set replicaCount=3 \
  --set "auth.apiKeys[0].name=admin" \
  --set "auth.apiKeys[0].key=rnv_prod_$(openssl rand -hex 24)" \
  --set "auth.apiKeys[0].roles={admin}" \
  --set backends.discovery.enabled=true \
  --set backends.discovery.serviceName=vllm-backends \
  --set serviceMonitor.enabled=true
```

### Using a Custom Values File

```bash
helm install ranvier ./deploy/helm/ranvier \
  --namespace ranvier \
  --create-namespace \
  -f my-values.yaml
```

## Configuration

### Core Settings

| Parameter | Description | Default |
|-----------|-------------|---------|
| `replicaCount` | Number of Ranvier replicas | `3` |
| `image.repository` | Container image repository | `ghcr.io/ranvier-systems/ranvier` |
| `image.tag` | Container image tag | `""` (uses chart appVersion) |
| `image.pullPolicy` | Image pull policy | `IfNotPresent` |

### Server Configuration

| Parameter | Description | Default |
|-----------|-------------|---------|
| `config.routingMode` | Routing algorithm: `prefix`, `radix`, `round_robin` | `prefix` |
| `config.prefixTokenLength` | Token prefix length for routing key | `128` |
| `config.apiPort` | HTTP API port | `8080` |
| `config.metricsPort` | Prometheus metrics port | `9180` |

### Cluster (Gossip) Configuration

| Parameter | Description | Default |
|-----------|-------------|---------|
| `cluster.enabled` | Enable gossip synchronization | `true` |
| `cluster.gossipPort` | UDP port for gossip protocol | `9190` |
| `cluster.gossipIntervalMs` | Gossip broadcast interval | `1000` |
| `cluster.heartbeatIntervalSeconds` | Peer heartbeat interval | `5` |
| `cluster.peerTimeoutSeconds` | Peer timeout (should be >= 2x heartbeat) | `15` |

### Backend Discovery

| Parameter | Description | Default |
|-----------|-------------|---------|
| `backends.discovery.enabled` | Enable K8s EndpointSlice discovery | `false` |
| `backends.discovery.serviceName` | Service name to watch for backends | `""` |
| `backends.discovery.namespace` | Namespace of backend service | Release namespace |
| `backends.discovery.targetPort` | Port on GPU backends | `8080` |
| `backends.discovery.labelSelector` | Label selector for filtering | `""` |

### Authentication

| Parameter | Description | Default |
|-----------|-------------|---------|
| `auth.apiKeys` | List of API keys with name, key, roles | `[]` |
| `auth.existingSecret` | Use existing secret for API keys | `""` |

### Telemetry

| Parameter | Description | Default |
|-----------|-------------|---------|
| `telemetry.enabled` | Enable OpenTelemetry tracing | `false` |
| `telemetry.otlpEndpoint` | OTLP collector endpoint | `http://otel-collector:4318` |
| `telemetry.serviceName` | Service name for traces | `ranvier` |
| `telemetry.sampleRate` | Trace sample rate (0.0-1.0) | `1.0` |

### Monitoring

| Parameter | Description | Default |
|-----------|-------------|---------|
| `serviceMonitor.enabled` | Create Prometheus ServiceMonitor | `false` |
| `serviceMonitor.interval` | Scrape interval | `30s` |
| `serviceMonitor.scrapeTimeout` | Scrape timeout | `10s` |
| `serviceMonitor.labels` | Additional labels for ServiceMonitor | `{}` |

### Resources & Scaling

| Parameter | Description | Default |
|-----------|-------------|---------|
| `resources.requests.memory` | Memory request | `1Gi` |
| `resources.requests.cpu` | CPU request | `500m` |
| `resources.limits.memory` | Memory limit | `2Gi` |
| `resources.limits.cpu` | CPU limit | `2` |
| `autoscaling.enabled` | Enable HPA | `false` |
| `autoscaling.minReplicas` | Minimum replicas | `3` |
| `autoscaling.maxReplicas` | Maximum replicas | `10` |

### Persistence

| Parameter | Description | Default |
|-----------|-------------|---------|
| `persistence.enabled` | Enable persistent storage | `true` |
| `persistence.storageClass` | Storage class | `""` (default) |
| `persistence.size` | Volume size | `1Gi` |
| `persistence.accessMode` | Access mode | `ReadWriteOnce` |

### Seastar Runtime

| Parameter | Description | Default |
|-----------|-------------|---------|
| `seastar.smp` | Number of CPU cores for Seastar | `1` |
| `seastar.memory` | Memory allocation for Seastar | `1G` |
| `seastar.extraArgs` | Additional Seastar arguments | `[]` |

## Example Values

### 3-Node Production Cluster

```yaml
# values-production.yaml
replicaCount: 3

image:
  repository: ghcr.io/ranvier-systems/ranvier
  tag: "1.0.0"

resources:
  requests:
    memory: "2Gi"
    cpu: "1"
  limits:
    memory: "4Gi"
    cpu: "4"

seastar:
  smp: 2
  memory: "2G"

config:
  routingMode: prefix
  prefixTokenLength: 128

  circuitBreaker:
    enabled: true
    failureThreshold: 5
    recoveryTimeoutSeconds: 30

cluster:
  enabled: true
  gossipPort: 9190
  reliableDelivery: true

backends:
  discovery:
    enabled: true
    serviceName: vllm-backends
    namespace: gpu-workloads
    targetPort: 8000

auth:
  apiKeys:
    - name: production-admin
      key: "rnv_prod_your-secure-key-here"
      roles:
        - admin
    - name: readonly-monitoring
      key: "rnv_readonly_monitoring-key"
      roles:
        - viewer

telemetry:
  enabled: true
  otlpEndpoint: "http://otel-collector.monitoring:4318"

serviceMonitor:
  enabled: true
  labels:
    release: prometheus

persistence:
  enabled: true
  storageClass: fast-ssd
  size: 10Gi

affinity:
  podAntiAffinity:
    preferredDuringSchedulingIgnoredDuringExecution:
      - weight: 100
        podAffinityTerm:
          labelSelector:
            matchLabels:
              app.kubernetes.io/name: ranvier
          topologyKey: kubernetes.io/hostname

topologySpreadConstraints:
  - maxSkew: 1
    topologyKey: kubernetes.io/hostname
    whenUnsatisfiable: DoNotSchedule
    labelSelector:
      matchLabels:
        app.kubernetes.io/name: ranvier
```

### Development/Testing

```yaml
# values-dev.yaml
replicaCount: 1

resources:
  requests:
    memory: "512Mi"
    cpu: "250m"
  limits:
    memory: "1Gi"
    cpu: "1"

seastar:
  smp: 1
  memory: "512M"

cluster:
  enabled: false  # Single node, no gossip needed

backends:
  discovery:
    enabled: false  # Manual backend registration

persistence:
  enabled: false  # Use ephemeral storage

telemetry:
  enabled: false

serviceMonitor:
  enabled: false
```

## Architecture

### StatefulSet vs Deployment

This chart uses a **StatefulSet** instead of a Deployment for several reasons:

1. **Stable Network Identities**: Each pod gets a predictable DNS name (e.g., `ranvier-0.ranvier-headless.default.svc`)
2. **Gossip Peer Discovery**: Pods can discover each other via DNS for gossip synchronization
3. **Ordered Deployment**: Controlled rollout behavior for cluster coordination
4. **Persistent Storage**: Each pod maintains its own SQLite database

### Gossip Peer Discovery

Ranvier uses DNS-based peer discovery for gossip synchronization:

```
ranvier-0  ─────┐
                │
ranvier-1  ─────┼──── ranvier-headless.default.svc.cluster.local
                │
ranvier-2  ─────┘
```

The headless service returns A records for all pod IPs, enabling automatic peer discovery.

### Backend Discovery

When `backends.discovery.enabled=true`, Ranvier watches Kubernetes EndpointSlices for the configured service. GPU backends are automatically registered/deregistered as pods scale.

Pod annotations customize backend behavior:
```yaml
metadata:
  annotations:
    ranvier.io/weight: "200"    # Higher weight = more traffic
    ranvier.io/priority: "0"    # Lower priority = preferred (for failover)
```

## Security

### Security Context

The chart configures secure defaults:

```yaml
securityContext:
  runAsNonRoot: true
  runAsUser: 10001
  runAsGroup: 10001
  readOnlyRootFilesystem: true
  allowPrivilegeEscalation: false
  capabilities:
    drop: ["ALL"]
    add: ["IPC_LOCK"]  # Required by Seastar
```

### API Key Management

For production, use an external secret management solution:

```yaml
auth:
  existingSecret: ranvier-api-keys  # Created by External Secrets, Vault, etc.
```

The secret should contain:
```yaml
apiVersion: v1
kind: Secret
metadata:
  name: ranvier-api-keys
stringData:
  RANVIER_ADMIN_API_KEY: "rnv_prod_your-key"
  api-keys.yaml: |
    auth:
      api_keys:
        - key: "rnv_prod_your-key"
          name: "admin"
          roles: ["admin"]
```

## Monitoring

### Prometheus Metrics

Key metrics exposed on port 9180:

| Metric | Type | Description |
|--------|------|-------------|
| `ranvier_http_requests_total` | Counter | Total HTTP requests |
| `ranvier_http_request_duration_seconds` | Histogram | Request latency |
| `ranvier_cache_hit_ratio` | Gauge | Routing cache hit ratio |
| `ranvier_active_proxy_requests` | Gauge | In-flight requests |
| `ranvier_circuit_breaker_opens` | Counter | Circuit breaker trips |
| `ranvier_backend_latency_seconds` | Histogram | Per-backend latency |

### Grafana Dashboard

Import the Ranvier dashboard from `dashboards/ranvier.json` (if available) or create one based on the exposed metrics.

## Troubleshooting

### Check Pod Status

```bash
kubectl get pods -l app.kubernetes.io/name=ranvier -n ranvier
```

### View Logs

```bash
kubectl logs -f ranvier-0 -n ranvier
```

### Verify Gossip Connectivity

```bash
# Check DNS resolution
kubectl exec ranvier-0 -n ranvier -- nslookup ranvier-headless

# Check gossip port connectivity
kubectl exec ranvier-0 -n ranvier -- nc -uzv ranvier-1.ranvier-headless 9190
```

### Check Metrics

```bash
kubectl port-forward svc/ranvier 9180:9180 -n ranvier
curl http://localhost:9180/metrics
```

### Debug Configuration

```bash
# View generated config
kubectl get configmap ranvier-config -n ranvier -o yaml

# Check environment variables
kubectl exec ranvier-0 -n ranvier -- env | grep RANVIER
```

## Upgrading

### Rolling Update

```bash
helm upgrade ranvier ./deploy/helm/ranvier \
  --namespace ranvier \
  -f my-values.yaml
```

### Configuration Changes

Most configuration changes trigger a rolling restart via checksum annotations on the ConfigMap and Secret.

### Scaling

```bash
# Scale via Helm
helm upgrade ranvier ./deploy/helm/ranvier \
  --namespace ranvier \
  --set replicaCount=5

# Or directly via kubectl
kubectl scale statefulset ranvier --replicas=5 -n ranvier
```

## Uninstalling

```bash
helm uninstall ranvier --namespace ranvier

# Optional: Remove PVCs (data will be lost!)
kubectl delete pvc -l app.kubernetes.io/name=ranvier -n ranvier
```

## Contributing

See [CONTRIBUTING.md](../../../CONTRIBUTING.md) for development guidelines.

## License

This chart is licensed under the Apache 2.0 License.
