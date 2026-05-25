// Ranvier Core - Gossip Cache-State Packet Unit Tests
//
// Tests for CacheStatePacket wire format (cache-residency-aware routing).
// These tests don't require Seastar runtime — the packet struct is replicated
// here (mirroring the production definition in gossip_protocol.hpp) so the wire
// format can be exercised without pulling in Seastar headers, exactly like
// gossip_cache_eviction_test.cpp.

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include <optional>
#include <cmath>

#include "types.hpp"

using namespace ranvier;

// =============================================================================
// Mirror of GossipPacketType + CacheStatePacket (no Seastar dependency)
// Wire format: [type:1][version:1][backend_id:4][cache_usage:2][residency:2] = 10 bytes
// =============================================================================

enum class GossipPacketType : uint8_t {
    ROUTE_ANNOUNCEMENT = 0x01,
    HEARTBEAT = 0x02,
    ROUTE_ACK = 0x03,
    NODE_STATE = 0x04,
    CACHE_EVICTION = 0x05,
    CACHE_STATE = 0x06,
};

static uint16_t quantize_unit(double v) {
    if (v <= 0.0) return 0;
    if (v >= 1.0) return 65535;
    return static_cast<uint16_t>(v * 65535.0 + 0.5);
}
static double dequantize_unit(uint16_t v) {
    return static_cast<double>(v) / 65535.0;
}

struct CacheStatePacket {
    static constexpr uint8_t PROTOCOL_VERSION = 1;
    static constexpr size_t PACKET_SIZE = 10;

    GossipPacketType type = GossipPacketType::CACHE_STATE;
    uint8_t version = PROTOCOL_VERSION;
    BackendId backend_id = 0;
    double cache_usage = 0.0;
    double residency_weight = 0.0;

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buffer;
        buffer.reserve(PACKET_SIZE);
        buffer.push_back(static_cast<uint8_t>(type));
        buffer.push_back(version);
        // backend_id big-endian
        buffer.push_back((backend_id >> 24) & 0xFF);
        buffer.push_back((backend_id >> 16) & 0xFF);
        buffer.push_back((backend_id >> 8) & 0xFF);
        buffer.push_back(backend_id & 0xFF);
        // cache_usage (u16 fixed-point, big-endian)
        uint16_t u = quantize_unit(cache_usage);
        buffer.push_back((u >> 8) & 0xFF);
        buffer.push_back(u & 0xFF);
        // residency_weight (u16 fixed-point, big-endian)
        uint16_t r = quantize_unit(residency_weight);
        buffer.push_back((r >> 8) & 0xFF);
        buffer.push_back(r & 0xFF);
        return buffer;
    }

    static std::optional<CacheStatePacket> deserialize(const uint8_t* data, size_t len) {
        // Forward-compat: accept len >= PACKET_SIZE (ignore trailing bytes).
        if (len < PACKET_SIZE) {
            return std::nullopt;
        }
        CacheStatePacket pkt;
        pkt.type = static_cast<GossipPacketType>(data[0]);
        pkt.version = data[1];
        if (pkt.type != GossipPacketType::CACHE_STATE) {
            return std::nullopt;
        }
        pkt.backend_id = (static_cast<BackendId>(data[2]) << 24) |
                         (static_cast<BackendId>(data[3]) << 16) |
                         (static_cast<BackendId>(data[4]) << 8) |
                         static_cast<BackendId>(data[5]);
        pkt.cache_usage = dequantize_unit((static_cast<uint16_t>(data[6]) << 8) | data[7]);
        pkt.residency_weight = dequantize_unit((static_cast<uint16_t>(data[8]) << 8) | data[9]);
        return pkt;
    }
};

class GossipCacheStateTest : public ::testing::Test {};

// ---- Wire format constants -------------------------------------------------

TEST_F(GossipCacheStateTest, PacketSize) {
    EXPECT_EQ(CacheStatePacket::PACKET_SIZE, 10u);
}

TEST_F(GossipCacheStateTest, PacketTypeValue) {
    EXPECT_EQ(static_cast<uint8_t>(GossipPacketType::CACHE_STATE), 0x06);
}

TEST_F(GossipCacheStateTest, ProtocolVersion) {
    EXPECT_EQ(CacheStatePacket::PROTOCOL_VERSION, 1);
}

// ---- Serialization ---------------------------------------------------------

TEST_F(GossipCacheStateTest, SerializeDefaultPacket) {
    CacheStatePacket pkt;
    auto data = pkt.serialize();
    ASSERT_EQ(data.size(), CacheStatePacket::PACKET_SIZE);
    EXPECT_EQ(data[0], 0x06);
    EXPECT_EQ(data[1], 0x01);
    for (size_t i = 2; i < data.size(); ++i) {
        EXPECT_EQ(data[i], 0) << "byte " << i << " should be zero";
    }
}

TEST_F(GossipCacheStateTest, SerializeBackendIdBigEndian) {
    CacheStatePacket pkt;
    pkt.backend_id = 0x01020304;
    auto data = pkt.serialize();
    EXPECT_EQ(data[2], 0x01);
    EXPECT_EQ(data[3], 0x02);
    EXPECT_EQ(data[4], 0x03);
    EXPECT_EQ(data[5], 0x04);
}

TEST_F(GossipCacheStateTest, SerializeFullScaleSignals) {
    CacheStatePacket pkt;
    pkt.cache_usage = 1.0;
    pkt.residency_weight = 1.0;
    auto data = pkt.serialize();
    EXPECT_EQ(data[6], 0xFF);
    EXPECT_EQ(data[7], 0xFF);
    EXPECT_EQ(data[8], 0xFF);
    EXPECT_EQ(data[9], 0xFF);
}

// ---- Round trip ------------------------------------------------------------

TEST_F(GossipCacheStateTest, RoundTripValues) {
    CacheStatePacket original;
    original.backend_id = 42;
    original.cache_usage = 0.85;
    original.residency_weight = 0.15;
    auto data = original.serialize();
    auto result = CacheStatePacket::deserialize(data.data(), data.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->backend_id, 42);
    // Fixed-point quantization tolerance (1/65535).
    EXPECT_NEAR(result->cache_usage, 0.85, 1e-4);
    EXPECT_NEAR(result->residency_weight, 0.15, 1e-4);
}

TEST_F(GossipCacheStateTest, RoundTripClampsOutOfRange) {
    CacheStatePacket original;
    original.cache_usage = 1.7;    // clamps to 1.0
    original.residency_weight = -0.3;  // clamps to 0.0
    auto data = original.serialize();
    auto result = CacheStatePacket::deserialize(data.data(), data.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_NEAR(result->cache_usage, 1.0, 1e-4);
    EXPECT_NEAR(result->residency_weight, 0.0, 1e-4);
}

// ---- Forward compatibility: a future version may append trailing fields ----

TEST_F(GossipCacheStateTest, DeserializeIgnoresTrailingBytes) {
    CacheStatePacket original;
    original.backend_id = 7;
    original.cache_usage = 0.5;
    original.residency_weight = 0.5;
    auto data = original.serialize();
    // Simulate a newer node appending two extra signal bytes + bumping version.
    data[1] = 2;  // future minor version of the same type
    data.push_back(0xAB);
    data.push_back(0xCD);

    auto result = CacheStatePacket::deserialize(data.data(), data.size());
    ASSERT_TRUE(result.has_value()) << "must read known fields and ignore the tail";
    EXPECT_EQ(result->backend_id, 7);
    EXPECT_NEAR(result->cache_usage, 0.5, 1e-4);
    EXPECT_NEAR(result->residency_weight, 0.5, 1e-4);
}

// ---- Error cases -----------------------------------------------------------

TEST_F(GossipCacheStateTest, DeserializeFailsTooShort) {
    uint8_t buf[9] = {0x06, 0x01, 0, 0, 0, 0, 0, 0, 0};
    auto result = CacheStatePacket::deserialize(buf, 9);
    EXPECT_FALSE(result.has_value());
}

TEST_F(GossipCacheStateTest, DeserializeFailsWrongType) {
    CacheStatePacket pkt;
    auto data = pkt.serialize();
    data[0] = static_cast<uint8_t>(GossipPacketType::ROUTE_ANNOUNCEMENT);
    auto result = CacheStatePacket::deserialize(data.data(), data.size());
    EXPECT_FALSE(result.has_value());
}

TEST_F(GossipCacheStateTest, DeserializeFailsEmptyBuffer) {
    auto result = CacheStatePacket::deserialize(nullptr, 0);
    EXPECT_FALSE(result.has_value());
}

// ---- Fixed size invariant --------------------------------------------------

TEST_F(GossipCacheStateTest, SerializedSizeAlwaysFixed) {
    for (uint32_t i = 0; i < 10; ++i) {
        CacheStatePacket pkt;
        pkt.backend_id = static_cast<BackendId>(i);
        pkt.cache_usage = static_cast<double>(i) / 10.0;
        pkt.residency_weight = 1.0 - static_cast<double>(i) / 10.0;
        EXPECT_EQ(pkt.serialize().size(), CacheStatePacket::PACKET_SIZE);
    }
}
