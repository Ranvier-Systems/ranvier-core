// Ranvier Core - Text Validator Unit Tests
//
// Tests for input validation before tokenizer to prevent crashes.

#include "text_validator.hpp"
#include <gtest/gtest.h>
#include <string>
#include <string_view>

using namespace ranvier;

// =============================================================================
// UTF-8 Validation Tests
// =============================================================================

class TextValidatorUtf8Test : public ::testing::Test {};

TEST_F(TextValidatorUtf8Test, ValidAscii) {
    EXPECT_TRUE(TextValidator::is_valid_utf8("Hello, World!"));
    EXPECT_TRUE(TextValidator::is_valid_utf8(""));
    EXPECT_TRUE(TextValidator::is_valid_utf8("0123456789"));
    EXPECT_TRUE(TextValidator::is_valid_utf8("!@#$%^&*()"));
}

TEST_F(TextValidatorUtf8Test, ValidMultibyteCharacters) {
    // 2-byte UTF-8 (Latin, Greek, etc.)
    EXPECT_TRUE(TextValidator::is_valid_utf8("\xC3\xA9"));  // é (U+00E9)
    EXPECT_TRUE(TextValidator::is_valid_utf8("café"));

    // 3-byte UTF-8 (CJK, etc.)
    EXPECT_TRUE(TextValidator::is_valid_utf8("\xE4\xB8\xAD"));  // 中 (U+4E2D)
    EXPECT_TRUE(TextValidator::is_valid_utf8("日本語"));

    // 4-byte UTF-8 (Emoji, etc.)
    EXPECT_TRUE(TextValidator::is_valid_utf8("\xF0\x9F\x98\x80"));  // 😀 (U+1F600)
    EXPECT_TRUE(TextValidator::is_valid_utf8("Hello 👋 World 🌍"));
}

TEST_F(TextValidatorUtf8Test, ValidMixedContent) {
    EXPECT_TRUE(TextValidator::is_valid_utf8("Hello, 世界! 🌍"));
    EXPECT_TRUE(TextValidator::is_valid_utf8("Cześć, świecie!"));
    EXPECT_TRUE(TextValidator::is_valid_utf8("Привет мир"));
}

TEST_F(TextValidatorUtf8Test, InvalidContinuationBytes) {
    // Continuation byte without leading byte
    EXPECT_FALSE(TextValidator::is_valid_utf8("\x80"));
    EXPECT_FALSE(TextValidator::is_valid_utf8("\xBF"));
    EXPECT_FALSE(TextValidator::is_valid_utf8("hello\x80world"));
}

TEST_F(TextValidatorUtf8Test, InvalidLeadingBytes) {
    // Invalid leading bytes (0xFE and 0xFF are never valid)
    EXPECT_FALSE(TextValidator::is_valid_utf8("\xFE"));
    EXPECT_FALSE(TextValidator::is_valid_utf8("\xFF"));
}

TEST_F(TextValidatorUtf8Test, TruncatedSequences) {
    // 2-byte sequence truncated
    EXPECT_FALSE(TextValidator::is_valid_utf8("\xC3"));

    // 3-byte sequence truncated after 1 byte
    EXPECT_FALSE(TextValidator::is_valid_utf8("\xE4"));

    // 3-byte sequence truncated after 2 bytes
    EXPECT_FALSE(TextValidator::is_valid_utf8("\xE4\xB8"));

    // 4-byte sequence truncated
    EXPECT_FALSE(TextValidator::is_valid_utf8("\xF0\x9F"));
    EXPECT_FALSE(TextValidator::is_valid_utf8("\xF0\x9F\x98"));
}

TEST_F(TextValidatorUtf8Test, OverlongEncodings) {
    // Overlong encoding of ASCII (should be rejected per RFC 3629)
    // '/' (U+002F) encoded as 2 bytes instead of 1
    EXPECT_FALSE(TextValidator::is_valid_utf8("\xC0\xAF"));

    // Overlong encoding using C0 or C1 leading byte
    EXPECT_FALSE(TextValidator::is_valid_utf8("\xC0\x80"));  // Overlong NUL
    EXPECT_FALSE(TextValidator::is_valid_utf8("\xC1\xBF"));  // Overlong DEL
}

TEST_F(TextValidatorUtf8Test, SurrogatePairs) {
    // UTF-16 surrogate pairs are invalid in UTF-8 (U+D800 to U+DFFF)
    EXPECT_FALSE(TextValidator::is_valid_utf8("\xED\xA0\x80"));  // U+D800
    EXPECT_FALSE(TextValidator::is_valid_utf8("\xED\xBF\xBF"));  // U+DFFF
}

TEST_F(TextValidatorUtf8Test, CodePointsAboveMax) {
    // Code points above U+10FFFF are invalid
    EXPECT_FALSE(TextValidator::is_valid_utf8("\xF4\x90\x80\x80"));  // U+110000
    EXPECT_FALSE(TextValidator::is_valid_utf8("\xF5\x80\x80\x80"));  // Would be > U+10FFFF
}

TEST_F(TextValidatorUtf8Test, ValidBoundaryCodePoints) {
    // U+007F - highest ASCII
    EXPECT_TRUE(TextValidator::is_valid_utf8("\x7F"));

    // U+0080 - lowest 2-byte
    EXPECT_TRUE(TextValidator::is_valid_utf8("\xC2\x80"));

    // U+07FF - highest 2-byte
    EXPECT_TRUE(TextValidator::is_valid_utf8("\xDF\xBF"));

    // U+0800 - lowest 3-byte
    EXPECT_TRUE(TextValidator::is_valid_utf8("\xE0\xA0\x80"));

    // U+FFFF - highest 3-byte (excluding surrogates)
    EXPECT_TRUE(TextValidator::is_valid_utf8("\xEF\xBF\xBF"));

    // U+10000 - lowest 4-byte
    EXPECT_TRUE(TextValidator::is_valid_utf8("\xF0\x90\x80\x80"));

    // U+10FFFF - highest valid code point
    EXPECT_TRUE(TextValidator::is_valid_utf8("\xF4\x8F\xBF\xBF"));
}

// =============================================================================
// Full Validation Tests
// =============================================================================

class TextValidatorFullTest : public ::testing::Test {};

TEST_F(TextValidatorFullTest, ValidInputPasses) {
    auto result = TextValidator::validate_for_tokenizer("Hello, World!");
    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.error.empty());
}

TEST_F(TextValidatorFullTest, EmptyInputPasses) {
    auto result = TextValidator::validate_for_tokenizer("");
    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.error.empty());
}

TEST_F(TextValidatorFullTest, NullByteRejected) {
    std::string with_null = "Hello\0World";
    with_null[5] = '\0';  // Ensure null is embedded
    std::string_view view(with_null.data(), 11);

    auto result = TextValidator::validate_for_tokenizer(view);
    EXPECT_FALSE(result.valid);
    EXPECT_NE(result.error.find("null byte"), std::string::npos);
}

TEST_F(TextValidatorFullTest, NullByteAtStart) {
    std::string_view view("\0Hello", 6);
    auto result = TextValidator::validate_for_tokenizer(view);
    EXPECT_FALSE(result.valid);
}

TEST_F(TextValidatorFullTest, NullByteAtEnd) {
    std::string_view view("Hello\0", 6);
    auto result = TextValidator::validate_for_tokenizer(view);
    EXPECT_FALSE(result.valid);
}

TEST_F(TextValidatorFullTest, InvalidUtf8Rejected) {
    auto result = TextValidator::validate_for_tokenizer("\xFF\xFE");
    EXPECT_FALSE(result.valid);
    EXPECT_NE(result.error.find("UTF-8"), std::string::npos);
}

TEST_F(TextValidatorFullTest, LengthLimitEnforced) {
    std::string long_input(1000, 'x');

    // Under limit - passes
    auto result1 = TextValidator::validate_for_tokenizer(long_input, 2000);
    EXPECT_TRUE(result1.valid);

    // Over limit - fails
    auto result2 = TextValidator::validate_for_tokenizer(long_input, 500);
    EXPECT_FALSE(result2.valid);
    EXPECT_NE(result2.error.find("length"), std::string::npos);
}

TEST_F(TextValidatorFullTest, ZeroLengthLimitMeansNoLimit) {
    std::string long_input(100000, 'x');
    auto result = TextValidator::validate_for_tokenizer(long_input, 0);
    EXPECT_TRUE(result.valid);
}

TEST_F(TextValidatorFullTest, LengthCheckBeforeUtf8Check) {
    // Invalid UTF-8 that's also too long
    // Length check is cheaper, so it should fail on length first
    std::string long_invalid(1000, '\xFF');
    auto result = TextValidator::validate_for_tokenizer(long_invalid, 500);
    EXPECT_FALSE(result.valid);
    EXPECT_NE(result.error.find("length"), std::string::npos);
}

TEST_F(TextValidatorFullTest, UnicodeTextPasses) {
    auto result = TextValidator::validate_for_tokenizer("Hello, 世界! 🌍 Привет");
    EXPECT_TRUE(result.valid);
}

TEST_F(TextValidatorFullTest, JsonLikeContentPasses) {
    std::string json = R"({"messages": [{"role": "user", "content": "Hello, how are you?"}]})";
    auto result = TextValidator::validate_for_tokenizer(json);
    EXPECT_TRUE(result.valid);
}

TEST_F(TextValidatorFullTest, LargeValidInputPasses) {
    // Simulate a large prompt (but under limit)
    std::string large_input;
    for (int i = 0; i < 1000; i++) {
        large_input += "This is a test sentence for tokenization. ";
    }
    auto result = TextValidator::validate_for_tokenizer(large_input, 512 * 1024);
    EXPECT_TRUE(result.valid);
}

// =============================================================================
// Edge Cases
// =============================================================================

class TextValidatorEdgeCaseTest : public ::testing::Test {};

TEST_F(TextValidatorEdgeCaseTest, SingleCharacterInputs) {
    EXPECT_TRUE(TextValidator::validate_for_tokenizer("a").valid);
    EXPECT_TRUE(TextValidator::validate_for_tokenizer(" ").valid);
    EXPECT_TRUE(TextValidator::validate_for_tokenizer("\n").valid);
    EXPECT_TRUE(TextValidator::validate_for_tokenizer("\t").valid);
}

TEST_F(TextValidatorEdgeCaseTest, ControlCharactersAllowed) {
    // Control characters (except null) should be allowed
    // The tokenizer handles these
    EXPECT_TRUE(TextValidator::validate_for_tokenizer("\x01\x02\x03").valid);
    EXPECT_TRUE(TextValidator::validate_for_tokenizer("\x1B[0m").valid);  // ANSI escape
}

TEST_F(TextValidatorEdgeCaseTest, MaxSingleByteUtf8) {
    // DEL character (0x7F) is valid UTF-8
    EXPECT_TRUE(TextValidator::validate_for_tokenizer("\x7F").valid);
}

TEST_F(TextValidatorEdgeCaseTest, BinaryDataRejected) {
    // Random binary data should be rejected (not valid UTF-8)
    // Use explicit length to avoid null termination issues
    const char binary[] = "\x80\x81\x82\xFF\xFE";
    std::string_view view(binary, sizeof(binary) - 1);  // -1 to exclude trailing null
    auto result = TextValidator::validate_for_tokenizer(view);
    EXPECT_FALSE(result.valid);
    EXPECT_NE(result.error.find("UTF-8"), std::string::npos);
}

TEST_F(TextValidatorEdgeCaseTest, ExactlyAtLimit) {
    std::string exact(100, 'x');
    auto result = TextValidator::validate_for_tokenizer(exact, 100);
    EXPECT_TRUE(result.valid);
}

TEST_F(TextValidatorEdgeCaseTest, OneOverLimit) {
    std::string over(101, 'x');
    auto result = TextValidator::validate_for_tokenizer(over, 100);
    EXPECT_FALSE(result.valid);
}
