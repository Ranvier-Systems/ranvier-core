#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ranvier {

/**
 * Sanitize a string for use as an HTTP header value.
 * Strips CR (\r) and LF (\n) characters to prevent header injection attacks.
 *
 * @param input The raw header value (may contain user-controlled content)
 * @return Sanitized string safe for header interpolation
 */
inline std::string sanitize_header_value(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    for (char c : input) {
        if (c != '\r' && c != '\n') {
            result += c;
        }
    }
    return result;
}

/**
 * Escape a string for safe inclusion in a JSON string value.
 * Handles double quotes, backslashes, control characters per RFC 8259.
 *
 * @param s The raw string to escape
 * @return JSON-safe escaped string (without surrounding quotes)
 */
inline std::string escape_json_string(const std::string& s) {
    std::string result;
    result.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    result += buf;
                } else {
                    result += c;
                }
        }
    }
    return result;
}

/**
 * Safe integer parsing utilities using std::from_chars.
 *
 * These functions replace bare std::stoi/stoul calls to avoid exception-based
 * error handling on external input (Rule #10). They use std::from_chars which:
 * - Returns error codes instead of throwing exceptions
 * - Has no locale dependency (faster, deterministic)
 * - Requires exact parsing (no trailing garbage allowed)
 */

/**
 * Parse a string as int32_t.
 *
 * @param input String to parse (must contain only digits, optional leading minus)
 * @return Parsed value, or std::nullopt if invalid/out of range
 */
inline std::optional<int32_t> parse_int32(std::string_view input) {
    if (input.empty()) {
        return std::nullopt;
    }

    int32_t value{};
    auto [ptr, ec] = std::from_chars(input.data(), input.data() + input.size(), value);

    // Require exact match - no trailing characters allowed
    if (ec != std::errc{} || ptr != input.data() + input.size()) {
        return std::nullopt;
    }

    return value;
}

/**
 * Parse a string as uint32_t.
 *
 * @param input String to parse (must contain only digits, no sign)
 * @return Parsed value, or std::nullopt if invalid/out of range/negative
 */
inline std::optional<uint32_t> parse_uint32(std::string_view input) {
    if (input.empty()) {
        return std::nullopt;
    }

    // Reject negative numbers (from_chars would parse "-1" as a large unsigned)
    if (input[0] == '-') {
        return std::nullopt;
    }

    uint32_t value{};
    auto [ptr, ec] = std::from_chars(input.data(), input.data() + input.size(), value);

    if (ec != std::errc{} || ptr != input.data() + input.size()) {
        return std::nullopt;
    }

    return value;
}

/**
 * Parse a string as a valid TCP/UDP port number (1-65535).
 *
 * @param input String to parse
 * @return Parsed port, or std::nullopt if invalid/out of range
 */
inline std::optional<uint16_t> parse_port(std::string_view input) {
    if (input.empty()) {
        return std::nullopt;
    }

    // Reject negative numbers
    if (input[0] == '-') {
        return std::nullopt;
    }

    // Parse as uint32_t first to detect overflow for uint16_t range
    uint32_t value{};
    auto [ptr, ec] = std::from_chars(input.data(), input.data() + input.size(), value);

    if (ec != std::errc{} || ptr != input.data() + input.size()) {
        return std::nullopt;
    }

    // Valid port range: 1-65535
    if (value < 1 || value > 65535) {
        return std::nullopt;
    }

    return static_cast<uint16_t>(value);
}

/**
 * Parse a string as TokenId (int32_t alias).
 * Convenience wrapper with clearer semantics for token parsing.
 *
 * @param input String to parse
 * @return Parsed token ID, or std::nullopt if invalid
 */
inline std::optional<int32_t> parse_token_id(std::string_view input) {
    return parse_int32(input);
}

/**
 * Parse a string as BackendId (int32_t alias).
 * Convenience wrapper with clearer semantics for backend ID parsing.
 *
 * @param input String to parse
 * @return Parsed backend ID, or std::nullopt if invalid
 */
inline std::optional<int32_t> parse_backend_id(std::string_view input) {
    return parse_int32(input);
}

} // namespace ranvier
