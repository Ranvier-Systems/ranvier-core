// Ranvier Core - Pre-compiled Chat Templates
//
// Formats chat messages according to model-specific chat templates so that
// Ranvier's tokenization matches what vLLM produces via apply_chat_template().
//
// Without this, Ranvier joins raw message content with "\n" separators.
// That produces different token sequences than vLLM's Jinja-rendered chat
// templates, which wrap messages with special tokens like <|start_header_id|>,
// <|im_start|>, etc.  The mismatch means:
//   1. Token counts disagree → prefix boundary detection is inaccurate
//   2. Forwarded prompt_token_ids don't match chat-template-formatted input
//   3. vLLM's Automatic Prefix Caching (APC) can't reuse KV blocks across
//      the two tokenization formats
//
// Pre-compiled templates avoid embedding a Jinja2 engine in C++ while
// covering the 3 formats that represent ~95% of production models:
//   - llama3:  Llama 3 / 3.1 / 3.2 / 4 family
//   - chatml:  ChatML (Qwen 2/2.5, Yi, DeepSeek, etc.)
//   - mistral: Mistral Instruct v1/v2/v3
//
// IMPORTANT: The tokenizer JSON (RANVIER_TOKENIZER_PATH) must be from the
// same model family.  These templates emit special token strings (e.g.
// <|start_header_id|>) that the tokenizer's added_tokens vocabulary maps
// to the correct token IDs.  Using a mismatched tokenizer (e.g. GPT-2
// tokenizer with llama3 template) will produce garbage token IDs.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ranvier {

// Supported pre-compiled chat template formats.
enum class ChatTemplateFormat {
    // No template — raw content joined with "\n" (legacy behavior).
    // Safe for routing-only use cases where exact vLLM alignment is not needed.
    none,

    // Llama 3 / 3.1 / 3.2 / 4 format.
    // Each message: <|start_header_id|>{role}<|end_header_id|>\n\n{content}<|eot_id|>
    // First message is preceded by <|begin_of_text|>.
    // Generation prompt: <|start_header_id|>assistant<|end_header_id|>\n\n
    llama3,

    // ChatML format (Qwen 2/2.5, Yi, DeepSeek, etc.).
    // Each message: <|im_start|>{role}\n{content}<|im_end|>\n
    // Generation prompt: <|im_start|>assistant\n
    chatml,

    // Mistral Instruct format (v1/v2/v3 basic — no tool call support).
    // Wraps user/assistant turns in [INST] / [/INST] markers.
    // System message is prepended inside the first [INST] block.
    mistral,
};

// Parse a chat template format name from string.
// Returns ChatTemplateFormat::none for unrecognized names.
inline ChatTemplateFormat parse_chat_template_format(std::string_view name) {
    if (name == "llama3" || name == "llama-3" || name == "llama") {
        return ChatTemplateFormat::llama3;
    }
    if (name == "chatml" || name == "qwen" || name == "deepseek") {
        return ChatTemplateFormat::chatml;
    }
    if (name == "mistral") {
        return ChatTemplateFormat::mistral;
    }
    // "none" or anything unrecognized → legacy behavior
    return ChatTemplateFormat::none;
}

inline std::string_view chat_template_format_name(ChatTemplateFormat fmt) {
    switch (fmt) {
        case ChatTemplateFormat::llama3:  return "llama3";
        case ChatTemplateFormat::chatml:  return "chatml";
        case ChatTemplateFormat::mistral: return "mistral";
        case ChatTemplateFormat::none:    return "none";
    }
    return "none";
}

// ---------------------------------------------------------------------------
// ChatTemplate — stateless formatter for a single pre-compiled template.
//
// Usage:
//   auto tpl = ChatTemplate(ChatTemplateFormat::llama3);
//   tpl.format_message(out, "system", "You are helpful.", /*is_first=*/true);
//   tpl.format_message(out, "user",   "Hello!",           /*is_first=*/false);
//   tpl.append_generation_prompt(out);
//   // → out contains the full chat-template-formatted string
// ---------------------------------------------------------------------------
class ChatTemplate {
public:
    ChatTemplate(ChatTemplateFormat fmt = ChatTemplateFormat::none)
        : _format(fmt) {}

    ChatTemplateFormat format() const { return _format; }
    bool is_none() const { return _format == ChatTemplateFormat::none; }

    // Append a single formatted message to `out`.
    // `is_first` controls whether a BOS / leading marker is prepended.
    void format_message(std::string& out,
                        std::string_view role,
                        std::string_view content,
                        bool is_first) const {
        switch (_format) {
            case ChatTemplateFormat::none:
                format_none(out, content, is_first);
                break;
            case ChatTemplateFormat::llama3:
                format_llama3(out, role, content, is_first);
                break;
            case ChatTemplateFormat::chatml:
                format_chatml(out, role, content, is_first);
                break;
            case ChatTemplateFormat::mistral:
                format_mistral(out, role, content, is_first);
                break;
        }
    }

    // Format a single message as a standalone string (for multi-depth boundary
    // tokenization where each message is tokenized individually).
    // Does NOT include BOS or generation prompt.
    std::string format_single_message(std::string_view role,
                                      std::string_view content) const {
        std::string out;
        out.reserve(role.size() + content.size() + 32);
        switch (_format) {
            case ChatTemplateFormat::none:
                // Legacy: "<|role|>\ncontent"
                out.append("<|");
                out.append(role);
                out.append("|>\n");
                out.append(content);
                break;
            case ChatTemplateFormat::llama3:
                // No BOS for individual message tokenization
                out.append("<|start_header_id|>");
                out.append(role);
                out.append("<|end_header_id|>\n\n");
                out.append(content);
                out.append("<|eot_id|>");
                break;
            case ChatTemplateFormat::chatml:
                out.append("<|im_start|>");
                out.append(role);
                out.push_back('\n');
                out.append(content);
                out.append("<|im_end|>\n");
                break;
            case ChatTemplateFormat::mistral:
                // For individual message tokenization, use a simplified format.
                // Mistral's [INST]/[/INST] wrapping spans multiple messages,
                // so per-message tokenization is inherently approximate.
                if (role == "user" || role == "system") {
                    out.append("[INST] ");
                    out.append(content);
                    out.append(" [/INST]");
                } else {
                    // assistant
                    out.append(content);
                    out.append("</s>");
                }
                break;
        }
        return out;
    }

    // Append the generation prompt (the trailing tokens that prompt the model
    // to start generating an assistant response).
    void append_generation_prompt(std::string& out) const {
        switch (_format) {
            case ChatTemplateFormat::none:
                // No generation prompt in legacy mode
                break;
            case ChatTemplateFormat::llama3:
                out.append("<|start_header_id|>assistant<|end_header_id|>\n\n");
                break;
            case ChatTemplateFormat::chatml:
                out.append("<|im_start|>assistant\n");
                break;
            case ChatTemplateFormat::mistral:
                // Mistral's generation prompt is implicit (after [/INST])
                break;
        }
    }

    // Get the special token that marks the start of each message.
    // Used by fast boundary detection: instead of re-tokenizing each message
    // individually (~5ms), scan the full token sequence for this marker (~1μs).
    // Returns empty string_view for templates without single-token markers.
    std::string_view message_start_marker() const {
        switch (_format) {
            case ChatTemplateFormat::llama3: return "<|start_header_id|>";
            case ChatTemplateFormat::chatml: return "<|im_start|>";
            default: return {};
        }
    }

    // Estimate the extra characters the template adds per message (for reserve).
    size_t overhead_per_message() const {
        switch (_format) {
            case ChatTemplateFormat::none:    return 5;   // "<|" + "|>\n"
            case ChatTemplateFormat::llama3:  return 60;  // header + eot tokens
            case ChatTemplateFormat::chatml:  return 30;  // im_start/end tokens
            case ChatTemplateFormat::mistral: return 20;  // [INST] markers
        }
        return 5;
    }

private:
    ChatTemplateFormat _format;

    // --- Legacy format: raw content with "\n" separator ---
    static void format_none(std::string& out, std::string_view content, bool is_first) {
        if (!is_first && !out.empty()) {
            out.push_back('\n');
        }
        out.append(content);
    }

    // --- Llama 3 format ---
    // Template (from Meta's tokenizer_config.json):
    //   {% for message in messages %}
    //     {% set content = '<|start_header_id|>' + message['role']
    //                    + '<|end_header_id|>\n\n' + message['content']|trim
    //                    + '<|eot_id|>' %}
    //     {% if loop.first %}{{ bos_token + content }}{% else %}{{ content }}{% endif %}
    //   {% endfor %}
    //   {% if add_generation_prompt %}
    //     {{ '<|start_header_id|>assistant<|end_header_id|>\n\n' }}
    //   {% endif %}
    static void format_llama3(std::string& out,
                              std::string_view role,
                              std::string_view content,
                              bool is_first) {
        if (is_first) {
            out.append("<|begin_of_text|>");
        }
        out.append("<|start_header_id|>");
        out.append(role);
        out.append("<|end_header_id|>\n\n");
        out.append(content);
        out.append("<|eot_id|>");
    }

    // --- ChatML format ---
    // Template (from Qwen's tokenizer_config.json):
    //   {% for message in messages %}
    //     {{ '<|im_start|>' + message['role'] + '\n'
    //        + message['content'] + '<|im_end|>' + '\n' }}
    //   {% endfor %}
    //   {% if add_generation_prompt %}{{ '<|im_start|>assistant\n' }}{% endif %}
    static void format_chatml(std::string& out,
                              std::string_view role,
                              std::string_view content,
                              bool is_first) {
        (void)is_first;  // ChatML has no BOS in the template
        out.append("<|im_start|>");
        out.append(role);
        out.push_back('\n');
        out.append(content);
        out.append("<|im_end|>\n");
    }

    // --- Mistral Instruct format ---
    // Template (simplified, no tool calls):
    //   <s>[INST] {system}\n\n{user} [/INST]{assistant}</s>[INST] {user2} [/INST]
    //
    // System message is prepended inside the first [INST] block, followed by
    // the first user message.  Subsequent turns alternate [INST]/[/INST] and
    // bare assistant text terminated by </s>.
    //
    // Since format_message() is called per-message in order, we detect whether
    // we're inside an open [INST] block by checking if `out` ends with "\n\n"
    // (left by a preceding system message).
    //
    // Note: Mistral's template is the hardest to decompose per-message because
    // system content is merged into the first [INST] block.  For per-message
    // boundary tokenization, use format_single_message() which is approximate.
    static void format_mistral(std::string& out,
                               std::string_view role,
                               std::string_view content,
                               bool is_first) {
        if (role == "system") {
            // System message: open [INST] block with <s> prefix.
            // A trailing "\n\n" signals to the next user message that the
            // [INST] block is already open.
            if (is_first) {
                out.append("<s>");
            }
            out.append("[INST] ");
            out.append(content);
            out.append("\n\n");
        } else if (role == "user") {
            // Check if we're continuing after a system message (open [INST] block).
            bool inside_inst = (out.size() >= 2 &&
                                out[out.size() - 1] == '\n' &&
                                out[out.size() - 2] == '\n');
            if (inside_inst) {
                // System message already opened [INST] — append content + close
                out.append(content);
                out.append(" [/INST]");
            } else if (is_first) {
                // First message is user (no system) — open fresh [INST] block
                out.append("<s>[INST] ");
                out.append(content);
                out.append(" [/INST]");
            } else {
                // Subsequent user turn (after </s> from assistant)
                out.append("[INST] ");
                out.append(content);
                out.append(" [/INST]");
            }
        } else {
            // assistant
            out.append(content);
            out.append("</s>");
        }
    }
};

}  // namespace ranvier
