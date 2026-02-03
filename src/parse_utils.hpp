#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ranvier {

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
