// Ranvier Core - Boundary Detection for Prefix Routing
//
// Pure computational strategies for detecting message boundaries in a token
// sequence.  These enable prefix-aware routing: the system prompt tokens hash
// to one backend while user-specific tokens hash to another.
//
// Two strategies, tried in priority order:
//
// 1. Marker scan — For templates with single-token message start markers
//    (llama3's <|start_header_id|>, chatml's <|im_start|>).  Scans the full
//    token array for the marker ID and derives exact boundaries from positions.
//    O(n) where n = total tokens.
//
// 2. Proportional estimation — Maps per-message char offsets to token positions
//    using the token/char ratio from primary tokenization.  Works for any
//    template (primarily none/mistral where markers are absent or multi-token).
//    O(m) where m = number of messages.
//
// Both functions are pure (no I/O, no side effects) and return a result struct
// that the caller unpacks.  The caller (HttpController) handles metrics,
// logging, and falls back to per-message re-tokenization if both return
// detected=false.

#pragma once

#include "request_rewriter.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ranvier {

// Configuration for boundary detection strategies.
// Mirrors the relevant subset of HttpController config.
struct BoundaryDetectionConfig {
    bool enable_multi_depth = false;
    size_t min_prefix_boundary_tokens = 4;
};

// Result of a boundary detection attempt.
struct BoundaryDetectionResult {
    size_t prefix_boundary = 0;           // System prefix boundary (token index)
    std::vector<size_t> prefix_boundaries; // Per-message boundaries (multi-depth)
    bool detected = false;                 // True if any boundary was found
};

// ---------------------------------------------------------------------------
// Strategy 1: Marker Scan
// ---------------------------------------------------------------------------
// Scan the full token sequence for a known message-start marker token ID.
// For llama3 each message starts with <|start_header_id|> (single token);
// for chatml, <|im_start|>.  The generation prompt also starts with one,
// so the expected marker count = total_message_count + 1.
//
// Returns detected=false if:
//   - marker count doesn't match expected (template mismatch or injection)
//   - fewer than 2 markers found
//   - no valid boundaries after filtering
inline BoundaryDetectionResult detect_boundaries_by_marker_scan(
        const std::vector<int32_t>& tokens,
        int32_t marker_token_id,
        const RequestRewriter::TextWithBoundaryInfo& text_info,
        const BoundaryDetectionConfig& config) {

    BoundaryDetectionResult result;

    if (tokens.empty() || text_info.total_message_count == 0) {
        return result;
    }

    // Scan for all marker positions
    std::vector<size_t> marker_pos;
    marker_pos.reserve(text_info.total_message_count + 2);
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] == marker_token_id) {
            marker_pos.push_back(i);
        }
    }

    // Validate: expect (num_messages + 1) markers (messages + generation prompt).
    // Mismatch indicates template mismatch or special-token injection in content.
    size_t expected = text_info.total_message_count + 1;
    if (marker_pos.size() != expected || marker_pos.size() < 2) {
        return result;
    }

    size_t bos_offset = marker_pos[0];  // 0 for chatml, 1 for llama3 (BOS)
    size_t sys_count = text_info.system_message_count;

    if (config.enable_multi_depth) {
        // Multi-depth: boundary after each message
        for (size_t i = 1; i < marker_pos.size(); ++i) {
            size_t boundary = marker_pos[i] - bos_offset;
            if (boundary > 0 && boundary < tokens.size()) {
                result.prefix_boundaries.push_back(boundary);
            }
        }

        if (!result.prefix_boundaries.empty()) {
            // System boundary: cumulative tokens before first non-system message
            size_t system_boundary = 0;
            if (sys_count > 0 && sys_count < marker_pos.size()) {
                system_boundary = marker_pos[sys_count] - bos_offset;
            }
            result.prefix_boundary = system_boundary;
            if (result.prefix_boundary == 0 && !result.prefix_boundaries.empty()) {
                result.prefix_boundary = result.prefix_boundaries.front();
            }
            result.detected = true;
        }
    } else {
        // Non-multi-depth: system boundary only
        if (text_info.has_system_messages && sys_count > 0 &&
            sys_count < marker_pos.size()) {
            size_t boundary = marker_pos[sys_count] - bos_offset;
            if (boundary >= config.min_prefix_boundary_tokens &&
                boundary < tokens.size()) {
                result.prefix_boundary = boundary;
                result.detected = true;
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Strategy 2: Proportional Estimation
// ---------------------------------------------------------------------------
// Estimate token boundaries from character offsets using the token/char ratio.
// Requires message_char_ends to be populated during text extraction.
//
// Accuracy: ±2-5 tokens.  For the "none" template this is comparable to the
// per-message tokenization fallback (which uses a different format than the
// combined text and is therefore also approximate).
//
// Returns detected=false if:
//   - message_char_ends is empty or text is empty
//   - no valid boundaries after filtering
inline BoundaryDetectionResult detect_boundaries_by_char_ratio(
        size_t total_token_count,
        const RequestRewriter::TextWithBoundaryInfo& text_info,
        const BoundaryDetectionConfig& config) {

    BoundaryDetectionResult result;

    if (text_info.message_char_ends.empty() || text_info.text.empty() ||
        total_token_count == 0) {
        return result;
    }

    double text_len = static_cast<double>(text_info.text.size());
    double token_count = static_cast<double>(total_token_count);
    size_t sys_count = text_info.system_message_count;

    if (config.enable_multi_depth) {
        // Multi-depth: estimate boundary after each message
        for (size_t i = 0; i < text_info.message_char_ends.size(); ++i) {
            size_t boundary = static_cast<size_t>(
                text_info.message_char_ends[i] * token_count / text_len);
            if (boundary > 0 && boundary < total_token_count) {
                result.prefix_boundaries.push_back(boundary);
            }
        }

        if (!result.prefix_boundaries.empty()) {
            size_t system_boundary = 0;
            if (sys_count > 0 &&
                sys_count <= text_info.message_char_ends.size()) {
                system_boundary = static_cast<size_t>(
                    text_info.message_char_ends[sys_count - 1]
                    * token_count / text_len);
            }
            result.prefix_boundary = system_boundary;
            if (result.prefix_boundary == 0 && !result.prefix_boundaries.empty()) {
                result.prefix_boundary = result.prefix_boundaries.front();
            }
            result.detected = true;
        }
    } else {
        // Non-multi-depth: system boundary only
        if (text_info.has_system_messages && sys_count > 0 &&
            text_info.has_system_prefix) {
            size_t boundary = static_cast<size_t>(
                text_info.system_prefix_end * token_count / text_len);
            if (boundary >= config.min_prefix_boundary_tokens &&
                boundary < total_token_count) {
                result.prefix_boundary = boundary;
                result.detected = true;
            }
        }
    }

    return result;
}

} // namespace ranvier
