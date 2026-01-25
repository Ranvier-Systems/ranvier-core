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
// extract_system_messages tests
// =============================================================================

TEST_F(RequestRewriterTest, ExtractSystemMessagesBasic) {
    std::string body = R"({
        "model": "gpt-4",
        "messages": [
            {"role": "system", "content": "You are a helpful assistant."},
            {"role": "user", "content": "What is 2+2?"}
        ]
    })";
    auto result = RequestRewriter::extract_system_messages(body);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "You are a helpful assistant.");
}

TEST_F(RequestRewriterTest, ExtractSystemMessagesMultiple) {
    std::string body = R"({
        "messages": [
            {"role": "system", "content": "You are a helpful assistant."},
            {"role": "system", "content": "Be concise in your responses."},
            {"role": "user", "content": "Hello"}
        ]
    })";
    auto result = RequestRewriter::extract_system_messages(body);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "You are a helpful assistant.\nBe concise in your responses.");
}

TEST_F(RequestRewriterTest, ExtractSystemMessagesNoneFound) {
    std::string body = R"({
        "messages": [
            {"role": "user", "content": "Hello"},
            {"role": "assistant", "content": "Hi there!"}
        ]
    })";
    auto result = RequestRewriter::extract_system_messages(body);
    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractSystemMessagesFromPromptField) {
    // Prompt field has no system message concept - should return nullopt
    std::string body = R"({"prompt": "Hello, world!", "max_tokens": 100})";
    auto result = RequestRewriter::extract_system_messages(body);
    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractSystemMessagesInvalidJson) {
    std::string body = "not valid json";
    auto result = RequestRewriter::extract_system_messages(body);
    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractSystemMessagesEmptyMessagesArray) {
    std::string body = R"({"messages": []})";
    auto result = RequestRewriter::extract_system_messages(body);
    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractSystemMessagesNoRoleField) {
    std::string body = R"({
        "messages": [
            {"content": "I have no role"},
            {"role": "user", "content": "Hello"}
        ]
    })";
    auto result = RequestRewriter::extract_system_messages(body);
    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractSystemMessagesNoContentField) {
    std::string body = R"({
        "messages": [
            {"role": "system"},
            {"role": "user", "content": "Hello"}
        ]
    })";
    auto result = RequestRewriter::extract_system_messages(body);
    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractSystemMessagesEmptyContent) {
    std::string body = R"({
        "messages": [
            {"role": "system", "content": ""},
            {"role": "user", "content": "Hello"}
        ]
    })";
    auto result = RequestRewriter::extract_system_messages(body);
    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractSystemMessagesUnicodeContent) {
    std::string body = R"({
        "messages": [
            {"role": "system", "content": "你是一个有帮助的助手。🤖"},
            {"role": "user", "content": "Hello"}
        ]
    })";
    auto result = RequestRewriter::extract_system_messages(body);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "你是一个有帮助的助手。🤖");
}

TEST_F(RequestRewriterTest, ExtractSystemMessagesInterleavedRoles) {
    // System messages can appear anywhere in the array
    std::string body = R"({
        "messages": [
            {"role": "system", "content": "First system message."},
            {"role": "user", "content": "Hello"},
            {"role": "assistant", "content": "Hi!"},
            {"role": "system", "content": "Second system message."},
            {"role": "user", "content": "How are you?"}
        ]
    })";
    auto result = RequestRewriter::extract_system_messages(body);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "First system message.\nSecond system message.");
}

TEST_F(RequestRewriterTest, ExtractSystemMessagesNonObjectMessage) {
    std::string body = R"({
        "messages": [
            "not an object",
            {"role": "system", "content": "Valid system message"}
        ]
    })";
    auto result = RequestRewriter::extract_system_messages(body);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "Valid system message");
}

TEST_F(RequestRewriterTest, ExtractSystemMessagesNonStringRole) {
    std::string body = R"({
        "messages": [
            {"role": 123, "content": "Invalid role type"},
            {"role": "system", "content": "Valid system message"}
        ]
    })";
    auto result = RequestRewriter::extract_system_messages(body);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "Valid system message");
}

TEST_F(RequestRewriterTest, ExtractSystemMessagesNonStringContent) {
    std::string body = R"({
        "messages": [
            {"role": "system", "content": 12345},
            {"role": "system", "content": "Valid content"}
        ]
    })";
    auto result = RequestRewriter::extract_system_messages(body);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "Valid content");
}

TEST_F(RequestRewriterTest, ExtractSystemMessagesLongContent) {
    // Test with a longer system message typical of real-world usage
    std::string long_system = std::string(1000, 'a');  // 1000 character system message
    std::string body = R"({"messages": [{"role": "system", "content": ")" + long_system + R"("}]})";
    auto result = RequestRewriter::extract_system_messages(body);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), long_system);
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

// =============================================================================
// extract_prefix_token_count tests
// =============================================================================

TEST_F(RequestRewriterTest, ExtractPrefixTokenCountBasic) {
    std::string body = R"({"prompt": "Hello", "prefix_token_count": 100})";
    auto result = RequestRewriter::extract_prefix_token_count(body);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 100);
}

TEST_F(RequestRewriterTest, ExtractPrefixTokenCountNotFound) {
    std::string body = R"({"prompt": "Hello", "max_tokens": 100})";
    auto result = RequestRewriter::extract_prefix_token_count(body);

    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractPrefixTokenCountInvalidJson) {
    std::string body = "not valid json";
    auto result = RequestRewriter::extract_prefix_token_count(body);

    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractPrefixTokenCountZeroValue) {
    // Zero is rejected - must be positive
    std::string body = R"({"prefix_token_count": 0})";
    auto result = RequestRewriter::extract_prefix_token_count(body);

    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractPrefixTokenCountNegativeValue) {
    // Negative values are rejected
    std::string body = R"({"prefix_token_count": -10})";
    auto result = RequestRewriter::extract_prefix_token_count(body);

    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractPrefixTokenCountLargeValue) {
    std::string body = R"({"prefix_token_count": 1000000})";
    auto result = RequestRewriter::extract_prefix_token_count(body);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1000000);
}

TEST_F(RequestRewriterTest, ExtractPrefixTokenCountStringValue) {
    // String values are rejected - must be integer
    std::string body = R"({"prefix_token_count": "100"})";
    auto result = RequestRewriter::extract_prefix_token_count(body);

    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractPrefixTokenCountFloatValue) {
    // Float values are rejected - must be integer
    std::string body = R"({"prefix_token_count": 100.5})";
    auto result = RequestRewriter::extract_prefix_token_count(body);

    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractPrefixTokenCountWithOtherFields) {
    std::string body = R"({
        "model": "gpt-4",
        "messages": [{"role": "user", "content": "Hello"}],
        "prefix_token_count": 256,
        "max_tokens": 100
    })";
    auto result = RequestRewriter::extract_prefix_token_count(body);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 256);
}

TEST_F(RequestRewriterTest, ExtractPrefixTokenCountInt64) {
    // Test with a value that requires int64
    std::string body = R"({"prefix_token_count": 2147483648})";  // INT32_MAX + 1
    auto result = RequestRewriter::extract_prefix_token_count(body);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 2147483648ULL);
}

TEST_F(RequestRewriterTest, ExtractPrefixTokenCountNull) {
    // Null value is rejected
    std::string body = R"({"prefix_token_count": null})";
    auto result = RequestRewriter::extract_prefix_token_count(body);

    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractPrefixTokenCountBoolValue) {
    // Bool value is rejected
    std::string body = R"({"prefix_token_count": true})";
    auto result = RequestRewriter::extract_prefix_token_count(body);

    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractPrefixTokenCountArrayValue) {
    // Array value is rejected - must be integer
    std::string body = R"({"prefix_token_count": [100]})";
    auto result = RequestRewriter::extract_prefix_token_count(body);

    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractPrefixTokenCountObjectValue) {
    // Object value is rejected - must be integer
    std::string body = R"({"prefix_token_count": {"value": 100}})";
    auto result = RequestRewriter::extract_prefix_token_count(body);

    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractPrefixTokenCountOne) {
    // Minimum valid value is 1
    std::string body = R"({"prefix_token_count": 1})";
    auto result = RequestRewriter::extract_prefix_token_count(body);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1);
}

// =============================================================================
// extract_prefix_boundaries tests (multi-depth routing)
// =============================================================================

TEST_F(RequestRewriterTest, ExtractPrefixBoundariesBasic) {
    std::string body = R"({"prefix_boundaries": [100, 200, 300]})";
    auto result = RequestRewriter::extract_prefix_boundaries(body);

    ASSERT_EQ(result.size(), 3);
    EXPECT_EQ(result[0], 100);
    EXPECT_EQ(result[1], 200);
    EXPECT_EQ(result[2], 300);
}

TEST_F(RequestRewriterTest, ExtractPrefixBoundariesNotFound) {
    std::string body = R"({"prompt": "Hello"})";
    auto result = RequestRewriter::extract_prefix_boundaries(body);

    EXPECT_TRUE(result.empty());
}

TEST_F(RequestRewriterTest, ExtractPrefixBoundariesEmptyArray) {
    std::string body = R"({"prefix_boundaries": []})";
    auto result = RequestRewriter::extract_prefix_boundaries(body);

    EXPECT_TRUE(result.empty());
}

TEST_F(RequestRewriterTest, ExtractPrefixBoundariesInvalidJson) {
    std::string body = "not valid json";
    auto result = RequestRewriter::extract_prefix_boundaries(body);

    EXPECT_TRUE(result.empty());
}

TEST_F(RequestRewriterTest, ExtractPrefixBoundariesNotArray) {
    std::string body = R"({"prefix_boundaries": 100})";
    auto result = RequestRewriter::extract_prefix_boundaries(body);

    EXPECT_TRUE(result.empty());
}

TEST_F(RequestRewriterTest, ExtractPrefixBoundariesFiltersZeroAndNegative) {
    // Zero and negative values should be filtered out
    std::string body = R"({"prefix_boundaries": [0, 100, -5, 200, 0]})";
    auto result = RequestRewriter::extract_prefix_boundaries(body);

    ASSERT_EQ(result.size(), 2);
    EXPECT_EQ(result[0], 100);
    EXPECT_EQ(result[1], 200);
}

TEST_F(RequestRewriterTest, ExtractPrefixBoundariesSortsAndDeduplicates) {
    // Result should be sorted and deduplicated
    std::string body = R"({"prefix_boundaries": [300, 100, 200, 100, 300]})";
    auto result = RequestRewriter::extract_prefix_boundaries(body);

    ASSERT_EQ(result.size(), 3);
    EXPECT_EQ(result[0], 100);
    EXPECT_EQ(result[1], 200);
    EXPECT_EQ(result[2], 300);
}

TEST_F(RequestRewriterTest, ExtractPrefixBoundariesMixedTypes) {
    // Should handle different integer types
    std::string body = R"({"prefix_boundaries": [100, 2147483648, 50]})";
    auto result = RequestRewriter::extract_prefix_boundaries(body);

    ASSERT_EQ(result.size(), 3);
    EXPECT_EQ(result[0], 50);
    EXPECT_EQ(result[1], 100);
    EXPECT_EQ(result[2], 2147483648ULL);
}

TEST_F(RequestRewriterTest, ExtractPrefixBoundariesIgnoresNonIntegers) {
    // Non-integer values should be ignored
    std::string body = R"({"prefix_boundaries": [100, "string", 200, null, 300]})";
    auto result = RequestRewriter::extract_prefix_boundaries(body);

    ASSERT_EQ(result.size(), 3);
    EXPECT_EQ(result[0], 100);
    EXPECT_EQ(result[1], 200);
    EXPECT_EQ(result[2], 300);
}

// =============================================================================
// extract_message_boundaries tests (multi-depth routing)
// =============================================================================

TEST_F(RequestRewriterTest, ExtractMessageBoundariesBasic) {
    std::string body = R"({
        "messages": [
            {"role": "system", "content": "You are helpful."},
            {"role": "user", "content": "Hello!"}
        ]
    })";

    // Simple tokenizer: 1 token per character
    auto tokenize_fn = [](const std::string& text) -> size_t {
        return text.length();
    };

    auto result = RequestRewriter::extract_message_boundaries(body, tokenize_fn);

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->boundaries.size(), 2);
    // Each message is "<|role|>\ncontent"
    // System: "<|system|>\nYou are helpful." = 11 + 16 = 27 chars
    // User: "<|user|>\nHello!" = 9 + 6 = 15 chars
    EXPECT_GT(result->boundaries[0], 0);
    EXPECT_GT(result->boundaries[1], result->boundaries[0]);
    EXPECT_GT(result->system_boundary, 0);
}

TEST_F(RequestRewriterTest, ExtractMessageBoundariesNoTokenizer) {
    std::string body = R"({"messages": [{"role": "user", "content": "Hello"}]})";

    auto result = RequestRewriter::extract_message_boundaries(body, nullptr);

    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractMessageBoundariesInvalidJson) {
    std::string body = "not valid json";

    auto tokenize_fn = [](const std::string&) -> size_t { return 10; };
    auto result = RequestRewriter::extract_message_boundaries(body, tokenize_fn);

    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractMessageBoundariesNoMessages) {
    std::string body = R"({"prompt": "Hello"})";

    auto tokenize_fn = [](const std::string&) -> size_t { return 10; };
    auto result = RequestRewriter::extract_message_boundaries(body, tokenize_fn);

    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractMessageBoundariesEmptyMessages) {
    std::string body = R"({"messages": []})";

    auto tokenize_fn = [](const std::string&) -> size_t { return 10; };
    auto result = RequestRewriter::extract_message_boundaries(body, tokenize_fn);

    EXPECT_FALSE(result.has_value());
}

TEST_F(RequestRewriterTest, ExtractMessageBoundariesSystemBoundary) {
    std::string body = R"({
        "messages": [
            {"role": "system", "content": "System prompt here"},
            {"role": "user", "content": "User message"}
        ]
    })";

    // Fixed tokenizer: each message = 100 tokens
    auto tokenize_fn = [](const std::string&) -> size_t { return 100; };

    auto result = RequestRewriter::extract_message_boundaries(body, tokenize_fn);

    ASSERT_TRUE(result.has_value());
    // System boundary should be at 100 (before user message)
    EXPECT_EQ(result->system_boundary, 100);
    // Boundaries should be [100, 200]
    ASSERT_EQ(result->boundaries.size(), 2);
    EXPECT_EQ(result->boundaries[0], 100);
    EXPECT_EQ(result->boundaries[1], 200);
}

TEST_F(RequestRewriterTest, ExtractMessageBoundariesOnlySystemMessages) {
    std::string body = R"({
        "messages": [
            {"role": "system", "content": "First system"},
            {"role": "system", "content": "Second system"}
        ]
    })";

    auto tokenize_fn = [](const std::string&) -> size_t { return 50; };

    auto result = RequestRewriter::extract_message_boundaries(body, tokenize_fn);

    ASSERT_TRUE(result.has_value());
    // All messages are system, so system_boundary should be at the end
    EXPECT_EQ(result->system_boundary, 100);
    ASSERT_EQ(result->boundaries.size(), 2);
}

TEST_F(RequestRewriterTest, ExtractMessageBoundariesMultiTurn) {
    std::string body = R"({
        "messages": [
            {"role": "system", "content": "You are helpful"},
            {"role": "user", "content": "Hello"},
            {"role": "assistant", "content": "Hi there!"},
            {"role": "user", "content": "How are you?"}
        ]
    })";

    // Each message = 10 tokens for simplicity
    auto tokenize_fn = [](const std::string&) -> size_t { return 10; };

    auto result = RequestRewriter::extract_message_boundaries(body, tokenize_fn);

    ASSERT_TRUE(result.has_value());
    // 4 messages = 4 boundaries at 10, 20, 30, 40
    ASSERT_EQ(result->boundaries.size(), 4);
    EXPECT_EQ(result->boundaries[0], 10);
    EXPECT_EQ(result->boundaries[1], 20);
    EXPECT_EQ(result->boundaries[2], 30);
    EXPECT_EQ(result->boundaries[3], 40);
    // System boundary is before first non-system message
    EXPECT_EQ(result->system_boundary, 10);
}

// =============================================================================
// BPE Tokenization Boundary Alignment Tests
// =============================================================================
// These tests verify that extract_system_messages() + "\n" is a text prefix of
// extract_text(). This property is critical for correct BPE tokenization:
//
// BPE tokenizers may produce different tokens for "text" vs "text\n" due to
// subword boundary effects. For example:
//   tokenize("helpful")     -> [1234]        (one way)
//   tokenize("helpful\n")   -> [5678]        (different token!)
//   tokenize("helpful\nHi") -> [5678, 9999]  (5678, not 1234)
//
// By ensuring system_messages + "\n" is a text prefix of the full text,
// we guarantee that tokenizing (system_messages + "\n") produces tokens
// that are a prefix of tokenizing the full text.

TEST_F(RequestRewriterTest, BPEBoundaryAlignmentBasic) {
    // Single system message followed by user message
    std::string body = R"({
        "messages": [
            {"role": "system", "content": "You are a helpful assistant."},
            {"role": "user", "content": "What is 2+2?"}
        ]
    })";

    auto full_text = RequestRewriter::extract_text(body);
    auto system_text = RequestRewriter::extract_system_messages(body);

    ASSERT_TRUE(full_text.has_value());
    ASSERT_TRUE(system_text.has_value());

    // The key property: system_text + "\n" should be a prefix of full_text
    std::string system_with_newline = *system_text + "\n";
    EXPECT_TRUE(full_text->starts_with(system_with_newline))
        << "Expected full_text to start with system_text + newline\n"
        << "full_text: \"" << *full_text << "\"\n"
        << "system_text + \\n: \"" << system_with_newline << "\"";
}

TEST_F(RequestRewriterTest, BPEBoundaryAlignmentMultipleSystemMessages) {
    // Multiple system messages followed by user message
    std::string body = R"({
        "messages": [
            {"role": "system", "content": "You are a helpful assistant."},
            {"role": "system", "content": "Be concise."},
            {"role": "user", "content": "Hello"}
        ]
    })";

    auto full_text = RequestRewriter::extract_text(body);
    auto system_text = RequestRewriter::extract_system_messages(body);

    ASSERT_TRUE(full_text.has_value());
    ASSERT_TRUE(system_text.has_value());

    // Multiple system messages are joined with \n, then another \n before user
    std::string system_with_newline = *system_text + "\n";
    EXPECT_TRUE(full_text->starts_with(system_with_newline))
        << "Expected full_text to start with system_text + newline\n"
        << "full_text: \"" << *full_text << "\"\n"
        << "system_text + \\n: \"" << system_with_newline << "\"";
}

TEST_F(RequestRewriterTest, BPEBoundaryAlignmentMultiTurnConversation) {
    // Multi-turn conversation with system, user, assistant, user
    std::string body = R"({
        "messages": [
            {"role": "system", "content": "You are helpful."},
            {"role": "user", "content": "Hi"},
            {"role": "assistant", "content": "Hello!"},
            {"role": "user", "content": "How are you?"}
        ]
    })";

    auto full_text = RequestRewriter::extract_text(body);
    auto system_text = RequestRewriter::extract_system_messages(body);

    ASSERT_TRUE(full_text.has_value());
    ASSERT_TRUE(system_text.has_value());

    std::string system_with_newline = *system_text + "\n";
    EXPECT_TRUE(full_text->starts_with(system_with_newline))
        << "Expected full_text to start with system_text + newline\n"
        << "full_text: \"" << *full_text << "\"\n"
        << "system_text + \\n: \"" << system_with_newline << "\"";
}

TEST_F(RequestRewriterTest, BPEBoundaryAlignmentSpecialCharacters) {
    // System message with characters that might affect BPE boundaries
    std::string body = R"({
        "messages": [
            {"role": "system", "content": "You are helpful.\nFollow instructions carefully."},
            {"role": "user", "content": "Test query"}
        ]
    })";

    auto full_text = RequestRewriter::extract_text(body);
    auto system_text = RequestRewriter::extract_system_messages(body);

    ASSERT_TRUE(full_text.has_value());
    ASSERT_TRUE(system_text.has_value());

    std::string system_with_newline = *system_text + "\n";
    EXPECT_TRUE(full_text->starts_with(system_with_newline))
        << "Expected full_text to start with system_text + newline\n"
        << "full_text: \"" << *full_text << "\"\n"
        << "system_text + \\n: \"" << system_with_newline << "\"";
}

TEST_F(RequestRewriterTest, BPEBoundaryAlignmentEmptyContent) {
    // Edge case: empty user content after system message
    std::string body = R"({
        "messages": [
            {"role": "system", "content": "System prompt"},
            {"role": "user", "content": ""}
        ]
    })";

    auto full_text = RequestRewriter::extract_text(body);
    auto system_text = RequestRewriter::extract_system_messages(body);

    ASSERT_TRUE(full_text.has_value());
    ASSERT_TRUE(system_text.has_value());

    // Even with empty user content, the format should be consistent
    std::string system_with_newline = *system_text + "\n";
    EXPECT_TRUE(full_text->starts_with(system_with_newline))
        << "Expected full_text to start with system_text + newline\n"
        << "full_text: \"" << *full_text << "\"\n"
        << "system_text + \\n: \"" << system_with_newline << "\"";
}
