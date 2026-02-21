// Ranvier Core - Chat Template Unit Tests
//
// Tests for pre-compiled chat template formatting (llama3, chatml, mistral)
// and the parse/name helper functions.

#include "chat_template.hpp"
#include <gtest/gtest.h>
#include <string>

using namespace ranvier;

// =============================================================================
// parse_chat_template_format Tests
// =============================================================================

class ParseChatTemplateFormatTest : public ::testing::Test {};

TEST_F(ParseChatTemplateFormatTest, Llama3Aliases) {
    EXPECT_EQ(parse_chat_template_format("llama3"), ChatTemplateFormat::llama3);
    EXPECT_EQ(parse_chat_template_format("llama-3"), ChatTemplateFormat::llama3);
    EXPECT_EQ(parse_chat_template_format("llama"), ChatTemplateFormat::llama3);
}

TEST_F(ParseChatTemplateFormatTest, ChatmlAliases) {
    EXPECT_EQ(parse_chat_template_format("chatml"), ChatTemplateFormat::chatml);
    EXPECT_EQ(parse_chat_template_format("qwen"), ChatTemplateFormat::chatml);
    EXPECT_EQ(parse_chat_template_format("deepseek"), ChatTemplateFormat::chatml);
}

TEST_F(ParseChatTemplateFormatTest, Mistral) {
    EXPECT_EQ(parse_chat_template_format("mistral"), ChatTemplateFormat::mistral);
}

TEST_F(ParseChatTemplateFormatTest, NoneAndUnrecognized) {
    EXPECT_EQ(parse_chat_template_format("none"), ChatTemplateFormat::none);
    EXPECT_EQ(parse_chat_template_format(""), ChatTemplateFormat::none);
    EXPECT_EQ(parse_chat_template_format("unknown"), ChatTemplateFormat::none);
    EXPECT_EQ(parse_chat_template_format("LLAMA3"), ChatTemplateFormat::none);  // case-sensitive
    EXPECT_EQ(parse_chat_template_format("ChatML"), ChatTemplateFormat::none);
}

// =============================================================================
// chat_template_format_name Tests
// =============================================================================

class ChatTemplateFormatNameTest : public ::testing::Test {};

TEST_F(ChatTemplateFormatNameTest, AllFormats) {
    EXPECT_EQ(chat_template_format_name(ChatTemplateFormat::none), "none");
    EXPECT_EQ(chat_template_format_name(ChatTemplateFormat::llama3), "llama3");
    EXPECT_EQ(chat_template_format_name(ChatTemplateFormat::chatml), "chatml");
    EXPECT_EQ(chat_template_format_name(ChatTemplateFormat::mistral), "mistral");
}

// =============================================================================
// ChatTemplate Construction Tests
// =============================================================================

class ChatTemplateConstructionTest : public ::testing::Test {};

TEST_F(ChatTemplateConstructionTest, DefaultIsNone) {
    ChatTemplate tpl;
    EXPECT_EQ(tpl.format(), ChatTemplateFormat::none);
    EXPECT_TRUE(tpl.is_none());
}

TEST_F(ChatTemplateConstructionTest, ExplicitFormat) {
    ChatTemplate tpl(ChatTemplateFormat::llama3);
    EXPECT_EQ(tpl.format(), ChatTemplateFormat::llama3);
    EXPECT_FALSE(tpl.is_none());
}

TEST_F(ChatTemplateConstructionTest, BraceInitialization) {
    // This is the pattern that triggered the original explicit-constructor warning
    ChatTemplate tpl = {ChatTemplateFormat::chatml};
    EXPECT_EQ(tpl.format(), ChatTemplateFormat::chatml);
}

// =============================================================================
// format_message — None (legacy) format
// =============================================================================

class ChatTemplateNoneTest : public ::testing::Test {
protected:
    ChatTemplate tpl{ChatTemplateFormat::none};
};

TEST_F(ChatTemplateNoneTest, FirstMessage) {
    std::string out;
    tpl.format_message(out, "system", "You are helpful.", true);
    EXPECT_EQ(out, "You are helpful.");
}

TEST_F(ChatTemplateNoneTest, SubsequentMessage) {
    std::string out = "First";
    tpl.format_message(out, "user", "Hello", false);
    EXPECT_EQ(out, "First\nHello");
}

TEST_F(ChatTemplateNoneTest, SubsequentOnEmptyBuffer) {
    // is_first=false but buffer is empty → no newline prefix
    std::string out;
    tpl.format_message(out, "user", "Hello", false);
    EXPECT_EQ(out, "Hello");
}

TEST_F(ChatTemplateNoneTest, MultipleMessages) {
    std::string out;
    tpl.format_message(out, "system", "Be nice.", true);
    tpl.format_message(out, "user", "Hi", false);
    tpl.format_message(out, "assistant", "Hello!", false);
    EXPECT_EQ(out, "Be nice.\nHi\nHello!");
}

TEST_F(ChatTemplateNoneTest, EmptyContent) {
    std::string out;
    tpl.format_message(out, "system", "", true);
    EXPECT_EQ(out, "");
}

// =============================================================================
// format_message — Llama 3 format
// =============================================================================

class ChatTemplateLlama3Test : public ::testing::Test {
protected:
    ChatTemplate tpl{ChatTemplateFormat::llama3};
};

TEST_F(ChatTemplateLlama3Test, FirstMessageHasBOS) {
    std::string out;
    tpl.format_message(out, "system", "You are helpful.", true);
    EXPECT_EQ(out,
        "<|begin_of_text|>"
        "<|start_header_id|>system<|end_header_id|>\n\n"
        "You are helpful."
        "<|eot_id|>");
}

TEST_F(ChatTemplateLlama3Test, SubsequentMessageNoBOS) {
    std::string out;
    tpl.format_message(out, "user", "Hello", false);
    EXPECT_EQ(out,
        "<|start_header_id|>user<|end_header_id|>\n\n"
        "Hello"
        "<|eot_id|>");
}

TEST_F(ChatTemplateLlama3Test, FullConversation) {
    std::string out;
    tpl.format_message(out, "system", "You are helpful.", true);
    tpl.format_message(out, "user", "Hello!", false);
    tpl.append_generation_prompt(out);

    std::string expected =
        "<|begin_of_text|>"
        "<|start_header_id|>system<|end_header_id|>\n\n"
        "You are helpful.<|eot_id|>"
        "<|start_header_id|>user<|end_header_id|>\n\n"
        "Hello!<|eot_id|>"
        "<|start_header_id|>assistant<|end_header_id|>\n\n";
    EXPECT_EQ(out, expected);
}

TEST_F(ChatTemplateLlama3Test, MultiTurnConversation) {
    std::string out;
    tpl.format_message(out, "system", "You are helpful.", true);
    tpl.format_message(out, "user", "Hi", false);
    tpl.format_message(out, "assistant", "Hello!", false);
    tpl.format_message(out, "user", "How are you?", false);
    tpl.append_generation_prompt(out);

    std::string expected =
        "<|begin_of_text|>"
        "<|start_header_id|>system<|end_header_id|>\n\nYou are helpful.<|eot_id|>"
        "<|start_header_id|>user<|end_header_id|>\n\nHi<|eot_id|>"
        "<|start_header_id|>assistant<|end_header_id|>\n\nHello!<|eot_id|>"
        "<|start_header_id|>user<|end_header_id|>\n\nHow are you?<|eot_id|>"
        "<|start_header_id|>assistant<|end_header_id|>\n\n";
    EXPECT_EQ(out, expected);
}

// =============================================================================
// format_message — ChatML format
// =============================================================================

class ChatTemplateChatmlTest : public ::testing::Test {
protected:
    ChatTemplate tpl{ChatTemplateFormat::chatml};
};

TEST_F(ChatTemplateChatmlTest, SingleMessage) {
    std::string out;
    tpl.format_message(out, "system", "You are Qwen.", true);
    EXPECT_EQ(out, "<|im_start|>system\nYou are Qwen.<|im_end|>\n");
}

TEST_F(ChatTemplateChatmlTest, IsFirstHasNoEffect) {
    // ChatML has no BOS — is_first should not change output
    std::string first, subsequent;
    tpl.format_message(first, "user", "Hello", true);
    tpl.format_message(subsequent, "user", "Hello", false);
    EXPECT_EQ(first, subsequent);
}

TEST_F(ChatTemplateChatmlTest, FullConversation) {
    std::string out;
    tpl.format_message(out, "system", "You are Qwen.", true);
    tpl.format_message(out, "user", "Hello!", false);
    tpl.append_generation_prompt(out);

    std::string expected =
        "<|im_start|>system\nYou are Qwen.<|im_end|>\n"
        "<|im_start|>user\nHello!<|im_end|>\n"
        "<|im_start|>assistant\n";
    EXPECT_EQ(out, expected);
}

TEST_F(ChatTemplateChatmlTest, MultiTurnConversation) {
    std::string out;
    tpl.format_message(out, "system", "Be helpful.", true);
    tpl.format_message(out, "user", "Hi", false);
    tpl.format_message(out, "assistant", "Hello!", false);
    tpl.format_message(out, "user", "Bye", false);
    tpl.append_generation_prompt(out);

    std::string expected =
        "<|im_start|>system\nBe helpful.<|im_end|>\n"
        "<|im_start|>user\nHi<|im_end|>\n"
        "<|im_start|>assistant\nHello!<|im_end|>\n"
        "<|im_start|>user\nBye<|im_end|>\n"
        "<|im_start|>assistant\n";
    EXPECT_EQ(out, expected);
}

// =============================================================================
// format_message — Mistral format
// =============================================================================

class ChatTemplateMistralTest : public ::testing::Test {
protected:
    ChatTemplate tpl{ChatTemplateFormat::mistral};
};

TEST_F(ChatTemplateMistralTest, SystemThenUser) {
    // System message opens [INST] block; user message closes it
    std::string out;
    tpl.format_message(out, "system", "You are helpful.", true);
    tpl.format_message(out, "user", "Hello", false);

    EXPECT_EQ(out, "<s>[INST] You are helpful.\n\nHello [/INST]");
}

TEST_F(ChatTemplateMistralTest, UserOnlyFirst) {
    // No system message — user is first
    std::string out;
    tpl.format_message(out, "user", "Hello", true);

    EXPECT_EQ(out, "<s>[INST] Hello [/INST]");
}

TEST_F(ChatTemplateMistralTest, AssistantMessage) {
    std::string out;
    tpl.format_message(out, "assistant", "Hi there!", false);
    EXPECT_EQ(out, "Hi there!</s>");
}

TEST_F(ChatTemplateMistralTest, FullConversationWithSystem) {
    std::string out;
    tpl.format_message(out, "system", "Be nice.", true);
    tpl.format_message(out, "user", "Hello", false);
    tpl.format_message(out, "assistant", "Hi!", false);
    tpl.format_message(out, "user", "Bye", false);

    std::string expected =
        "<s>[INST] Be nice.\n\nHello [/INST]"
        "Hi!</s>"
        "[INST] Bye [/INST]";
    EXPECT_EQ(out, expected);
}

TEST_F(ChatTemplateMistralTest, FullConversationWithoutSystem) {
    std::string out;
    tpl.format_message(out, "user", "Hello", true);
    tpl.format_message(out, "assistant", "Hi!", false);
    tpl.format_message(out, "user", "Bye", false);

    std::string expected =
        "<s>[INST] Hello [/INST]"
        "Hi!</s>"
        "[INST] Bye [/INST]";
    EXPECT_EQ(out, expected);
}

TEST_F(ChatTemplateMistralTest, SystemNotFirst) {
    // Edge case: system message not marked as first (no <s> prefix)
    std::string out;
    tpl.format_message(out, "system", "Late system", false);
    EXPECT_EQ(out, "[INST] Late system\n\n");
}

// =============================================================================
// append_generation_prompt Tests
// =============================================================================

class AppendGenerationPromptTest : public ::testing::Test {};

TEST_F(AppendGenerationPromptTest, NoneIsNoop) {
    ChatTemplate tpl(ChatTemplateFormat::none);
    std::string out = "prefix";
    tpl.append_generation_prompt(out);
    EXPECT_EQ(out, "prefix");
}

TEST_F(AppendGenerationPromptTest, Llama3) {
    ChatTemplate tpl(ChatTemplateFormat::llama3);
    std::string out;
    tpl.append_generation_prompt(out);
    EXPECT_EQ(out, "<|start_header_id|>assistant<|end_header_id|>\n\n");
}

TEST_F(AppendGenerationPromptTest, Chatml) {
    ChatTemplate tpl(ChatTemplateFormat::chatml);
    std::string out;
    tpl.append_generation_prompt(out);
    EXPECT_EQ(out, "<|im_start|>assistant\n");
}

TEST_F(AppendGenerationPromptTest, MistralIsNoop) {
    ChatTemplate tpl(ChatTemplateFormat::mistral);
    std::string out = "prefix";
    tpl.append_generation_prompt(out);
    EXPECT_EQ(out, "prefix");
}

// =============================================================================
// format_single_message Tests
// =============================================================================

class FormatSingleMessageTest : public ::testing::Test {};

TEST_F(FormatSingleMessageTest, NoneFormat) {
    ChatTemplate tpl(ChatTemplateFormat::none);
    EXPECT_EQ(tpl.format_single_message("user", "Hello"),
              "<|user|>\nHello");
    EXPECT_EQ(tpl.format_single_message("system", "Be nice"),
              "<|system|>\nBe nice");
}

TEST_F(FormatSingleMessageTest, Llama3Format) {
    ChatTemplate tpl(ChatTemplateFormat::llama3);
    EXPECT_EQ(tpl.format_single_message("user", "Hello"),
              "<|start_header_id|>user<|end_header_id|>\n\nHello<|eot_id|>");
    EXPECT_EQ(tpl.format_single_message("system", "Be nice"),
              "<|start_header_id|>system<|end_header_id|>\n\nBe nice<|eot_id|>");
}

TEST_F(FormatSingleMessageTest, ChatmlFormat) {
    ChatTemplate tpl(ChatTemplateFormat::chatml);
    EXPECT_EQ(tpl.format_single_message("user", "Hello"),
              "<|im_start|>user\nHello<|im_end|>\n");
    EXPECT_EQ(tpl.format_single_message("assistant", "Hi"),
              "<|im_start|>assistant\nHi<|im_end|>\n");
}

TEST_F(FormatSingleMessageTest, MistralUserAndSystem) {
    ChatTemplate tpl(ChatTemplateFormat::mistral);
    EXPECT_EQ(tpl.format_single_message("user", "Hello"),
              "[INST] Hello [/INST]");
    EXPECT_EQ(tpl.format_single_message("system", "Be nice"),
              "[INST] Be nice [/INST]");
}

TEST_F(FormatSingleMessageTest, MistralAssistant) {
    ChatTemplate tpl(ChatTemplateFormat::mistral);
    EXPECT_EQ(tpl.format_single_message("assistant", "Hi there"),
              "Hi there</s>");
}

// =============================================================================
// overhead_per_message Tests
// =============================================================================

class OverheadPerMessageTest : public ::testing::Test {};

TEST_F(OverheadPerMessageTest, AllFormatsReturnPositive) {
    EXPECT_GT(ChatTemplate(ChatTemplateFormat::none).overhead_per_message(), 0u);
    EXPECT_GT(ChatTemplate(ChatTemplateFormat::llama3).overhead_per_message(), 0u);
    EXPECT_GT(ChatTemplate(ChatTemplateFormat::chatml).overhead_per_message(), 0u);
    EXPECT_GT(ChatTemplate(ChatTemplateFormat::mistral).overhead_per_message(), 0u);
}

TEST_F(OverheadPerMessageTest, Llama3HasMostOverhead) {
    // Llama 3 has the most verbose wrapping tokens
    auto llama3 = ChatTemplate(ChatTemplateFormat::llama3).overhead_per_message();
    auto chatml = ChatTemplate(ChatTemplateFormat::chatml).overhead_per_message();
    auto mistral = ChatTemplate(ChatTemplateFormat::mistral).overhead_per_message();
    EXPECT_GT(llama3, chatml);
    EXPECT_GT(llama3, mistral);
}

// =============================================================================
// Roundtrip: parse → name → parse
// =============================================================================

class RoundtripTest : public ::testing::Test {};

TEST_F(RoundtripTest, AllCanonicalNamesRoundtrip) {
    for (auto fmt : {ChatTemplateFormat::none,
                     ChatTemplateFormat::llama3,
                     ChatTemplateFormat::chatml,
                     ChatTemplateFormat::mistral}) {
        auto name = chat_template_format_name(fmt);
        EXPECT_EQ(parse_chat_template_format(name), fmt)
            << "Roundtrip failed for: " << name;
    }
}
