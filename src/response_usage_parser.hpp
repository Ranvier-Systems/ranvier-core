// Ranvier Core - Response-side usage parser (pure)
//
// Extracts the engine's authoritative token counts from an OpenAI-compatible
// response so the usage-ledger sink (§20.2 P1.5) and request_attribution row can
// record ACTUALS instead of pre-flight estimates. Scope memo:
// docs/architecture/response-usage-accounting.md.
//
// Pure + dependency-free (no rapidjson, no Seastar) so it can be unit-tested and
// called cheaply from the streaming snoop. It does NOT full-parse the body: a
// streaming response is an SSE blob (`data: {...}` lines + `[DONE]`), not a
// single JSON value, so we scan for the `usage` keys instead. `prompt_tokens` /
// `completion_tokens` appear only inside the response `usage` object, and the
// quoted-key match (trailing `"`) disambiguates them from `prompt_tokens_details`
// and the like.

#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ranvier {

struct ResponseUsage {
    int64_t prompt_tokens = 0;
    int64_t completion_tokens = 0;
};

namespace usage_parse_detail {

// Find the LAST non-negative integer following a quoted JSON key. Tolerant of
// SSE framing because it scans for the key rather than parsing the blob.
// Returns nullopt if the key never appears followed by an integer.
inline std::optional<int64_t> find_uint_after_key(std::string_view s,
                                                  std::string_view quoted_key) {
    std::optional<int64_t> result;
    size_t pos = 0;
    while ((pos = s.find(quoted_key, pos)) != std::string_view::npos) {
        size_t i = pos + quoted_key.size();
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' ||
                                s[i] == '\r' || s[i] == ':')) {
            ++i;
        }
        const size_t start = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            ++i;
        }
        if (i > start) {
            int64_t v = 0;
            auto [ptr, ec] = std::from_chars(s.data() + start, s.data() + i, v);
            (void)ptr;
            if (ec == std::errc()) {
                result = v;  // last occurrence wins (the usage event)
            }
        }
        pos += quoted_key.size();
    }
    return result;
}

}  // namespace usage_parse_detail

// Extract `usage.{prompt_tokens, completion_tokens}` from a response fragment
// (a non-streaming JSON body, or a streaming SSE blob whose final event carries
// `usage`). Returns nullopt unless BOTH are present, so callers cleanly fall
// back to estimates when the backend didn't report usage (e.g. a streaming
// request without `stream_options.include_usage`).
inline std::optional<ResponseUsage> parse_response_usage(std::string_view data) {
    // Hot path: this runs on every streamed chunk, but `usage` only rides the
    // final one. Probe the rarer key first and short-circuit, so the common
    // (no-usage) chunk costs a single early-exiting scan rather than two.
    const auto completion =
        usage_parse_detail::find_uint_after_key(data, "\"completion_tokens\"");
    if (!completion) {
        return std::nullopt;
    }
    const auto prompt = usage_parse_detail::find_uint_after_key(data, "\"prompt_tokens\"");
    if (prompt) {
        return ResponseUsage{*prompt, *completion};
    }
    return std::nullopt;
}

}  // namespace ranvier
