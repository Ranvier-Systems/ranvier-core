# Ranvier Core Architecture

## Overview

Ranvier Core is a content-aware Layer 7+ load balancer for LLM inference that solves GPU KV-cache thrashing by routing requests to GPUs that already have the relevant token prefix cached.

## Component Diagram

```mermaid
flowchart TB
    subgraph Clients
        C1[LLM Client]
        C2[Admin Client]
    end

    subgraph "Ranvier Core"
        subgraph "Presentation Layer"
            HTTP[HttpController<br/>:8080]
            PROM[Prometheus<br/>:9180]
        end

        subgraph "Domain Layer"
            RS[RouterService]
            RT[RadixTree<br/>ART: Node4→16→48→256]
            CB[Circuit Breaker]
        end

        subgraph "Infrastructure Layer"
            TOK[TokenizerService<br/>GPT-2]
            HS[HealthService]
            CP[ConnectionPool<br/>LRU + Bounded]
        end

        subgraph "Persistence Layer"
            PS[(SqlitePersistence<br/>WAL Mode)]
        end
    end

    subgraph "GPU Backends"
        GPU1[Backend 1<br/>Ollama/vLLM]
        GPU2[Backend 2<br/>Ollama/vLLM]
        GPU3[Backend N<br/>Ollama/vLLM]
    end

    %% Client flows
    C1 -->|POST /v1/chat/completions| HTTP
    C2 -->|POST/DELETE /admin/*| HTTP

    %% Internal flows
    HTTP -->|encode| TOK
    HTTP -->|lookup/learn| RS
    RS --> RT
    RS --> CB
    HTTP -->|get/put connection| CP
    HTTP -->|save/load| PS

    %% Health checking
    HS -->|ping| GPU1
    HS -->|ping| GPU2
    HS -->|ping| GPU3
    HS -->|set_backend_status| RS

    %% Backend connections
    CP -->|proxy request| GPU1
    CP -->|proxy request| GPU2
    CP -->|proxy request| GPU3

    %% Metrics
    RS -->|counters| PROM

    %% Styling
    classDef storage fill:#f9f,stroke:#333
    classDef external fill:#bbf,stroke:#333
    class PS storage
    class GPU1,GPU2,GPU3,C1,C2 external
```

## Layer Responsibilities

### Presentation Layer
- **HttpController**: Handles HTTP endpoints for data plane (proxy) and control plane (admin)
- **Prometheus**: Exposes metrics for monitoring (cache hits/misses, latency)

### Domain Layer
- **RouterService**: Core routing logic with cross-shard broadcasting
- **RadixTree**: Adaptive Radix Tree (ART) for O(k) prefix lookups
- **Circuit Breaker**: Quarantines unhealthy backends

### Infrastructure Layer
- **TokenizerService**: GPT-2 tokenization for request content
- **HealthService**: Periodic health checks on backends
- **ConnectionPool**: Reusable connections with LRU eviction

### Persistence Layer
- **SqlitePersistence**: Durable storage for routes and backends (survives restarts)
