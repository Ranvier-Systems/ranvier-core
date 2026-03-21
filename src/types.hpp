#pragma once

#include <cstddef>
#include <cstdint>

namespace ranvier {

// Token IDs are 32-bit integers from the Tokenizer
using TokenId = int32_t;

// Backend ID is the GPU Pool ID (0, 1, 2...)
using BackendId = int32_t;

// Rule #17: Number of loop iterations between co_await maybe_yield() calls.
// Balances reactor responsiveness against yield overhead (~20ns each).
// 128 iterations ≈ 10-50μs of CPU work, well within Seastar's 500μs task quota.
inline constexpr size_t kYieldInterval = 128;

}  // namespace ranvier
