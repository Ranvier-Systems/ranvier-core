#pragma once

#include <cstdint>

namespace ranvier {

// Token IDs are 32-bit integers from the Tokenizer
using TokenId = int32_t;

// Backend ID is the GPU Pool ID (0, 1, 2...)
using BackendId = int32_t;

}  // namespace ranvier
