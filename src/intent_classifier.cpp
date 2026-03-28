// Ranvier Core - Intent Classification Implementation

#include "intent_classifier.hpp"

#include <algorithm>

namespace ranvier {

RequestIntent classify_intent(std::string_view endpoint,
                              std::string_view body_view,
                              const IntentClassifierConfig& config) {

    // Step 1: FIM detection — check for top-level FIM fields in body.
    // Uses substring search on raw JSON to avoid re-parsing (the fields are
    // distinctive enough that false positives are negligible for classification).
    for (const auto& field : config.fim_fields) {
        // Search for "field_name" pattern (quoted JSON key)
        std::string quoted = "\"" + field + "\"";
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

        // Extract a reasonable window of the system content for scanning.
        // We don't need to find the exact closing quote — scanning the first
        // ~2KB of system content is sufficient for keyword detection.
        size_t scan_end = std::min(quote_start + 2048, body_view.size());
        std::string_view system_content = body_view.substr(quote_start, scan_end - quote_start);

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
