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
    static constexpr uint8_t PROTOCOL_VERSION = 1;
    static constexpr size_t HEADER_SIZE = 8;
    static constexpr size_t MAX_HASHES = 128;

    GossipPacketType type = GossipPacketType::HOT_PREFIX_DIGEST;
    uint8_t version = PROTOCOL_VERSION;
    BackendId backend_id = 0;
    std::vector<uint64_t> prefix_hashes;

    std::vector<uint8_t> serialize() const {
        uint16_t count = static_cast<uint16_t>(
            std::min(prefix_hashes.size(), static_cast<size_t>(MAX_HASHES)));
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
        return p;  // any bytes past `need` are ignored (forward-compat tail)
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

TEST(HotPrefixDigestPacket, DeserializeIgnoresTrailingBytes) {
    // Forward-compat: a future version appends fields; an older parser reads the
    // hashes it knows and ignores the tail.
    HotPrefixDigestPacket pkt;
    pkt.backend_id = 5;
    pkt.prefix_hashes = {0xAAAA, 0xBBBB};
    auto bytes = pkt.serialize();
    bytes.push_back(0xDE);
    bytes.push_back(0xAD);
    auto got = HotPrefixDigestPacket::deserialize(bytes.data(), bytes.size());
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->prefix_hashes.size(), 2u);
    EXPECT_EQ(got->backend_id, 5);
}

TEST(HotPrefixDigestPacket, RejectsTooShortAndWrongType) {
    std::vector<uint8_t> tiny(HotPrefixDigestPacket::HEADER_SIZE - 1, 0);
    EXPECT_FALSE(HotPrefixDigestPacket::deserialize(tiny.data(), tiny.size()).has_value());

    std::vector<uint8_t> wrong(HotPrefixDigestPacket::HEADER_SIZE, 0);
    wrong[0] = 0x06;  // CACHE_STATE, not HOT_PREFIX_DIGEST
    EXPECT_FALSE(HotPrefixDigestPacket::deserialize(wrong.data(), wrong.size()).has_value());
}
