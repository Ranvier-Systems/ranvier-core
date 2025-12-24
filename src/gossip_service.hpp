// Ranvier Core - Gossip Service for Distributed State Synchronization
//
// Implements a simple gossip protocol for cluster state sync:
// - UDP-based route announcements between cluster nodes
// - Stateless packet format for route propagation
// - Thread-local service following Seastar's shared-nothing model

#pragma once

#include "config.hpp"
#include "types.hpp"
#include "logging.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <seastar/core/future.hh>
#include <seastar/core/timer.hh>
#include <seastar/net/api.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/net/udp.hh>

namespace ranvier {

// Gossip logger
inline seastar::logger log_gossip("ranvier.gossip");

// Packet types for gossip protocol
enum class GossipPacketType : uint8_t {
    ROUTE_ANNOUNCEMENT = 0x01,  // New route learned
    HEARTBEAT = 0x02,           // Keep-alive (future use)
};

// Wire format for route announcements
// Fixed header + variable-length token array
// Format: [type:1][version:1][backend_id:4][token_count:2][tokens:4*N]
struct RouteAnnouncementPacket {
    static constexpr uint8_t PROTOCOL_VERSION = 1;
    static constexpr size_t HEADER_SIZE = 8;  // type + version + backend_id + token_count
    static constexpr size_t MAX_TOKENS = 256; // Max tokens per announcement
    static constexpr size_t MAX_PACKET_SIZE = HEADER_SIZE + (MAX_TOKENS * sizeof(TokenId));

    GossipPacketType type = GossipPacketType::ROUTE_ANNOUNCEMENT;
    uint8_t version = PROTOCOL_VERSION;
    BackendId backend_id = 0;
    uint16_t token_count = 0;
    std::vector<TokenId> tokens;

    // Serialize to bytes for UDP transmission
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buffer;
        buffer.reserve(HEADER_SIZE + tokens.size() * sizeof(TokenId));

        // Header
        buffer.push_back(static_cast<uint8_t>(type));
        buffer.push_back(version);

        // Backend ID (big-endian)
        buffer.push_back((backend_id >> 24) & 0xFF);
        buffer.push_back((backend_id >> 16) & 0xFF);
        buffer.push_back((backend_id >> 8) & 0xFF);
        buffer.push_back(backend_id & 0xFF);

        // Token count (big-endian)
        uint16_t count = static_cast<uint16_t>(std::min(tokens.size(), static_cast<size_t>(MAX_TOKENS)));
        buffer.push_back((count >> 8) & 0xFF);
        buffer.push_back(count & 0xFF);

        // Tokens (big-endian)
        for (size_t i = 0; i < count; ++i) {
            TokenId t = tokens[i];
            buffer.push_back((t >> 24) & 0xFF);
            buffer.push_back((t >> 16) & 0xFF);
            buffer.push_back((t >> 8) & 0xFF);
            buffer.push_back(t & 0xFF);
        }

        return buffer;
    }

    // Deserialize from bytes received via UDP
    static std::optional<RouteAnnouncementPacket> deserialize(const uint8_t* data, size_t len) {
        if (len < HEADER_SIZE) {
            return std::nullopt;
        }

        RouteAnnouncementPacket pkt;
        pkt.type = static_cast<GossipPacketType>(data[0]);
        pkt.version = data[1];

        // Only handle route announcements with matching version
        if (pkt.type != GossipPacketType::ROUTE_ANNOUNCEMENT || pkt.version != PROTOCOL_VERSION) {
            return std::nullopt;
        }

        // Backend ID (big-endian)
        pkt.backend_id = (static_cast<BackendId>(data[2]) << 24) |
                         (static_cast<BackendId>(data[3]) << 16) |
                         (static_cast<BackendId>(data[4]) << 8) |
                         static_cast<BackendId>(data[5]);

        // Token count (big-endian)
        pkt.token_count = (static_cast<uint16_t>(data[6]) << 8) | static_cast<uint16_t>(data[7]);

        // Validate packet size
        size_t expected_size = HEADER_SIZE + pkt.token_count * sizeof(TokenId);
        if (len != expected_size || pkt.token_count > MAX_TOKENS) {
            return std::nullopt;
        }

        // Tokens (big-endian)
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

// Callback for handling received route announcements
// Called with (tokens, backend_id) when a route announcement is received
using RouteLearnCallback = std::function<seastar::future<>(std::vector<TokenId>, BackendId)>;

// GossipService: Thread-local UDP gossip for cluster state sync
// Runs on shard 0 only (broadcasts received routes to all shards via RouterService)
class GossipService {
public:
    explicit GossipService(const ClusterConfig& config);

    // Initialize the UDP channel (must be called after Seastar starts)
    seastar::future<> start();

    // Stop the gossip service
    seastar::future<> stop();

    // Set callback for handling received route announcements
    void set_route_learn_callback(RouteLearnCallback callback);

    // Broadcast a route announcement to all peers
    // Called by RouterService when a new route is learned locally
    seastar::future<> broadcast_route(const std::vector<TokenId>& tokens, BackendId backend);

    // Check if gossip is enabled
    bool is_enabled() const { return _config.enabled; }

private:
    ClusterConfig _config;
    RouteLearnCallback _route_learn_callback;

    // UDP channel for gossip
    std::optional<seastar::net::udp_channel> _channel;

    seastar::socket_address _my_address;

    // Parsed peer addresses
    std::vector<seastar::socket_address> _peer_addresses;

    // Running flag
    bool _running = false;

    // Metrics
    uint64_t _packets_sent = 0;
    uint64_t _packets_received = 0;
    uint64_t _packets_invalid = 0;

    // Seastar metrics registration
    seastar::metrics::metric_groups _metrics;

    seastar::future<> _receive_loop_future;

    // Receive loop (runs continuously while service is active)
    seastar::future<> receive_loop();

    // Handle a received packet
    seastar::future<> handle_packet(seastar::net::udp_datagram&& dgram);

    // Parse peer address string "IP:Port" to socket_address
    static std::optional<seastar::socket_address> parse_peer_address(const std::string& peer);
};

}  // namespace ranvier
