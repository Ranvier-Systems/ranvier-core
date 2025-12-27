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

## Completed

_Move completed items here with completion date._
