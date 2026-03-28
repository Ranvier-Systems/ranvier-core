// Ranvier Core - Intent Classification Implementation

#include "intent_classifier.hpp"

#include <algorithm>

namespace ranvier {

RequestIntent classify_intent(std::string_view endpoint,
                              std::string_view body_view,
                              const IntentClassifierConfig& config) {

    // Step 1: FIM detection — check for top-level FIM fields in body.
    // Uses pre-computed quoted field names ("\"suffix\"", etc.) to avoid
    // heap allocation per request on the hot path.
    for (const auto& quoted : config.fim_fields_quoted) {
        if (body_view.find(quoted) != std::string_view::npos) {
            return RequestIntent::AUTOCOMPLETE;
        }
    }

    // Step 2: Endpoint-based FIM detection
    if (endpoint == "/v1/completions") {
        return RequestIntent::AUTOCOMPLETE;
    }

    // Step 3: Edit detection — scan first system message for keywords/tags.
    // Find the first system message content without full JSON parsing.
    // Look for "role":"system" (or "role": "system") then extract its "content" value.
    auto system_pos = body_view.find("\"role\"");
    while (system_pos != std::string_view::npos) {
        // Check if this role is "system"
        auto role_value_start = body_view.find("\"system\"", system_pos);
        if (role_value_start == std::string_view::npos ||
            role_value_start > system_pos + 30) {
            // Not a system role at this position, skip to next "role"
            system_pos = body_view.find("\"role\"", system_pos + 6);
            continue;
        }

        // Found a system role message — now find its content
        auto content_key = body_view.find("\"content\"", role_value_start);
        if (content_key == std::string_view::npos) break;

        // Find the content value start (skip past "content":)
        auto colon_pos = body_view.find(':', content_key + 9);
        if (colon_pos == std::string_view::npos) break;

        // Find the opening quote of the content value
        auto quote_start = body_view.find('"', colon_pos + 1);
        if (quote_start == std::string_view::npos) break;

        // Find the closing quote of the content value (skip escaped quotes).
        // Cap the scan at 2KB as a safety limit for very long system messages.
        size_t max_scan = std::min(quote_start + 2048, body_view.size());
        size_t content_end = quote_start + 1;
        while (content_end < max_scan) {
            if (body_view[content_end] == '"') {
                // Count consecutive backslashes before this quote.
                // Even count (including 0) → real closing quote.
                // Odd count → escaped quote (\"), keep scanning.
                size_t backslashes = 0;
                size_t pos = content_end;
                while (pos > quote_start + 1 && body_view[pos - 1] == '\\') {
                    ++backslashes;
                    --pos;
                }
                if (backslashes % 2 == 0) {
                    break;
                }
            }
            ++content_end;
        }
        std::string_view system_content = body_view.substr(quote_start + 1, content_end - quote_start - 1);

        // Case-insensitive keyword matching against system content
        for (const auto& keyword : config.edit_system_keywords) {
            if (keyword.empty()) continue;
            for (size_t i = 0; i + keyword.size() <= system_content.size(); ++i) {
                bool match = true;
                for (size_t j = 0; j < keyword.size(); ++j) {
                    if (std::tolower(static_cast<unsigned char>(system_content[i + j])) !=
                        std::tolower(static_cast<unsigned char>(keyword[j]))) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    return RequestIntent::EDIT;
                }
            }
        }

        // Literal tag pattern matching (case-sensitive)
        for (const auto& tag : config.edit_tag_patterns) {
            if (!tag.empty() && system_content.find(tag) != std::string_view::npos) {
                return RequestIntent::EDIT;
            }
        }

        // Only scan the first system message
        break;
    }

    // Step 4: Default fallback
    return RequestIntent::CHAT;
}

}  // namespace ranvier
