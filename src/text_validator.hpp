#pragma once

#include <string>
#include <string_view>
#include <optional>

namespace ranvier {

/**
 * Text validation utilities for safe tokenizer input.
 *
 * The tokenizer (HuggingFace tokenizers via Rust FFI) can crash on malformed input.
 * This validator checks input before passing to the tokenizer to prevent crashes.
 */
class TextValidator {
public:
    struct ValidationResult {
        bool valid;
        std::string error;  // Empty if valid
    };

    /**
     * Validate text is safe for tokenization.
     *
     * Checks:
     * 1. Valid UTF-8 encoding (JSON spec requires UTF-8)
     * 2. No embedded null bytes (breaks C-string handling in FFI)
     * 3. Length within limits (prevents edge cases with huge inputs)
     *
     * @param text The text to validate
     * @param max_length Maximum allowed length in bytes (0 = no limit)
     * @return ValidationResult with valid=true if safe, or error message if not
     */
    static ValidationResult validate_for_tokenizer(std::string_view text, size_t max_length = 0) {
        // Check length limit first (cheapest check)
        if (max_length > 0 && text.size() > max_length) {
            return {false, "Input exceeds maximum length (" + std::to_string(text.size()) +
                          " > " + std::to_string(max_length) + " bytes)"};
        }

        // Check for null bytes
        if (text.find('\0') != std::string_view::npos) {
            return {false, "Input contains null byte"};
        }

        // Validate UTF-8 encoding
        if (!is_valid_utf8(text)) {
            return {false, "Input contains invalid UTF-8"};
        }

        return {true, ""};
    }

    /**
     * Check if a string is valid UTF-8.
     *
     * Validates according to RFC 3629:
     * - No overlong encodings
     * - No surrogate pairs (U+D800 to U+DFFF)
     * - No code points above U+10FFFF
     *
     * @param text The text to validate
     * @return true if valid UTF-8, false otherwise
     */
    static bool is_valid_utf8(std::string_view text) {
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(text.data());
        const unsigned char* end = bytes + text.size();

        while (bytes < end) {
            if (bytes[0] <= 0x7F) {
                // ASCII: 0xxxxxxx
                bytes += 1;
            } else if ((bytes[0] & 0xE0) == 0xC0) {
                // 2-byte sequence: 110xxxxx 10xxxxxx
                if (end - bytes < 2) return false;
                if ((bytes[1] & 0xC0) != 0x80) return false;
                // Check for overlong encoding (code point < 0x80)
                if ((bytes[0] & 0x1E) == 0) return false;
                bytes += 2;
            } else if ((bytes[0] & 0xF0) == 0xE0) {
                // 3-byte sequence: 1110xxxx 10xxxxxx 10xxxxxx
                if (end - bytes < 3) return false;
                if ((bytes[1] & 0xC0) != 0x80) return false;
                if ((bytes[2] & 0xC0) != 0x80) return false;
                // Check for overlong encoding (code point < 0x800)
                if (bytes[0] == 0xE0 && (bytes[1] & 0x20) == 0) return false;
                // Check for surrogate pairs (U+D800 to U+DFFF)
                if (bytes[0] == 0xED && (bytes[1] & 0x20) != 0) return false;
                bytes += 3;
            } else if ((bytes[0] & 0xF8) == 0xF0) {
                // 4-byte sequence: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
                if (end - bytes < 4) return false;
                if ((bytes[1] & 0xC0) != 0x80) return false;
                if ((bytes[2] & 0xC0) != 0x80) return false;
                if ((bytes[3] & 0xC0) != 0x80) return false;
                // Check for overlong encoding (code point < 0x10000)
                if (bytes[0] == 0xF0 && (bytes[1] & 0x30) == 0) return false;
                // Check for code points above U+10FFFF
                if (bytes[0] > 0xF4) return false;
                if (bytes[0] == 0xF4 && bytes[1] > 0x8F) return false;
                bytes += 4;
            } else {
                // Invalid leading byte
                return false;
            }
        }

        return true;
    }
};

} // namespace ranvier
