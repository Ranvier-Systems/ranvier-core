// Ranvier Core - Text Boundary Info
//
// Lightweight struct containing pre-computed boundary metadata from a single
// JSON parse of the request body.  Separated from request_rewriter.hpp to
// avoid pulling in rapidjson for consumers that only need the result type
// (boundary_detector.hpp, unit tests, etc.).

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ranvier {

// Result of combined text extraction with pre-computed boundary metadata.
// Produced by a single JSON parse, eliminating redundant re-parsing
// that would occur when calling extract_text(), extract_system_messages(),
// and extract_message_boundaries() separately.
struct TextWithBoundaryInfo {
    std::string text;                    // Full combined text for tokenization

    // True if text came from "prompt" field (no message structure)
    bool from_prompt = false;

    // System message prefix boundary.
    // When system messages are contiguous at the start of the messages array,
    // text.substr(0, system_prefix_end) gives the formatted system content.
    // This enables tokenizing the prefix substring directly without
    // re-extracting and re-tokenizing system messages.
    bool has_system_prefix = false;      // True if contiguous system msgs at start
    size_t system_prefix_end = 0;        // Char offset in text after last system message

    // System message text for non-contiguous fallback.
    // Pre-extracted during JSON parsing to avoid re-parsing later.
    // Matches extract_system_messages() output format.
    std::string system_text;             // System content joined by "\n" (empty if none)
    bool has_system_messages = false;

    // Message counts for fast boundary detection (always populated).
    // system_message_count: leading contiguous system messages only.
    // total_message_count: all messages with valid role+content.
    size_t system_message_count = 0;
    size_t total_message_count = 0;

    // Cumulative char offsets in `text` after each message (always populated).
    // message_char_ends[i] = text.size() after formatting message i.
    // Used for proportional boundary estimation: avoids re-tokenizing each
    // message by mapping char offsets to token positions via the token/char
    // ratio from primary tokenization.
    std::vector<size_t> message_char_ends;

    // Per-message formatted strings for multi-depth boundary computation.
    // Pre-computed during JSON parsing to avoid re-parsing.
    // Format depends on chat_template:
    //   none:   "<|role|>\ncontent" (legacy)
    //   llama3: "<|start_header_id|>role<|end_header_id|>\n\ncontent<|eot_id|>"
    //   chatml: "<|im_start|>role\ncontent<|im_end|>\n"
    struct FormattedMessage {
        std::string text;
        bool is_system;
    };
    std::vector<FormattedMessage> formatted_messages;
};

} // namespace ranvier
