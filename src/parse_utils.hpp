#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

/**
 * Parse a string as a boolean value.
 * Accepts: "true", "1", "yes" (-> true), "false", "0", "no" (-> false).
 * Case-insensitive comparison to avoid silent misinterpretation of "True", "FALSE", etc.
 *
 * @param input String to parse
 * @return Parsed boolean, or std::nullopt if unrecognized
 */
inline std::optional<bool> parse_bool(std::string_view input) {
    if (input.empty()) {
        return std::nullopt;
    }

    // Case-insensitive comparison via lowercasing (ASCII-safe for these keywords)
    std::string lower;
    lower.reserve(input.size());
    for (char c : input) {
        lower += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }

    if (lower == "true" || lower == "1" || lower == "yes") {
        return true;
    }
    if (lower == "false" || lower == "0" || lower == "no") {
        return false;
    }
    return std::nullopt;
}

/**
 * Parse a string as double.
 * Uses std::strtod with strict validation (Rule #10: no unchecked conversions).
 * Rejects empty input, trailing characters, Inf, and NaN.
 *
 * @param input String to parse
 * @return Parsed value, or std::nullopt if invalid
 */
inline std::optional<double> parse_double(std::string_view input) {
    if (input.empty()) {
        return std::nullopt;
    }

    // std::strtod requires null-terminated string
    std::string str(input);
    char* end = nullptr;
    double value = std::strtod(str.c_str(), &end);

    // Require exact match - no trailing characters allowed
    if (end != str.c_str() + str.size()) {
        return std::nullopt;
    }

    // Reject Inf/NaN
    if (!std::isfinite(value)) {
        return std::nullopt;
    }

    return value;
}

// =============================================================================
// Prefix Hash Utilities
// =============================================================================

// FNV-1a constants for prefix hashing
constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;

/**
 * Hash function for prefix tokens using FNV-1a.
 * Aligns to block_alignment boundary for vLLM PagedAttention compatibility.
 *
 * Used by:
 * - RouterService for consistent hash fallback routing
 * - HttpController for X-Ranvier-Prefix-Hash header injection
 * - Cache event handler for eviction lookup
 *
 * @param tokens Pointer to token ID array
 * @param count Number of tokens
 * @param block_alignment vLLM block alignment (typically 16)
 * @return 64-bit FNV-1a hash of the aligned token prefix
 */
inline uint64_t hash_prefix(const int32_t* tokens, size_t count, uint32_t block_alignment) {
    // Align to block_alignment boundary
    size_t aligned_len = (count / block_alignment) * block_alignment;
    if (aligned_len == 0) aligned_len = count;

    uint64_t hash = FNV_OFFSET_BASIS;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(tokens);
    size_t byte_len = aligned_len * sizeof(int32_t);

    for (size_t i = 0; i < byte_len; ++i) {
        hash ^= data[i];
        hash *= FNV_PRIME;
    }

    return hash;
}

// Buffer size for hex-encoded prefix hash (16 hex chars + null terminator)
constexpr size_t PREFIX_HASH_HEX_BUF_SIZE = 17;

// Number of hex digits in an encoded prefix hash
constexpr size_t PREFIX_HASH_HEX_LEN = 16;

/**
 * Encode a uint64_t hash as a 16-character lowercase hex string.
 *
 * @param hash The hash value to encode
 * @param out Output buffer (must be at least PREFIX_HASH_HEX_BUF_SIZE bytes)
 */
inline void encode_prefix_hash_hex(uint64_t hash, char* out) {
    snprintf(out, PREFIX_HASH_HEX_BUF_SIZE, "%016llx",
             static_cast<unsigned long long>(hash));
}

/**
 * Decode a hex string to a uint64_t hash.
 *
 * @param hex_str Hex string (up to 16 characters)
 * @return Decoded hash value, or std::nullopt if invalid hex
 */
inline std::optional<uint64_t> decode_prefix_hash_hex(std::string_view hex_str) {
    if (hex_str.empty() || hex_str.size() > 16) {
        return std::nullopt;
    }

    uint64_t result = 0;
    for (char c : hex_str) {
        result <<= 4;
        if (c >= '0' && c <= '9') {
            result |= static_cast<uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            result |= static_cast<uint64_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            result |= static_cast<uint64_t>(c - 'A' + 10);
        } else {
            return std::nullopt;
        }
    }
    return result;
}

// =============================================================================
// HTTP Auth Constants
// =============================================================================

constexpr std::string_view BEARER_PREFIX = "Bearer ";

/**
 * Extract the Bearer token from an Authorization header value.
 *
 * @param auth_header_value The full Authorization header value
 * @return The token string, or std::nullopt if not a Bearer token
 */
inline std::optional<std::string_view> extract_bearer_token(std::string_view auth_header_value) {
    if (auth_header_value.size() > BEARER_PREFIX.size() &&
        auth_header_value.substr(0, BEARER_PREFIX.size()) == BEARER_PREFIX) {
        return auth_header_value.substr(BEARER_PREFIX.size());
    }
    return std::nullopt;
}

// =============================================================================
// Base64 Decoding
// =============================================================================

/**
 * Decode a standard base64 string (RFC 4648). Used to unwrap K8s Secret
 * `data` fields, which are always base64-encoded UTF-8 strings.
 *
 * Accepts canonical base64 with `=` padding. Rejects URL-safe base64
 * (`-` and `_`) and whitespace inside the input — K8s never produces those.
 *
 * Returns std::nullopt for any malformed input (bad characters, wrong
 * length). On success returns the decoded bytes; the result may contain
 * arbitrary bytes including nulls (caller decides whether to treat as text).
 *
 * @param input Base64-encoded string (no leading/trailing whitespace)
 * @return Decoded bytes, or std::nullopt if input is malformed
 */
inline std::optional<std::string> base64_decode(std::string_view input) {
    if (input.size() % 4 != 0) return std::nullopt;

    // Lookup table: ASCII -> 6-bit value, or 0xFF for invalid characters.
    // Built once at static-init time. '=' (padding) maps to 0xFE so we can
    // distinguish it from invalid characters.
    static constexpr auto table = [] {
        std::array<uint8_t, 256> t{};
        for (auto& v : t) v = 0xFF;
        for (uint8_t i = 0; i < 26; ++i) {
            t['A' + i] = i;
            t['a' + i] = 26 + i;
        }
        for (uint8_t i = 0; i < 10; ++i) t['0' + i] = 52 + i;
        t['+'] = 62;
        t['/'] = 63;
        t['='] = 0xFE;
        return t;
    }();

    std::string out;
    out.reserve((input.size() / 4) * 3);

    for (size_t i = 0; i < input.size(); i += 4) {
        uint8_t a = table[static_cast<uint8_t>(input[i])];
        uint8_t b = table[static_cast<uint8_t>(input[i + 1])];
        uint8_t c = table[static_cast<uint8_t>(input[i + 2])];
        uint8_t d = table[static_cast<uint8_t>(input[i + 3])];

        // First two chars must be data, never padding.
        if (a == 0xFF || a == 0xFE || b == 0xFF || b == 0xFE) return std::nullopt;
        if (c == 0xFF || d == 0xFF) return std::nullopt;
        // Padding is only allowed at the end (last quartet).
        bool last = (i + 4 == input.size());
        if (!last && (c == 0xFE || d == 0xFE)) return std::nullopt;
        // If c is padding, d must also be padding (= pattern: XX==).
        if (c == 0xFE && d != 0xFE) return std::nullopt;

        out.push_back(static_cast<char>((a << 2) | (b >> 4)));
        if (c != 0xFE) out.push_back(static_cast<char>(((b & 0x0F) << 4) | (c >> 2)));
        if (d != 0xFE) out.push_back(static_cast<char>(((c & 0x03) << 6) | d));
    }
    return out;
}

} // namespace ranvier
