// Ranvier Core - Gossip Cache Eviction Packet Unit Tests
//
// Tests for CacheEvictionPacket wire format (Phase 2: cluster propagation).
// These tests don't require Seastar runtime.

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include <optional>

// Include only the types we need (avoid Seastar headers)
#include "types.hpp"

using namespace ranvier;

// =============================================================================
// CacheEvictionPacket - Replicated here to avoid Seastar dependencies
// Wire format: [type:1][version:1][seq_num:4][backend_id:4][prefix_hash:8] = 18 bytes
// =============================================================================

enum class GossipPacketType : uint8_t {
    ROUTE_ANNOUNCEMENT = 0x01,
    HEARTBEAT = 0x02,
    ROUTE_ACK = 0x03,
    NODE_STATE = 0x04,
    CACHE_EVICTION = 0x05,
};

struct CacheEvictionPacket {
    static constexpr uint8_t PROTOCOL_VERSION = 2;
    static constexpr size_t PACKET_SIZE = 18;

    GossipPacketType type = GossipPacketType::CACHE_EVICTION;
    uint8_t version = PROTOCOL_VERSION;
    uint32_t seq_num = 0;
    BackendId backend_id = 0;
    uint64_t prefix_hash = 0;

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buffer;
        buffer.reserve(PACKET_SIZE);

        buffer.push_back(static_cast<uint8_t>(type));
        buffer.push_back(version);

        // Sequence number (big-endian)
        buffer.push_back((seq_num >> 24) & 0xFF);
        buffer.push_back((seq_num >> 16) & 0xFF);
        buffer.push_back((seq_num >> 8) & 0xFF);
        buffer.push_back(seq_num & 0xFF);

        // Backend ID (big-endian)
        buffer.push_back((backend_id >> 24) & 0xFF);
        buffer.push_back((backend_id >> 16) & 0xFF);
        buffer.push_back((backend_id >> 8) & 0xFF);
        buffer.push_back(backend_id & 0xFF);

        // Prefix hash (big-endian, 8 bytes)
        buffer.push_back((prefix_hash >> 56) & 0xFF);
        buffer.push_back((prefix_hash >> 48) & 0xFF);
        buffer.push_back((prefix_hash >> 40) & 0xFF);
        buffer.push_back((prefix_hash >> 32) & 0xFF);
        buffer.push_back((prefix_hash >> 24) & 0xFF);
        buffer.push_back((prefix_hash >> 16) & 0xFF);
        buffer.push_back((prefix_hash >> 8) & 0xFF);
        buffer.push_back(prefix_hash & 0xFF);

        return buffer;
    }

    static std::optional<CacheEvictionPacket> deserialize(const uint8_t* data, size_t len) {
        if (len != PACKET_SIZE) {
            return std::nullopt;
        }

        CacheEvictionPacket pkt;
        pkt.type = static_cast<GossipPacketType>(data[0]);
        pkt.version = data[1];

        if (pkt.type != GossipPacketType::CACHE_EVICTION || pkt.version != PROTOCOL_VERSION) {
            return std::nullopt;
        }

        pkt.seq_num = (static_cast<uint32_t>(data[2]) << 24) |
                      (static_cast<uint32_t>(data[3]) << 16) |
                      (static_cast<uint32_t>(data[4]) << 8) |
                      static_cast<uint32_t>(data[5]);

        pkt.backend_id = (static_cast<BackendId>(data[6]) << 24) |
                         (static_cast<BackendId>(data[7]) << 16) |
                         (static_cast<BackendId>(data[8]) << 8) |
                         static_cast<BackendId>(data[9]);

        pkt.prefix_hash = (static_cast<uint64_t>(data[10]) << 56) |
                          (static_cast<uint64_t>(data[11]) << 48) |
                          (static_cast<uint64_t>(data[12]) << 40) |
                          (static_cast<uint64_t>(data[13]) << 32) |
                          (static_cast<uint64_t>(data[14]) << 24) |
                          (static_cast<uint64_t>(data[15]) << 16) |
                          (static_cast<uint64_t>(data[16]) << 8) |
                          static_cast<uint64_t>(data[17]);

        return pkt;
    }
};

// =============================================================================
// Test Fixture
// =============================================================================

class GossipCacheEvictionTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// =============================================================================
// Wire Format Constants
// =============================================================================

TEST_F(GossipCacheEvictionTest, PacketSize) {
    EXPECT_EQ(CacheEvictionPacket::PACKET_SIZE, 18u);
}

TEST_F(GossipCacheEvictionTest, PacketTypeValue) {
    EXPECT_EQ(static_cast<uint8_t>(GossipPacketType::CACHE_EVICTION), 0x05);
}

TEST_F(GossipCacheEvictionTest, ProtocolVersion) {
    EXPECT_EQ(CacheEvictionPacket::PROTOCOL_VERSION, 2);
}

// =============================================================================
// Serialization Tests
// =============================================================================

TEST_F(GossipCacheEvictionTest, SerializeDefaultPacket) {
    CacheEvictionPacket pkt;
    auto data = pkt.serialize();

    ASSERT_EQ(data.size(), CacheEvictionPacket::PACKET_SIZE);
    EXPECT_EQ(data[0], static_cast<uint8_t>(GossipPacketType::CACHE_EVICTION));
    EXPECT_EQ(data[1], CacheEvictionPacket::PROTOCOL_VERSION);

    // All fields zero
    for (size_t i = 2; i < CacheEvictionPacket::PACKET_SIZE; ++i) {
        EXPECT_EQ(data[i], 0) << "byte " << i << " should be zero";
    }
}

TEST_F(GossipCacheEvictionTest, SerializeWithValues) {
    CacheEvictionPacket pkt;
    pkt.seq_num = 0x00000001;
    pkt.backend_id = 42;
    pkt.prefix_hash = 0xA1B2C3D4E5F60708ULL;

    auto data = pkt.serialize();
    ASSERT_EQ(data.size(), 18u);

    // Type + version
    EXPECT_EQ(data[0], 0x05);
    EXPECT_EQ(data[1], 0x02);

    // Seq num = 1
    EXPECT_EQ(data[2], 0x00);
    EXPECT_EQ(data[3], 0x00);
    EXPECT_EQ(data[4], 0x00);
    EXPECT_EQ(data[5], 0x01);

    // Backend ID = 42 (0x0000002A)
    EXPECT_EQ(data[6], 0x00);
    EXPECT_EQ(data[7], 0x00);
    EXPECT_EQ(data[8], 0x00);
    EXPECT_EQ(data[9], 0x2A);

    // Prefix hash = 0xA1B2C3D4E5F60708 (big-endian)
    EXPECT_EQ(data[10], 0xA1);
    EXPECT_EQ(data[11], 0xB2);
    EXPECT_EQ(data[12], 0xC3);
    EXPECT_EQ(data[13], 0xD4);
    EXPECT_EQ(data[14], 0xE5);
    EXPECT_EQ(data[15], 0xF6);
    EXPECT_EQ(data[16], 0x07);
    EXPECT_EQ(data[17], 0x08);
}

TEST_F(GossipCacheEvictionTest, SerializeLargeSeqNum) {
    CacheEvictionPacket pkt;
    pkt.seq_num = 0xDEADBEEF;

    auto data = pkt.serialize();

    EXPECT_EQ(data[2], 0xDE);
    EXPECT_EQ(data[3], 0xAD);
    EXPECT_EQ(data[4], 0xBE);
    EXPECT_EQ(data[5], 0xEF);
}

TEST_F(GossipCacheEvictionTest, SerializeLargeBackendId) {
    CacheEvictionPacket pkt;
    pkt.backend_id = 0x7FFFFFFF; // max positive int32

    auto data = pkt.serialize();

    EXPECT_EQ(data[6], 0x7F);
    EXPECT_EQ(data[7], 0xFF);
    EXPECT_EQ(data[8], 0xFF);
    EXPECT_EQ(data[9], 0xFF);
}

TEST_F(GossipCacheEvictionTest, SerializeMaxPrefixHash) {
    CacheEvictionPacket pkt;
    pkt.prefix_hash = 0xFFFFFFFFFFFFFFFFULL;

    auto data = pkt.serialize();

    for (size_t i = 10; i < 18; ++i) {
        EXPECT_EQ(data[i], 0xFF) << "byte " << i << " should be 0xFF";
    }
}

// =============================================================================
// Deserialization Round-Trip Tests
// =============================================================================

TEST_F(GossipCacheEvictionTest, RoundTripDefault) {
    CacheEvictionPacket original;
    auto data = original.serialize();
    auto result = CacheEvictionPacket::deserialize(data.data(), data.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type, GossipPacketType::CACHE_EVICTION);
    EXPECT_EQ(result->version, CacheEvictionPacket::PROTOCOL_VERSION);
    EXPECT_EQ(result->seq_num, 0u);
    EXPECT_EQ(result->backend_id, 0);
    EXPECT_EQ(result->prefix_hash, 0u);
}

TEST_F(GossipCacheEvictionTest, RoundTripWithValues) {
    CacheEvictionPacket original;
    original.seq_num = 12345;
    original.backend_id = 7;
    original.prefix_hash = 0xCAFEBABEDEADBEEFULL;

    auto data = original.serialize();
    auto result = CacheEvictionPacket::deserialize(data.data(), data.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->seq_num, 12345u);
    EXPECT_EQ(result->backend_id, 7);
    EXPECT_EQ(result->prefix_hash, 0xCAFEBABEDEADBEEFULL);
}

TEST_F(GossipCacheEvictionTest, RoundTripMaxValues) {
    CacheEvictionPacket original;
    original.seq_num = 0xFFFFFFFF;
    original.backend_id = 0x7FFFFFFF;
    original.prefix_hash = 0xFFFFFFFFFFFFFFFFULL;

    auto data = original.serialize();
    auto result = CacheEvictionPacket::deserialize(data.data(), data.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->seq_num, 0xFFFFFFFFu);
    EXPECT_EQ(result->backend_id, 0x7FFFFFFF);
    EXPECT_EQ(result->prefix_hash, 0xFFFFFFFFFFFFFFFFULL);
}

// =============================================================================
// Deserialization Error Cases
// =============================================================================

TEST_F(GossipCacheEvictionTest, DeserializeFailsTooShort) {
    uint8_t short_data[10] = {0x05, 0x02, 0, 0, 0, 0, 0, 0, 0, 0};
    auto result = CacheEvictionPacket::deserialize(short_data, 10);
    EXPECT_FALSE(result.has_value());
}

TEST_F(GossipCacheEvictionTest, DeserializeFailsTooLong) {
    uint8_t long_data[20] = {};
    long_data[0] = 0x05;
    long_data[1] = 0x02;
    auto result = CacheEvictionPacket::deserialize(long_data, 20);
    EXPECT_FALSE(result.has_value());
}

TEST_F(GossipCacheEvictionTest, DeserializeFailsEmptyBuffer) {
    auto result = CacheEvictionPacket::deserialize(nullptr, 0);
    EXPECT_FALSE(result.has_value());
}

TEST_F(GossipCacheEvictionTest, DeserializeFailsWrongType) {
    CacheEvictionPacket pkt;
    auto data = pkt.serialize();

    // Change type to ROUTE_ANNOUNCEMENT
    data[0] = static_cast<uint8_t>(GossipPacketType::ROUTE_ANNOUNCEMENT);

    auto result = CacheEvictionPacket::deserialize(data.data(), data.size());
    EXPECT_FALSE(result.has_value());
}

TEST_F(GossipCacheEvictionTest, DeserializeFailsWrongVersion) {
    CacheEvictionPacket pkt;
    auto data = pkt.serialize();

    // Change version to 99
    data[1] = 99;

    auto result = CacheEvictionPacket::deserialize(data.data(), data.size());
    EXPECT_FALSE(result.has_value());
}

TEST_F(GossipCacheEvictionTest, DeserializeFailsHeartbeatType) {
    CacheEvictionPacket pkt;
    auto data = pkt.serialize();

    data[0] = static_cast<uint8_t>(GossipPacketType::HEARTBEAT);

    auto result = CacheEvictionPacket::deserialize(data.data(), data.size());
    EXPECT_FALSE(result.has_value());
}

TEST_F(GossipCacheEvictionTest, DeserializeFailsSingleByte) {
    uint8_t one_byte = 0x05;
    auto result = CacheEvictionPacket::deserialize(&one_byte, 1);
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// Byte Order Verification
// =============================================================================

TEST_F(GossipCacheEvictionTest, BigEndianByteOrder) {
    // Verify each multi-byte field is serialized in big-endian order
    CacheEvictionPacket pkt;
    pkt.seq_num = 0x01020304;
    pkt.backend_id = 0x05060708;
    pkt.prefix_hash = 0x090A0B0C0D0E0F10ULL;

    auto data = pkt.serialize();

    // seq_num at bytes 2-5
    EXPECT_EQ(data[2], 0x01);
    EXPECT_EQ(data[3], 0x02);
    EXPECT_EQ(data[4], 0x03);
    EXPECT_EQ(data[5], 0x04);

    // backend_id at bytes 6-9
    EXPECT_EQ(data[6], 0x05);
    EXPECT_EQ(data[7], 0x06);
    EXPECT_EQ(data[8], 0x07);
    EXPECT_EQ(data[9], 0x08);

    // prefix_hash at bytes 10-17
    EXPECT_EQ(data[10], 0x09);
    EXPECT_EQ(data[11], 0x0A);
    EXPECT_EQ(data[12], 0x0B);
    EXPECT_EQ(data[13], 0x0C);
    EXPECT_EQ(data[14], 0x0D);
    EXPECT_EQ(data[15], 0x0E);
    EXPECT_EQ(data[16], 0x0F);
    EXPECT_EQ(data[17], 0x10);
}

// =============================================================================
// Fixed Size Invariant
// =============================================================================

TEST_F(GossipCacheEvictionTest, SerializedSizeAlwaysFixed) {
    // Unlike RouteAnnouncementPacket which has variable size,
    // CacheEvictionPacket is always exactly PACKET_SIZE bytes
    for (uint32_t i = 0; i < 10; ++i) {
        CacheEvictionPacket pkt;
        pkt.seq_num = i * 1000;
        pkt.backend_id = static_cast<BackendId>(i);
        pkt.prefix_hash = static_cast<uint64_t>(i) << 32 | i;

        auto data = pkt.serialize();
        EXPECT_EQ(data.size(), CacheEvictionPacket::PACKET_SIZE)
            << "Iteration " << i << " produced wrong size";
    }
}
