# Locust ↔ C++ Sync Map

When modifying either the C++ router or the Python load test files, check this
map to ensure both sides stay in sync.  Mismatches cause **silent** wrong
metrics (cache-hit tracking, latency breakdowns) rather than visible errors.

Last verified: 2026-02-20

## Files involved

| Python | C++ |
|--------|-----|
| `tests/integration/locustfile_real.py` | `src/router_service.cpp` |
| `tests/integration/locustfile.py` | `src/http_controller.cpp` |
| `tests/integration/prompt_loader.py` | `src/request_rewriter.hpp` |
| `tests/integration/README.md` | `src/metrics_service.hpp` |
| | `src/gossip_service.cpp` |

## Hash pipeline (router_service.cpp)

Most fragile coupling — if any value drifts, cache-hit tracking silently
breaks for the `CLIENT_TOKENIZE` fallback path.

| Python | C++ | Notes |
|--------|-----|-------|
| `FNV_OFFSET_BASIS = 14695981039346656037` | `FNV_OFFSET_BASIS` | Constant |
| `FNV_PRIME = 1099511628211` | `FNV_PRIME` | Constant |
| `DEFAULT_BLOCK_ALIGNMENT = 16` | `config.block_alignment` | Default value |
| `DEFAULT_PREFIX_TOKEN_LENGTH = 128` | `config.prefix_token_length` | Default value |
| `fnv1a_hash_tokens()` | `hash_prefix()` | Algorithm: block alignment, int32 LE byte layout, XOR-then-multiply |
| `jump_consistent_hash()` | `jump_consistent_hash()` | Algorithm: multiply constant `2862933555777941757`, shift/divide |
| `predict_backend_from_hash()` | `get_backend_for_prefix()` step 2 | Two-stage: FNV-1a → JUMP; `bucket + 1` for 1-indexed IDs |

## Text extraction (request_rewriter.hpp)

| Python | C++ | Notes |
|--------|-----|-------|
| `tokenize_system_messages()` | `extract_system_messages()` | Join: `"\n"` between messages, **no** trailing newline |
| `hash_prompt_prefix()` — system role only | `extract_system_messages()` — `role == "system"` | Which messages are the "prefix" |

## Response headers (http_controller.cpp)

| Python | C++ | Notes |
|--------|-----|-------|
| `get_backend_from_response()` → `"X-Backend-ID"` | `add_header("X-Backend-ID", ...)` | Case-sensitive |
| `verify_routing_mode_matches()` → `"X-Routing-Mode"` | `add_header("X-Routing-Mode", ...)` | Values: `"prefix"`, `"hash"`, `"random"` |

## Admin API (http_controller.cpp)

| Python | C++ | Notes |
|--------|-----|-------|
| `POST /admin/backends?id=&ip=&port=` | Route registration | Path, method, query param names |

## Prometheus metrics

Seastar auto-prefixes `seastar_` + group name (`ranvier`).  Python must use
the full exported name; C++ registers without the prefix.

| Python queries | C++ registers (in group `"ranvier"`) | Source file |
|----------------|---------------------------------------|-------------|
| `seastar_ranvier_router_routing_latency_seconds` | `router_routing_latency_seconds` | metrics_service.hpp |
| `seastar_ranvier_router_tokenization_latency_seconds` | `router_tokenization_latency_seconds` | metrics_service.hpp |
| `seastar_ranvier_router_primary_tokenization_latency_seconds` | `router_primary_tokenization_latency_seconds` | metrics_service.hpp |
| `seastar_ranvier_router_boundary_detection_latency_seconds` | `router_boundary_detection_latency_seconds` | metrics_service.hpp |
| `seastar_ranvier_backend_connect_duration_seconds` | `backend_connect_duration_seconds` | metrics_service.hpp |
| `seastar_ranvier_prefix_boundary_used` | `prefix_boundary_used` | metrics_service.hpp |
| `seastar_ranvier_prefix_boundary_skipped` | `prefix_boundary_skipped` | metrics_service.hpp |
| `router_cluster_sync_invalid` | `router_cluster_sync_invalid` | gossip_service.cpp |
| `router_cluster_sync_untrusted` | `router_cluster_sync_untrusted` | gossip_service.cpp |

Note: the gossip metrics above do **not** get the `seastar_ranvier_` prefix in
the Python code because `get_metric_value()` does a substring match — the full
exported name is `seastar_ranvier_router_cluster_sync_invalid` but the
substring `router_cluster_sync_invalid` matches it.

## Request body schema (http_controller.cpp, request_rewriter.hpp)

| Python field | C++ extractor | Notes |
|--------------|--------------|-------|
| `"messages"` array with `"role"` / `"content"` | `extract_text()`, `extract_system_messages()` | JSON field names |
| `"prompt_token_ids"` in `/v1/completions` body | `extract_prompt_token_ids()` | Array of int32 |
| `"prefix_token_count"` in request body | `extract_prefix_token_count()` | Positive int, must be < total token count |
| `"stream": True` | SSE streaming path | Response format |

## Checklist for changes

- [ ] **Renaming a metric?** Update both C++ registration and Python query string.
- [ ] **Changing hash constants or algorithm?** Update both `router_service.cpp` and `locustfile_real.py`.
- [ ] **Changing `extract_system_messages()` format?** Update `tokenize_system_messages()` in Python.
- [ ] **Adding/removing response headers?** Update `get_backend_from_response()` or `verify_routing_mode_matches()`.
- [ ] **Changing admin API params?** Update `register_backends_on_all_nodes()` in both locustfiles.
- [ ] **Adding a new hash strategy?** Document in `predict_backend_from_hash()` docstring; consider Python impl.
