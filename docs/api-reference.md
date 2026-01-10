# API Reference

## Endpoints Overview

```mermaid
flowchart LR
    subgraph "Data Plane :8080"
        D1[POST /v1/chat/completions]
    end

    subgraph "Control Plane :8080"
        C1[POST /admin/backends]
        C2[DELETE /admin/backends]
        C3[POST /admin/routes]
        C4[DELETE /admin/routes]
        C5[POST /admin/clear]
    end

    subgraph "Metrics :9180"
        M1[GET /metrics]
    end
```

## Data Plane

### POST /v1/chat/completions

Proxies requests to backend LLM servers with intelligent routing.

**Request**: OpenAI-compatible chat completion request
```json
{
  "model": "llama2",
  "messages": [{"role": "user", "content": "Hello"}],
  "stream": true
}
```

**Response**: Streamed response from backend (SSE format)

**Routing Logic**:
1. Tokenize request body
2. Look up longest prefix match in RadixTree
3. If hit → route to matched backend
4. If miss → route to random healthy backend
5. On success → learn route for future requests

---

## Control Plane

### POST /admin/backends

Register a new GPU backend.

**Query Parameters**:
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| id | int | Yes | Unique backend identifier |
| ip | string | Yes | Backend IP address |
| port | int | Yes | Backend port |

**Example**:
```bash
curl -X POST "http://localhost:8080/admin/backends?id=1&ip=192.168.1.100&port=11434"
```

**Response**:
```json
{"status": "ok"}
```

---

### DELETE /admin/backends

Remove a backend and all its associated routes.

**Query Parameters**:
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| id | int | Yes | Backend ID to remove |

**Example**:
```bash
curl -X DELETE "http://localhost:8080/admin/backends?id=1"
```

**Response**:
```json
{"status": "ok", "backend_deleted": 1}
```

---

### POST /admin/routes

Manually add a route (prefix → backend mapping).

**Query Parameters**:
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| backend_id | int | Yes | Target backend ID |

**Body**: Text content to tokenize and use as route prefix

**Example**:
```bash
curl -X POST "http://localhost:8080/admin/routes?backend_id=1" \
  -d "System prompt for my assistant..."
```

**Response**:
```json
{"status": "ok", "route_added": 1}
```

---

### DELETE /admin/routes

Remove all routes for a specific backend.

**Query Parameters**:
| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| backend_id | int | Yes | Backend ID whose routes to remove |

**Example**:
```bash
curl -X DELETE "http://localhost:8080/admin/routes?backend_id=1"
```

**Response**:
```json
{
  "status": "ok",
  "routes_deleted_for_backend": 1,
  "note": "In-memory routes will be cleared on restart"
}
```

---

### POST /admin/clear

Clear all persisted data (backends and routes). **Destructive!**

**Example**:
```bash
curl -X POST "http://localhost:8080/admin/clear"
```

**Response**:
```json
{
  "status": "ok",
  "warning": "All persisted data cleared. Restart to clear in-memory state."
}
```

---

## Metrics

### GET :9180/metrics

Prometheus metrics endpoint.

**Key Metrics**:
| Metric | Type | Description |
|--------|------|-------------|
| ranvier_router_cache_hits | counter | Prefix cache hits |
| ranvier_router_cache_misses | counter | Prefix cache misses |

**Example**:
```bash
curl http://localhost:9180/metrics
```

---

## rvctl CLI Tool

The `rvctl` command-line tool provides a human-friendly interface for inspecting and managing a running Ranvier instance. It communicates with the Admin API via JSON/HTTP.

**Location**: `tools/rvctl`

### Installation

```bash
# Make executable
chmod +x tools/rvctl

# Optionally symlink to PATH
ln -s $(pwd)/tools/rvctl /usr/local/bin/rvctl
```

### Authentication

Set the `RANVIER_ADMIN_KEY` environment variable or use the `--admin-key` flag:

```bash
export RANVIER_ADMIN_KEY=your-api-key
# or
rvctl --admin-key your-api-key inspect routes
```

### Commands

#### inspect routes

Display the radix tree structure with route mappings.

```bash
# Show all routes
rvctl inspect routes

# Filter by prefix (comma-separated token IDs)
rvctl inspect routes --prefix "1234,5678,9012"
```

**Output**: ASCII tree visualization showing node types, prefixes, backend mappings, and route origins (LOCAL/REMOTE).

#### inspect backends

Show backend health status in table format.

```bash
rvctl inspect backends
```

**Output**:
```
Backend Status (Shard 0)
================================================================================
  ID │       Address       │ Weight │ Priority │     Status
────┼─────────────────────┼────────┼──────────┼───────────────
   1 │ 192.168.1.100:11434 │    100 │        0 │    HEALTHY
   2 │ 192.168.1.101:11434 │    100 │        0 │   DRAINING
     │ └─ Draining since 5m ago

Total Backends: 2
  Healthy: 1  Draining: 1  Dead: 0
```

#### cluster status

Show cluster health and peer status.

```bash
rvctl cluster status
```

**Output**: Quorum state, peer count, local backend ID, and peer table with last-seen timestamps.

#### drain

Initiate graceful drain of a backend.

```bash
rvctl drain <backend_id>
```

**Example**:
```bash
rvctl drain 2
# Success: Backend 2 drain initiated
```

#### route add

Manually register a route (prefix → backend mapping).

```bash
# Content from argument
rvctl route add --backend 1 --content "You are a helpful assistant."

# Content from stdin
echo "System prompt here" | rvctl route add --backend 1 --stdin
```

### Global Options

| Option | Short | Description |
|--------|-------|-------------|
| `--url` | `-u` | Ranvier Admin API URL (default: `http://localhost:8080`) |
| `--admin-key` | `-k` | Admin API key |
| `--verbose` | `-v` | Enable verbose output |

### Environment Variables

| Variable | Description |
|----------|-------------|
| `RANVIER_ADMIN_KEY` | Default admin API key |

---
