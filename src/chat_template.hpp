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

    // Kimi K2 / K3 (Moonshot) format.
    // Each message: {role-token}{role}<|im_middle|>{content}<|im_end|>
    //   role-token ∈ { <|im_user|>, <|im_assistant|>, <|im_system|> }
    //   — system and tool turns both open with <|im_system|>.
    // Generation prompt: <|im_assistant|>assistant<|im_middle|>
    // Unlike ChatML, role tokens are per-role rather than a shared <|im_start|>,
    // so there is no single message-start marker to scan (see
    // message_start_marker) — boundary detection falls back to estimation.
    // Confirmed against Moonshot's chat_template.jinja: no BOS is rendered and
    // turns concatenate with no separator whitespace. Default-system injection
    // for system-less conversations is applied by the caller (see
    // default_system_prompt); tool-result framing is not reproduced (see
    // format_kimi).
    kimi,
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
    if (name == "kimi" || name == "kimi-k2" || name == "kimi-k3" ||
        name == "moonshot") {
        return ChatTemplateFormat::kimi;
    }
    // "none" or anything unrecognized → legacy behavior
    return ChatTemplateFormat::none;
}

inline std::string_view chat_template_format_name(ChatTemplateFormat fmt) {
    switch (fmt) {
        case ChatTemplateFormat::llama3:  return "llama3";
        case ChatTemplateFormat::chatml:  return "chatml";
        case ChatTemplateFormat::mistral: return "mistral";
        case ChatTemplateFormat::kimi:    return "kimi";
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
            case ChatTemplateFormat::kimi:
                format_kimi(out, role, content, is_first);
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
            case ChatTemplateFormat::kimi:
                // No BOS for individual message tokenization (mirrors llama3).
                out.append(kimi_role_token(role));
                out.append(role);
                out.append("<|im_middle|>");
                out.append(content);
                out.append("<|im_end|>");
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
            case ChatTemplateFormat::kimi:
                out.append("<|im_assistant|>assistant<|im_middle|>");
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
            // Kimi opens each turn with a role-specific token, so a single-marker
            // scan can't count messages; callers fall back to estimation.
            case ChatTemplateFormat::kimi:   return {};
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
            case ChatTemplateFormat::kimi:    return 45;  // role + im_middle + im_end
        }
        return 5;
    }

    // System content the backend's apply_chat_template injects before the first
    // turn when a conversation has no leading system message. Only Kimi's
    // reference template defines one; every other format returns empty (they
    // inject nothing). The caller fires this only when messages[0] is not itself
    // a system turn — a request that supplies its own system message suppresses
    // it — matching the reference's `loop.first and messages[0].role != 'system'`.
    std::string_view default_system_prompt() const {
        switch (_format) {
            case ChatTemplateFormat::kimi: return kKimiDefaultSystemPrompt;
            default: return {};
        }
    }

private:
    ChatTemplateFormat _format;

    // Verbatim from Moonshot's chat_template.jinja (K2). Part of the template
    // definition, like the token strings above; update if a future Kimi revision
    // changes it. A client that sends its own system message overrides it.
    static constexpr std::string_view kKimiDefaultSystemPrompt =
        "You are Kimi, an AI assistant created by Moonshot AI.";

    // Empty: Kimi's chat_template.jinja renders no BOS (its "[BOS]" token is
    // never emitted, and tokenizer_config carries no add_bos_token). Kept as a
    // named seam should a future variant reintroduce one — setting it must also
    // update ChatTemplateKimiTest.
    static constexpr std::string_view kKimiBosToken{};

    // Kimi opens each turn with a role-specific token. system and tool turns
    // both open with <|im_system|> (a tool turn renders as
    // <|im_system|>tool<|im_middle|>...); user/assistant get their own tokens.
    static std::string_view kimi_role_token(std::string_view role) {
        if (role == "user")      return "<|im_user|>";
        if (role == "assistant") return "<|im_assistant|>";
        return "<|im_system|>";
    }

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

    // --- Kimi K2 / K3 (Moonshot) format ---
    // Per-turn: {role-token}{role}<|im_middle|>{content}<|im_end|>
    // Segments are delimited by their own special tokens; no separator newline
    // is emitted between turns.
    //
    // format_message stays verbatim to the message it is given (as the other
    // formats do). The reference chat_template.jinja's default-system injection
    // for system-less conversations is handled one level up, by the caller that
    // owns the message list (see default_system_prompt) — this per-message
    // function can't see whether the whole conversation lacks a system turn.
    // Tool-result turns' "## Return of {id}" wrapper and the tool_call/media
    // framing are not reproduced; routing keys on the system/user prefix.
    static void format_kimi(std::string& out,
                            std::string_view role,
                            std::string_view content,
                            bool is_first) {
        if (is_first && !kKimiBosToken.empty()) {
            out.append(kKimiBosToken);
        }
        out.append(kimi_role_token(role));
        out.append(role);
        out.append("<|im_middle|>");
        out.append(content);
        out.append("<|im_end|>");
    }
};

}  // namespace ranvier
