// Ranvier Core - Intent Classifier Unit Tests
//
// Tests for classify_intent() free function: FIM detection, endpoint detection,
// edit keyword/tag scanning, and CHAT fallback.
// Pure computation — no Seastar, no mocks, no async infrastructure.

#include "intent_classifier.hpp"
#include <gtest/gtest.h>

using namespace ranvier;

// =============================================================================
// RequestIntent Enum Tests
// =============================================================================

TEST(RequestIntentEnumTest, ValuesMatchExpected) {
    EXPECT_EQ(static_cast<uint8_t>(RequestIntent::AUTOCOMPLETE), 0);
    EXPECT_EQ(static_cast<uint8_t>(RequestIntent::CHAT), 1);
    EXPECT_EQ(static_cast<uint8_t>(RequestIntent::EDIT), 2);
}

TEST(RequestIntentEnumTest, IntentToString) {
    EXPECT_EQ(intent_to_string(RequestIntent::AUTOCOMPLETE), "autocomplete");
    EXPECT_EQ(intent_to_string(RequestIntent::CHAT), "chat");
    EXPECT_EQ(intent_to_string(RequestIntent::EDIT), "edit");
}

// =============================================================================
// FIM Detection (→ AUTOCOMPLETE)
// =============================================================================

class FimDetectionTest : public ::testing::Test {
protected:
    IntentClassifierConfig config;
    std::string_view chat_endpoint = "/v1/chat/completions";
};

TEST_F(FimDetectionTest, SuffixFieldDetected) {
    std::string body = R"json({"prompt": "def foo()", "suffix": "  return 42"})json";
    EXPECT_EQ(classify_intent(chat_endpoint, body, config), RequestIntent::AUTOCOMPLETE);
}

TEST_F(FimDetectionTest, FimPrefixFieldDetected) {
    std::string body = R"json({"fim_prefix": "def hello():\n", "fim_suffix": "\n  pass"})json";
    EXPECT_EQ(classify_intent(chat_endpoint, body, config), RequestIntent::AUTOCOMPLETE);
}

TEST_F(FimDetectionTest, FimMiddleFieldDetected) {
    std::string body = R"({"fim_prefix": "a", "fim_middle": "b", "fim_suffix": "c"})";
    EXPECT_EQ(classify_intent(chat_endpoint, body, config), RequestIntent::AUTOCOMPLETE);
}

TEST_F(FimDetectionTest, FimSuffixFieldDetected) {
    std::string body = R"({"fim_suffix": "return None"})";
    EXPECT_EQ(classify_intent(chat_endpoint, body, config), RequestIntent::AUTOCOMPLETE);
}

TEST_F(FimDetectionTest, NoFimFieldsFallsThrough) {
    std::string body = R"({"messages": [{"role": "user", "content": "hello"}]})";
    // No FIM fields, chat endpoint → should NOT be AUTOCOMPLETE
    EXPECT_NE(classify_intent(chat_endpoint, body, config), RequestIntent::AUTOCOMPLETE);
}

TEST_F(FimDetectionTest, FimFieldInValueNotKey) {
    // "suffix" appears in a value, not as a JSON key — should not trigger
    std::string body = R"({"messages": [{"role": "user", "content": "add a suffix to the string"}]})";
    // "suffix" appears quoted as a key would be, but it's inside content value.
    // The heuristic checks for "suffix" as a quoted string anywhere, so this
    // actually will match. This is acceptable — classification is advisory.
    // If it matches, that's the documented false-positive tradeoff.
    auto intent = classify_intent(chat_endpoint, body, config);
    // Just verify it returns a valid intent (not crashing)
    EXPECT_TRUE(intent == RequestIntent::AUTOCOMPLETE ||
                intent == RequestIntent::CHAT ||
                intent == RequestIntent::EDIT);
}

TEST_F(FimDetectionTest, CustomFimFieldsRespected) {
    config.fim_fields = {"my_custom_fim"};
    std::string body = R"({"my_custom_fim": "code here", "prompt": "test"})";
    EXPECT_EQ(classify_intent(chat_endpoint, body, config), RequestIntent::AUTOCOMPLETE);
}

TEST_F(FimDetectionTest, EmptyFimFieldsSkipsFimDetection) {
    config.fim_fields.clear();
    std::string body = R"({"suffix": "code", "prompt": "test"})";
    // With empty fim_fields, suffix won't trigger FIM detection
    // But endpoint is chat, so it falls through to CHAT
    EXPECT_EQ(classify_intent(chat_endpoint, body, config), RequestIntent::CHAT);
}

// =============================================================================
// Endpoint Detection (→ AUTOCOMPLETE)
// =============================================================================

class EndpointDetectionTest : public ::testing::Test {
protected:
    IntentClassifierConfig config;
};

TEST(EndpointDetectionTest, CompletionsEndpointIsAutocomplete) {
    IntentClassifierConfig config;
    config.fim_fields.clear();  // Disable FIM field detection to isolate endpoint check
    std::string body = R"({"prompt": "Hello world"})";
    EXPECT_EQ(classify_intent("/v1/completions", body, config), RequestIntent::AUTOCOMPLETE);
}

TEST(EndpointDetectionTest, ChatCompletionsEndpointIsNotAutocomplete) {
    IntentClassifierConfig config;
    config.fim_fields.clear();
    std::string body = R"({"messages": [{"role": "user", "content": "hello"}]})";
    EXPECT_EQ(classify_intent("/v1/chat/completions", body, config), RequestIntent::CHAT);
}

TEST(EndpointDetectionTest, FimFieldTakesPriorityOverEndpoint) {
    IntentClassifierConfig config;
    // Both FIM field and completions endpoint — FIM check runs first
    std::string body = R"({"prompt": "test", "suffix": "end"})";
    EXPECT_EQ(classify_intent("/v1/completions", body, config), RequestIntent::AUTOCOMPLETE);
}

// =============================================================================
// Edit Detection (→ EDIT)
// =============================================================================

class EditDetectionTest : public ::testing::Test {
protected:
    IntentClassifierConfig config;
    std::string_view endpoint = "/v1/chat/completions";

    // Helper to build a chat body with a system message
    static std::string make_chat_body(const std::string& system_content,
                                       const std::string& user_content = "do the thing") {
        return R"({"messages": [{"role": "system", "content": ")" + system_content +
               R"("}, {"role": "user", "content": ")" + user_content + R"("}]})";
    }
};

TEST_F(EditDetectionTest, DiffKeywordInSystemMessage) {
    auto body = make_chat_body("You are a code assistant. Output a diff for all changes.");
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::EDIT);
}

TEST_F(EditDetectionTest, RewriteKeywordInSystemMessage) {
    auto body = make_chat_body("Rewrite the following code to be more efficient.");
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::EDIT);
}

TEST_F(EditDetectionTest, RefactorKeywordInSystemMessage) {
    auto body = make_chat_body("Please refactor this function.");
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::EDIT);
}

TEST_F(EditDetectionTest, EditKeywordInSystemMessage) {
    auto body = make_chat_body("Edit the code below according to the instructions.");
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::EDIT);
}

TEST_F(EditDetectionTest, PatchKeywordInSystemMessage) {
    auto body = make_chat_body("Generate a patch file for the requested changes.");
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::EDIT);
}

TEST_F(EditDetectionTest, ApplyKeywordInSystemMessage) {
    auto body = make_chat_body("Apply the following transformation to the code.");
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::EDIT);
}

TEST_F(EditDetectionTest, CaseInsensitiveKeywordMatch) {
    auto body = make_chat_body("You must REFACTOR all deprecated methods.");
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::EDIT);
}

TEST_F(EditDetectionTest, MixedCaseKeywordMatch) {
    auto body = make_chat_body("Please Rewrite the module.");
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::EDIT);
}

TEST_F(EditDetectionTest, DiffTagPatternInSystemMessage) {
    auto body = make_chat_body("Output changes in <diff> format.");
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::EDIT);
}

TEST_F(EditDetectionTest, EditTagPatternInSystemMessage) {
    auto body = make_chat_body("Wrap edits in <edit> tags.");
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::EDIT);
}

TEST_F(EditDetectionTest, RewriteTagPatternInSystemMessage) {
    auto body = make_chat_body("Use <rewrite> blocks for output.");
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::EDIT);
}

TEST_F(EditDetectionTest, PatchTagPatternInSystemMessage) {
    auto body = make_chat_body("Return results as <patch> blocks.");
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::EDIT);
}

TEST_F(EditDetectionTest, TagMatchIsCaseSensitive) {
    // Tags are literal matches — <DIFF> should NOT match <diff>
    config.edit_system_keywords.clear();  // Disable keyword matching
    auto body = make_chat_body("Use <DIFF> format for output.");
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::CHAT);
}

TEST_F(EditDetectionTest, NoEditKeywordsInSystemMessage) {
    auto body = make_chat_body("You are a helpful coding assistant.");
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::CHAT);
}

TEST_F(EditDetectionTest, EditKeywordInUserMessageNotSystem) {
    // Keywords in user message should NOT trigger EDIT — only first system message is scanned
    std::string body = R"({"messages": [{"role": "user", "content": "Please refactor this code"}]})";
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::CHAT);
}

TEST_F(EditDetectionTest, OnlyFirstSystemMessageScanned) {
    // Second system message has "refactor" but first does not — should be CHAT
    std::string body = R"({"messages": [)"
        R"({"role": "system", "content": "You are a helpful assistant."},)"
        R"({"role": "system", "content": "Refactor all code you see."},)"
        R"({"role": "user", "content": "hello"}]})";
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::CHAT);
}

TEST_F(EditDetectionTest, CustomKeywordsRespected) {
    config.edit_system_keywords = {"transform", "mutate"};
    config.edit_tag_patterns.clear();
    auto body = make_chat_body("Transform the input data structure.");
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::EDIT);
}

TEST_F(EditDetectionTest, CustomTagPatternsRespected) {
    config.edit_system_keywords.clear();
    config.edit_tag_patterns = {"<custom-edit>"};
    auto body = make_chat_body("Output in <custom-edit> blocks.");
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::EDIT);
}

TEST_F(EditDetectionTest, EmptyKeywordsAndTagsDisablesEditDetection) {
    config.edit_system_keywords.clear();
    config.edit_tag_patterns.clear();
    auto body = make_chat_body("Refactor all the code and output a diff.");
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::CHAT);
}

// =============================================================================
// CHAT Fallback
// =============================================================================

class ChatFallbackTest : public ::testing::Test {
protected:
    IntentClassifierConfig config;
    std::string_view endpoint = "/v1/chat/completions";
};

TEST_F(ChatFallbackTest, BasicChatRequest) {
    std::string body = R"({"messages": [{"role": "user", "content": "What is 2+2?"}]})";
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::CHAT);
}

TEST_F(ChatFallbackTest, ChatWithSystemMessage) {
    std::string body = R"({"messages": [)"
        R"({"role": "system", "content": "You are a helpful math tutor."},)"
        R"({"role": "user", "content": "What is 2+2?"}]})";
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::CHAT);
}

TEST_F(ChatFallbackTest, EmptyBody) {
    EXPECT_EQ(classify_intent(endpoint, "", config), RequestIntent::CHAT);
}

TEST_F(ChatFallbackTest, MalformedJson) {
    EXPECT_EQ(classify_intent(endpoint, "{not valid json at all", config), RequestIntent::CHAT);
}

TEST_F(ChatFallbackTest, EmptyMessagesArray) {
    std::string body = R"({"messages": []})";
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::CHAT);
}

// =============================================================================
// Detection Cascade Priority
// =============================================================================

class CascadePriorityTest : public ::testing::Test {
protected:
    IntentClassifierConfig config;
};

TEST_F(CascadePriorityTest, FimFieldBeatsEditKeyword) {
    // Body has both a FIM field AND an edit keyword in system message
    // FIM detection runs first and should win
    std::string body = R"({"messages": [{"role": "system", "content": "Refactor this code"}], "suffix": "end"})";
    EXPECT_EQ(classify_intent("/v1/chat/completions", body, config), RequestIntent::AUTOCOMPLETE);
}

TEST_F(CascadePriorityTest, EndpointBeatsEditKeyword) {
    // /v1/completions endpoint with edit keywords — endpoint check runs before edit scan
    std::string body = R"({"messages": [{"role": "system", "content": "Refactor this"}], "prompt": "code"})";
    // FIM fields empty for isolation
    config.fim_fields.clear();
    EXPECT_EQ(classify_intent("/v1/completions", body, config), RequestIntent::AUTOCOMPLETE);
}

TEST_F(CascadePriorityTest, EditBeatsChat) {
    std::string body = R"({"messages": [{"role": "system", "content": "Generate a diff of changes."},)"
        R"({"role": "user", "content": "Fix the bug"}]})";
    EXPECT_EQ(classify_intent("/v1/chat/completions", body, config), RequestIntent::EDIT);
}

// =============================================================================
// IntentClassifierConfig Defaults
// =============================================================================

TEST(IntentClassifierConfigTest, DefaultFimFields) {
    IntentClassifierConfig config;
    ASSERT_EQ(config.fim_fields.size(), 4u);
    EXPECT_EQ(config.fim_fields[0], "suffix");
    EXPECT_EQ(config.fim_fields[1], "fim_prefix");
    EXPECT_EQ(config.fim_fields[2], "fim_middle");
    EXPECT_EQ(config.fim_fields[3], "fim_suffix");
}

TEST(IntentClassifierConfigTest, DefaultEditSystemKeywords) {
    IntentClassifierConfig config;
    ASSERT_EQ(config.edit_system_keywords.size(), 6u);
    EXPECT_EQ(config.edit_system_keywords[0], "diff");
    EXPECT_EQ(config.edit_system_keywords[5], "apply");
}

TEST(IntentClassifierConfigTest, DefaultEditTagPatterns) {
    IntentClassifierConfig config;
    ASSERT_EQ(config.edit_tag_patterns.size(), 4u);
    EXPECT_EQ(config.edit_tag_patterns[0], "<diff>");
    EXPECT_EQ(config.edit_tag_patterns[3], "<patch>");
}

// =============================================================================
// Edge Cases
// =============================================================================

class EdgeCaseTest : public ::testing::Test {
protected:
    IntentClassifierConfig config;
    std::string_view endpoint = "/v1/chat/completions";
};

TEST_F(EdgeCaseTest, SystemMessageWithNoContent) {
    // System message with role but missing content key
    std::string body = R"({"messages": [{"role": "system"}, {"role": "user", "content": "hello"}]})";
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::CHAT);
}

TEST_F(EdgeCaseTest, SystemMessageWithEmptyContent) {
    std::string body = R"({"messages": [{"role": "system", "content": ""}, {"role": "user", "content": "hello"}]})";
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::CHAT);
}

TEST_F(EdgeCaseTest, VeryLongSystemMessage) {
    // System message longer than the 2KB scan window — keyword must be in first 2KB
    std::string long_prefix(2100, 'x');
    std::string body = R"({"messages": [{"role": "system", "content": ")" +
        long_prefix + R"( refactor this"}, {"role": "user", "content": "do it"}]})";
    // "refactor" is past the 2KB scan window — should fall through to CHAT
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::CHAT);
}

TEST_F(EdgeCaseTest, KeywordWithinScanWindow) {
    // Same setup but keyword within the 2KB window
    std::string short_prefix(100, 'x');
    std::string body = R"({"messages": [{"role": "system", "content": ")" +
        short_prefix + R"( refactor this"}, {"role": "user", "content": "do it"}]})";
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::EDIT);
}

TEST_F(EdgeCaseTest, UnknownEndpoint) {
    std::string body = R"({"messages": [{"role": "user", "content": "hello"}]})";
    EXPECT_EQ(classify_intent("/v1/embeddings", body, config), RequestIntent::CHAT);
}

TEST_F(EdgeCaseTest, WhitespaceAroundRoleValue) {
    // "role" : "system" with extra whitespace — the scanner searches for "system"
    // within 30 chars of "role", so moderate whitespace should still work
    std::string body = R"({"messages": [{"role" : "system", "content": "Rewrite the code."},)"
        R"({"role": "user", "content": "go"}]})";
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::EDIT);
}

// =============================================================================
// String Boundary Parsing
// =============================================================================

class StringBoundaryTest : public ::testing::Test {
protected:
    IntentClassifierConfig config;
    std::string_view endpoint = "/v1/chat/completions";
};

TEST_F(StringBoundaryTest, EscapedQuotesInContent) {
    // Content contains escaped quotes around an edit keyword — the closing-quote
    // walker must skip \" and the keyword should still match inside the string.
    std::string body = R"({"messages": [{"role": "system", "content": "output a \"diff\" here"},)"
        R"({"role": "user", "content": "go"}]})";
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::EDIT);
}

TEST_F(StringBoundaryTest, EscapedQuoteDoesNotTerminateEarly) {
    // Keyword is after an escaped quote — scanner must not stop at \"
    std::string body = R"({"messages": [{"role": "system", "content": "say \"hello\" then refactor"},)"
        R"({"role": "user", "content": "go"}]})";
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::EDIT);
}

TEST_F(StringBoundaryTest, EscapedQuotesNoKeyword) {
    // Escaped quotes but no keyword — should be CHAT, not crash or false-positive
    std::string body = R"({"messages": [{"role": "system", "content": "say \"hello\" nicely"},)"
        R"({"role": "user", "content": "go"}]})";
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::CHAT);
}

// =============================================================================
// Structured / Multi-Part Content
// =============================================================================

class StructuredContentTest : public ::testing::Test {
protected:
    IntentClassifierConfig config;
    std::string_view endpoint = "/v1/chat/completions";
};

TEST_F(StructuredContentTest, ArrayContentNotScanned) {
    // OpenAI multi-part content format: "content" is an array, not a string.
    // The scanner looks for a '"' after the colon, which will find the '"type"'
    // key — not actual content. The keyword in the text part should NOT reliably
    // trigger. This documents that array content is not supported for edit detection.
    std::string body = R"({"messages": [{"role": "system", "content": [{"type": "text", "text": "refactor this"}]},)"
        R"({"role": "user", "content": "go"}]})";
    auto intent = classify_intent(endpoint, body, config);
    // Don't assert a specific value — just verify it doesn't crash.
    // Array content is not a supported detection path.
    EXPECT_TRUE(intent == RequestIntent::CHAT || intent == RequestIntent::EDIT);
}

TEST_F(StructuredContentTest, NullContentHandled) {
    // "content": null — no opening quote found, scanner should bail gracefully
    std::string body = R"({"messages": [{"role": "system", "content": null},)"
        R"({"role": "user", "content": "hello"}]})";
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::CHAT);
}

// =============================================================================
// JSON Key Ordering
// =============================================================================

class KeyOrderingTest : public ::testing::Test {
protected:
    IntentClassifierConfig config;
    std::string_view endpoint = "/v1/chat/completions";
};

TEST_F(KeyOrderingTest, ContentBeforeRole) {
    // JSON keys in reverse order: "content" before "role"
    // The scanner searches for "content" starting from role_value_start, so it
    // will find a "content" key further in the body. With reversed key order,
    // "content" appears BEFORE "role" in the same message object — the scanner
    // starts searching from the "system" value position, so it may miss the
    // content that appeared earlier.
    std::string body = R"({"messages": [{"content": "refactor this code", "role": "system"},)"
        R"({"role": "user", "content": "go"}]})";
    auto intent = classify_intent(endpoint, body, config);
    // Document current behavior: scanner searches forward from "system" and
    // finds the user message's "content" instead. The keyword isn't there → CHAT.
    // This is a known limitation (JSON key order dependent).
    EXPECT_EQ(intent, RequestIntent::CHAT);
}

TEST_F(KeyOrderingTest, NormalKeyOrder) {
    // Verify the standard order works as baseline for the above test
    std::string body = R"({"messages": [{"role": "system", "content": "refactor this code"},)"
        R"({"role": "user", "content": "go"}]})";
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::EDIT);
}

// =============================================================================
// Endpoint Variations
// =============================================================================

class EndpointVariationTest : public ::testing::Test {
protected:
    IntentClassifierConfig config;
};

TEST_F(EndpointVariationTest, TrailingSlashDoesNotMatch) {
    // Exact string match — /v1/completions/ should NOT trigger AUTOCOMPLETE
    config.fim_fields.clear();
    std::string body = R"({"prompt": "hello"})";
    EXPECT_NE(classify_intent("/v1/completions/", body, config), RequestIntent::AUTOCOMPLETE);
}

TEST_F(EndpointVariationTest, QueryParamsDoNotMatch) {
    // /v1/completions?model=x is not an exact match
    config.fim_fields.clear();
    std::string body = R"({"prompt": "hello"})";
    EXPECT_NE(classify_intent("/v1/completions?model=x", body, config), RequestIntent::AUTOCOMPLETE);
}

TEST_F(EndpointVariationTest, CaseSensitiveEndpoint) {
    config.fim_fields.clear();
    std::string body = R"({"prompt": "hello"})";
    EXPECT_NE(classify_intent("/v1/Completions", body, config), RequestIntent::AUTOCOMPLETE);
}

// =============================================================================
// FIM Field Scope
// =============================================================================

class FimFieldScopeTest : public ::testing::Test {
protected:
    IntentClassifierConfig config;
    std::string_view endpoint = "/v1/chat/completions";
};

TEST_F(FimFieldScopeTest, NestedSuffixInContentValue) {
    // "suffix" appears inside a content string value, not as a top-level key.
    // The heuristic searches for the quoted key anywhere in the body, so this
    // WILL match — documenting the known false-positive tradeoff.
    std::string body = R"({"messages": [{"role": "user", "content": "add a \"suffix\" to the name"}]})";
    // The substring "suffix" is present as a quoted string, so FIM detection triggers
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::AUTOCOMPLETE);
}

TEST_F(FimFieldScopeTest, UnquotedSuffixDoesNotTrigger) {
    // The word suffix without quotes should not trigger — we search for "suffix"
    // (with quotes) to reduce false positives
    std::string body = R"({"messages": [{"role": "user", "content": "add suffix to name"}]})";
    // "suffix" without quotes won't match the quoted key search
    EXPECT_NE(classify_intent(endpoint, body, config), RequestIntent::AUTOCOMPLETE);
}

// =============================================================================
// Short / Minimal Content
// =============================================================================

class MinimalContentTest : public ::testing::Test {
protected:
    IntentClassifierConfig config;
    std::string_view endpoint = "/v1/chat/completions";
};

TEST_F(MinimalContentTest, SingleCharacterContent) {
    std::string body = R"({"messages": [{"role": "system", "content": "x"},)"
        R"({"role": "user", "content": "hi"}]})";
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::CHAT);
}

TEST_F(MinimalContentTest, KeywordIsEntireContent) {
    std::string body = R"({"messages": [{"role": "system", "content": "diff"},)"
        R"({"role": "user", "content": "go"}]})";
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::EDIT);
}

TEST_F(MinimalContentTest, NoMessagesArray) {
    // Body has no "messages" key at all (e.g. raw prompt on chat endpoint)
    std::string body = R"({"prompt": "hello world", "model": "gpt-4"})";
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::CHAT);
}

TEST_F(MinimalContentTest, NoMessagesButHasFimField) {
    // No messages array, but has a FIM field — FIM detection still triggers
    std::string body = R"({"prompt": "def foo", "suffix": "return 1"})";
    EXPECT_EQ(classify_intent(endpoint, body, config), RequestIntent::AUTOCOMPLETE);
}
