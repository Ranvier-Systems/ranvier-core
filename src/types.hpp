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
//
// WIRE CONTRACT: this enum is also a telemetry window-report bucket-key
// dimension (engine class — see telemetry_schema.hpp). Its ordinals are
// therefore part of that wire format: pinned explicitly, append-only, never
// renumbered, with the underlying type fixed to uint8_t to lock the wire
// width. The persisted/admin form is the STRING (backend_type_to_string);
// the integer was never persisted, which is why pinning the current implicit
// values now is free. There is deliberately NO UNSPECIFIED sentinel — every
// backend that emits telemetry has a real engine class, so an "unset" state
// would be a dead value.
enum class BackendType : uint8_t {
    VLLM              = 0,
    SGLANG            = 1,
    TRT_LLM           = 2,
    OLLAMA            = 3,
    LM_STUDIO         = 4,
    CEREBRAS          = 5,
    OPENAI_COMPATIBLE = 6,
    // Append only — add a new engine class at the next integer.
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

// Per-backend hardware label. An operator-applied tag resolved at backend
// registration — NOT inferred by the data plane (nothing in routing derives a
// hardware regime). Defaults to UNSPECIFIED so the dimension degrades
// gracefully when the operator hasn't labelled their fleet. Used by the
// telemetry sink to bucket aggregate routing/cache outcomes by a physical
// axis (HBM capacity / compute class) — chosen over market tiers like
// "flagship/mainstream" because a card's market tier drifts year-over-year
// while its physical regime does not, which keeps cross-deployment and
// cross-time comparability honest.
//
// (Renamed from HardwareTier: "tier" implied a routing-derived value, but the
//  code only ever stores an operator-supplied per-backend label.)
//
// WIRE CONTRACT (read carefully before editing):
//
//   - These ordinals are part of the telemetry window-report wire format and
//     MUST be stable across releases. Never renumber an existing label. Add
//     new labels only at the end (append-only). Renaming the C++ symbol is
//     fine (consumers key on the integer); redefining what an existing label
//     MEANS (e.g. moving the small/large boundary) is NOT — that silently
//     breaks comparability of everything already aggregated under the old
//     meaning, and the catalog can't tell the two cohorts apart.
//
//   - UNSPECIFIED = 0 is the default-when-unset sentinel. It is permanent.
//
// Same forward-compat discipline as CacheStatePacket — see gossip_protocol.hpp.
enum class HardwareLabel : uint8_t {
    UNSPECIFIED = 0,
    GPU_SMALL   = 1,
    GPU_LARGE   = 2,
    // Append only. Add GPU_MEDIUM=3 (etc.) when fleet data warrants a finer
    // split. CPU=N is the obvious next addition once Ranvier formally
    // supports local CPU-LLM backends as a first-class bucket.
};

// Stable string label for HardwareLabel. Used as a Prometheus-safe label value
// and as the operator-facing identifier in YAML / admin APIs. Lower-case,
// snake-case; matches parse_hardware_label() round-trip.
inline std::string_view hardware_label_to_string(HardwareLabel t) {
    switch (t) {
        case HardwareLabel::UNSPECIFIED: return "unspecified";
        case HardwareLabel::GPU_SMALL:   return "gpu_small";
        case HardwareLabel::GPU_LARGE:   return "gpu_large";
    }
    return "unspecified";
}

// Inverse of hardware_label_to_string(). Returns std::nullopt for unknown
// strings; service layer logs and defaults to UNSPECIFIED (Rule #7).
inline std::optional<HardwareLabel> parse_hardware_label(std::string_view s) {
    if (s == "unspecified") return HardwareLabel::UNSPECIFIED;
    if (s == "gpu_small")   return HardwareLabel::GPU_SMALL;
    if (s == "gpu_large")   return HardwareLabel::GPU_LARGE;
    return std::nullopt;
}

}  // namespace ranvier
