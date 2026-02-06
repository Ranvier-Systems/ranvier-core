// Ranvier Core - Logging Utilities Unit Tests
//
// Tests for request ID generation and header extraction helpers.
// Uses stub headers for Seastar/fmt (tests/stubs/) to keep this a pure-C++ test.

#include "logging.hpp"
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <string_view>
#include <unordered_set>

using namespace ranvier;

// =============================================================================
// generate_request_id Tests
// =============================================================================

class GenerateRequestIdTest : public ::testing::Test {};

TEST_F(GenerateRequestIdTest, ReturnsNonEmpty) {
    auto id = generate_request_id();
    EXPECT_FALSE(id.empty());
}

TEST_F(GenerateRequestIdTest, StartsWithReqPrefix) {
    auto id = generate_request_id();
    EXPECT_EQ(id.substr(0, 4), "req-");
}

TEST_F(GenerateRequestIdTest, ContainsShardId) {
    auto id = generate_request_id();
    // Stub shard ID is 0, so format is "req-0-..."
    EXPECT_EQ(id.substr(0, 6), "req-0-");
}

TEST_F(GenerateRequestIdTest, HasExpectedSegmentCount) {
    auto id = generate_request_id();
    // Format: "req-{shard}-{timestamp_hex}-{seq_hex}"
    // Should have exactly 4 segments separated by '-'
    int dash_count = 0;
    for (char c : id) {
        if (c == '-') ++dash_count;
    }
    EXPECT_EQ(dash_count, 3);
}

TEST_F(GenerateRequestIdTest, UniqueAcrossMultipleCalls) {
    std::unordered_set<std::string> ids;
    for (int i = 0; i < 100; ++i) {
        ids.insert(generate_request_id());
    }
    EXPECT_EQ(ids.size(), 100u);
}

TEST_F(GenerateRequestIdTest, SequenceNumberIncreases) {
    // The last segment is the hex sequence counter.
    // Extract it from two consecutive calls and verify ordering.
    auto id1 = generate_request_id();
    auto id2 = generate_request_id();

    auto last_segment = [](const std::string& id) {
        auto pos = id.rfind('-');
        return id.substr(pos + 1);
    };

    auto seq1 = std::stoul(last_segment(id1), nullptr, 16);
    auto seq2 = std::stoul(last_segment(id2), nullptr, 16);
    EXPECT_LT(seq1, seq2);
}

// =============================================================================
// extract_request_id_view Tests
// =============================================================================

class ExtractRequestIdViewTest : public ::testing::Test {
protected:
    using Headers = std::map<std::string, std::string>;
};

TEST_F(ExtractRequestIdViewTest, ReturnsXRequestID) {
    Headers headers = {{"X-Request-ID", "abc-123"}};
    auto view = extract_request_id_view(headers);
    EXPECT_EQ(view, "abc-123");
}

TEST_F(ExtractRequestIdViewTest, ReturnsCorrelationIdAsFallback) {
    Headers headers = {{"X-Correlation-ID", "corr-456"}};
    auto view = extract_request_id_view(headers);
    EXPECT_EQ(view, "corr-456");
}

TEST_F(ExtractRequestIdViewTest, PrefersXRequestIdOverCorrelationId) {
    Headers headers = {
        {"X-Request-ID", "req-primary"},
        {"X-Correlation-ID", "corr-secondary"}
    };
    auto view = extract_request_id_view(headers);
    EXPECT_EQ(view, "req-primary");
}

TEST_F(ExtractRequestIdViewTest, SkipsEmptyXRequestId) {
    // When X-Request-ID exists but is empty, falls through to X-Correlation-ID
    Headers headers = {
        {"X-Request-ID", ""},
        {"X-Correlation-ID", "corr-fallback"}
    };
    auto view = extract_request_id_view(headers);
    EXPECT_EQ(view, "corr-fallback");
}

TEST_F(ExtractRequestIdViewTest, ReturnsEmptyWhenNoHeaders) {
    Headers headers;
    auto view = extract_request_id_view(headers);
    EXPECT_TRUE(view.empty());
}

TEST_F(ExtractRequestIdViewTest, ReturnsEmptyWhenUnrelatedHeaders) {
    Headers headers = {
        {"Content-Type", "application/json"},
        {"Authorization", "Bearer token"}
    };
    auto view = extract_request_id_view(headers);
    EXPECT_TRUE(view.empty());
}

TEST_F(ExtractRequestIdViewTest, ReturnsEmptyWhenBothEmpty) {
    Headers headers = {
        {"X-Request-ID", ""},
        {"X-Correlation-ID", ""}
    };
    auto view = extract_request_id_view(headers);
    EXPECT_TRUE(view.empty());
}

TEST_F(ExtractRequestIdViewTest, PreservesExactValue) {
    Headers headers = {{"X-Request-ID", "  spaces-and-stuff  "}};
    auto view = extract_request_id_view(headers);
    EXPECT_EQ(view, "  spaces-and-stuff  ");
}

// Also test the copying variant
TEST_F(ExtractRequestIdViewTest, CopyingVariantReturnsString) {
    Headers headers = {{"X-Request-ID", "abc-123"}};
    std::string result = extract_request_id(headers);
    EXPECT_EQ(result, "abc-123");
}

TEST_F(ExtractRequestIdViewTest, CopyingVariantReturnsEmptyString) {
    Headers headers;
    std::string result = extract_request_id(headers);
    EXPECT_TRUE(result.empty());
}

// =============================================================================
// extract_traceparent_view Tests
// =============================================================================

class ExtractTraceparentViewTest : public ::testing::Test {
protected:
    using Headers = std::map<std::string, std::string>;
    static constexpr const char* kSampleTraceparent =
        "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";
};

TEST_F(ExtractTraceparentViewTest, FindsLowercaseTraceparent) {
    Headers headers = {{"traceparent", kSampleTraceparent}};
    auto view = extract_traceparent_view(headers);
    EXPECT_EQ(view, kSampleTraceparent);
}

TEST_F(ExtractTraceparentViewTest, FindsTitleCaseTraceparent) {
    Headers headers = {{"Traceparent", kSampleTraceparent}};
    auto view = extract_traceparent_view(headers);
    EXPECT_EQ(view, kSampleTraceparent);
}

TEST_F(ExtractTraceparentViewTest, PrefersLowercaseOverTitleCase) {
    Headers headers = {
        {"traceparent", "lowercase-value"},
        {"Traceparent", "titlecase-value"}
    };
    auto view = extract_traceparent_view(headers);
    EXPECT_EQ(view, "lowercase-value");
}

TEST_F(ExtractTraceparentViewTest, SkipsEmptyLowercaseFallsToTitleCase) {
    Headers headers = {
        {"traceparent", ""},
        {"Traceparent", "titlecase-value"}
    };
    auto view = extract_traceparent_view(headers);
    EXPECT_EQ(view, "titlecase-value");
}

TEST_F(ExtractTraceparentViewTest, ReturnsEmptyWhenMissing) {
    Headers headers = {{"Content-Type", "application/json"}};
    auto view = extract_traceparent_view(headers);
    EXPECT_TRUE(view.empty());
}

TEST_F(ExtractTraceparentViewTest, ReturnsEmptyWhenNoHeaders) {
    Headers headers;
    auto view = extract_traceparent_view(headers);
    EXPECT_TRUE(view.empty());
}

TEST_F(ExtractTraceparentViewTest, ReturnsEmptyWhenBothEmpty) {
    Headers headers = {
        {"traceparent", ""},
        {"Traceparent", ""}
    };
    auto view = extract_traceparent_view(headers);
    EXPECT_TRUE(view.empty());
}

// Also test the copying variant
TEST_F(ExtractTraceparentViewTest, CopyingVariantReturnsString) {
    Headers headers = {{"traceparent", kSampleTraceparent}};
    std::string result = extract_traceparent(headers);
    EXPECT_EQ(result, kSampleTraceparent);
}

TEST_F(ExtractTraceparentViewTest, CopyingVariantReturnsEmptyString) {
    Headers headers;
    std::string result = extract_traceparent(headers);
    EXPECT_TRUE(result.empty());
}
