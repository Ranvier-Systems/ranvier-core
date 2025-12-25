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

inline void RequestRewriter::write_token_array(
    rapidjson::Writer<rapidjson::StringBuffer>& writer,
    const std::vector<int32_t>& tokens) {

    writer.StartArray();
    for (int32_t token : tokens) {
        writer.Int(token);
    }
    writer.EndArray();
}

}  // namespace ranvier
