# Ranvier Core - TODO / Feature Requests

This file tracks potential enhancements and feature requests for Ranvier Core.

---

## Feature Requests

### Accept pre-tokenized input from clients

**Status:** Proposed
**Priority:** Medium
**Complexity:** Medium

#### Summary

Add support for clients to send pre-tokenized input (`prompt_token_ids`) that the router uses directly for prefix-cache routing, instead of always tokenizing text internally.

#### Current Behavior

The router always tokenizes the text itself for routing purposes:

```cpp
// http_controller.cpp:276-285
auto extracted_text = RequestRewriter::extract_text(body);
if (extracted_text.has_value()) {
    tokens = _tokenizer.encode(extracted_text.value());  // Always tokenizes
}
```

If a client sends `prompt_token_ids`, the router:
1. Ignores them for routing/prefix-caching
2. Tokenizes the text itself
3. Only preserves client's tokens (won't overwrite when forwarding)

#### Proposed Behavior

Add a config option (e.g., `accept_client_tokens`) that allows the router to use client-provided `prompt_token_ids` for routing:

```cpp
if (doc.HasMember("prompt_token_ids") && config.accept_client_tokens) {
    tokens = extract_tokens_from_json(doc);  // Use client's tokens
} else {
    tokens = _tokenizer.encode(text);  // Tokenize ourselves
}
```

#### Benefits

- **Skip redundant tokenization** in the router for high-throughput clients
- **Guarantee consistency** - client controls exact tokens used for routing
- **Support token-native clients** - systems that already work with tokens

#### Considerations

- Client must use the **same tokenizer** as the router (vocabulary mismatch = broken routing)
- May need validation that token IDs are legitimate (security)
- Text field should still be required for logging/debugging
- Could add a validation mode that compares client tokens vs router-computed tokens

#### Use Cases

- High-throughput clients that tokenize once and reuse across retries
- Systems with expensive tokenization (very long prompts)
- Architectures where an upstream service already tokenizes

---

### Absolute performance benchmarking

**Status:** Proposed
**Priority:** Medium
**Complexity:** Low-Medium

#### Summary

Extend the existing Locust benchmark infrastructure (`make benchmark`) to support absolute performance measurements against real vLLM backends, not just relative comparisons using the mock backend.

#### Current Behavior

The benchmark setup measures TTFT (Time To First Token) using a mock backend that responds instantly. This is useful for:
- A/B comparisons (e.g., token forwarding on vs off)
- Detecting regressions in router overhead
- Testing under load

But the numbers are **relative** - they don't represent real-world inference performance.

#### Proposed Changes

1. **Add comprehensive metrics to locustfile.py:**
   - Tokens per second (throughput)
   - Inter-token latency (time between consecutive SSE chunks)
   - Total generation time
   - Input/output token counts (parse from SSE `usage` field)

2. **Create `docker-compose.benchmark.yml`** as a template for real backend benchmarking:
   - Environment variables for external vLLM endpoints
   - No mock backend dependency
   - Documentation for required backend configuration

3. **Add `make benchmark-external` target** that:
   - Skips mock backend startup
   - Requires `VLLM_ENDPOINTS` environment variable
   - Outputs all metrics to CSV

4. **Update tests/integration/README.md** with:
   - Requirements for reproducible absolute benchmarks (hardware, environment)
   - Standardized workload recommendations (prompt lengths, models)
   - How to interpret absolute vs relative numbers

#### Benefits

- Measure real inference performance through the router
- Compare against direct vLLM access (router overhead)
- Publishable benchmark numbers for documentation

#### Considerations

- Requires access to real vLLM instances with loaded models
- Results vary by hardware - document reference environment
- May want standardized prompt sets (short, medium, long)

---

## Completed

_Move completed items here with completion date._
