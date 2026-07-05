// Ranvier Core - Gossip Extension Packet Unit Tests
//
// Tests for ExtensionPacket wire format (opaque downstream payload channel).
// These tests don't require Seastar runtime — the packet struct is replicated
// here (mirroring the production definition in gossip_protocol.hpp) so the wire
// format can be exercised without pulling in Seastar headers, exactly like
// gossip_cache_state_test.cpp.
//
// The no-handler-drop / dispatch path lives in handle_packet (needs a reactor)
// and verifies in the dev-container integration build.

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

// =============================================================================
// Mirror of GossipPacketType + ExtensionPacket (no Seastar dependency)
// Wire format: [type:1][version:1][payload_len:u16][payload bytes], HEADER = 4
// =============================================================================

enum class GossipPacketType : uint8_t {
    ROUTE_ANNOUNCEMENT = 0x01,
    CACHE_STATE = 0x06,
    HOT_PREFIX_DIGEST = 0x07,
    EXTENSION = 0x08,
};

struct ExtensionPacket {
    static constexpr uint8_t PROTOCOL_VERSION = 1;
    static constexpr size_t HEADER_SIZE = 4;
    static constexpr size_t MAX_PAYLOAD = 1024;
    static constexpr size_t MAX_PACKET_SIZE = HEADER_SIZE + MAX_PAYLOAD;

    GossipPacketType type = GossipPacketType::EXTENSION;
    uint8_t version = PROTOCOL_VERSION;
    std::vector<uint8_t> payload;

    std::vector<uint8_t> serialize() const {
        uint16_t payload_len = static_cast<uint16_t>(
            std::min(payload.size(), static_cast<size_t>(MAX_PAYLOAD)));
        std::vector<uint8_t> buffer;
        buffer.reserve(HEADER_SIZE + payload_len);
        buffer.push_back(static_cast<uint8_t>(type));
        buffer.push_back(version);
        buffer.push_back((payload_len >> 8) & 0xFF);  // big-endian u16
        buffer.push_back(payload_len & 0xFF);
        buffer.insert(buffer.end(), payload.begin(), payload.begin() + payload_len);
        return buffer;
    }

    static std::optional<ExtensionPacket> deserialize(const uint8_t* data, size_t len) {
        if (len < HEADER_SIZE) {
            return std::nullopt;
        }
        ExtensionPacket pkt;
        pkt.type = static_cast<GossipPacketType>(data[0]);
        pkt.version = data[1];
        if (pkt.type != GossipPacketType::EXTENSION) {
            return std::nullopt;
        }
        uint16_t payload_len = (static_cast<uint16_t>(data[2]) << 8) | data[3];
        if (payload_len > MAX_PAYLOAD) {
            return std::nullopt;
        }
        if (len < HEADER_SIZE + static_cast<size_t>(payload_len)) {
            return std::nullopt;
        }
        pkt.payload.assign(data + HEADER_SIZE, data + HEADER_SIZE + payload_len);
        return pkt;
    }
};

class GossipExtensionTest : public ::testing::Test {};

// ---- Wire format constants -------------------------------------------------

TEST_F(GossipExtensionTest, PacketTypeValue) {
    EXPECT_EQ(static_cast<uint8_t>(GossipPacketType::EXTENSION), 0x08);
}

TEST_F(GossipExtensionTest, HeaderSizeAndBound) {
    EXPECT_EQ(ExtensionPacket::HEADER_SIZE, 4u);
    EXPECT_EQ(ExtensionPacket::MAX_PAYLOAD, 1024u);
    // MTU discipline: total stays well under the ~1400B budget before DTLS.
    EXPECT_LT(ExtensionPacket::MAX_PACKET_SIZE, 1400u);
}

// ---- Serialization ---------------------------------------------------------

TEST_F(GossipExtensionTest, SerializeEmptyPayload) {
    ExtensionPacket pkt;
    auto data = pkt.serialize();
    ASSERT_EQ(data.size(), ExtensionPacket::HEADER_SIZE);
    EXPECT_EQ(data[0], 0x08);           // type
    EXPECT_EQ(data[1], 0x01);           // version
    EXPECT_EQ(data[2], 0x00);           // payload_len high
    EXPECT_EQ(data[3], 0x00);           // payload_len low
}

TEST_F(GossipExtensionTest, SerializeLengthBigEndian) {
    ExtensionPacket pkt;
    pkt.payload = std::vector<uint8_t>(258, 0xAB);  // 0x0102
    auto data = pkt.serialize();
    ASSERT_EQ(data.size(), ExtensionPacket::HEADER_SIZE + 258u);
    EXPECT_EQ(data[2], 0x01);
    EXPECT_EQ(data[3], 0x02);
}

// ---- Round trip ------------------------------------------------------------

TEST_F(GossipExtensionTest, RoundTripPayload) {
    ExtensionPacket original;
    original.payload = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x7F};
    auto data = original.serialize();
    auto result = ExtensionPacket::deserialize(data.data(), data.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->version, ExtensionPacket::PROTOCOL_VERSION);
    EXPECT_EQ(result->payload, original.payload);
}

TEST_F(GossipExtensionTest, RoundTripEmptyPayload) {
    ExtensionPacket original;
    auto data = original.serialize();
    auto result = ExtensionPacket::deserialize(data.data(), data.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->payload.empty());
}

TEST_F(GossipExtensionTest, RoundTripMaxPayload) {
    ExtensionPacket original;
    original.payload = std::vector<uint8_t>(ExtensionPacket::MAX_PAYLOAD, 0x5A);
    auto data = original.serialize();
    auto result = ExtensionPacket::deserialize(data.data(), data.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload.size(), ExtensionPacket::MAX_PAYLOAD);
}

// ---- Forward compatibility: version recorded, trailing bytes tolerated -----

TEST_F(GossipExtensionTest, DeserializeRecordsVersionNeverRejects) {
    ExtensionPacket original;
    original.payload = {0x01, 0x02};
    auto data = original.serialize();
    data[1] = 0x09;  // a future downstream payload-schema version
    auto result = ExtensionPacket::deserialize(data.data(), data.size());
    ASSERT_TRUE(result.has_value()) << "version must be recorded, not a rejection boundary";
    EXPECT_EQ(result->version, 0x09);
    EXPECT_EQ(result->payload, original.payload);
}

TEST_F(GossipExtensionTest, DeserializeIgnoresTrailingBytes) {
    ExtensionPacket original;
    original.payload = {0xAA, 0xBB};
    auto data = original.serialize();
    // A newer node appends bytes past the declared payload.
    data.push_back(0xCC);
    data.push_back(0xDD);
    auto result = ExtensionPacket::deserialize(data.data(), data.size());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->payload, original.payload) << "declared payload only; tail ignored";
}

// ---- Error cases -----------------------------------------------------------

TEST_F(GossipExtensionTest, DeserializeFailsTooShortHeader) {
    uint8_t buf[3] = {0x08, 0x01, 0x00};
    auto result = ExtensionPacket::deserialize(buf, 3);
    EXPECT_FALSE(result.has_value());
}

TEST_F(GossipExtensionTest, DeserializeFailsTruncatedPayload) {
    // Header declares 4 bytes of payload but only 1 is present.
    uint8_t buf[5] = {0x08, 0x01, 0x00, 0x04, 0xFF};
    auto result = ExtensionPacket::deserialize(buf, 5);
    EXPECT_FALSE(result.has_value());
}

TEST_F(GossipExtensionTest, DeserializeFailsOverCapLength) {
    // payload_len = MAX_PAYLOAD + 1 must be rejected before allocating (Rule #4),
    // even if the buffer claims to carry that many bytes.
    const size_t over = ExtensionPacket::MAX_PAYLOAD + 1;
    std::vector<uint8_t> buf(ExtensionPacket::HEADER_SIZE + over, 0x00);
    buf[0] = 0x08;
    buf[1] = 0x01;
    buf[2] = (over >> 8) & 0xFF;
    buf[3] = over & 0xFF;
    auto result = ExtensionPacket::deserialize(buf.data(), buf.size());
    EXPECT_FALSE(result.has_value());
}

TEST_F(GossipExtensionTest, DeserializeFailsWrongType) {
    ExtensionPacket pkt;
    pkt.payload = {0x01};
    auto data = pkt.serialize();
    data[0] = static_cast<uint8_t>(GossipPacketType::CACHE_STATE);
    auto result = ExtensionPacket::deserialize(data.data(), data.size());
    EXPECT_FALSE(result.has_value());
}

TEST_F(GossipExtensionTest, DeserializeFailsEmptyBuffer) {
    auto result = ExtensionPacket::deserialize(nullptr, 0);
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// Broadcaster slot semantics (faked)
//
// Mirrors the process-wide send-side slot in gossip_service.hpp
// (gossip_extension_broadcaster_slot / set / get). That header can't be included
// reactor-free — it drags in Seastar's dns / socket / metrics headers — so the
// function-local-static slot mechanism is replicated here to lock its semantics:
// default empty (not running), set-at-start makes get non-empty (running), and
// clear-on-stop returns get to empty. Production's std::function returns
// seastar::future<>; the fake returns bool to stay Seastar-free — only the
// emptiness transitions are under test. The real start/stop wiring (capturing
// the GossipService and calling broadcast_extension) verifies in the dev
// container.
// =============================================================================

namespace {

using FakeBroadcaster = std::function<bool(std::vector<uint8_t>)>;

FakeBroadcaster& fake_broadcaster_slot() {
    static FakeBroadcaster slot;  // empty until "start" writes it
    return slot;
}
void fake_set_broadcaster(FakeBroadcaster b) { fake_broadcaster_slot() = std::move(b); }
FakeBroadcaster fake_get_broadcaster() { return fake_broadcaster_slot(); }

}  // namespace

TEST_F(GossipExtensionTest, BroadcasterSlotDefaultsToEmpty) {
    fake_set_broadcaster(nullptr);  // clean slate
    EXPECT_FALSE(static_cast<bool>(fake_get_broadcaster()))
        << "unset slot must read empty — the not-running signal";
}

TEST_F(GossipExtensionTest, BroadcasterSlotSetAtStartIsInvocable) {
    bool called = false;
    fake_set_broadcaster([&called](std::vector<uint8_t>) {
        called = true;
        return true;
    });

    auto b = fake_get_broadcaster();
    ASSERT_TRUE(static_cast<bool>(b)) << "after start the slot must read non-empty";
    EXPECT_TRUE(b({0x01, 0x02}));
    EXPECT_TRUE(called);

    fake_set_broadcaster(nullptr);  // reset for other tests
}

TEST_F(GossipExtensionTest, BroadcasterSlotClearedOnStopReadsEmpty) {
    fake_set_broadcaster([](std::vector<uint8_t>) { return true; });
    ASSERT_TRUE(static_cast<bool>(fake_get_broadcaster()));

    fake_set_broadcaster(nullptr);  // stop() clears the slot
    EXPECT_FALSE(static_cast<bool>(fake_get_broadcaster()))
        << "after stop the slot must read empty again — back to not-running";
}
