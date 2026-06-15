// Ranvier Core - Response-usage parser unit tests (pure)
//
// Covers src/response_usage_parser.hpp: extracting the engine's authoritative
// `usage` from non-streaming bodies and streaming SSE blobs, the quoted-key
// disambiguation, last-occurrence-wins, and the both-keys-required fallback.

#include "response_usage_parser.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace ranvier;

TEST(ResponseUsageParser, NonStreamingBody) {
    const std::string body = R"({"id":"x","choices":[{"message":{"content":"hi"}}],)"
                             R"("usage":{"prompt_tokens":42,"completion_tokens":17,"total_tokens":59}})";
    auto u = parse_response_usage(body);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->prompt_tokens, 42);
    EXPECT_EQ(u->completion_tokens, 17);
}

TEST(ResponseUsageParser, StreamingSseBlobUsageEvent) {
    // The usage event rides a final SSE data line (choices empty) before [DONE].
    const std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"hel\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":8,\"completion_tokens\":3}}\n\n"
        "data: [DONE]\n\n";
    auto u = parse_response_usage(sse);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->prompt_tokens, 8);
    EXPECT_EQ(u->completion_tokens, 3);
}

TEST(ResponseUsageParser, NoUsageReturnsNullopt) {
    const std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
        "data: [DONE]\n\n";
    EXPECT_FALSE(parse_response_usage(sse).has_value());
}

TEST(ResponseUsageParser, OnlyOneKeyReturnsNullopt) {
    // A malformed/partial usage with only prompt_tokens -> fall back to estimates.
    EXPECT_FALSE(parse_response_usage(R"({"usage":{"prompt_tokens":5}})").has_value());
}

TEST(ResponseUsageParser, QuotedKeyDisambiguatesFromDetails) {
    // prompt_tokens_details must NOT be mistaken for prompt_tokens.
    const std::string body =
        R"({"usage":{"prompt_tokens_details":{"cached_tokens":4},)"
        R"("prompt_tokens":100,"completion_tokens":20}})";
    auto u = parse_response_usage(body);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->prompt_tokens, 100);
    EXPECT_EQ(u->completion_tokens, 20);
}

TEST(ResponseUsageParser, ZeroCompletionIsValid) {
    auto u = parse_response_usage(R"({"usage":{"prompt_tokens":12,"completion_tokens":0}})");
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->prompt_tokens, 12);
    EXPECT_EQ(u->completion_tokens, 0);
}

TEST(ResponseUsageParser, ToleratesWhitespaceAroundColon) {
    auto u = parse_response_usage(R"({"usage": {"prompt_tokens" :  7 , "completion_tokens":  9}})");
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->prompt_tokens, 7);
    EXPECT_EQ(u->completion_tokens, 9);
}

TEST(ResponseUsageParser, LastOccurrenceWins) {
    // Defensive: if two usage objects appear, the final one (the real usage
    // event) is authoritative.
    const std::string blob =
        R"({"usage":{"prompt_tokens":1,"completion_tokens":1}})"
        R"({"usage":{"prompt_tokens":50,"completion_tokens":60}})";
    auto u = parse_response_usage(blob);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->prompt_tokens, 50);
    EXPECT_EQ(u->completion_tokens, 60);
}

TEST(ResponseUsageParser, EmptyInputIsNullopt) {
    EXPECT_FALSE(parse_response_usage("").has_value());
}
