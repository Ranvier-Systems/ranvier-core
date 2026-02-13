// Ranvier Core - Parse Utils Unit Tests
//
// Tests for safe string-to-number conversion utilities (Rule #10).

#include "parse_utils.hpp"
#include <gtest/gtest.h>
#include <cstdint>
#include <limits>
#include <string>

using namespace ranvier;

// =============================================================================
// parse_int32 Tests
// =============================================================================

class ParseInt32Test : public ::testing::Test {};

TEST_F(ParseInt32Test, ValidPositiveIntegers) {
    EXPECT_EQ(parse_int32("0"), 0);
    EXPECT_EQ(parse_int32("1"), 1);
    EXPECT_EQ(parse_int32("42"), 42);
    EXPECT_EQ(parse_int32("12345"), 12345);
    EXPECT_EQ(parse_int32("999999999"), 999999999);
}

TEST_F(ParseInt32Test, ValidNegativeIntegers) {
    EXPECT_EQ(parse_int32("-1"), -1);
    EXPECT_EQ(parse_int32("-42"), -42);
    EXPECT_EQ(parse_int32("-12345"), -12345);
}

TEST_F(ParseInt32Test, BoundaryValues) {
    EXPECT_EQ(parse_int32("2147483647"), std::numeric_limits<int32_t>::max());
    EXPECT_EQ(parse_int32("-2147483648"), std::numeric_limits<int32_t>::min());
}

TEST_F(ParseInt32Test, Overflow) {
    EXPECT_EQ(parse_int32("2147483648"), std::nullopt);   // INT32_MAX + 1
    EXPECT_EQ(parse_int32("9999999999"), std::nullopt);
    EXPECT_EQ(parse_int32("99999999999999"), std::nullopt);
}

TEST_F(ParseInt32Test, Underflow) {
    EXPECT_EQ(parse_int32("-2147483649"), std::nullopt);   // INT32_MIN - 1
    EXPECT_EQ(parse_int32("-9999999999"), std::nullopt);
}

TEST_F(ParseInt32Test, TrailingGarbage) {
    EXPECT_EQ(parse_int32("123abc"), std::nullopt);
    EXPECT_EQ(parse_int32("42 "), std::nullopt);
    EXPECT_EQ(parse_int32("0x10"), std::nullopt);
    EXPECT_EQ(parse_int32("1.5"), std::nullopt);
    EXPECT_EQ(parse_int32("100,000"), std::nullopt);
}

TEST_F(ParseInt32Test, LeadingGarbage) {
    EXPECT_EQ(parse_int32("abc123"), std::nullopt);
    EXPECT_EQ(parse_int32("+42"), std::nullopt);   // from_chars does not accept '+'
}

TEST_F(ParseInt32Test, EmptyAndWhitespace) {
    EXPECT_EQ(parse_int32(""), std::nullopt);
    EXPECT_EQ(parse_int32(" "), std::nullopt);
    EXPECT_EQ(parse_int32("  "), std::nullopt);
    EXPECT_EQ(parse_int32("\t"), std::nullopt);
    EXPECT_EQ(parse_int32(" 42"), std::nullopt);   // leading whitespace
}

// =============================================================================
// parse_uint32 Tests
// =============================================================================

class ParseUint32Test : public ::testing::Test {};

TEST_F(ParseUint32Test, ValidValues) {
    EXPECT_EQ(parse_uint32("0"), 0u);
    EXPECT_EQ(parse_uint32("1"), 1u);
    EXPECT_EQ(parse_uint32("42"), 42u);
    EXPECT_EQ(parse_uint32("65535"), 65535u);
}

TEST_F(ParseUint32Test, BoundaryValues) {
    EXPECT_EQ(parse_uint32("4294967295"), std::numeric_limits<uint32_t>::max());
}

TEST_F(ParseUint32Test, Overflow) {
    EXPECT_EQ(parse_uint32("4294967296"), std::nullopt);   // UINT32_MAX + 1
    EXPECT_EQ(parse_uint32("9999999999"), std::nullopt);
}

TEST_F(ParseUint32Test, NegativeValuesRejected) {
    EXPECT_EQ(parse_uint32("-1"), std::nullopt);
    EXPECT_EQ(parse_uint32("-0"), std::nullopt);
    EXPECT_EQ(parse_uint32("-999"), std::nullopt);
}

TEST_F(ParseUint32Test, TrailingGarbage) {
    EXPECT_EQ(parse_uint32("123abc"), std::nullopt);
    EXPECT_EQ(parse_uint32("42 "), std::nullopt);
    EXPECT_EQ(parse_uint32("0x10"), std::nullopt);
}

TEST_F(ParseUint32Test, EmptyAndWhitespace) {
    EXPECT_EQ(parse_uint32(""), std::nullopt);
    EXPECT_EQ(parse_uint32(" "), std::nullopt);
    EXPECT_EQ(parse_uint32("\t"), std::nullopt);
}

// =============================================================================
// parse_port Tests
// =============================================================================

class ParsePortTest : public ::testing::Test {};

TEST_F(ParsePortTest, ValidPorts) {
    EXPECT_EQ(parse_port("1"), 1);
    EXPECT_EQ(parse_port("80"), 80);
    EXPECT_EQ(parse_port("443"), 443);
    EXPECT_EQ(parse_port("8080"), 8080);
    EXPECT_EQ(parse_port("65535"), 65535);
}

TEST_F(ParsePortTest, ZeroRejected) {
    EXPECT_EQ(parse_port("0"), std::nullopt);
}

TEST_F(ParsePortTest, AboveMaxRejected) {
    EXPECT_EQ(parse_port("65536"), std::nullopt);
    EXPECT_EQ(parse_port("99999"), std::nullopt);
    EXPECT_EQ(parse_port("100000"), std::nullopt);
}

TEST_F(ParsePortTest, NegativeRejected) {
    EXPECT_EQ(parse_port("-1"), std::nullopt);
    EXPECT_EQ(parse_port("-80"), std::nullopt);
}

TEST_F(ParsePortTest, TrailingGarbage) {
    EXPECT_EQ(parse_port("80abc"), std::nullopt);
    EXPECT_EQ(parse_port("443 "), std::nullopt);
}

TEST_F(ParsePortTest, EmptyAndWhitespace) {
    EXPECT_EQ(parse_port(""), std::nullopt);
    EXPECT_EQ(parse_port(" "), std::nullopt);
}

TEST_F(ParsePortTest, BoundaryPorts) {
    // Minimum valid port
    EXPECT_EQ(parse_port("1"), static_cast<uint16_t>(1));
    // Maximum valid port
    EXPECT_EQ(parse_port("65535"), static_cast<uint16_t>(65535));
    // Just below minimum
    EXPECT_EQ(parse_port("0"), std::nullopt);
    // Just above maximum
    EXPECT_EQ(parse_port("65536"), std::nullopt);
}

// =============================================================================
// parse_token_id / parse_backend_id Tests (convenience wrappers)
// =============================================================================

class ParseAliasTest : public ::testing::Test {};

TEST_F(ParseAliasTest, TokenIdParsesLikeInt32) {
    EXPECT_EQ(parse_token_id("42"), 42);
    EXPECT_EQ(parse_token_id("-1"), -1);
    EXPECT_EQ(parse_token_id("0"), 0);
    EXPECT_EQ(parse_token_id("abc"), std::nullopt);
    EXPECT_EQ(parse_token_id(""), std::nullopt);
}

TEST_F(ParseAliasTest, BackendIdParsesLikeInt32) {
    EXPECT_EQ(parse_backend_id("42"), 42);
    EXPECT_EQ(parse_backend_id("-1"), -1);
    EXPECT_EQ(parse_backend_id("0"), 0);
    EXPECT_EQ(parse_backend_id("abc"), std::nullopt);
    EXPECT_EQ(parse_backend_id(""), std::nullopt);
}
