#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ranvier {

// Token IDs are 32-bit integers from the Tokenizer
using TokenId = int32_t;

// Backend ID is the GPU Pool ID (0, 1, 2...)
using BackendId = int32_t;

// Rule #17: Number of loop iterations between co_await maybe_yield() calls.
// Balances reactor responsiveness against yield overhead (~20ns each).
// 128 iterations ≈ 10-50μs of CPU work, well within Seastar's 500μs task quota.
inline constexpr size_t kYieldInterval = 128;

// Per-backend type tag. Gates type-specific behaviour (route learning,
// metrics scraping, request-body rewriting). See BACKLOG §19 for context.
// GPU-class backends benefit from prefix-affinity routing; non-GPU classes
// (CEREBRAS, OPENAI_COMPATIBLE) still use Ranvier's L7 plumbing but skip
// prefix learning and the vLLM-shaped metrics scrape.
enum class BackendType {
    VLLM,
    SGLANG,
    TRT_LLM,
    OLLAMA,
    LM_STUDIO,
    CEREBRAS,
    OPENAI_COMPATIBLE,
};

// Persisted/wire string for BackendType. Stable across releases; used by
// the SQLite schema and the admin API. Keep in sync with parse_backend_type().
inline std::string_view backend_type_to_string(BackendType t) {
    switch (t) {
        case BackendType::VLLM:              return "vllm";
        case BackendType::SGLANG:            return "sglang";
        case BackendType::TRT_LLM:           return "trt_llm";
        case BackendType::OLLAMA:            return "ollama";
        case BackendType::LM_STUDIO:         return "lm_studio";
        case BackendType::CEREBRAS:          return "cerebras";
        case BackendType::OPENAI_COMPATIBLE: return "openai_compatible";
    }
    return "vllm";
}

// Inverse of backend_type_to_string(). Returns std::nullopt for unknown
// strings; the service layer logs and defaults to VLLM (Rule #7: parsing
// happens at the service boundary, persistence stores the raw string).
inline std::optional<BackendType> parse_backend_type(std::string_view s) {
    if (s == "vllm")              return BackendType::VLLM;
    if (s == "sglang")            return BackendType::SGLANG;
    if (s == "trt_llm")           return BackendType::TRT_LLM;
    if (s == "ollama")            return BackendType::OLLAMA;
    if (s == "lm_studio")         return BackendType::LM_STUDIO;
    if (s == "cerebras")          return BackendType::CEREBRAS;
    if (s == "openai_compatible") return BackendType::OPENAI_COMPATIBLE;
    return std::nullopt;
}

}  // namespace ranvier
