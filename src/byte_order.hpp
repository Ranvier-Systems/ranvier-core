// Ranvier Core - Big-endian byte order helpers
//
// Header-only helpers for reading/writing big-endian integers to byte buffers.
// Used by:
//   - Gossip packet serialization (std::vector<uint8_t> buffers)
//   - Radix tree TokenId <-> byte encoding (hot path, uses
//     absl::InlinedVector<uint8_t, N> to stay allocation-free)
//
// The write helpers are templated on the output container type so callers
// can pass any buffer that supports `push_back(uint8_t)`.  Existing callers
// using std::vector<uint8_t> are unaffected — the template is instantiated
// implicitly from the call site.

#pragma once

#include <cstdint>
#include <vector>

namespace ranvier {

template <typename Out>
inline void be_write_u16(Out& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

template <typename Out>
inline void be_write_u32(Out& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

template <typename Out>
inline void be_write_u64(Out& buf, uint64_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 56) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 48) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 40) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 32) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

inline uint16_t be_read_u16(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) |
           static_cast<uint16_t>(p[1]);
}

inline uint32_t be_read_u32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline uint64_t be_read_u64(const uint8_t* p) {
    return (static_cast<uint64_t>(p[0]) << 56) |
           (static_cast<uint64_t>(p[1]) << 48) |
           (static_cast<uint64_t>(p[2]) << 40) |
           (static_cast<uint64_t>(p[3]) << 32) |
           (static_cast<uint64_t>(p[4]) << 24) |
           (static_cast<uint64_t>(p[5]) << 16) |
           (static_cast<uint64_t>(p[6]) << 8) |
           static_cast<uint64_t>(p[7]);
}

}  // namespace ranvier
