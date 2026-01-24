// Ranvier Core - Request Rewriter
//
// Rewrites incoming LLM requests to include pre-computed token IDs.
// This allows vLLM backends to skip tokenization, improving latency
// and ensuring routing consistency (same tokens used for cache lookup).
//
// Supports vLLM "Online Serving" schema:
// - If prompt_token_ids is present, backend skips internal tokenization
// - Original prompt field is preserved for logging/debugging

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

namespace ranvier {

// Result of request rewriting
struct RewriteResult {
    std::string body;           // Rewritten request body (or original on failure)
    bool success;               // Whether rewriting was successful
    std::string error;          // Error message if rewriting failed
};

// Request rewriter for injecting token IDs into LLM requests
class RequestRewriter {
public:
    // Rewrite the request body to include prompt_token_ids
    //
    // The rewriter looks for:
    // 1. "prompt" field (string) - common in completion APIs
    // 2. "messages" field (array) - common in chat completion APIs
    //
    // If tokens are provided and the request has a prompt/messages field,
    // the rewriter adds a "prompt_token_ids" array with the pre-computed tokens.
    //
    // Parameters:
    //   body: Original request body (JSON string)
    //   tokens: Pre-computed token IDs from TokenizerService
    //
    // Returns:
    //   RewriteResult with the modified body and success status
    //
    // Thread-safe: This function is stateless and can be called concurrently
    static RewriteResult rewrite(std::string_view body, const std::vector<int32_t>& tokens);

    // Extract the text to tokenize from a request body
    //
    // For "prompt" field: Returns the prompt string directly
    // For "messages" field: Concatenates all message content strings
    //
    // This is used to ensure we tokenize exactly what we'll send token IDs for.
    //
    // Parameters:
    //   body: Request body (JSON string)
    //
    // Returns:
    //   The extracted text, or nullopt if no tokenizable content found
    static std::optional<std::string> extract_text(std::string_view body);

    // Extract only system message content from a chat completion request
    //
    // For prefix-aware routing, we need to identify the "shared prefix" boundary.
    // In multi-turn conversations, all requests typically share the same system
    // message(s), while user/assistant content varies. By tokenizing system
    // messages separately, we can store routes at this shared prefix boundary.
    //
    // For "messages" field: Concatenates content from messages with role="system"
    // For "prompt" field: Returns nullopt (no system message concept)
    //
    // Parameters:
    //   body: Request body (JSON string)
    //
    // Returns:
    //   The concatenated system message content, or nullopt if none found
    static std::optional<std::string> extract_system_messages(std::string_view body);

    // Result of extracting message boundaries from a request
    struct MessageBoundaries {
        std::vector<size_t> boundaries;  // Cumulative token counts at each message boundary
        size_t system_boundary = 0;      // Token count at end of system messages (first non-system)
    };

    // Extract message boundaries from a chat completion request
    //
    // This identifies natural prefix boundaries in multi-turn conversations by
    // calculating cumulative token counts at each message. Used for multi-depth
    // route storage (Option C) to enable cache reuse at any conversation depth.
    //
    // For a request like:
    //   [system: 256 tokens][user: 50 tokens][assistant: 100 tokens][user: 30 tokens]
    //
    // Returns boundaries: [256, 306, 406, 436]
    //   - 256: end of system message
    //   - 306: end of first user message
    //   - 406: end of assistant response
    //   - 436: end of second user message
    //
    // Parameters:
    //   body: Request body (JSON string)
    //   tokenize_fn: Function to tokenize message content (returns token count)
    //
    // Returns:
    //   MessageBoundaries with cumulative boundaries, or nullopt if parsing fails
    //
    // Note: This requires a tokenizer callback since boundaries are in token space.
    //       The callback should match the tokenizer used for routing.
    static std::optional<MessageBoundaries> extract_message_boundaries(
        std::string_view body,
        std::function<size_t(const std::string&)> tokenize_fn);

    // Extract client-provided prefix_token_count from a request body
    //
    // Clients can specify how many tokens constitute their "shared prefix" for
    // routing purposes. This is useful when clients know their prefix structure
    // better than the router's automatic system message detection.
    //
    // For example, if a client has a 1000-token system prompt, they can include
    // "prefix_token_count": 1000 in their request. The router will use this value
    // instead of automatically detecting system messages.
    //
    // Parameters:
    //   body: Request body (JSON string)
    //
    // Returns:
    //   The prefix_token_count value, or nullopt if not present/invalid
    //
    // Validation:
    //   - Must be a positive integer
    //   - Zero or negative values are rejected (returns nullopt)
    static std::optional<size_t> extract_prefix_token_count(std::string_view body);

    // Extract client-provided prefix boundaries (array form) from a request body
    //
    // Clients can specify multiple prefix boundaries for multi-depth route storage.
    // This enables routes to be stored at multiple conversation depths for optimal
    // cache reuse in branching or continuing conversations.
    //
    // Example: "prefix_boundaries": [256, 306, 406]
    //   - Routes stored at token 256 (system), 306 (system+user1), 406 (system+user1+asst1)
    //
    // Parameters:
    //   body: Request body (JSON string)
    //
    // Returns:
    //   Vector of boundaries (sorted, deduplicated), or empty if not present/invalid
    static std::vector<size_t> extract_prefix_boundaries(std::string_view body);

    // Result of extracting prompt_token_ids from a request
    struct TokenExtractionResult {
        std::vector<int32_t> tokens;  // Extracted token IDs (empty if not found)
        bool found;                    // Whether prompt_token_ids field was present
        bool valid;                    // Whether tokens passed validation
        std::string error;             // Error message if validation failed
    };

    // Extract pre-tokenized prompt_token_ids from a request body
    //
    // Clients can send pre-computed token IDs to skip router tokenization.
    // This is useful for high-throughput clients that want to guarantee
    // consistency with their own tokenization.
    //
    // Parameters:
    //   body: Request body (JSON string)
    //   max_token_id: Maximum valid token ID for security validation
    //
    // Returns:
    //   TokenExtractionResult with extracted tokens and validation status
    //
    // Security notes:
    //   - Token IDs are validated to be within [0, max_token_id]
    //   - Negative token IDs are rejected
    //   - Array must not be empty if present
    static TokenExtractionResult extract_prompt_token_ids(std::string_view body, int32_t max_token_id);

    // Strip prompt_token_ids from a request body
    //
    // This is used when forwarding to backends that don't support prompt_token_ids
    // (e.g., vLLM's /v1/chat/completions endpoint).
    //
    // Parameters:
    //   body: Request body (JSON string) that may contain prompt_token_ids
    //
    // Returns:
    //   Modified body with prompt_token_ids removed, or original if not present
    static std::string strip_prompt_token_ids(std::string_view body);

private:
    // Internal helper to write token array to JSON
    static void write_token_array(rapidjson::Writer<rapidjson::StringBuffer>& writer,
                                  const std::vector<int32_t>& tokens);
};

// Implementation

inline RewriteResult RequestRewriter::rewrite(std::string_view body,
                                               const std::vector<int32_t>& tokens) {
    // Fast path: if no tokens, return original body
    if (tokens.empty()) {
        return RewriteResult{std::string(body), false, "No tokens provided"};
    }

    // Parse the JSON document
    rapidjson::Document doc;
    doc.Parse(body.data(), body.size());

    if (doc.HasParseError()) {
        return RewriteResult{std::string(body), false, "JSON parse error"};
    }

    if (!doc.IsObject()) {
        return RewriteResult{std::string(body), false, "Request body is not a JSON object"};
    }

    // Check if this request has tokenizable content (prompt or messages)
    bool has_prompt = doc.HasMember("prompt") && doc["prompt"].IsString();
    bool has_messages = doc.HasMember("messages") && doc["messages"].IsArray();

    if (!has_prompt && !has_messages) {
        return RewriteResult{std::string(body), false, "No prompt or messages field found"};
    }

    // Check if prompt_token_ids already exists (don't overwrite)
    if (doc.HasMember("prompt_token_ids")) {
        return RewriteResult{std::string(body), false, "prompt_token_ids already present"};
    }

    // Add prompt_token_ids array
    rapidjson::Value token_array(rapidjson::kArrayType);
    token_array.Reserve(static_cast<rapidjson::SizeType>(tokens.size()), doc.GetAllocator());

    for (int32_t token : tokens) {
        token_array.PushBack(token, doc.GetAllocator());
    }

    doc.AddMember("prompt_token_ids", token_array, doc.GetAllocator());

    // Serialize back to string
    rapidjson::StringBuffer buffer;
    buffer.Reserve(body.size() + tokens.size() * 8);  // Estimate: ~8 chars per token ID

    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    return RewriteResult{
        std::string(buffer.GetString(), buffer.GetSize()),
        true,
        ""
    };
}

inline std::optional<std::string> RequestRewriter::extract_text(std::string_view body) {
    rapidjson::Document doc;
    doc.Parse(body.data(), body.size());

    if (doc.HasParseError() || !doc.IsObject()) {
        return std::nullopt;
    }

    // Check for "prompt" field first (completion API style)
    if (doc.HasMember("prompt") && doc["prompt"].IsString()) {
        return std::string(doc["prompt"].GetString(), doc["prompt"].GetStringLength());
    }

    // Check for "messages" field (chat completion API style)
    // Concatenate all message content for tokenization
    if (doc.HasMember("messages") && doc["messages"].IsArray()) {
        std::string combined;
        const auto& messages = doc["messages"];

        for (rapidjson::SizeType i = 0; i < messages.Size(); ++i) {
            const auto& msg = messages[i];
            if (msg.IsObject() && msg.HasMember("content")) {
                const auto& content = msg["content"];
                if (content.IsString()) {
                    if (!combined.empty()) {
                        combined += "\n";  // Separator between messages
                    }
                    combined.append(content.GetString(), content.GetStringLength());
                }
            }
        }

        if (!combined.empty()) {
            return combined;
        }
    }

    return std::nullopt;
}

inline std::optional<std::string> RequestRewriter::extract_system_messages(std::string_view body) {
    rapidjson::Document doc;
    doc.Parse(body.data(), body.size());

    if (doc.HasParseError() || !doc.IsObject()) {
        return std::nullopt;
    }

    // System messages only exist in chat completion API (messages field)
    // The completion API (prompt field) has no system message concept
    if (!doc.HasMember("messages") || !doc["messages"].IsArray()) {
        return std::nullopt;
    }

    std::string combined;
    const auto& messages = doc["messages"];

    for (rapidjson::SizeType i = 0; i < messages.Size(); ++i) {
        const auto& msg = messages[i];
        if (!msg.IsObject()) continue;

        // Check for role="system"
        if (!msg.HasMember("role") || !msg["role"].IsString()) continue;
        std::string_view role(msg["role"].GetString(), msg["role"].GetStringLength());
        if (role != "system") continue;

        // Extract content
        if (!msg.HasMember("content")) continue;
        const auto& content = msg["content"];
        if (content.IsString()) {
            if (!combined.empty()) {
                combined += "\n";  // Separator between system messages
            }
            combined.append(content.GetString(), content.GetStringLength());
        }
    }

    if (combined.empty()) {
        return std::nullopt;
    }

    return combined;
}

inline std::optional<size_t> RequestRewriter::extract_prefix_token_count(std::string_view body) {
    // Fast path: if prefix_token_count not present, return early
    if (body.find("prefix_token_count") == std::string_view::npos) {
        return std::nullopt;
    }

    rapidjson::Document doc;
    doc.Parse(body.data(), body.size());

    if (doc.HasParseError() || !doc.IsObject()) {
        return std::nullopt;
    }

    // Check if prefix_token_count field exists
    if (!doc.HasMember("prefix_token_count")) {
        return std::nullopt;
    }

    const auto& value = doc["prefix_token_count"];

    // Must be a positive integer
    if (value.IsUint64()) {
        uint64_t count = value.GetUint64();
        if (count > 0) {
            return static_cast<size_t>(count);
        }
    } else if (value.IsInt64()) {
        int64_t count = value.GetInt64();
        if (count > 0) {
            return static_cast<size_t>(count);
        }
    } else if (value.IsUint()) {
        uint32_t count = value.GetUint();
        if (count > 0) {
            return static_cast<size_t>(count);
        }
    } else if (value.IsInt()) {
        int32_t count = value.GetInt();
        if (count > 0) {
            return static_cast<size_t>(count);
        }
    }

    // Not a valid positive integer
    return std::nullopt;
}

inline std::vector<size_t> RequestRewriter::extract_prefix_boundaries(std::string_view body) {
    // Fast path: if prefix_boundaries not present, return early
    if (body.find("prefix_boundaries") == std::string_view::npos) {
        return {};
    }

    rapidjson::Document doc;
    doc.Parse(body.data(), body.size());

    if (doc.HasParseError() || !doc.IsObject()) {
        return {};
    }

    // Check if prefix_boundaries field exists and is an array
    if (!doc.HasMember("prefix_boundaries") || !doc["prefix_boundaries"].IsArray()) {
        return {};
    }

    const auto& arr = doc["prefix_boundaries"];
    std::vector<size_t> boundaries;
    boundaries.reserve(arr.Size());

    for (rapidjson::SizeType i = 0; i < arr.Size(); ++i) {
        const auto& value = arr[i];
        size_t boundary = 0;

        if (value.IsUint64()) {
            uint64_t v = value.GetUint64();
            if (v > 0) boundary = static_cast<size_t>(v);
        } else if (value.IsInt64()) {
            int64_t v = value.GetInt64();
            if (v > 0) boundary = static_cast<size_t>(v);
        } else if (value.IsUint()) {
            uint32_t v = value.GetUint();
            if (v > 0) boundary = static_cast<size_t>(v);
        } else if (value.IsInt()) {
            int32_t v = value.GetInt();
            if (v > 0) boundary = static_cast<size_t>(v);
        }

        if (boundary > 0) {
            boundaries.push_back(boundary);
        }
    }

    // Sort and deduplicate
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

    return boundaries;
}

inline std::optional<RequestRewriter::MessageBoundaries> RequestRewriter::extract_message_boundaries(
    std::string_view body,
    std::function<size_t(const std::string&)> tokenize_fn) {

    if (!tokenize_fn) {
        return std::nullopt;
    }

    rapidjson::Document doc;
    doc.Parse(body.data(), body.size());

    if (doc.HasParseError() || !doc.IsObject()) {
        return std::nullopt;
    }

    // Only works with chat completion API (messages field)
    if (!doc.HasMember("messages") || !doc["messages"].IsArray()) {
        return std::nullopt;
    }

    const auto& messages = doc["messages"];
    if (messages.Size() == 0) {
        return std::nullopt;
    }

    MessageBoundaries result;
    size_t cumulative_tokens = 0;
    bool found_non_system = false;

    for (rapidjson::SizeType i = 0; i < messages.Size(); ++i) {
        const auto& msg = messages[i];
        if (!msg.IsObject()) continue;

        // Get role
        if (!msg.HasMember("role") || !msg["role"].IsString()) continue;
        std::string_view role(msg["role"].GetString(), msg["role"].GetStringLength());

        // Get content
        if (!msg.HasMember("content")) continue;
        const auto& content = msg["content"];
        if (!content.IsString()) continue;

        // Build the formatted message (matching tokenize_messages format)
        std::string formatted = "<|" + std::string(role) + "|>\n";
        formatted.append(content.GetString(), content.GetStringLength());

        // Tokenize and accumulate
        size_t token_count = tokenize_fn(formatted);
        cumulative_tokens += token_count;

        // Track system boundary (first non-system message starts user content)
        if (!found_non_system && role != "system") {
            result.system_boundary = cumulative_tokens - token_count;  // Boundary is BEFORE this message
            found_non_system = true;
        }

        // Store boundary after each message
        result.boundaries.push_back(cumulative_tokens);
    }

    // If all messages were system messages, system_boundary is at the end
    if (!found_non_system && cumulative_tokens > 0) {
        result.system_boundary = cumulative_tokens;
    }

    if (result.boundaries.empty()) {
        return std::nullopt;
    }

    return result;
}

inline RequestRewriter::TokenExtractionResult RequestRewriter::extract_prompt_token_ids(
    std::string_view body, int32_t max_token_id) {

    TokenExtractionResult result;
    result.found = false;
    result.valid = false;

    rapidjson::Document doc;
    doc.Parse(body.data(), body.size());

    if (doc.HasParseError() || !doc.IsObject()) {
        result.error = "Invalid JSON";
        return result;
    }

    // Check if prompt_token_ids field exists
    if (!doc.HasMember("prompt_token_ids")) {
        // Not found is not an error - just means we should tokenize normally
        return result;
    }

    result.found = true;

    const auto& token_array = doc["prompt_token_ids"];
    if (!token_array.IsArray()) {
        result.error = "prompt_token_ids must be an array";
        return result;
    }

    if (token_array.Empty()) {
        result.error = "prompt_token_ids array is empty";
        return result;
    }

    // Reserve space for tokens
    result.tokens.reserve(token_array.Size());

    // Extract and validate each token
    for (rapidjson::SizeType i = 0; i < token_array.Size(); ++i) {
        const auto& elem = token_array[i];

        if (!elem.IsInt()) {
            result.error = "prompt_token_ids[" + std::to_string(i) + "] is not an integer";
            result.tokens.clear();
            return result;
        }

        int32_t token_id = elem.GetInt();

        // Security validation: check token ID bounds
        if (token_id < 0) {
            result.error = "prompt_token_ids[" + std::to_string(i) + "] is negative: " + std::to_string(token_id);
            result.tokens.clear();
            return result;
        }

        if (token_id > max_token_id) {
            result.error = "prompt_token_ids[" + std::to_string(i) + "] exceeds max_token_id (" +
                           std::to_string(token_id) + " > " + std::to_string(max_token_id) + ")";
            result.tokens.clear();
            return result;
        }

        result.tokens.push_back(token_id);
    }

    result.valid = true;
    return result;
}

inline void RequestRewriter::write_token_array(
    rapidjson::Writer<rapidjson::StringBuffer>& writer,
    const std::vector<int32_t>& tokens) {

    writer.StartArray();
    for (int32_t token : tokens) {
        writer.Int(token);
    }
    writer.EndArray();
}

inline std::string RequestRewriter::strip_prompt_token_ids(std::string_view body) {
    // Fast path: if prompt_token_ids not present, return original
    if (body.find("prompt_token_ids") == std::string_view::npos) {
        return std::string(body);
    }

    // Parse the JSON document
    rapidjson::Document doc;
    doc.Parse(body.data(), body.size());

    if (doc.HasParseError() || !doc.IsObject()) {
        return std::string(body);
    }

    // Check if prompt_token_ids exists
    if (!doc.HasMember("prompt_token_ids")) {
        return std::string(body);
    }

    // Remove the prompt_token_ids field
    doc.RemoveMember("prompt_token_ids");

    // Serialize back to string
    rapidjson::StringBuffer buffer;
    buffer.Reserve(body.size());

    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    return std::string(buffer.GetString(), buffer.GetSize());
}

}  // namespace ranvier
