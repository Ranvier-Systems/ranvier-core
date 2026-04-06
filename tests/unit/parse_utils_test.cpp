// Ranvier Core - Parse Utils Unit Tests
//
// Tests for safe string-to-number conversion utilities (Rule #10),
// prefix hash hex encoding/decoding, hash_prefix(), Bearer token
// extraction, and cache event JSON parsing.

#include "parse_utils.hpp"
#include "cache_event_parser.hpp"
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

// =============================================================================
// parse_double Tests
// =============================================================================

class ParseDoubleTest : public ::testing::Test {};

TEST_F(ParseDoubleTest, ValidValues) {
    EXPECT_DOUBLE_EQ(parse_double("1.0").value(), 1.0);
    EXPECT_DOUBLE_EQ(parse_double("6.0").value(), 6.0);
    EXPECT_DOUBLE_EQ(parse_double("0.5").value(), 0.5);
    EXPECT_DOUBLE_EQ(parse_double("100").value(), 100.0);
    EXPECT_DOUBLE_EQ(parse_double("-3.14").value(), -3.14);
    EXPECT_DOUBLE_EQ(parse_double("0").value(), 0.0);
}

TEST_F(ParseDoubleTest, ScientificNotation) {
    EXPECT_DOUBLE_EQ(parse_double("1e3").value(), 1000.0);
    EXPECT_DOUBLE_EQ(parse_double("2.5e-1").value(), 0.25);
}

TEST_F(ParseDoubleTest, TrailingGarbageRejected) {
    EXPECT_EQ(parse_double("1.0abc"), std::nullopt);
    EXPECT_EQ(parse_double("3.14 "), std::nullopt);
    EXPECT_EQ(parse_double("1.0 2.0"), std::nullopt);
}

TEST_F(ParseDoubleTest, NonNumericRejected) {
    EXPECT_EQ(parse_double("abc"), std::nullopt);
    EXPECT_EQ(parse_double(""), std::nullopt);
    EXPECT_EQ(parse_double(" "), std::nullopt);
}

TEST_F(ParseDoubleTest, InfAndNanRejected) {
    EXPECT_EQ(parse_double("inf"), std::nullopt);
    EXPECT_EQ(parse_double("-inf"), std::nullopt);
    EXPECT_EQ(parse_double("nan"), std::nullopt);
    EXPECT_EQ(parse_double("NaN"), std::nullopt);
    EXPECT_EQ(parse_double("infinity"), std::nullopt);
}

// =============================================================================
// Prefix Hash Hex Encoding/Decoding Tests
// =============================================================================

class PrefixHashHexTest : public ::testing::Test {};

TEST_F(PrefixHashHexTest, EncodeProducesLowercaseHex) {
    char hex[17];
    encode_prefix_hash_hex(0, hex);
    EXPECT_STREQ(hex, "0000000000000000");

    encode_prefix_hash_hex(0xDEADBEEFCAFE1234ULL, hex);
    EXPECT_STREQ(hex, "deadbeefcafe1234");

    encode_prefix_hash_hex(UINT64_MAX, hex);
    EXPECT_STREQ(hex, "ffffffffffffffff");
}

TEST_F(PrefixHashHexTest, DecodeValidHex) {
    auto result = decode_prefix_hash_hex("0000000000000000");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0ULL);

    result = decode_prefix_hash_hex("deadbeefcafe1234");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0xDEADBEEFCAFE1234ULL);

    result = decode_prefix_hash_hex("ffffffffffffffff");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, UINT64_MAX);
}

TEST_F(PrefixHashHexTest, DecodeUppercaseHex) {
    auto result = decode_prefix_hash_hex("DEADBEEFCAFE1234");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0xDEADBEEFCAFE1234ULL);
}

TEST_F(PrefixHashHexTest, DecodeMixedCaseHex) {
    auto result = decode_prefix_hash_hex("DeAdBeEf00000001");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0xDEADBEEF00000001ULL);
}

TEST_F(PrefixHashHexTest, DecodeShortHex) {
    auto result = decode_prefix_hash_hex("ff");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0xFFULL);

    result = decode_prefix_hash_hex("1");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1ULL);
}

TEST_F(PrefixHashHexTest, DecodeInvalidHex) {
    EXPECT_FALSE(decode_prefix_hash_hex("").has_value());
    EXPECT_FALSE(decode_prefix_hash_hex("g1234567890abcde").has_value());
    EXPECT_FALSE(decode_prefix_hash_hex("xyz").has_value());
    EXPECT_FALSE(decode_prefix_hash_hex("12345678901234567").has_value());  // Too long (17 chars)
}

TEST_F(PrefixHashHexTest, RoundTrip) {
    uint64_t original = 0xA1B2C3D4E5F60718ULL;
    char hex[17];
    encode_prefix_hash_hex(original, hex);
    auto decoded = decode_prefix_hash_hex(hex);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, original);
}

// =============================================================================
// hash_prefix() Tests
// =============================================================================

class HashPrefixTest : public ::testing::Test {};

TEST_F(HashPrefixTest, EmptyTokensReturnsConsistentHash) {
    int32_t tokens[] = {1};  // Need at least one for pointer validity
    uint64_t h1 = hash_prefix(tokens, 0, 16);
    uint64_t h2 = hash_prefix(tokens, 0, 16);
    EXPECT_EQ(h1, h2);
}

TEST_F(HashPrefixTest, SameTokensSameHash) {
    std::vector<int32_t> tokens = {100, 200, 300, 400, 500, 600, 700, 800,
                                    900, 1000, 1100, 1200, 1300, 1400, 1500, 1600};
    uint64_t h1 = hash_prefix(tokens.data(), tokens.size(), 16);
    uint64_t h2 = hash_prefix(tokens.data(), tokens.size(), 16);
    EXPECT_EQ(h1, h2);
}

TEST_F(HashPrefixTest, DifferentTokensDifferentHash) {
    std::vector<int32_t> tokens1 = {100, 200, 300, 400, 500, 600, 700, 800,
                                     900, 1000, 1100, 1200, 1300, 1400, 1500, 1600};
    std::vector<int32_t> tokens2 = {100, 200, 300, 400, 500, 600, 700, 800,
                                     900, 1000, 1100, 1200, 1300, 1400, 1500, 1601};
    uint64_t h1 = hash_prefix(tokens1.data(), tokens1.size(), 16);
    uint64_t h2 = hash_prefix(tokens2.data(), tokens2.size(), 16);
    EXPECT_NE(h1, h2);
}

TEST_F(HashPrefixTest, BlockAlignmentTruncates) {
    // With block_alignment=16, 20 tokens should hash the same as 16 tokens
    // because aligned_len = (20/16)*16 = 16
    std::vector<int32_t> tokens20(20, 42);
    std::vector<int32_t> tokens16(tokens20.begin(), tokens20.begin() + 16);

    uint64_t h20 = hash_prefix(tokens20.data(), tokens20.size(), 16);
    uint64_t h16 = hash_prefix(tokens16.data(), tokens16.size(), 16);
    EXPECT_EQ(h20, h16);
}

TEST_F(HashPrefixTest, SmallTokenCountUsesAll) {
    // When count < block_alignment, aligned_len falls back to count
    std::vector<int32_t> tokens = {1, 2, 3, 4};
    uint64_t h1 = hash_prefix(tokens.data(), tokens.size(), 16);
    uint64_t h2 = hash_prefix(tokens.data(), tokens.size(), 16);
    EXPECT_EQ(h1, h2);
}

// =============================================================================
// extract_bearer_token() Tests
// =============================================================================

class ExtractBearerTokenTest : public ::testing::Test {};

TEST_F(ExtractBearerTokenTest, ValidBearerToken) {
    auto token = extract_bearer_token("Bearer my-secret-token");
    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(*token, "my-secret-token");
}

TEST_F(ExtractBearerTokenTest, EmptyToken) {
    // "Bearer " with nothing after — too short, size equals prefix size
    auto token = extract_bearer_token("Bearer ");
    EXPECT_FALSE(token.has_value());
}

TEST_F(ExtractBearerTokenTest, NotBearer) {
    EXPECT_FALSE(extract_bearer_token("Basic dXNlcjpwYXNz").has_value());
    EXPECT_FALSE(extract_bearer_token("Token abc").has_value());
    EXPECT_FALSE(extract_bearer_token("").has_value());
}

TEST_F(ExtractBearerTokenTest, CaseSensitive) {
    // "bearer " (lowercase) should not match
    EXPECT_FALSE(extract_bearer_token("bearer my-token").has_value());
}

// =============================================================================
// parse_cache_events() Tests
// =============================================================================

class ParseCacheEventsTest : public ::testing::Test {
protected:
    static constexpr uint32_t MAX_EVENTS = 100;
    static constexpr uint64_t MAX_AGE_MS = 60000;
    static constexpr uint64_t NOW_MS = 1700000000000ULL;
};

TEST_F(ParseCacheEventsTest, EmptyBody) {
    auto result = parse_cache_events("", MAX_EVENTS, MAX_AGE_MS, NOW_MS);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "Empty request body");
}

TEST_F(ParseCacheEventsTest, InvalidJson) {
    auto result = parse_cache_events("{not json", MAX_EVENTS, MAX_AGE_MS, NOW_MS);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "Invalid JSON");
}

TEST_F(ParseCacheEventsTest, MissingType) {
    auto result = parse_cache_events(R"({"version": 1, "events": []})",
                                      MAX_EVENTS, MAX_AGE_MS, NOW_MS);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("type"), std::string::npos);
}

TEST_F(ParseCacheEventsTest, WrongType) {
    auto result = parse_cache_events(
        R"({"type": "wrong", "version": 1, "events": []})",
        MAX_EVENTS, MAX_AGE_MS, NOW_MS);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("type"), std::string::npos);
}

TEST_F(ParseCacheEventsTest, WrongVersion) {
    auto result = parse_cache_events(
        R"({"type": "cache_event", "version": 99, "events": []})",
        MAX_EVENTS, MAX_AGE_MS, NOW_MS);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("version"), std::string::npos);
}

TEST_F(ParseCacheEventsTest, MissingEvents) {
    auto result = parse_cache_events(
        R"({"type": "cache_event", "version": 1})",
        MAX_EVENTS, MAX_AGE_MS, NOW_MS);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("events"), std::string::npos);
}

TEST_F(ParseCacheEventsTest, TooManyEvents) {
    // Build JSON with 3 events but set max to 2
    auto result = parse_cache_events(
        R"({"type": "cache_event", "version": 1, "events": [
            {"event": "evicted", "prefix_hash": "0a", "timestamp_ms": 1700000000000},
            {"event": "evicted", "prefix_hash": "0b", "timestamp_ms": 1700000000000},
            {"event": "evicted", "prefix_hash": "0c", "timestamp_ms": 1700000000000}
        ]})",
        2, MAX_AGE_MS, NOW_MS);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("Too many"), std::string::npos);
}

TEST_F(ParseCacheEventsTest, ValidSingleEvent) {
    auto result = parse_cache_events(
        R"({"type": "cache_event", "version": 1, "events": [
            {"event": "evicted", "prefix_hash": "deadbeef", "timestamp_ms": 1700000000000}
        ]})",
        MAX_EVENTS, MAX_AGE_MS, NOW_MS);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.events.size(), 1u);
    EXPECT_EQ(result.events[0].event_type, "evicted");
    EXPECT_EQ(result.events[0].prefix_hash, 0xDEADBEEFULL);
    EXPECT_EQ(result.events[0].timestamp_ms, NOW_MS);
    EXPECT_EQ(result.events[0].backend_id, 0);
    EXPECT_FALSE(result.events[0].age_expired);
}

TEST_F(ParseCacheEventsTest, EventWithBackendId) {
    auto result = parse_cache_events(
        R"({"type": "cache_event", "version": 1, "events": [
            {"event": "evicted", "prefix_hash": "ff", "timestamp_ms": 1700000000000, "backend_id": 42}
        ]})",
        MAX_EVENTS, MAX_AGE_MS, NOW_MS);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.events.size(), 1u);
    EXPECT_EQ(result.events[0].backend_id, 42);
}

TEST_F(ParseCacheEventsTest, AgeExpiredEvent) {
    // Event from 2 minutes ago, max age is 1 minute
    uint64_t old_timestamp = NOW_MS - 120000;
    std::string json = R"({"type": "cache_event", "version": 1, "events": [
        {"event": "evicted", "prefix_hash": "aa", "timestamp_ms": )" +
        std::to_string(old_timestamp) + "}]}";
    auto result = parse_cache_events(json, MAX_EVENTS, MAX_AGE_MS, NOW_MS);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.events.size(), 1u);
    EXPECT_TRUE(result.events[0].age_expired);
}

TEST_F(ParseCacheEventsTest, FreshEventNotExpired) {
    // Event from 5 seconds ago, max age is 1 minute
    uint64_t recent_timestamp = NOW_MS - 5000;
    std::string json = R"({"type": "cache_event", "version": 1, "events": [
        {"event": "evicted", "prefix_hash": "bb", "timestamp_ms": )" +
        std::to_string(recent_timestamp) + "}]}";
    auto result = parse_cache_events(json, MAX_EVENTS, MAX_AGE_MS, NOW_MS);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.events.size(), 1u);
    EXPECT_FALSE(result.events[0].age_expired);
}

TEST_F(ParseCacheEventsTest, SkipsMalformedEvents) {
    auto result = parse_cache_events(
        R"({"type": "cache_event", "version": 1, "events": [
            {"event": "evicted"},
            42,
            {"event": "evicted", "prefix_hash": "cc", "timestamp_ms": 1700000000000}
        ]})",
        MAX_EVENTS, MAX_AGE_MS, NOW_MS);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.events.size(), 1u);
    EXPECT_EQ(result.skipped, 2u);
}

TEST_F(ParseCacheEventsTest, SkipsInvalidHex) {
    auto result = parse_cache_events(
        R"({"type": "cache_event", "version": 1, "events": [
            {"event": "evicted", "prefix_hash": "not_hex!", "timestamp_ms": 1700000000000}
        ]})",
        MAX_EVENTS, MAX_AGE_MS, NOW_MS);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.events.size(), 0u);
    EXPECT_EQ(result.skipped, 1u);
}

TEST_F(ParseCacheEventsTest, EmptyEventsArray) {
    auto result = parse_cache_events(
        R"({"type": "cache_event", "version": 1, "events": []})",
        MAX_EVENTS, MAX_AGE_MS, NOW_MS);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.events.size(), 0u);
    EXPECT_EQ(result.skipped, 0u);
}

TEST_F(ParseCacheEventsTest, MultipleMixedEvents) {
    auto result = parse_cache_events(
        R"({"type": "cache_event", "version": 1, "events": [
            {"event": "evicted", "prefix_hash": "aabb", "timestamp_ms": 1700000000000},
            {"event": "loaded", "prefix_hash": "ccdd", "timestamp_ms": 1700000000000},
            {"event": "evicted", "prefix_hash": "eeff", "timestamp_ms": 1700000000000, "backend_id": 7}
        ]})",
        MAX_EVENTS, MAX_AGE_MS, NOW_MS);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.events.size(), 3u);
    EXPECT_EQ(result.events[0].event_type, "evicted");
    EXPECT_EQ(result.events[1].event_type, "loaded");
    EXPECT_EQ(result.events[2].backend_id, 7);
}
