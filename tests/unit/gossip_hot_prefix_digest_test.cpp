// Ranvier Core - Gossip Hot-Prefix-Digest Packet Unit Tests
//
// Wire-format tests for HotPrefixDigestPacket (BACKLOG §21 P2). Reactor-free:
// the packet is replicated here (mirroring gossip_protocol.hpp) so the wire
// format is exercised without pulling in Seastar — exactly like
// gossip_cache_state_test.cpp / gossip_cache_eviction_test.cpp. The production
// serialize/deserialize MUST match this spec.
//
// Format: [type:1][version:1][backend_id:4][count:2][ prefix_hash:8 × count ]

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "types.hpp"

using namespace ranvier;

namespace {

enum class GossipPacketType : uint8_t {
    ROUTE_ANNOUNCEMENT = 0x01,
    HEARTBEAT = 0x02,
    ROUTE_ACK = 0x03,
    NODE_STATE = 0x04,
    CACHE_EVICTION = 0x05,
    CACHE_STATE = 0x06,
    HOT_PREFIX_DIGEST = 0x07,
};

struct HotPrefixDigestPacket {
    static constexpr uint8_t PROTOCOL_VERSION = 2;  // 5b: verified-resident tail
    static constexpr size_t HEADER_SIZE = 8;
    static constexpr size_t MAX_HASHES = 128;

    GossipPacketType type = GossipPacketType::HOT_PREFIX_DIGEST;
    uint8_t version = PROTOCOL_VERSION;
    BackendId backend_id = 0;
    std::vector<uint64_t> prefix_hashes;
    // 5b: subset of prefix_hashes verified-resident on the sender (membership order).
    std::vector<uint64_t> verified_hashes;

    std::vector<uint8_t> serialize() const {
        uint16_t count = static_cast<uint16_t>(
            std::min(prefix_hashes.size(), static_cast<size_t>(MAX_HASHES)));
        const size_t bitmap_bytes =
            verified_hashes.empty() ? 0 : (static_cast<size_t>(count) + 7) / 8;
        std::vector<uint8_t> b;
        b.push_back(static_cast<uint8_t>(type));
        b.push_back(version);
        uint32_t bid = static_cast<uint32_t>(backend_id);
        b.push_back((bid >> 24) & 0xFF);
        b.push_back((bid >> 16) & 0xFF);
        b.push_back((bid >> 8) & 0xFF);
        b.push_back(bid & 0xFF);
        b.push_back((count >> 8) & 0xFF);
        b.push_back(count & 0xFF);
        for (size_t i = 0; i < count; ++i) {
            uint64_t h = prefix_hashes[i];
            for (int s = 56; s >= 0; s -= 8) b.push_back((h >> s) & 0xFF);
        }
        if (bitmap_bytes > 0) {
            const size_t off = b.size();
            b.resize(off + bitmap_bytes, 0);
            for (size_t i = 0; i < count; ++i) {
                if (std::find(verified_hashes.begin(), verified_hashes.end(),
                              prefix_hashes[i]) != verified_hashes.end()) {
                    b[off + (i >> 3)] |= static_cast<uint8_t>(1u << (i & 7));
                }
            }
        }
        return b;
    }

    static std::optional<HotPrefixDigestPacket> deserialize(const uint8_t* d, size_t len) {
        if (len < HEADER_SIZE) return std::nullopt;
        HotPrefixDigestPacket p;
        p.type = static_cast<GossipPacketType>(d[0]);
        p.version = d[1];
        if (p.type != GossipPacketType::HOT_PREFIX_DIGEST) return std::nullopt;
        p.backend_id = static_cast<BackendId>(
            (uint32_t(d[2]) << 24) | (uint32_t(d[3]) << 16) |
            (uint32_t(d[4]) << 8) | uint32_t(d[5]));
        uint16_t count = static_cast<uint16_t>((uint16_t(d[6]) << 8) | d[7]);
        if (count > MAX_HASHES) return std::nullopt;          // Rule #4 bound
        size_t need = HEADER_SIZE + static_cast<size_t>(count) * 8;
        if (len < need) return std::nullopt;                  // truncated
        p.prefix_hashes.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* q = d + HEADER_SIZE + i * 8;
            uint64_t h = 0;
            for (int j = 0; j < 8; ++j) h = (h << 8) | q[j];
            p.prefix_hashes.push_back(h);
        }
        // 5b: optional verified-resident bitmap tail. Present only when non-empty;
        // a partial tail is ignored. Bytes past the bitmap are forward-compat.
        const size_t bitmap_bytes = (static_cast<size_t>(count) + 7) / 8;
        if (count > 0 && len >= need + bitmap_bytes) {
            const uint8_t* bm = d + need;
            for (size_t i = 0; i < count; ++i) {
                if (bm[i >> 3] & static_cast<uint8_t>(1u << (i & 7))) {
                    p.verified_hashes.push_back(p.prefix_hashes[i]);
                }
            }
        }
        return p;
    }
};

}  // namespace

TEST(HotPrefixDigestPacket, RoundTrip) {
    HotPrefixDigestPacket pkt;
    pkt.backend_id = 42;
    pkt.prefix_hashes = {0x0123456789abcdefULL, 0xfedcba9876543210ULL, 7};
    auto bytes = pkt.serialize();
    EXPECT_EQ(bytes.size(), HotPrefixDigestPacket::HEADER_SIZE + 3u * 8u);
    EXPECT_EQ(bytes[0], 0x07);  // wire type ordinal must stay 0x07

    auto got = HotPrefixDigestPacket::deserialize(bytes.data(), bytes.size());
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->backend_id, 42);
    EXPECT_EQ(got->prefix_hashes, pkt.prefix_hashes);
}

TEST(HotPrefixDigestPacket, EmptyDigest) {
    HotPrefixDigestPacket pkt;
    pkt.backend_id = 1;
    auto bytes = pkt.serialize();
    EXPECT_EQ(bytes.size(), HotPrefixDigestPacket::HEADER_SIZE);
    auto got = HotPrefixDigestPacket::deserialize(bytes.data(), bytes.size());
    ASSERT_TRUE(got.has_value());
    EXPECT_TRUE(got->prefix_hashes.empty());
}

TEST(HotPrefixDigestPacket, SerializeCapsAtMaxHashes) {
    HotPrefixDigestPacket pkt;
    pkt.backend_id = 9;
    pkt.prefix_hashes.assign(HotPrefixDigestPacket::MAX_HASHES + 50, 0xABCDull);
    auto bytes = pkt.serialize();
    EXPECT_EQ(bytes.size(),
              HotPrefixDigestPacket::HEADER_SIZE + HotPrefixDigestPacket::MAX_HASHES * 8);
    auto got = HotPrefixDigestPacket::deserialize(bytes.data(), bytes.size());
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->prefix_hashes.size(), HotPrefixDigestPacket::MAX_HASHES);
}

TEST(HotPrefixDigestPacket, DeserializeRejectsOverCapCount) {
    // Hand-crafted header claiming MAX_HASHES+1 entries (Rule #4 bound on the wire).
    std::vector<uint8_t> b(HotPrefixDigestPacket::HEADER_SIZE, 0);
    b[0] = 0x07;
    b[1] = 1;
    uint16_t bad = HotPrefixDigestPacket::MAX_HASHES + 1;
    b[6] = (bad >> 8) & 0xFF;
    b[7] = bad & 0xFF;
    EXPECT_FALSE(HotPrefixDigestPacket::deserialize(b.data(), b.size()).has_value());
}

TEST(HotPrefixDigestPacket, DeserializeRejectsTruncated) {
    HotPrefixDigestPacket pkt;
    pkt.prefix_hashes = {1, 2, 3};
    auto bytes = pkt.serialize();
    bytes.resize(bytes.size() - 1);  // chop a byte off the last hash
    EXPECT_FALSE(HotPrefixDigestPacket::deserialize(bytes.data(), bytes.size()).has_value());
}

TEST(HotPrefixDigestPacket, DeserializeIgnoresTrailingBytesPastBitmap) {
    // Forward-compat: a future version appends fields after the verified bitmap;
    // this parser reads membership + bitmap it knows and ignores the rest.
    HotPrefixDigestPacket pkt;
    pkt.backend_id = 5;
    pkt.prefix_hashes = {0xAAAA, 0xBBBB};
    pkt.verified_hashes = {0xBBBB};
    auto bytes = pkt.serialize();
    bytes.push_back(0xDE);  // bytes past the 1-byte bitmap are future fields
    bytes.push_back(0xAD);
    auto got = HotPrefixDigestPacket::deserialize(bytes.data(), bytes.size());
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->prefix_hashes.size(), 2u);
    EXPECT_EQ(got->backend_id, 5);
    EXPECT_EQ(got->verified_hashes, (std::vector<uint64_t>{0xBBBB}));
}

// ----- BACKLOG §21 Phase 5b: verified-resident bitmap tail -----

TEST(HotPrefixDigestPacket, VerifiedSubsetRoundTrip) {
    HotPrefixDigestPacket pkt;
    pkt.backend_id = 7;
    pkt.prefix_hashes = {10, 20, 30, 40, 50};
    pkt.verified_hashes = {20, 40};  // a strict, non-contiguous subset
    auto bytes = pkt.serialize();
    // 5 hashes => 1 bitmap byte appended.
    EXPECT_EQ(bytes.size(), HotPrefixDigestPacket::HEADER_SIZE + 5u * 8u + 1u);
    EXPECT_EQ(bytes[1], 2);  // PROTOCOL_VERSION bumped to 2

    auto got = HotPrefixDigestPacket::deserialize(bytes.data(), bytes.size());
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->prefix_hashes, pkt.prefix_hashes);
    EXPECT_EQ(got->verified_hashes, (std::vector<uint64_t>{20, 40}));  // membership order
}

TEST(HotPrefixDigestPacket, EmptyVerifiedSubsetEmitsNoTail) {
    // Membership-only window: byte-identical to a v1 (tail-less) digest.
    HotPrefixDigestPacket pkt;
    pkt.backend_id = 3;
    pkt.prefix_hashes = {1, 2, 3};
    // verified_hashes left empty.
    auto bytes = pkt.serialize();
    EXPECT_EQ(bytes.size(), HotPrefixDigestPacket::HEADER_SIZE + 3u * 8u);  // no bitmap
    auto got = HotPrefixDigestPacket::deserialize(bytes.data(), bytes.size());
    ASSERT_TRUE(got.has_value());
    EXPECT_TRUE(got->verified_hashes.empty());
}

TEST(HotPrefixDigestPacket, AllMembershipVerified) {
    HotPrefixDigestPacket pkt;
    pkt.backend_id = 1;
    pkt.prefix_hashes = {100, 200, 300};
    pkt.verified_hashes = {100, 200, 300};
    auto bytes = pkt.serialize();
    auto got = HotPrefixDigestPacket::deserialize(bytes.data(), bytes.size());
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->verified_hashes, pkt.prefix_hashes);
}

TEST(HotPrefixDigestPacket, V1PeerWithoutTailDecodesEmptyVerified) {
    // Simulate an old v1 sender: header(v1) + membership, no bitmap tail.
    std::vector<uint8_t> b;
    b.push_back(0x07);
    b.push_back(1);  // version 1
    for (int i = 0; i < 4; ++i) b.push_back(0);  // backend_id = 0
    b.push_back(0); b.push_back(2);              // count = 2
    for (uint64_t h : {uint64_t(0xAB), uint64_t(0xCD)})
        for (int s = 56; s >= 0; s -= 8) b.push_back((h >> s) & 0xFF);
    auto got = HotPrefixDigestPacket::deserialize(b.data(), b.size());
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->prefix_hashes.size(), 2u);
    EXPECT_TRUE(got->verified_hashes.empty());  // no native trust => membership-only
}

TEST(HotPrefixDigestPacket, PartialBitmapTailIsIgnored) {
    // 9 membership entries need a 2-byte bitmap; supply only 1 tail byte. The
    // membership stays authoritative and the half-tail is dropped (not rejected).
    HotPrefixDigestPacket pkt;
    pkt.backend_id = 2;
    pkt.prefix_hashes.assign(9, 0);
    for (int i = 0; i < 9; ++i) pkt.prefix_hashes[i] = 1000 + i;
    pkt.verified_hashes = {1000};
    auto bytes = pkt.serialize();  // HEADER + 9*8 + 2 bitmap bytes
    bytes.resize(bytes.size() - 1);  // chop one bitmap byte => partial tail
    auto got = HotPrefixDigestPacket::deserialize(bytes.data(), bytes.size());
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->prefix_hashes.size(), 9u);
    EXPECT_TRUE(got->verified_hashes.empty());
}

TEST(HotPrefixDigestPacket, VerifiedBitmapSizedToMaxHashes) {
    HotPrefixDigestPacket pkt;
    pkt.backend_id = 9;
    pkt.prefix_hashes.resize(HotPrefixDigestPacket::MAX_HASHES);
    for (size_t i = 0; i < pkt.prefix_hashes.size(); ++i) pkt.prefix_hashes[i] = i + 1;
    pkt.verified_hashes = pkt.prefix_hashes;  // all verified
    auto bytes = pkt.serialize();
    // 128 entries => 16 bitmap bytes.
    EXPECT_EQ(bytes.size(),
              HotPrefixDigestPacket::HEADER_SIZE + 128u * 8u + 16u);
    auto got = HotPrefixDigestPacket::deserialize(bytes.data(), bytes.size());
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->verified_hashes.size(), HotPrefixDigestPacket::MAX_HASHES);
}

TEST(HotPrefixDigestPacket, RejectsTooShortAndWrongType) {
    std::vector<uint8_t> tiny(HotPrefixDigestPacket::HEADER_SIZE - 1, 0);
    EXPECT_FALSE(HotPrefixDigestPacket::deserialize(tiny.data(), tiny.size()).has_value());

    std::vector<uint8_t> wrong(HotPrefixDigestPacket::HEADER_SIZE, 0);
    wrong[0] = 0x06;  // CACHE_STATE, not HOT_PREFIX_DIGEST
    EXPECT_FALSE(HotPrefixDigestPacket::deserialize(wrong.data(), wrong.size()).has_value());
}
