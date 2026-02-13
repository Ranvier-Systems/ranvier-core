// Ranvier Core - Google Mock for UdpChannel
//
// Abstract UdpChannel interface and GMock mock for dependency isolation.
//
// Production code uses GossipTransport (src/gossip_transport.hpp), which is
// a concrete class tightly coupled to Seastar networking (seastar::net::udp_channel,
// DTLS encryption). This interface extracts the UDP send/receive contract so
// that gossip protocol logic can be tested without real sockets or DTLS.
//
// Future refactoring: GossipTransport could implement this interface to enable
// full dependency injection in production code.
//
// Usage:
//   #include "mocks/mock_udp_channel.hpp"
//   MockUdpChannel channel;
//   EXPECT_CALL(channel, send("10.0.0.1", 7946, _))
//       .WillOnce(Return(true));

#pragma once

#include <gmock/gmock.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ranvier {

// Abstract interface for UDP communication.
// Decouples gossip protocol logic from Seastar networking and DTLS.
class UdpChannel {
public:
    virtual ~UdpChannel() = default;

    // Send a datagram to the specified peer.
    // Returns true if the send succeeded.
    virtual bool send(const std::string& peer_host, uint16_t peer_port,
                      const std::vector<uint8_t>& data) = 0;

    // Broadcast a datagram to multiple peers.
    // Returns the number of peers successfully sent to.
    virtual size_t broadcast(const std::vector<std::pair<std::string, uint16_t>>& peers,
                             const std::vector<uint8_t>& data) = 0;

    // Receive a datagram. Returns nullopt if no data is available.
    virtual std::optional<std::vector<uint8_t>> receive() = 0;

    // Check if the channel is open and ready for communication.
    virtual bool is_ready() const = 0;
};

class MockUdpChannel : public UdpChannel {
public:
    MOCK_METHOD(bool, send,
                (const std::string& peer_host, uint16_t peer_port,
                 const std::vector<uint8_t>& data),
                (override));
    MOCK_METHOD(size_t, broadcast,
                (const std::vector<std::pair<std::string, uint16_t>>& peers,
                 const std::vector<uint8_t>& data),
                (override));
    MOCK_METHOD(std::optional<std::vector<uint8_t>>, receive, (), (override));
    MOCK_METHOD(bool, is_ready, (), (const, override));
};

}  // namespace ranvier
