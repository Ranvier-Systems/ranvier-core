// Ranvier Core - Tracing Service Unit Tests (W3C traceparent parsing)
//
// Tests for TraceContext::parse() and TraceContext::to_traceparent().
// Compiled with RANVIER_NO_TELEMETRY to avoid OpenTelemetry dependency.
// The W3C parsing logic and w3c:: helpers are provided below (extracted
// from tracing_service.cpp) since they are pure string logic.

#define RANVIER_NO_TELEMETRY
#include "tracing_service.hpp"
#include <gtest/gtest.h>
#include <string>
#include <string_view>

// =============================================================================
// W3C Trace Context helpers and parse/to_traceparent implementations
// (Extracted from tracing_service.cpp — pure string logic, no OTel deps)
// =============================================================================

namespace ranvier {
namespace w3c {

constexpr size_t VERSION_HEX_LEN = 2;
constexpr size_t TRACE_ID_HEX_LEN = 32;
constexpr size_t SPAN_ID_HEX_LEN = 16;
constexpr size_t TRACE_FLAGS_HEX_LEN = 2;
constexpr size_t SEPARATOR_LEN = 1;

constexpr size_t VERSION_OFFSET = 0;
constexpr size_t TRACE_ID_OFFSET = VERSION_HEX_LEN + SEPARATOR_LEN;  // 3
constexpr size_t SPAN_ID_OFFSET = TRACE_ID_OFFSET + TRACE_ID_HEX_LEN + SEPARATOR_LEN;  // 36
constexpr size_t FLAGS_OFFSET = SPAN_ID_OFFSET + SPAN_ID_HEX_LEN + SEPARATOR_LEN;  // 53

constexpr size_t SEP_AFTER_VERSION = VERSION_HEX_LEN;  // 2
constexpr size_t SEP_AFTER_TRACE_ID = TRACE_ID_OFFSET + TRACE_ID_HEX_LEN;  // 35
constexpr size_t SEP_AFTER_SPAN_ID = SPAN_ID_OFFSET + SPAN_ID_HEX_LEN;  // 52

constexpr size_t MIN_TRACEPARENT_LEN = FLAGS_OFFSET + TRACE_FLAGS_HEX_LEN;  // 55

constexpr char SEPARATOR = '-';
constexpr std::string_view SUPPORTED_VERSION = "00";
constexpr std::string_view SAMPLED_FLAG = "01";
constexpr std::string_view UNSAMPLED_FLAG = "00";

constexpr bool is_hex_char(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline bool is_valid_hex_string(std::string_view s) noexcept {
    for (char c : s) {
        if (!is_hex_char(c)) return false;
    }
    return true;
}

inline bool is_all_zeros(std::string_view s) noexcept {
    for (char c : s) {
        if (c != '0') return false;
    }
    return true;
}

}  // namespace w3c

// Override the no-op parse() stub with the real implementation
TraceContext TraceContext::parse(std::string_view traceparent) {
    TraceContext ctx;

    if (traceparent.length() < w3c::MIN_TRACEPARENT_LEN) {
        return ctx;
    }

    std::string_view version = traceparent.substr(w3c::VERSION_OFFSET, w3c::VERSION_HEX_LEN);
    if (version != w3c::SUPPORTED_VERSION) {
        return ctx;
    }

    if (traceparent[w3c::SEP_AFTER_VERSION] != w3c::SEPARATOR ||
        traceparent[w3c::SEP_AFTER_TRACE_ID] != w3c::SEPARATOR ||
        traceparent[w3c::SEP_AFTER_SPAN_ID] != w3c::SEPARATOR) {
        return ctx;
    }

    std::string_view trace_id_view = traceparent.substr(w3c::TRACE_ID_OFFSET, w3c::TRACE_ID_HEX_LEN);
    if (!w3c::is_valid_hex_string(trace_id_view)) {
        return ctx;
    }

    std::string_view span_id_view = traceparent.substr(w3c::SPAN_ID_OFFSET, w3c::SPAN_ID_HEX_LEN);
    if (!w3c::is_valid_hex_string(span_id_view)) {
        return ctx;
    }

    if (w3c::is_all_zeros(trace_id_view)) {
        return ctx;
    }

    std::string_view flags = traceparent.substr(w3c::FLAGS_OFFSET, w3c::TRACE_FLAGS_HEX_LEN);
    ctx.sampled = (flags == w3c::SAMPLED_FLAG);

    ctx.trace_id = std::string(trace_id_view);
    ctx.parent_span_id = std::string(span_id_view);
    ctx.valid = true;
    return ctx;
}

std::string TraceContext::to_traceparent(std::string_view span_id) const {
    if (!valid || span_id.empty()) {
        return "";
    }
    std::string result;
    result.reserve(w3c::MIN_TRACEPARENT_LEN);
    result.append(w3c::SUPPORTED_VERSION);
    result.push_back(w3c::SEPARATOR);
    result.append(trace_id);
    result.push_back(w3c::SEPARATOR);
    result.append(span_id);
    result.push_back(w3c::SEPARATOR);
    result.append(sampled ? w3c::SAMPLED_FLAG : w3c::UNSAMPLED_FLAG);
    return result;
}

}  // namespace ranvier

using namespace ranvier;

// =============================================================================
// Valid Traceparent Parsing Tests
// =============================================================================

class TraceContextParseValidTest : public ::testing::Test {
protected:
    // Standard valid traceparent (sampled)
    static constexpr const char* kValidSampled =
        "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";
    // Standard valid traceparent (unsampled)
    static constexpr const char* kValidUnsampled =
        "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-00";
};

TEST_F(TraceContextParseValidTest, ValidSampledTraceparent) {
    auto ctx = TraceContext::parse(kValidSampled);
    EXPECT_TRUE(ctx.valid);
    EXPECT_EQ(ctx.trace_id, "0af7651916cd43dd8448eb211c80319c");
    EXPECT_EQ(ctx.parent_span_id, "b7ad6b7169203331");
    EXPECT_TRUE(ctx.sampled);
}

TEST_F(TraceContextParseValidTest, ValidUnsampledTraceparent) {
    auto ctx = TraceContext::parse(kValidUnsampled);
    EXPECT_TRUE(ctx.valid);
    EXPECT_EQ(ctx.trace_id, "0af7651916cd43dd8448eb211c80319c");
    EXPECT_EQ(ctx.parent_span_id, "b7ad6b7169203331");
    EXPECT_FALSE(ctx.sampled);
}

TEST_F(TraceContextParseValidTest, UpperCaseHexAccepted) {
    auto ctx = TraceContext::parse(
        "00-0AF7651916CD43DD8448EB211C80319C-B7AD6B7169203331-01");
    EXPECT_TRUE(ctx.valid);
    EXPECT_EQ(ctx.trace_id, "0AF7651916CD43DD8448EB211C80319C");
}

TEST_F(TraceContextParseValidTest, MixedCaseHexAccepted) {
    auto ctx = TraceContext::parse(
        "00-0aF7651916cD43dd8448eB211C80319c-b7Ad6b7169203331-01");
    EXPECT_TRUE(ctx.valid);
}

TEST_F(TraceContextParseValidTest, AllFsTraceId) {
    auto ctx = TraceContext::parse(
        "00-ffffffffffffffffffffffffffffffff-ffffffffffffffff-01");
    EXPECT_TRUE(ctx.valid);
    EXPECT_EQ(ctx.trace_id, "ffffffffffffffffffffffffffffffff");
}

TEST_F(TraceContextParseValidTest, MinimalNonZeroTraceId) {
    // trace-id with just the last character non-zero
    auto ctx = TraceContext::parse(
        "00-00000000000000000000000000000001-0000000000000001-00");
    EXPECT_TRUE(ctx.valid);
}

TEST_F(TraceContextParseValidTest, UnknownFlagsStillParsed) {
    // Flags "02" is not "01", so sampled should be false
    auto ctx = TraceContext::parse(
        "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-02");
    EXPECT_TRUE(ctx.valid);
    EXPECT_FALSE(ctx.sampled);
}

// =============================================================================
// Invalid Traceparent Parsing Tests
// =============================================================================

class TraceContextParseInvalidTest : public ::testing::Test {};

TEST_F(TraceContextParseInvalidTest, EmptyString) {
    auto ctx = TraceContext::parse("");
    EXPECT_FALSE(ctx.valid);
}

TEST_F(TraceContextParseInvalidTest, TooShort) {
    auto ctx = TraceContext::parse("00-0af765-b7ad6b-01");
    EXPECT_FALSE(ctx.valid);
}

TEST_F(TraceContextParseInvalidTest, ExactlyOneByteTooShort) {
    // 54 chars instead of 55
    auto ctx = TraceContext::parse(
        "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-0");
    EXPECT_FALSE(ctx.valid);
}

TEST_F(TraceContextParseInvalidTest, WrongVersion) {
    auto ctx = TraceContext::parse(
        "01-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
    EXPECT_FALSE(ctx.valid);
}

TEST_F(TraceContextParseInvalidTest, VersionFF) {
    auto ctx = TraceContext::parse(
        "ff-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
    EXPECT_FALSE(ctx.valid);
}

TEST_F(TraceContextParseInvalidTest, MissingSeparatorAfterVersion) {
    auto ctx = TraceContext::parse(
        "00X0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
    EXPECT_FALSE(ctx.valid);
}

TEST_F(TraceContextParseInvalidTest, MissingSeparatorAfterTraceId) {
    auto ctx = TraceContext::parse(
        "00-0af7651916cd43dd8448eb211c80319cXb7ad6b7169203331-01");
    EXPECT_FALSE(ctx.valid);
}

TEST_F(TraceContextParseInvalidTest, MissingSeparatorAfterSpanId) {
    auto ctx = TraceContext::parse(
        "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331X01");
    EXPECT_FALSE(ctx.valid);
}

TEST_F(TraceContextParseInvalidTest, NonHexInTraceId) {
    auto ctx = TraceContext::parse(
        "00-0af7651916cd43dd8448eb211c80319g-b7ad6b7169203331-01");
    EXPECT_FALSE(ctx.valid);
}

TEST_F(TraceContextParseInvalidTest, NonHexInSpanId) {
    auto ctx = TraceContext::parse(
        "00-0af7651916cd43dd8448eb211c80319c-b7ad6b716920333z-01");
    EXPECT_FALSE(ctx.valid);
}

TEST_F(TraceContextParseInvalidTest, AllZerosTraceId) {
    auto ctx = TraceContext::parse(
        "00-00000000000000000000000000000000-b7ad6b7169203331-01");
    EXPECT_FALSE(ctx.valid);
}

TEST_F(TraceContextParseInvalidTest, AllZerosSpanIdStillValid) {
    // W3C spec only prohibits all-zero trace-id, not span-id
    auto ctx = TraceContext::parse(
        "00-0af7651916cd43dd8448eb211c80319c-0000000000000000-01");
    EXPECT_TRUE(ctx.valid);
}

TEST_F(TraceContextParseInvalidTest, SpacesInInput) {
    auto ctx = TraceContext::parse(
        " 00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
    EXPECT_FALSE(ctx.valid);
}

TEST_F(TraceContextParseInvalidTest, JustDashes) {
    auto ctx = TraceContext::parse("----------------------------------");
    EXPECT_FALSE(ctx.valid);
}

// =============================================================================
// Field Length Enforcement Tests
// =============================================================================

class TraceContextFieldLengthTest : public ::testing::Test {};

TEST_F(TraceContextFieldLengthTest, ExactMinimumLength) {
    // Exactly 55 chars — should parse
    std::string tp = "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";
    EXPECT_EQ(tp.size(), 55u);
    auto ctx = TraceContext::parse(tp);
    EXPECT_TRUE(ctx.valid);
}

TEST_F(TraceContextFieldLengthTest, TraceIdIs32Hex) {
    auto ctx = TraceContext::parse(
        "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
    EXPECT_TRUE(ctx.valid);
    EXPECT_EQ(ctx.trace_id.size(), 32u);
}

TEST_F(TraceContextFieldLengthTest, SpanIdIs16Hex) {
    auto ctx = TraceContext::parse(
        "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
    EXPECT_TRUE(ctx.valid);
    EXPECT_EQ(ctx.parent_span_id.size(), 16u);
}

// =============================================================================
// Flag Parsing Tests
// =============================================================================

class TraceContextFlagsTest : public ::testing::Test {};

TEST_F(TraceContextFlagsTest, Flag00IsUnsampled) {
    auto ctx = TraceContext::parse(
        "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-00");
    EXPECT_TRUE(ctx.valid);
    EXPECT_FALSE(ctx.sampled);
}

TEST_F(TraceContextFlagsTest, Flag01IsSampled) {
    auto ctx = TraceContext::parse(
        "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
    EXPECT_TRUE(ctx.valid);
    EXPECT_TRUE(ctx.sampled);
}

TEST_F(TraceContextFlagsTest, Flag02IsNotSampled) {
    // Only "01" maps to sampled=true
    auto ctx = TraceContext::parse(
        "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-02");
    EXPECT_TRUE(ctx.valid);
    EXPECT_FALSE(ctx.sampled);
}

TEST_F(TraceContextFlagsTest, FlagFFIsNotSampled) {
    auto ctx = TraceContext::parse(
        "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-ff");
    EXPECT_TRUE(ctx.valid);
    EXPECT_FALSE(ctx.sampled);
}

// =============================================================================
// to_traceparent Tests
// =============================================================================

class TraceContextToTraceparentTest : public ::testing::Test {};

TEST_F(TraceContextToTraceparentTest, ValidContextProducesTraceparent) {
    auto ctx = TraceContext::parse(
        "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
    ASSERT_TRUE(ctx.valid);

    auto result = ctx.to_traceparent("1234567890abcdef");
    EXPECT_EQ(result, "00-0af7651916cd43dd8448eb211c80319c-1234567890abcdef-01");
}

TEST_F(TraceContextToTraceparentTest, UnsampledFlagInOutput) {
    auto ctx = TraceContext::parse(
        "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-00");
    ASSERT_TRUE(ctx.valid);

    auto result = ctx.to_traceparent("1234567890abcdef");
    EXPECT_EQ(result, "00-0af7651916cd43dd8448eb211c80319c-1234567890abcdef-00");
}

TEST_F(TraceContextToTraceparentTest, InvalidContextReturnsEmpty) {
    TraceContext ctx;  // valid = false by default
    auto result = ctx.to_traceparent("1234567890abcdef");
    EXPECT_TRUE(result.empty());
}

TEST_F(TraceContextToTraceparentTest, EmptySpanIdReturnsEmpty) {
    auto ctx = TraceContext::parse(
        "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
    ASSERT_TRUE(ctx.valid);

    auto result = ctx.to_traceparent("");
    EXPECT_TRUE(result.empty());
}

TEST_F(TraceContextToTraceparentTest, RoundTripPreservesFields) {
    const std::string original =
        "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";
    auto ctx = TraceContext::parse(original);
    ASSERT_TRUE(ctx.valid);

    // Use the original parent span ID to reconstruct
    auto result = ctx.to_traceparent(ctx.parent_span_id);
    EXPECT_EQ(result, original);
}

// =============================================================================
// w3c Helper Tests
// =============================================================================

class W3cHelperTest : public ::testing::Test {};

TEST_F(W3cHelperTest, IsHexCharValid) {
    for (char c = '0'; c <= '9'; ++c) EXPECT_TRUE(w3c::is_hex_char(c));
    for (char c = 'a'; c <= 'f'; ++c) EXPECT_TRUE(w3c::is_hex_char(c));
    for (char c = 'A'; c <= 'F'; ++c) EXPECT_TRUE(w3c::is_hex_char(c));
}

TEST_F(W3cHelperTest, IsHexCharInvalid) {
    EXPECT_FALSE(w3c::is_hex_char('g'));
    EXPECT_FALSE(w3c::is_hex_char('G'));
    EXPECT_FALSE(w3c::is_hex_char(' '));
    EXPECT_FALSE(w3c::is_hex_char('-'));
    EXPECT_FALSE(w3c::is_hex_char('\0'));
}

TEST_F(W3cHelperTest, IsValidHexString) {
    EXPECT_TRUE(w3c::is_valid_hex_string("0123456789abcdef"));
    EXPECT_TRUE(w3c::is_valid_hex_string("ABCDEF"));
    EXPECT_TRUE(w3c::is_valid_hex_string(""));
    EXPECT_FALSE(w3c::is_valid_hex_string("xyz"));
    EXPECT_FALSE(w3c::is_valid_hex_string("01g3"));
}

TEST_F(W3cHelperTest, IsAllZeros) {
    EXPECT_TRUE(w3c::is_all_zeros("0000"));
    EXPECT_TRUE(w3c::is_all_zeros("0"));
    EXPECT_TRUE(w3c::is_all_zeros(""));
    EXPECT_FALSE(w3c::is_all_zeros("0001"));
    EXPECT_FALSE(w3c::is_all_zeros("1000"));
}

TEST_F(W3cHelperTest, MinTraceparentLenIs55) {
    EXPECT_EQ(w3c::MIN_TRACEPARENT_LEN, 55u);
}
