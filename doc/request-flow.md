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
        HTTP->>PS: save_route(tokens, backend_id)
    end

    HTTP->>CP: put(connection)
    HTTP-->>Client: response
```

## Key Points

1. **Tokenization**: Request body is tokenized to create the lookup key
2. **Prefix Lookup**: RadixTree finds the longest matching prefix
3. **Cache Hit**: Route directly to the backend that owns this prefix
4. **Cache Miss**: Pick a random healthy backend
5. **Snooping**: On successful response, learn the route for future requests
6. **Persistence**: Learned routes are saved to SQLite for durability

## Backend Registration Flow

```mermaid
sequenceDiagram
    participant Admin
    participant HTTP as HttpController
    participant RS as RouterService
    participant PS as SQLite
    participant HS as HealthService

    Admin->>HTTP: POST /admin/backends?id=1&ip=X&port=Y
    HTTP->>PS: save_backend(id, ip, port)
    HTTP->>RS: register_backend_global(id, addr)

    Note over RS: Broadcast to all CPU shards

    RS-->>HTTP: success
    HTTP-->>Admin: {"status": "ok"}

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
    participant PS as SQLite

    Admin->>HTTP: DELETE /admin/backends?id=1
    HTTP->>PS: remove_routes_for_backend(id)
    HTTP->>PS: remove_backend(id)
    HTTP->>RS: unregister_backend_global(id)

    Note over RS: Broadcast removal to all CPU shards

    RS-->>HTTP: success
    HTTP-->>Admin: {"status": "ok", "backend_deleted": 1}
```
