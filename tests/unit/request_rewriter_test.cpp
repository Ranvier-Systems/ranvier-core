// Unit tests for RequestRewriter
//
// Tests the JSON parsing and rewriting functionality for
// injecting prompt_token_ids into LLM requests.

#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include "request_rewriter.hpp"

using namespace ranvier;

class RequestRewriterTest : public ::testing::Test {
protected:
    // Helper to parse JSON and check for prompt_token_ids
    bool has_prompt_token_ids(const std::string& json) {
        rapidjson::Document doc;
        doc.Parse(json.c_str());
        return !doc.HasParseError() && doc.HasMember("prompt_token_ids");
    }

    // Helper to get prompt_token_ids as vector
    std::vector<int32_t> get_token_ids(const std::string& json) {
        rapidjson::Document doc;
        doc.Parse(json.c_str());
        std::vector<int32_t> result;
        if (!doc.HasParseError() && doc.HasMember("prompt_token_ids") &&
            doc["prompt_token_ids"].IsArray()) {
            const auto& arr = doc["prompt_token_ids"];
            for (rapidjson::SizeType i = 0; i < arr.Size(); ++i) {
                result.push_back(arr[i].GetInt());
            }
        }
        return result;
    }
};

// =============================================================================
// extract_text tests
// =============================================================================

TEST_F(RequestRewriterTest, ExtractTextFromPromptField) {
    std::string body = R"({"prompt": "Hello, world!", "max_tokens": 100})";
    auto result = RequestRewriter::extract_text(body);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "Hello, world!");
}

TEST_F(RequestRewriterTest, ExtractTextFromMessages) {
    std::string body = R"({
        "model": "gpt-4",
        "messages": [
            {"role": "system", "content": "You are a helpful assistant."},
            {"role": "user", "content": "What is 2+2?"}
        ],
        "max_tokens": 100
    })";
    auto result = RequestRewriter::extract_text(body);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "You are a helpful assistant.\nWhat is 2+2?");
}

TEST_F(RequestRewriterTest, ExtractTextPrefersPromptOverMessages) {
    // If both prompt and messages exist, prompt takes precedence
    std::string body = R"({
        "prompt": "Direct prompt",
        "messages": [{"role": "user", "content": "Message content"}]
    })";
    auto result = RequestRewriter::extract_text(body);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "Direct prompt");
}

TEST_F(RequestRewriterTest, ExtractTextReturnsNulloptForNoContent) {
    std::string body = R"({"model": "gpt-4", "max_tokens": 100})";
    auto result = RequestRewriter::extract_text(body);
    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractTextReturnsNulloptForInvalidJson) {
    std::string body = "not valid json";
    auto result = RequestRewriter::extract_text(body);
    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractTextReturnsNulloptForEmptyMessages) {
    std::string body = R"({"messages": []})";
    auto result = RequestRewriter::extract_text(body);
    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractTextHandlesEmptyPrompt) {
    std::string body = R"({"prompt": ""})";
    auto result = RequestRewriter::extract_text(body);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "");
}

TEST_F(RequestRewriterTest, ExtractTextSkipsMessagesWithoutContent) {
    std::string body = R"({
        "messages": [
            {"role": "system"},
            {"role": "user", "content": "Hello"}
        ]
    })";
    auto result = RequestRewriter::extract_text(body);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "Hello");
}

// =============================================================================
// rewrite tests
// =============================================================================

TEST_F(RequestRewriterTest, RewriteAddsTokenIds) {
    std::string body = R"({"prompt": "Hello", "max_tokens": 100})";
    std::vector<int32_t> tokens = {15496, 11, 995};  // Example token IDs

    auto result = RequestRewriter::rewrite(body, tokens);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.error.empty());

    auto retrieved = get_token_ids(result.body);
    EXPECT_EQ(retrieved, tokens);
}

TEST_F(RequestRewriterTest, RewritePreservesOriginalFields) {
    std::string body = R"({"prompt": "Hello", "max_tokens": 100, "temperature": 0.7})";
    std::vector<int32_t> tokens = {15496};

    auto result = RequestRewriter::rewrite(body, tokens);
    EXPECT_TRUE(result.success);

    rapidjson::Document doc;
    doc.Parse(result.body.c_str());
    EXPECT_FALSE(doc.HasParseError());
    EXPECT_TRUE(doc.HasMember("prompt"));
    EXPECT_TRUE(doc.HasMember("max_tokens"));
    EXPECT_TRUE(doc.HasMember("temperature"));
    EXPECT_EQ(doc["max_tokens"].GetInt(), 100);
    EXPECT_DOUBLE_EQ(doc["temperature"].GetDouble(), 0.7);
}

TEST_F(RequestRewriterTest, RewriteWorksWithMessages) {
    std::string body = R"({"messages": [{"role": "user", "content": "Hi"}]})";
    std::vector<int32_t> tokens = {1, 2, 3};

    auto result = RequestRewriter::rewrite(body, tokens);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(has_prompt_token_ids(result.body));
}

TEST_F(RequestRewriterTest, RewriteFailsWithEmptyTokens) {
    std::string body = R"({"prompt": "Hello"})";
    std::vector<int32_t> tokens;  // Empty

    auto result = RequestRewriter::rewrite(body, tokens);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.body, body);  // Original body returned
}

TEST_F(RequestRewriterTest, RewriteFailsWithInvalidJson) {
    std::string body = "not valid json";
    std::vector<int32_t> tokens = {1, 2, 3};

    auto result = RequestRewriter::rewrite(body, tokens);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.body, body);
}

TEST_F(RequestRewriterTest, RewriteFailsWithNonObjectJson) {
    std::string body = R"([1, 2, 3])";  // Array, not object
    std::vector<int32_t> tokens = {1, 2, 3};

    auto result = RequestRewriter::rewrite(body, tokens);
    EXPECT_FALSE(result.success);
}

TEST_F(RequestRewriterTest, RewriteFailsWithNoPromptOrMessages) {
    std::string body = R"({"model": "gpt-4", "max_tokens": 100})";
    std::vector<int32_t> tokens = {1, 2, 3};

    auto result = RequestRewriter::rewrite(body, tokens);
    EXPECT_FALSE(result.success);
}

TEST_F(RequestRewriterTest, RewriteDoesNotOverwriteExistingTokenIds) {
    std::string body = R"({"prompt": "Hello", "prompt_token_ids": [999]})";
    std::vector<int32_t> tokens = {1, 2, 3};

    auto result = RequestRewriter::rewrite(body, tokens);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.body, body);  // Original returned

    // Verify original token_ids preserved
    auto ids = get_token_ids(result.body);
    EXPECT_EQ(ids.size(), 1);
    EXPECT_EQ(ids[0], 999);
}

TEST_F(RequestRewriterTest, RewriteHandlesLargeTokenArrays) {
    std::string body = R"({"prompt": "A long prompt..."})";
    std::vector<int32_t> tokens;
    tokens.reserve(10000);
    for (int i = 0; i < 10000; ++i) {
        tokens.push_back(i);
    }

    auto result = RequestRewriter::rewrite(body, tokens);
    EXPECT_TRUE(result.success);

    auto retrieved = get_token_ids(result.body);
    EXPECT_EQ(retrieved.size(), 10000);
    EXPECT_EQ(retrieved[0], 0);
    EXPECT_EQ(retrieved[9999], 9999);
}

TEST_F(RequestRewriterTest, RewriteHandlesNegativeTokenIds) {
    std::string body = R"({"prompt": "Test"})";
    std::vector<int32_t> tokens = {-1, 0, 1, INT32_MAX, INT32_MIN};

    auto result = RequestRewriter::rewrite(body, tokens);
    EXPECT_TRUE(result.success);

    auto retrieved = get_token_ids(result.body);
    EXPECT_EQ(retrieved, tokens);
}

TEST_F(RequestRewriterTest, RewriteHandlesUnicodeContent) {
    std::string body = R"({"prompt": "Hello, 世界! 🌍"})";
    std::vector<int32_t> tokens = {1, 2, 3};

    auto result = RequestRewriter::rewrite(body, tokens);
    EXPECT_TRUE(result.success);

    rapidjson::Document doc;
    doc.Parse(result.body.c_str());
    EXPECT_FALSE(doc.HasParseError());
    EXPECT_STREQ(doc["prompt"].GetString(), "Hello, 世界! 🌍");
}

TEST_F(RequestRewriterTest, RewriteHandlesNestedJsonInMessages) {
    std::string body = R"({
        "messages": [
            {"role": "user", "content": "Parse this: {\"key\": \"value\"}"}
        ]
    })";
    std::vector<int32_t> tokens = {1, 2, 3};

    auto result = RequestRewriter::rewrite(body, tokens);
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(has_prompt_token_ids(result.body));
}

// =============================================================================
// Content-Length change verification
// =============================================================================

TEST_F(RequestRewriterTest, RewriteIncreasesBodySize) {
    std::string body = R"({"prompt": "Hi"})";
    std::vector<int32_t> tokens = {1, 2, 3, 4, 5};

    auto result = RequestRewriter::rewrite(body, tokens);
    EXPECT_TRUE(result.success);
    EXPECT_GT(result.body.size(), body.size());
}

TEST_F(RequestRewriterTest, RewriteProducesValidJson) {
    std::string body = R"({"prompt": "Test", "extra": {"nested": true}})";
    std::vector<int32_t> tokens = {100, 200, 300};

    auto result = RequestRewriter::rewrite(body, tokens);
    EXPECT_TRUE(result.success);

    rapidjson::Document doc;
    doc.Parse(result.body.c_str());
    EXPECT_FALSE(doc.HasParseError());
    EXPECT_TRUE(doc.IsObject());
}

// =============================================================================
// extract_prompt_token_ids tests
// =============================================================================

TEST_F(RequestRewriterTest, ExtractPromptTokenIdsBasic) {
    std::string body = R"({"prompt": "Hello", "prompt_token_ids": [1, 2, 3, 4, 5]})";
    auto result = RequestRewriter::extract_prompt_token_ids(body, 100000);

    EXPECT_TRUE(result.found);
    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.error.empty());
    EXPECT_EQ(result.tokens.size(), 5);
    EXPECT_EQ(result.tokens[0], 1);
    EXPECT_EQ(result.tokens[4], 5);
}

TEST_F(RequestRewriterTest, ExtractPromptTokenIdsNotFound) {
    std::string body = R"({"prompt": "Hello", "max_tokens": 100})";
    auto result = RequestRewriter::extract_prompt_token_ids(body, 100000);

    EXPECT_FALSE(result.found);
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.tokens.empty());
    EXPECT_TRUE(result.error.empty());  // Not found is not an error
}

TEST_F(RequestRewriterTest, ExtractPromptTokenIdsInvalidJson) {
    std::string body = "not valid json";
    auto result = RequestRewriter::extract_prompt_token_ids(body, 100000);

    EXPECT_FALSE(result.found);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error, "Invalid JSON");
}

TEST_F(RequestRewriterTest, ExtractPromptTokenIdsNotArray) {
    std::string body = R"({"prompt_token_ids": "not an array"})";
    auto result = RequestRewriter::extract_prompt_token_ids(body, 100000);

    EXPECT_TRUE(result.found);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error, "prompt_token_ids must be an array");
}

TEST_F(RequestRewriterTest, ExtractPromptTokenIdsEmptyArray) {
    std::string body = R"({"prompt_token_ids": []})";
    auto result = RequestRewriter::extract_prompt_token_ids(body, 100000);

    EXPECT_TRUE(result.found);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error, "prompt_token_ids array is empty");
}

TEST_F(RequestRewriterTest, ExtractPromptTokenIdsNonIntegerElement) {
    std::string body = R"({"prompt_token_ids": [1, 2, "three", 4]})";
    auto result = RequestRewriter::extract_prompt_token_ids(body, 100000);

    EXPECT_TRUE(result.found);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error, "prompt_token_ids[2] is not an integer");
    EXPECT_TRUE(result.tokens.empty());
}

TEST_F(RequestRewriterTest, ExtractPromptTokenIdsNegativeValue) {
    std::string body = R"({"prompt_token_ids": [1, 2, -5, 4]})";
    auto result = RequestRewriter::extract_prompt_token_ids(body, 100000);

    EXPECT_TRUE(result.found);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error, "prompt_token_ids[2] is negative: -5");
    EXPECT_TRUE(result.tokens.empty());
}

TEST_F(RequestRewriterTest, ExtractPromptTokenIdsExceedsMax) {
    std::string body = R"({"prompt_token_ids": [1, 2, 999999, 4]})";
    auto result = RequestRewriter::extract_prompt_token_ids(body, 50000);

    EXPECT_TRUE(result.found);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error, "prompt_token_ids[2] exceeds max_token_id (999999 > 50000)");
    EXPECT_TRUE(result.tokens.empty());
}

TEST_F(RequestRewriterTest, ExtractPromptTokenIdsBoundaryValid) {
    std::string body = R"({"prompt_token_ids": [0, 50000]})";
    auto result = RequestRewriter::extract_prompt_token_ids(body, 50000);

    EXPECT_TRUE(result.found);
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.tokens.size(), 2);
    EXPECT_EQ(result.tokens[0], 0);
    EXPECT_EQ(result.tokens[1], 50000);
}

TEST_F(RequestRewriterTest, ExtractPromptTokenIdsBoundaryInvalid) {
    std::string body = R"({"prompt_token_ids": [0, 50001]})";
    auto result = RequestRewriter::extract_prompt_token_ids(body, 50000);

    EXPECT_TRUE(result.found);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error, "prompt_token_ids[1] exceeds max_token_id (50001 > 50000)");
}

TEST_F(RequestRewriterTest, ExtractPromptTokenIdsLargeArray) {
    // Build a large token array
    std::string body = R"({"prompt_token_ids": [)";
    for (int i = 0; i < 10000; ++i) {
        if (i > 0) body += ",";
        body += std::to_string(i);
    }
    body += "]}";

    auto result = RequestRewriter::extract_prompt_token_ids(body, 100000);

    EXPECT_TRUE(result.found);
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.tokens.size(), 10000);
    EXPECT_EQ(result.tokens[0], 0);
    EXPECT_EQ(result.tokens[9999], 9999);
}

TEST_F(RequestRewriterTest, ExtractPromptTokenIdsWithOtherFields) {
    std::string body = R"({
        "model": "gpt-4",
        "prompt": "Hello world",
        "prompt_token_ids": [15496, 995],
        "max_tokens": 100,
        "temperature": 0.7
    })";
    auto result = RequestRewriter::extract_prompt_token_ids(body, 100000);

    EXPECT_TRUE(result.found);
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.tokens.size(), 2);
    EXPECT_EQ(result.tokens[0], 15496);
    EXPECT_EQ(result.tokens[1], 995);
}

TEST_F(RequestRewriterTest, ExtractPromptTokenIdsZeroMaxTokenId) {
    // Edge case: max_token_id of 0 means only token ID 0 is valid
    std::string body = R"({"prompt_token_ids": [0]})";
    auto result = RequestRewriter::extract_prompt_token_ids(body, 0);

    EXPECT_TRUE(result.found);
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.tokens.size(), 1);
    EXPECT_EQ(result.tokens[0], 0);
}

TEST_F(RequestRewriterTest, ExtractPromptTokenIdsFloatInArray) {
    std::string body = R"({"prompt_token_ids": [1, 2.5, 3]})";
    auto result = RequestRewriter::extract_prompt_token_ids(body, 100000);

    EXPECT_TRUE(result.found);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.error, "prompt_token_ids[1] is not an integer");
}
