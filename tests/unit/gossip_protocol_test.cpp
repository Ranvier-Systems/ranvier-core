// Ranvier Core - Gossip Protocol Unit Tests
//
// Tests for gossip protocol wire format and packet serialization.
// These tests don't require Seastar runtime.

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include <optional>

// Include only the types we need (avoid Seastar headers)
#include "types.hpp"

using namespace ranvier;

// =============================================================================
// RouteAnnouncementPacket - Replicated here to avoid Seastar dependencies
// Wire format v2: [type:1][version:1][seq_num:4][backend_id:4][token_count:2][tokens:4*N]
// =============================================================================

enum class GossipPacketType : uint8_t {
    ROUTE_ANNOUNCEMENT = 0x01,
    HEARTBEAT = 0x02,
    ROUTE_ACK = 0x03,
};

struct RouteAnnouncementPacket {
    static constexpr uint8_t PROTOCOL_VERSION = 2;  // Version 2 includes seq_num
    static constexpr size_t HEADER_SIZE = 12;  // type + version + seq_num + backend_id + token_count
    static constexpr size_t MAX_TOKENS = 256;
    static constexpr size_t MAX_PACKET_SIZE = HEADER_SIZE + (MAX_TOKENS * sizeof(TokenId));

    GossipPacketType type = GossipPacketType::ROUTE_ANNOUNCEMENT;
    uint8_t version = PROTOCOL_VERSION;
    uint32_t seq_num = 0;  // Sequence number for reliable delivery
    BackendId backend_id = 0;
    uint16_t token_count = 0;
    std::vector<TokenId> tokens;

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buffer;
        buffer.reserve(HEADER_SIZE + tokens.size() * sizeof(TokenId));

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

        uint16_t count = static_cast<uint16_t>(std::min(tokens.size(), static_cast<size_t>(MAX_TOKENS)));
        buffer.push_back((count >> 8) & 0xFF);
        buffer.push_back(count & 0xFF);

        for (size_t i = 0; i < count; ++i) {
            TokenId t = tokens[i];
            buffer.push_back((t >> 24) & 0xFF);
            buffer.push_back((t >> 16) & 0xFF);
            buffer.push_back((t >> 8) & 0xFF);
            buffer.push_back(t & 0xFF);
        }

        return buffer;
    }

    static std::optional<RouteAnnouncementPacket> deserialize(const uint8_t* data, size_t len) {
        if (len < HEADER_SIZE) {
            return std::nullopt;
        }

        RouteAnnouncementPacket pkt;
        pkt.type = static_cast<GossipPacketType>(data[0]);
        pkt.version = data[1];

        if (pkt.type != GossipPacketType::ROUTE_ANNOUNCEMENT || pkt.version != PROTOCOL_VERSION) {
            return std::nullopt;
        }

        // Sequence number (big-endian)
        pkt.seq_num = (static_cast<uint32_t>(data[2]) << 24) |
                      (static_cast<uint32_t>(data[3]) << 16) |
                      (static_cast<uint32_t>(data[4]) << 8) |
                      static_cast<uint32_t>(data[5]);

        // Backend ID (big-endian)
        pkt.backend_id = (static_cast<BackendId>(data[6]) << 24) |
                         (static_cast<BackendId>(data[7]) << 16) |
                         (static_cast<BackendId>(data[8]) << 8) |
                         static_cast<BackendId>(data[9]);

        pkt.token_count = (static_cast<uint16_t>(data[10]) << 8) | static_cast<uint16_t>(data[11]);

        size_t expected_size = HEADER_SIZE + pkt.token_count * sizeof(TokenId);
        if (len != expected_size || pkt.token_count > MAX_TOKENS) {
            return std::nullopt;
        }

        pkt.tokens.reserve(pkt.token_count);
        for (size_t i = 0; i < pkt.token_count; ++i) {
            size_t offset = HEADER_SIZE + i * sizeof(TokenId);
            TokenId t = (static_cast<TokenId>(data[offset]) << 24) |
                        (static_cast<TokenId>(data[offset + 1]) << 16) |
                        (static_cast<TokenId>(data[offset + 2]) << 8) |
                        static_cast<TokenId>(data[offset + 3]);
            pkt.tokens.push_back(t);
        }

        return pkt;
    }
};

// Wire format for route acknowledgments
// Format: [type:1][version:1][seq_num:4]
struct RouteAckPacket {
    static constexpr uint8_t PROTOCOL_VERSION = 2;
    static constexpr size_t PACKET_SIZE = 6;  // type + version + seq_num

    GossipPacketType type = GossipPacketType::ROUTE_ACK;
    uint8_t version = PROTOCOL_VERSION;
    uint32_t seq_num = 0;  // Sequence number being acknowledged

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

        return buffer;
    }

    static std::optional<RouteAckPacket> deserialize(const uint8_t* data, size_t len) {
        if (len != PACKET_SIZE) {
            return std::nullopt;
        }

        RouteAckPacket pkt;
        pkt.type = static_cast<GossipPacketType>(data[0]);
        pkt.version = data[1];

        if (pkt.type != GossipPacketType::ROUTE_ACK || pkt.version != PROTOCOL_VERSION) {
            return std::nullopt;
        }

        // Sequence number (big-endian)
        pkt.seq_num = (static_cast<uint32_t>(data[2]) << 24) |
                      (static_cast<uint32_t>(data[3]) << 16) |
                      (static_cast<uint32_t>(data[4]) << 8) |
                      static_cast<uint32_t>(data[5]);

        return pkt;
    }
};

// =============================================================================
// Wire Format Tests
// =============================================================================

class GossipProtocolTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(GossipProtocolTest, PacketHeaderSize) {
    EXPECT_EQ(RouteAnnouncementPacket::HEADER_SIZE, 12u);  // v2: type + version + seq_num + backend_id + token_count
}

TEST_F(GossipProtocolTest, MaxTokensLimit) {
    EXPECT_EQ(RouteAnnouncementPacket::MAX_TOKENS, 256u);
}

TEST_F(GossipProtocolTest, MaxPacketSize) {
    // Header (12) + 256 tokens * 4 bytes = 1036 bytes
    EXPECT_EQ(RouteAnnouncementPacket::MAX_PACKET_SIZE, 12u + 256u * 4u);
}

// =============================================================================
// Serialization Tests
// =============================================================================

TEST_F(GossipProtocolTest, SerializeEmptyPacket) {
    RouteAnnouncementPacket pkt;
    pkt.backend_id = 0;
    pkt.tokens = {};

    auto data = pkt.serialize();

    ASSERT_EQ(data.size(), RouteAnnouncementPacket::HEADER_SIZE);
    EXPECT_EQ(data[0], static_cast<uint8_t>(GossipPacketType::ROUTE_ANNOUNCEMENT));
    EXPECT_EQ(data[1], RouteAnnouncementPacket::PROTOCOL_VERSION);
    // Seq num = 0
    EXPECT_EQ(data[2], 0);
    EXPECT_EQ(data[3], 0);
    EXPECT_EQ(data[4], 0);
    EXPECT_EQ(data[5], 0);
    // Backend ID = 0
    EXPECT_EQ(data[6], 0);
    EXPECT_EQ(data[7], 0);
    EXPECT_EQ(data[8], 0);
    EXPECT_EQ(data[9], 0);
    // Token count = 0
    EXPECT_EQ(data[10], 0);
    EXPECT_EQ(data[11], 0);
}

TEST_F(GossipProtocolTest, SerializeSingleToken) {
    RouteAnnouncementPacket pkt;
    pkt.backend_id = 42;
    pkt.seq_num = 1;
    pkt.tokens = {0x12345678};

    auto data = pkt.serialize();

    ASSERT_EQ(data.size(), RouteAnnouncementPacket::HEADER_SIZE + 4);

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

    // Token count = 1
    EXPECT_EQ(data[10], 0x00);
    EXPECT_EQ(data[11], 0x01);

    // Token 0x12345678 in big-endian
    EXPECT_EQ(data[12], 0x12);
    EXPECT_EQ(data[13], 0x34);
    EXPECT_EQ(data[14], 0x56);
    EXPECT_EQ(data[15], 0x78);
}

TEST_F(GossipProtocolTest, SerializeMultipleTokens) {
    RouteAnnouncementPacket pkt;
    pkt.backend_id = 100;
    pkt.tokens = {1, 2, 3, 4, 5};

    auto data = pkt.serialize();

    ASSERT_EQ(data.size(), RouteAnnouncementPacket::HEADER_SIZE + 5 * 4);

    // Token count = 5
    EXPECT_EQ(data[10], 0x00);
    EXPECT_EQ(data[11], 0x05);
}

TEST_F(GossipProtocolTest, SerializeLargeBackendId) {
    RouteAnnouncementPacket pkt;
    pkt.backend_id = 0xDEADBEEF;
    pkt.tokens = {};

    auto data = pkt.serialize();

    // Backend ID in big-endian (at offset 6-9)
    EXPECT_EQ(data[6], 0xDE);
    EXPECT_EQ(data[7], 0xAD);
    EXPECT_EQ(data[8], 0xBE);
    EXPECT_EQ(data[9], 0xEF);
}

TEST_F(GossipProtocolTest, SerializeTruncatesExcessTokens) {
    RouteAnnouncementPacket pkt;
    pkt.backend_id = 1;

    // Create more tokens than MAX_TOKENS
    pkt.tokens.resize(RouteAnnouncementPacket::MAX_TOKENS + 100);
    for (size_t i = 0; i < pkt.tokens.size(); ++i) {
        pkt.tokens[i] = static_cast<TokenId>(i);
    }

    auto data = pkt.serialize();

    // Should only serialize MAX_TOKENS
    size_t expected_size = RouteAnnouncementPacket::HEADER_SIZE +
                           RouteAnnouncementPacket::MAX_TOKENS * sizeof(TokenId);
    EXPECT_EQ(data.size(), expected_size);

    // Token count should be MAX_TOKENS (at offset 10-11)
    uint16_t token_count = (static_cast<uint16_t>(data[10]) << 8) | data[11];
    EXPECT_EQ(token_count, RouteAnnouncementPacket::MAX_TOKENS);
}

// =============================================================================
// Deserialization Tests
// =============================================================================

TEST_F(GossipProtocolTest, DeserializeEmptyPacket) {
    RouteAnnouncementPacket original;
    original.backend_id = 123;
    original.tokens = {};

    auto data = original.serialize();
    auto result = RouteAnnouncementPacket::deserialize(data.data(), data.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type, GossipPacketType::ROUTE_ANNOUNCEMENT);
    EXPECT_EQ(result->version, RouteAnnouncementPacket::PROTOCOL_VERSION);
    EXPECT_EQ(result->backend_id, 123);
    EXPECT_EQ(result->token_count, 0);
    EXPECT_TRUE(result->tokens.empty());
}

TEST_F(GossipProtocolTest, DeserializeSingleToken) {
    RouteAnnouncementPacket original;
    original.backend_id = 42;
    original.tokens = {0x12345678};

    auto data = original.serialize();
    auto result = RouteAnnouncementPacket::deserialize(data.data(), data.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->backend_id, 42);
    ASSERT_EQ(result->tokens.size(), 1u);
    EXPECT_EQ(result->tokens[0], 0x12345678);
}

TEST_F(GossipProtocolTest, DeserializeMultipleTokens) {
    RouteAnnouncementPacket original;
    original.backend_id = 999;
    original.tokens = {100, 200, 300, 400, 500};

    auto data = original.serialize();
    auto result = RouteAnnouncementPacket::deserialize(data.data(), data.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->backend_id, 999);
    ASSERT_EQ(result->tokens.size(), 5u);
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(result->tokens[i], original.tokens[i]);
    }
}

TEST_F(GossipProtocolTest, DeserializeMaxTokens) {
    RouteAnnouncementPacket original;
    original.backend_id = 1;
    original.tokens.resize(RouteAnnouncementPacket::MAX_TOKENS);
    for (size_t i = 0; i < original.tokens.size(); ++i) {
        original.tokens[i] = static_cast<TokenId>(i * 7);
    }

    auto data = original.serialize();
    auto result = RouteAnnouncementPacket::deserialize(data.data(), data.size());

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->tokens.size(), RouteAnnouncementPacket::MAX_TOKENS);
    for (size_t i = 0; i < result->tokens.size(); ++i) {
        EXPECT_EQ(result->tokens[i], original.tokens[i]);
    }
}

// =============================================================================
// Deserialization Error Cases
// =============================================================================

TEST_F(GossipProtocolTest, DeserializeFailsTooShort) {
    uint8_t short_data[4] = {0x01, 0x01, 0x00, 0x00};
    auto result = RouteAnnouncementPacket::deserialize(short_data, 4);
    EXPECT_FALSE(result.has_value());
}

TEST_F(GossipProtocolTest, DeserializeFailsWrongType) {
    RouteAnnouncementPacket pkt;
    pkt.tokens = {};
    auto data = pkt.serialize();

    // Change type to HEARTBEAT
    data[0] = static_cast<uint8_t>(GossipPacketType::HEARTBEAT);

    auto result = RouteAnnouncementPacket::deserialize(data.data(), data.size());
    EXPECT_FALSE(result.has_value());
}

TEST_F(GossipProtocolTest, DeserializeFailsWrongVersion) {
    RouteAnnouncementPacket pkt;
    pkt.tokens = {};
    auto data = pkt.serialize();

    // Change version to invalid
    data[1] = 99;

    auto result = RouteAnnouncementPacket::deserialize(data.data(), data.size());
    EXPECT_FALSE(result.has_value());
}

TEST_F(GossipProtocolTest, DeserializeFailsSizeMismatch) {
    RouteAnnouncementPacket pkt;
    pkt.tokens = {1, 2, 3};
    auto data = pkt.serialize();

    // Truncate packet (remove last token)
    auto result = RouteAnnouncementPacket::deserialize(data.data(), data.size() - 4);
    EXPECT_FALSE(result.has_value());
}

TEST_F(GossipProtocolTest, DeserializeFailsTooManyTokens) {
    // Manually create packet with token_count > MAX_TOKENS
    std::vector<uint8_t> data(RouteAnnouncementPacket::HEADER_SIZE);
    data[0] = static_cast<uint8_t>(GossipPacketType::ROUTE_ANNOUNCEMENT);
    data[1] = RouteAnnouncementPacket::PROTOCOL_VERSION;
    // seq_num = 0
    data[2] = 0; data[3] = 0; data[4] = 0; data[5] = 0;
    // backend_id = 1
    data[6] = 0; data[7] = 0; data[8] = 0; data[9] = 1;

    // Set token_count to 257 (> MAX_TOKENS)
    data[10] = 0x01;
    data[11] = 0x01;

    auto result = RouteAnnouncementPacket::deserialize(data.data(), data.size());
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// Round-Trip Tests
// =============================================================================

TEST_F(GossipProtocolTest, RoundTripPreservesData) {
    RouteAnnouncementPacket original;
    original.backend_id = 0x12345678;
    original.tokens = {0x1AAAAAAA, 0x2BBBBBBB, 0x3CCCCCCC, 0x4DDDDDDD};

    auto data = original.serialize();
    auto result = RouteAnnouncementPacket::deserialize(data.data(), data.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->backend_id, original.backend_id);
    EXPECT_EQ(result->tokens, original.tokens);
}

TEST_F(GossipProtocolTest, RoundTripEdgeCases) {
    // Test with backend_id = 0
    {
        RouteAnnouncementPacket pkt;
        pkt.backend_id = 0;
        pkt.tokens = {0};
        auto data = pkt.serialize();
        auto result = RouteAnnouncementPacket::deserialize(data.data(), data.size());
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->backend_id, 0);
    }

    // Test with max int32 values
    {
        RouteAnnouncementPacket pkt;
        pkt.backend_id = 0x7FFFFFFF;  // Max positive int32
        pkt.tokens = {0x7FFFFFFF, -1};  // Max positive and -1
        auto data = pkt.serialize();
        auto result = RouteAnnouncementPacket::deserialize(data.data(), data.size());
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->backend_id, 0x7FFFFFFF);
        EXPECT_EQ(result->tokens[0], 0x7FFFFFFF);
        EXPECT_EQ(result->tokens[1], -1);
    }

    // Test with negative values (valid for signed TokenId)
    {
        RouteAnnouncementPacket pkt;
        pkt.backend_id = -1;
        pkt.tokens = {-100, -200, -300};
        auto data = pkt.serialize();
        auto result = RouteAnnouncementPacket::deserialize(data.data(), data.size());
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->backend_id, -1);
        EXPECT_EQ(result->tokens[0], -100);
        EXPECT_EQ(result->tokens[1], -200);
        EXPECT_EQ(result->tokens[2], -300);
    }
}

// =============================================================================
// Heartbeat Packet Tests
// =============================================================================

TEST_F(GossipProtocolTest, HeartbeatPacketFormat) {
    // Heartbeat is just 2 bytes: type + version
    std::vector<uint8_t> heartbeat = {
        static_cast<uint8_t>(GossipPacketType::HEARTBEAT),
        RouteAnnouncementPacket::PROTOCOL_VERSION
    };

    EXPECT_EQ(heartbeat.size(), 2u);
    EXPECT_EQ(heartbeat[0], 0x02);  // HEARTBEAT
    EXPECT_EQ(heartbeat[1], 0x02);  // VERSION 2
}

// =============================================================================
// Packet Type Enum Tests
// =============================================================================

TEST_F(GossipProtocolTest, PacketTypeValues) {
    EXPECT_EQ(static_cast<uint8_t>(GossipPacketType::ROUTE_ANNOUNCEMENT), 0x01);
    EXPECT_EQ(static_cast<uint8_t>(GossipPacketType::HEARTBEAT), 0x02);
    EXPECT_EQ(static_cast<uint8_t>(GossipPacketType::ROUTE_ACK), 0x03);
}

// =============================================================================
// Route ACK Packet Tests
// =============================================================================

TEST_F(GossipProtocolTest, RouteAckPacketSize) {
    EXPECT_EQ(RouteAckPacket::PACKET_SIZE, 6u);  // type + version + seq_num
}

TEST_F(GossipProtocolTest, RouteAckSerialize) {
    RouteAckPacket ack;
    ack.seq_num = 0x12345678;

    auto data = ack.serialize();

    ASSERT_EQ(data.size(), RouteAckPacket::PACKET_SIZE);
    EXPECT_EQ(data[0], static_cast<uint8_t>(GossipPacketType::ROUTE_ACK));
    EXPECT_EQ(data[1], RouteAckPacket::PROTOCOL_VERSION);
    // Seq num in big-endian
    EXPECT_EQ(data[2], 0x12);
    EXPECT_EQ(data[3], 0x34);
    EXPECT_EQ(data[4], 0x56);
    EXPECT_EQ(data[5], 0x78);
}

TEST_F(GossipProtocolTest, RouteAckDeserialize) {
    RouteAckPacket original;
    original.seq_num = 42;

    auto data = original.serialize();
    auto result = RouteAckPacket::deserialize(data.data(), data.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->type, GossipPacketType::ROUTE_ACK);
    EXPECT_EQ(result->version, RouteAckPacket::PROTOCOL_VERSION);
    EXPECT_EQ(result->seq_num, 42u);
}

TEST_F(GossipProtocolTest, RouteAckDeserializeFailsWrongSize) {
    uint8_t short_data[4] = {0x03, 0x02, 0x00, 0x00};
    auto result = RouteAckPacket::deserialize(short_data, 4);
    EXPECT_FALSE(result.has_value());
}

TEST_F(GossipProtocolTest, RouteAckDeserializeFailsWrongType) {
    RouteAckPacket ack;
    ack.seq_num = 1;
    auto data = ack.serialize();

    // Change type to ROUTE_ANNOUNCEMENT
    data[0] = static_cast<uint8_t>(GossipPacketType::ROUTE_ANNOUNCEMENT);

    auto result = RouteAckPacket::deserialize(data.data(), data.size());
    EXPECT_FALSE(result.has_value());
}

TEST_F(GossipProtocolTest, RouteAnnouncementWithSeqNum) {
    // Test sequence number round-trip
    RouteAnnouncementPacket pkt;
    pkt.seq_num = 0xFEDCBA98;
    pkt.backend_id = 42;
    pkt.tokens = {1, 2, 3};

    auto data = pkt.serialize();
    auto result = RouteAnnouncementPacket::deserialize(data.data(), data.size());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->seq_num, 0xFEDCBA98u);
    EXPECT_EQ(result->backend_id, 42);
    ASSERT_EQ(result->tokens.size(), 3u);
}

// =============================================================================
// Crypto Offload Threshold Tests
// =============================================================================
// These tests verify the threshold logic used by GossipService to decide when
// to offload crypto operations to background threads to avoid reactor stalls.

class CryptoOffloadTest : public ::testing::Test {
protected:
    // Thresholds matching gossip_service.hpp constants
    static constexpr size_t CRYPTO_OFFLOAD_PEER_THRESHOLD = 10;
    static constexpr size_t CRYPTO_OFFLOAD_BYTES_THRESHOLD = 1024;
    static constexpr uint64_t CRYPTO_STALL_WARNING_US = 100;

    // Simulates the decision logic in send_encrypted()
    enum class EncryptPath {
        INLINE,      // Small packets: encrypt directly on reactor
        OFFLOADED    // Large packets: use seastar::async
    };

    EncryptPath get_encrypt_path(size_t packet_size) const {
        return packet_size > CRYPTO_OFFLOAD_BYTES_THRESHOLD
            ? EncryptPath::OFFLOADED
            : EncryptPath::INLINE;
    }

    // Simulates the decision logic in broadcast_encrypted()
    enum class BroadcastPath {
        PARALLEL_FOR_EACH,  // Small peer count: parallel_for_each
        BATCH_THREAD        // Large peer count: seastar::thread with batching
    };

    BroadcastPath get_broadcast_path(size_t peer_count) const {
        return peer_count > CRYPTO_OFFLOAD_PEER_THRESHOLD
            ? BroadcastPath::BATCH_THREAD
            : BroadcastPath::PARALLEL_FOR_EACH;
    }

    // Simulates the batch stall threshold calculation
    uint64_t calculate_batch_threshold(size_t peer_count) const {
        return CRYPTO_STALL_WARNING_US * peer_count;
    }
};

TEST_F(CryptoOffloadTest, ThresholdConstants) {
    // Verify thresholds are reasonable values
    EXPECT_EQ(CRYPTO_OFFLOAD_PEER_THRESHOLD, 10u);
    EXPECT_EQ(CRYPTO_OFFLOAD_BYTES_THRESHOLD, 1024u);
    EXPECT_EQ(CRYPTO_STALL_WARNING_US, 100u);
}

TEST_F(CryptoOffloadTest, SmallPacketUsesInlinePath) {
    // Packets <= 1024 bytes should encrypt inline
    EXPECT_EQ(get_encrypt_path(0), EncryptPath::INLINE);
    EXPECT_EQ(get_encrypt_path(100), EncryptPath::INLINE);
    EXPECT_EQ(get_encrypt_path(512), EncryptPath::INLINE);
    EXPECT_EQ(get_encrypt_path(1024), EncryptPath::INLINE);
}

TEST_F(CryptoOffloadTest, LargePacketUsesOffloadPath) {
    // Packets > 1024 bytes should offload to background thread
    EXPECT_EQ(get_encrypt_path(1025), EncryptPath::OFFLOADED);
    EXPECT_EQ(get_encrypt_path(2048), EncryptPath::OFFLOADED);
    EXPECT_EQ(get_encrypt_path(65536), EncryptPath::OFFLOADED);
}

TEST_F(CryptoOffloadTest, SmallPeerCountUsesParallelForEach) {
    // <= 10 peers should use parallel_for_each
    EXPECT_EQ(get_broadcast_path(0), BroadcastPath::PARALLEL_FOR_EACH);
    EXPECT_EQ(get_broadcast_path(1), BroadcastPath::PARALLEL_FOR_EACH);
    EXPECT_EQ(get_broadcast_path(5), BroadcastPath::PARALLEL_FOR_EACH);
    EXPECT_EQ(get_broadcast_path(10), BroadcastPath::PARALLEL_FOR_EACH);
}

TEST_F(CryptoOffloadTest, LargePeerCountUsesBatchThread) {
    // > 10 peers should use batched thread execution
    EXPECT_EQ(get_broadcast_path(11), BroadcastPath::BATCH_THREAD);
    EXPECT_EQ(get_broadcast_path(50), BroadcastPath::BATCH_THREAD);
    EXPECT_EQ(get_broadcast_path(100), BroadcastPath::BATCH_THREAD);
}

TEST_F(CryptoOffloadTest, BatchThresholdScalesWithPeerCount) {
    // Batch stall threshold should be 100μs per peer
    EXPECT_EQ(calculate_batch_threshold(1), 100u);
    EXPECT_EQ(calculate_batch_threshold(10), 1000u);
    EXPECT_EQ(calculate_batch_threshold(50), 5000u);
    EXPECT_EQ(calculate_batch_threshold(100), 10000u);
}

TEST_F(CryptoOffloadTest, TypicalHeartbeatPacketIsInline) {
    // Heartbeat packet is 2 bytes - should always be inline
    constexpr size_t HEARTBEAT_SIZE = 2;
    EXPECT_EQ(get_encrypt_path(HEARTBEAT_SIZE), EncryptPath::INLINE);
}

TEST_F(CryptoOffloadTest, TypicalRouteAnnouncementPathSelection) {
    // Test typical route announcement sizes

    // Empty announcement: just header (12 bytes)
    EXPECT_EQ(get_encrypt_path(RouteAnnouncementPacket::HEADER_SIZE), EncryptPath::INLINE);

    // Small announcement: 10 tokens = 12 + 40 = 52 bytes
    constexpr size_t SMALL_ANNOUNCEMENT = RouteAnnouncementPacket::HEADER_SIZE + 10 * sizeof(TokenId);
    EXPECT_EQ(get_encrypt_path(SMALL_ANNOUNCEMENT), EncryptPath::INLINE);

    // Large announcement: 256 tokens = 12 + 1024 = 1036 bytes
    constexpr size_t LARGE_ANNOUNCEMENT = RouteAnnouncementPacket::MAX_PACKET_SIZE;
    EXPECT_EQ(get_encrypt_path(LARGE_ANNOUNCEMENT), EncryptPath::OFFLOADED);
}

TEST_F(CryptoOffloadTest, MaxPacketSizeExceedsThreshold) {
    // Verify that MAX_PACKET_SIZE (1036 bytes) exceeds the offload threshold (1024)
    // This ensures large route announcements are always offloaded
    EXPECT_GT(RouteAnnouncementPacket::MAX_PACKET_SIZE, CRYPTO_OFFLOAD_BYTES_THRESHOLD);
}

// =============================================================================
// Stall Timing Simulation Tests
// =============================================================================

class StallTimingTest : public ::testing::Test {
protected:
    static constexpr uint64_t STALL_WARNING_US = 100;

    bool would_trigger_stall_warning(uint64_t elapsed_us) const {
        return elapsed_us > STALL_WARNING_US;
    }
};

TEST_F(StallTimingTest, FastOperationNoWarning) {
    // Operations under 100μs should not trigger warnings
    EXPECT_FALSE(would_trigger_stall_warning(0));
    EXPECT_FALSE(would_trigger_stall_warning(50));
    EXPECT_FALSE(would_trigger_stall_warning(99));
    EXPECT_FALSE(would_trigger_stall_warning(100));  // Exactly at threshold = no warning
}

TEST_F(StallTimingTest, SlowOperationTriggersWarning) {
    // Operations over 100μs should trigger warnings
    EXPECT_TRUE(would_trigger_stall_warning(101));
    EXPECT_TRUE(would_trigger_stall_warning(200));
    EXPECT_TRUE(would_trigger_stall_warning(1000));
    EXPECT_TRUE(would_trigger_stall_warning(10000));
}

TEST_F(StallTimingTest, TypicalCryptoOperationTiming) {
    // Typical AES-GCM encryption times for various packet sizes:
    // - 64 bytes: ~5-10μs (well under threshold)
    // - 1KB: ~20-40μs (under threshold)
    // - 4KB: ~80-120μs (borderline)
    // - 16KB: ~300-500μs (would trigger warning)

    // These tests document expected behavior for typical hardware
    EXPECT_FALSE(would_trigger_stall_warning(10));   // 64 bytes
    EXPECT_FALSE(would_trigger_stall_warning(40));   // 1KB
    EXPECT_TRUE(would_trigger_stall_warning(120));   // 4KB on slow hardware
    EXPECT_TRUE(would_trigger_stall_warning(400));   // 16KB
}

// =============================================================================
// Packet Size to Token Count Mapping Tests
// =============================================================================

class PacketSizeTest : public ::testing::Test {
protected:
    static constexpr size_t HEADER_SIZE = 12;
    static constexpr size_t TOKEN_SIZE = sizeof(TokenId);  // 4 bytes
    static constexpr size_t OFFLOAD_THRESHOLD = 1024;

    size_t calculate_packet_size(size_t token_count) const {
        return HEADER_SIZE + token_count * TOKEN_SIZE;
    }

    size_t max_inline_tokens() const {
        // Find max tokens that still result in inline encryption
        // packet_size <= 1024
        // HEADER_SIZE + token_count * 4 <= 1024
        // token_count <= (1024 - 12) / 4 = 253
        return (OFFLOAD_THRESHOLD - HEADER_SIZE) / TOKEN_SIZE;
    }
};

TEST_F(PacketSizeTest, MaxInlineTokenCount) {
    // Calculate maximum tokens that can be encrypted inline
    size_t max_tokens = max_inline_tokens();
    EXPECT_EQ(max_tokens, 253u);

    // Verify this packet size is at or below threshold
    EXPECT_LE(calculate_packet_size(max_tokens), OFFLOAD_THRESHOLD);

    // Verify one more token would exceed threshold
    EXPECT_GT(calculate_packet_size(max_tokens + 1), OFFLOAD_THRESHOLD);
}

TEST_F(PacketSizeTest, TokenCountToPacketSize) {
    EXPECT_EQ(calculate_packet_size(0), 12u);
    EXPECT_EQ(calculate_packet_size(1), 16u);
    EXPECT_EQ(calculate_packet_size(10), 52u);
    EXPECT_EQ(calculate_packet_size(100), 412u);
    EXPECT_EQ(calculate_packet_size(253), 1024u);  // Max inline
    EXPECT_EQ(calculate_packet_size(254), 1028u);  // First offloaded
    EXPECT_EQ(calculate_packet_size(256), 1036u);  // MAX_PACKET_SIZE
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
