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

#include <cstdint>
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
