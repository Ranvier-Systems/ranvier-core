# Request Flow

## Proxy Request Sequence

This diagram shows how a client request flows through Ranvier Core to a backend GPU.

```mermaid
sequenceDiagram
    participant Client
    participant HTTP as HttpController
    participant TOK as Tokenizer
    participant RS as RouterService
    participant RT as RadixTree
    participant CP as ConnectionPool
    participant GPU as Backend GPU
    participant APM as AsyncPersistence
    participant PS as SQLite

    Client->>HTTP: POST /v1/chat/completions
    HTTP->>TOK: encode(body)
    TOK-->>HTTP: tokens[]

    HTTP->>RS: lookup(tokens)
    RS->>RT: longest_prefix_match

    alt Cache Hit
        RT-->>RS: backend_id
        RS-->>HTTP: backend_id
    else Cache Miss
        RT-->>RS: nullopt
        RS-->>HTTP: random_backend()
    end

    HTTP->>CP: get(backend_addr)
    CP-->>HTTP: connection

    HTTP->>GPU: proxy request
    GPU-->>HTTP: response (streaming)

    alt Success + Cache Miss
        HTTP->>RS: learn_route_global(tokens, backend_id)
        RS->>RT: insert (broadcast to all shards)
        HTTP->>APM: queue_save_route(tokens, backend_id)
        Note over APM: Fire-and-forget (non-blocking)
    end

    Note over APM,PS: Background (every 100ms)
    APM->>PS: batch_save_routes()

    HTTP->>CP: put(connection)
    HTTP-->>Client: response
```

## Key Points

1. **Tokenization**: Request body is tokenized to create the lookup key
2. **Prefix Lookup**: RadixTree finds the longest matching prefix
3. **Cache Hit**: Route directly to the backend that owns this prefix
4. **Cache Miss**: Pick a random healthy backend
5. **Snooping**: On successful response, learn the route for future requests
6. **Async Persistence**: Routes are queued for background persistence (non-blocking)
7. **Batched Writes**: AsyncPersistenceManager flushes batches to SQLite every 100ms

## Backend Registration Flow

```mermaid
sequenceDiagram
    participant Admin
    participant HTTP as HttpController
    participant RS as RouterService
    participant APM as AsyncPersistence
    participant PS as SQLite
    participant HS as HealthService

    Admin->>HTTP: POST /admin/backends?id=1&ip=X&port=Y
    HTTP->>APM: queue_save_backend(id, ip, port)
    Note over APM: Returns immediately
    HTTP->>RS: register_backend_global(id, addr)

    Note over RS: Broadcast to all CPU shards

    RS-->>HTTP: success
    HTTP-->>Admin: {"status": "ok"}

    Note over APM,PS: Background flush
    APM->>PS: save_backend(id, ip, port)

    loop Every 5 seconds
        HS->>RS: get_all_backend_ids()
        RS-->>HS: [1, 2, 3, ...]
        HS->>HS: ping each backend
        alt Backend Down
            HS->>RS: set_backend_status_global(id, false)
            Note over RS: Circuit breaker activated
        end
    end
```

## Backend Removal Flow

```mermaid
sequenceDiagram
    participant Admin
    participant HTTP as HttpController
    participant RS as RouterService
    participant APM as AsyncPersistence
    participant PS as SQLite

    Admin->>HTTP: DELETE /admin/backends?id=1
    HTTP->>APM: queue_remove_routes_for_backend(id)
    HTTP->>APM: queue_remove_backend(id)
    Note over APM: Operations queued (non-blocking)
    HTTP->>RS: unregister_backend_global(id)

    Note over RS: Broadcast removal to all CPU shards

    RS-->>HTTP: success
    HTTP-->>Admin: {"status": "ok", "backend_deleted": 1}

    Note over APM,PS: Background flush
    APM->>PS: remove_routes_for_backend(id)
    APM->>PS: remove_backend(id)
```

## Token Forwarding Flow

This diagram shows how the router injects pre-computed `prompt_token_ids` into requests, reducing backend CPU load by eliminating redundant tokenization.

```mermaid
sequenceDiagram
    participant Client
    participant HTTP as HttpController
    participant TOK as Tokenizer
    participant RS as RouterService
    participant RW as RequestRewriter
    participant GPU as Backend GPU

    Client->>HTTP: POST /v1/chat/completions<br/>{"prompt": "Hello world", ...}

    alt Client provides prompt_token_ids
        Note over HTTP: accept_client_tokens=true
        HTTP->>HTTP: extract_prompt_token_ids()
        Note over HTTP: Validate token bounds [0, max_token_id]
        HTTP->>RS: lookup(client_tokens)
    else Router tokenizes
        HTTP->>TOK: encode(body)
        TOK-->>HTTP: tokens[]
        HTTP->>RS: lookup(tokens)
    end

    RS-->>HTTP: backend_id

    alt Token Forwarding Enabled
        Note over HTTP: enable_token_forwarding=true
        HTTP->>RW: rewrite(body, tokens)
        RW-->>HTTP: {"prompt": "...", "prompt_token_ids": [15496, 995], ...}
        Note over RW: Inject token array into JSON body
    end

    HTTP->>GPU: proxy request with prompt_token_ids

    Note over GPU: Backend skips tokenization<br/>(vLLM/SGLang optimization)

    GPU-->>HTTP: response (streaming)
    HTTP-->>Client: response
```

### Token Forwarding Benefits

1. **Reduced Backend CPU**: Backends like vLLM skip internal tokenization when `prompt_token_ids` is present, reducing CPU overhead by 10-15%.
2. **Guaranteed Routing Consistency**: Same tokens used for cache lookup are sent to the backend, ensuring KV-cache hits align with routing decisions.
3. **Pre-tokenized Client Support**: High-throughput clients can send pre-computed tokens (with `accept_client_tokens=true`) to skip router tokenization entirely.

### Configuration

Enable token forwarding in `ranvier.yaml`:
```yaml
router:
  enable_token_forwarding: true   # Inject tokens into backend requests
  accept_client_tokens: false     # Accept client-provided tokens (optional)
  max_token_id: 100000            # Security: reject out-of-range tokens
```
