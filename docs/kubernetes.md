# Kubernetes Deployment Guide

This guide covers deploying Ranvier Core to Kubernetes using the official Helm chart.

## Prerequisites

- Kubernetes 1.23+
- Helm 3.8+
- `kubectl` configured for your cluster
- PersistentVolume provisioner (for SQLite storage)
- [Optional] Prometheus Operator (for ServiceMonitor)
- [Optional] GPU backends (vLLM, TGI, etc.) running in the cluster

## Quick Start

```bash
# Install from the repository
helm install ranvier ./deploy/helm/ranvier \
  --namespace ranvier \
  --create-namespace

# Verify the installation
kubectl get pods -n ranvier
kubectl get svc -n ranvier
```

## Architecture

The Helm chart deploys Ranvier as a **StatefulSet** with the following components:

```
┌─────────────────────────────────────────────────────────────────┐
│                        Kubernetes Cluster                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                   │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐              │
│  │  ranvier-0  │  │  ranvier-1  │  │  ranvier-2  │              │
│  │             │  │             │  │             │              │
│  │  Port 8080  │  │  Port 8080  │  │  Port 8080  │  ◄── API     │
│  │  Port 9180  │  │  Port 9180  │  │  Port 9180  │  ◄── Metrics │
│  │  Port 9190  │  │  Port 9190  │  │  Port 9190  │  ◄── Gossip  │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘              │
│         │                │                │                      │
│         └────────────────┼────────────────┘                      │
│                          │                                       │
│              ┌───────────▼───────────┐                          │
│              │  ranvier-headless     │  (DNS-based discovery)   │
│              │  ClusterIP: None      │                          │
│              └───────────────────────┘                          │
│                                                                   │
│              ┌───────────────────────┐                          │
│              │  ranvier (Service)    │  (Load-balanced API)     │
│              │  ClusterIP/LB         │                          │
│              └───────────────────────┘                          │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
```

### Why StatefulSet?

- **Stable Network Identities**: Each pod gets a predictable DNS name (`ranvier-0.ranvier-headless.default.svc`)
- **Gossip Peer Discovery**: Pods discover each other via DNS for route synchronization
- **Persistent Storage**: Each pod maintains its own SQLite database across restarts

## Installation

### Basic Installation

```bash
helm install ranvier ./deploy/helm/ranvier \
  --namespace ranvier \
  --create-namespace
```

### Production Installation

```bash
helm install ranvier ./deploy/helm/ranvier \
  --namespace ranvier \
  --create-namespace \
  -f ./deploy/helm/ranvier/values-production.yaml \
  --set "auth.apiKeys[0].key=rnv_prod_$(openssl rand -hex 24)"
```

### Custom Values File

Create a `my-values.yaml`:

```yaml
replicaCount: 5

resources:
  requests:
    memory: "4Gi"
    cpu: "2"
  limits:
    memory: "8Gi"
    cpu: "4"

backends:
  discovery:
    enabled: true
    serviceName: vllm-backends
    namespace: gpu-workloads

serviceMonitor:
  enabled: true
  labels:
    release: prometheus
```

Install with:

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
| `replicaCount` | Number of Ranvier pods | `3` |
| `config.routingMode` | `prefix`, `radix`, or `round_robin` | `prefix` |
| `config.apiPort` | HTTP API port | `8080` |
| `config.metricsPort` | Prometheus metrics port | `9180` |
| `cluster.gossipPort` | UDP gossip port | `9190` |

### Backend Discovery

Ranvier can automatically discover GPU backends via Kubernetes EndpointSlices:

```yaml
backends:
  discovery:
    enabled: true
    serviceName: vllm-backends      # K8s service to watch
    namespace: gpu-workloads        # Namespace (default: release namespace)
    targetPort: 8000                # Port on the GPU backends
    labelSelector: "gpu=a100"       # Optional: filter endpoints
```

**GPU Backend Pod Annotations:**

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: vllm-0
  annotations:
    ranvier.io/weight: "200"     # Higher weight = more traffic
    ranvier.io/priority: "0"     # Lower = preferred (failover groups)
```

### Authentication

```yaml
auth:
  apiKeys:
    - name: production-admin
      key: "rnv_prod_your-secure-key-here"
      roles:
        - admin
    - name: monitoring
      key: "rnv_readonly_monitoring-key"
      roles:
        - viewer
```

For production, use an external secret:

```yaml
auth:
  existingSecret: ranvier-api-keys
```

### Monitoring

#### Prometheus ServiceMonitor

```yaml
serviceMonitor:
  enabled: true
  labels:
    release: prometheus  # Match your Prometheus Operator selector
  interval: 30s
  scrapeTimeout: 10s
```

#### OpenTelemetry Tracing

```yaml
telemetry:
  enabled: true
  otlpEndpoint: "http://otel-collector.monitoring:4318"
  serviceName: "ranvier"
  sampleRate: 0.1  # 10% sampling for production
```

### High Availability

```yaml
# Spread pods across nodes
affinity:
  podAntiAffinity:
    preferredDuringSchedulingIgnoredDuringExecution:
      - weight: 100
        podAffinityTerm:
          labelSelector:
            matchLabels:
              app.kubernetes.io/name: ranvier
          topologyKey: kubernetes.io/hostname

# Spread across availability zones
topologySpreadConstraints:
  - maxSkew: 1
    topologyKey: topology.kubernetes.io/zone
    whenUnsatisfiable: ScheduleAnyway
    labelSelector:
      matchLabels:
        app.kubernetes.io/name: ranvier
```

## Operations

### Scaling

```bash
# Scale via Helm
helm upgrade ranvier ./deploy/helm/ranvier \
  --namespace ranvier \
  --set replicaCount=5

# Or directly via kubectl
kubectl scale statefulset ranvier --replicas=5 -n ranvier
```

### Checking Gossip Health

```bash
# Verify DNS resolution
kubectl exec ranvier-0 -n ranvier -- nslookup ranvier-headless

# Check metrics for gossip peers
kubectl port-forward svc/ranvier 9180:9180 -n ranvier
curl -s http://localhost:9180/metrics | grep gossip
```

### Reloading API Keys

After updating the secret:

```bash
# Via signal
kubectl exec ranvier-0 -n ranvier -- kill -HUP 1

# Or via API (requires valid key)
kubectl exec ranvier-0 -n ranvier -- \
  curl -X POST http://localhost:8080/admin/keys/reload \
    -H "Authorization: Bearer $API_KEY"
```

### Viewing Logs

```bash
# All pods
kubectl logs -l app.kubernetes.io/name=ranvier -n ranvier --tail=100

# Specific pod
kubectl logs ranvier-0 -n ranvier -f
```

### Upgrading

```bash
helm upgrade ranvier ./deploy/helm/ranvier \
  --namespace ranvier \
  -f my-values.yaml
```

The StatefulSet performs a rolling update, maintaining cluster availability.

### Uninstalling

```bash
helm uninstall ranvier --namespace ranvier

# Remove PVCs (data will be lost!)
kubectl delete pvc -l app.kubernetes.io/name=ranvier -n ranvier
```

## Testing

### Validate Chart Locally

```bash
# Lint the chart
make helm-lint

# Render templates without installing
make helm-template

# Dry-run against a cluster
make helm-dry-run
```

### Deploy to Local Cluster

```bash
# Create a kind cluster
kind create cluster --name ranvier-test

# Install the chart
helm install ranvier ./deploy/helm/ranvier \
  --namespace ranvier \
  --create-namespace \
  --set persistence.enabled=false \
  --set cluster.enabled=true

# Port-forward to test
kubectl port-forward svc/ranvier 8080:8080 -n ranvier

# Test the API
curl http://localhost:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model": "test", "messages": [{"role": "user", "content": "Hello"}]}'
```

## Troubleshooting

### Pods Not Starting

```bash
# Check events
kubectl describe pod ranvier-0 -n ranvier

# Common issues:
# - IPC_LOCK capability not granted (check securityContext)
# - PVC not binding (check storage class)
# - Image pull errors (check imagePullSecrets)
```

### Gossip Not Forming

```bash
# Verify headless service exists
kubectl get svc ranvier-headless -n ranvier

# Check DNS resolution from a pod
kubectl exec ranvier-0 -n ranvier -- nslookup ranvier-headless

# Verify UDP port 9190 is not blocked by NetworkPolicy
```

### Backend Discovery Not Working

```bash
# Check RBAC permissions
kubectl auth can-i list endpointslices --as=system:serviceaccount:ranvier:ranvier -n ranvier

# Verify the target service exists
kubectl get svc vllm-backends -n gpu-workloads
kubectl get endpointslices -l kubernetes.io/service-name=vllm-backends -n gpu-workloads
```

## Example: Full Production Setup

```yaml
# values-production-full.yaml
replicaCount: 3

image:
  repository: ghcr.io/ranvier-systems/ranvier
  tag: "1.0.0"

resources:
  requests:
    memory: "4Gi"
    cpu: "2"
  limits:
    memory: "8Gi"
    cpu: "4"

seastar:
  smp: 2
  memory: "4G"

config:
  routingMode: prefix
  prefixTokenLength: 128
  rateLimit:
    enabled: true
    requestsPerSecond: 1000
    burstSize: 200

cluster:
  enabled: true
  gossipPort: 9190

backends:
  discovery:
    enabled: true
    serviceName: vllm-backends
    namespace: gpu-workloads
    targetPort: 8000

auth:
  existingSecret: ranvier-api-keys  # Managed by External Secrets

telemetry:
  enabled: true
  otlpEndpoint: "http://otel-collector.monitoring:4318"
  sampleRate: 0.1

serviceMonitor:
  enabled: true
  labels:
    release: prometheus

persistence:
  enabled: true
  storageClass: fast-ssd
  size: 10Gi

ingress:
  enabled: true
  className: nginx
  annotations:
    cert-manager.io/cluster-issuer: letsencrypt-prod
    nginx.ingress.kubernetes.io/proxy-read-timeout: "300"
  hosts:
    - host: ranvier.example.com
      paths:
        - path: /
          pathType: Prefix
  tls:
    - secretName: ranvier-tls
      hosts:
        - ranvier.example.com

affinity:
  podAntiAffinity:
    requiredDuringSchedulingIgnoredDuringExecution:
      - labelSelector:
          matchLabels:
            app.kubernetes.io/name: ranvier
        topologyKey: kubernetes.io/hostname

topologySpreadConstraints:
  - maxSkew: 1
    topologyKey: topology.kubernetes.io/zone
    whenUnsatisfiable: DoNotSchedule
    labelSelector:
      matchLabels:
        app.kubernetes.io/name: ranvier

priorityClassName: high-priority
terminationGracePeriodSeconds: 90
```

Deploy:

```bash
helm install ranvier ./deploy/helm/ranvier \
  --namespace ranvier \
  --create-namespace \
  -f values-production-full.yaml
```
