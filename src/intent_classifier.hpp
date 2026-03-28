// Ranvier Core - Intent Classification
//
// Pure computational inspection of request wire format to classify intent as
// AUTOCOMPLETE (FIM/completions), EDIT (code rewrite), or CHAT (default).
// The classified intent provides routing hints for downstream decisions
// (e.g., prefer lowest-latency backend for autocomplete, highest-capability
// for edits).
//
// Detection cascade:
//   1. FIM field detection (top-level JSON keys) → AUTOCOMPLETE
//   2. /v1/completions endpoint → AUTOCOMPLETE
//   3. Edit keyword/tag scan in first system message → EDIT
//   4. Fallback → CHAT
//
// Pure function: no I/O, no Seastar types, no side effects.
// Designed for easy unit testing with raw string bodies.

#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ranvier {

// =============================================================================
// RequestIntent Enum
// =============================================================================

enum class RequestIntent : uint8_t {
    AUTOCOMPLETE = 0,  // FIM / inline completion — prefer fastest backend
    CHAT         = 1,  // Interactive conversation — normal routing (default)
    EDIT         = 2,  // Code rewrite / refactor — prefer smartest backend
};

// Convert RequestIntent to string label for metrics and logging
inline std::string_view intent_to_string(RequestIntent intent) {
    switch (intent) {
        case RequestIntent::AUTOCOMPLETE: return "autocomplete";
        case RequestIntent::CHAT:         return "chat";
        case RequestIntent::EDIT:         return "edit";
    }
    return "chat";  // unreachable, but satisfies -Wreturn-type
}

// =============================================================================
// IntentClassifierConfig
// =============================================================================
// Lightweight config struct consumed by classify_intent().  Populated from
// IntentClassificationConfig (config_schema.hpp) at controller init time.

struct IntentClassifierConfig {
    // FIM field names — if any appears as a top-level JSON key, → AUTOCOMPLETE
    std::vector<std::string> fim_fields = {"suffix", "fim_prefix", "fim_middle", "fim_suffix"};

    // Pre-computed quoted FIM field names for hot-path substring search.
    // Each entry is "\"field\"" — avoids heap allocation per request.
    // Populated by rebuild_quoted_fields(), which must be called after
    // modifying fim_fields.
    std::vector<std::string> fim_fields_quoted;

    // Keywords matched case-insensitively in the first system message → EDIT
    std::vector<std::string> edit_system_keywords = {"diff", "rewrite", "refactor", "edit", "patch", "apply"};

    // Tag patterns matched literally in the first system message → EDIT
    std::vector<std::string> edit_tag_patterns = {"<diff>", "<edit>", "<rewrite>", "<patch>"};

    // Rebuild fim_fields_quoted from fim_fields. Called automatically by the
    // constructor; must also be called after modifying fim_fields at runtime
    // (e.g., config reload, test setup).
    void rebuild_quoted_fields() {
        fim_fields_quoted.clear();
        fim_fields_quoted.reserve(fim_fields.size());
        for (const auto& f : fim_fields) {
            fim_fields_quoted.push_back("\"" + f + "\"");
        }
    }

    IntentClassifierConfig() { rebuild_quoted_fields(); }
};

// =============================================================================
// classify_intent — free function
// =============================================================================
// Classifies request intent from the API endpoint and raw JSON body.
// Does NOT re-parse JSON; uses substring matching on the raw body which is
// sufficient for the distinctive field names and keyword patterns involved.
//
// @param endpoint  API path (e.g., "/v1/chat/completions" or "/v1/completions")
// @param body_view Raw JSON request body
// @param config    Classification parameters (field names, keywords, tags)
// @return          Classified RequestIntent
RequestIntent classify_intent(std::string_view endpoint,
                              std::string_view body_view,
                              const IntentClassifierConfig& config);

}  // namespace ranvier
