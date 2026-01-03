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
#include <deque>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include <seastar/core/future.hh>
#include <seastar/core/timer.hh>
#include <seastar/net/api.hh>
#include <seastar/net/dns.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/net/udp.hh>

namespace ranvier {

// Gossip logger
inline seastar::logger log_gossip("ranvier.gossip");

// Packet types for gossip protocol
enum class GossipPacketType : uint8_t {
    ROUTE_ANNOUNCEMENT = 0x01,  // New route learned
    HEARTBEAT = 0x02,           // Keep-alive
    ROUTE_ACK = 0x03,           // Acknowledgment for route announcement
};

// Wire format for route announcements (v2 with sequence numbers)
// Fixed header + variable-length token array
// Format: [type:1][version:1][seq_num:4][backend_id:4][token_count:2][tokens:4*N]
struct RouteAnnouncementPacket {
    static constexpr uint8_t PROTOCOL_VERSION = 2;  // Bumped for seq_num support
    static constexpr size_t HEADER_SIZE = 12;  // type + version + seq_num + backend_id + token_count
    static constexpr size_t MAX_TOKENS = 256; // Max tokens per announcement
    static constexpr size_t MAX_PACKET_SIZE = HEADER_SIZE + (MAX_TOKENS * sizeof(TokenId));

    GossipPacketType type = GossipPacketType::ROUTE_ANNOUNCEMENT;
    uint8_t version = PROTOCOL_VERSION;
    uint32_t seq_num = 0;  // Sequence number for reliable delivery
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

        // Token count (big-endian)
        pkt.token_count = (static_cast<uint16_t>(data[10]) << 8) | static_cast<uint16_t>(data[11]);

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

// Wire format for route acknowledgments
// Format: [type:1][version:1][seq_num:4]
struct RouteAckPacket {
    static constexpr uint8_t PROTOCOL_VERSION = 2;
    static constexpr size_t PACKET_SIZE = 6;  // type + version + seq_num

    GossipPacketType type = GossipPacketType::ROUTE_ACK;
    uint8_t version = PROTOCOL_VERSION;
    uint32_t seq_num = 0;  // Sequence number being acknowledged

    // Serialize to bytes for UDP transmission
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

    // Deserialize from bytes received via UDP
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

    // Callback type for pruning routes
    using RoutePruneCallback = std::function<seastar::future<>(BackendId)>;

    void set_route_prune_callback(RoutePruneCallback callback) {
        _route_prune_callback = std::move(callback);
    }

private:
    ClusterConfig _config;
    RouteLearnCallback _route_learn_callback;

    // UDP channel for gossip
    std::optional<seastar::net::udp_channel> _channel;

    // Parsed peer addresses
    std::vector<seastar::socket_address> _peer_addresses;

    // Running flag
    bool _running = false;

    // Metrics
    uint64_t _packets_sent = 0;
    uint64_t _packets_received = 0;
    uint64_t _packets_invalid = 0;
    uint64_t _packets_untrusted = 0;
    uint64_t _stats_cluster_peers_alive = 0;
    uint64_t _dns_discovery_success = 0;
    uint64_t _dns_discovery_failure = 0;

    // Reliable delivery metrics
    uint64_t _acks_sent = 0;
    uint64_t _acks_received = 0;
    uint64_t _retries_sent = 0;
    uint64_t _duplicates_suppressed = 0;
    uint64_t _max_retries_exceeded = 0;

    // Seastar metrics registration
    seastar::metrics::metric_groups _metrics;

    seastar::future<> _receive_loop_future;

    // Timer to trigger periodic heartbeats
    seastar::timer<> _heartbeat_timer;

    struct PeerState {
        seastar::lowres_clock::time_point last_seen;
        bool is_alive = true;
        std::optional<BackendId> associated_backend; // Track which backend this peer represents
    };

    RoutePruneCallback _route_prune_callback;

    // Reliable delivery: pending ACK tracking
    struct PendingAck {
        uint32_t seq_num;
        std::vector<uint8_t> serialized_packet;  // Original packet for retransmission
        seastar::lowres_clock::time_point next_retry;
        uint32_t retry_count = 0;
    };

    // Per-peer outbound sequence counter
    std::unordered_map<seastar::socket_address, uint32_t> _peer_seq_counters;

    // Per-peer pending ACKs (keyed by seq_num for quick lookup)
    std::unordered_map<seastar::socket_address, std::unordered_map<uint32_t, PendingAck>> _pending_acks;

    // Per-peer received sequence window for duplicate detection (sliding window)
    // Uses a deque to maintain insertion order for window sliding
    std::unordered_map<seastar::socket_address, std::deque<uint32_t>> _received_seq_windows;
    std::unordered_map<seastar::socket_address, std::unordered_set<uint32_t>> _received_seq_sets;  // For O(1) lookup

    // Timer for processing retries
    seastar::timer<> _retry_timer;

    // Shard-local table (only populated on Shard 0)
    std::unordered_map<seastar::socket_address, PeerState> _peer_table;

    seastar::timer<> _liveness_timer;

    // DNS-based peer discovery members (shard 0 only)
    seastar::net::dns_resolver _dns_resolver;
    seastar::timer<> _discovery_timer;
    bool _discovery_enabled = false;
    seastar::future<> _discovery_future;

    void update_peer_liveness(const seastar::socket_address& addr);
    void check_liveness();

    // Receive loop (runs continuously while service is active)
    seastar::future<> receive_loop();

    // Handle a received packet
    seastar::future<> handle_packet(seastar::net::udp_datagram&& dgram);

    // Internal method to send a heartbeat to all peers
    seastar::future<> broadcast_heartbeat();

    // DNS-based peer discovery: refresh peer list from DNS
    // Only runs on shard 0, broadcasts updates to other shards
    seastar::future<> refresh_peers();

    // Parse peer address string "IP:Port" to socket_address
    static std::optional<seastar::socket_address> parse_peer_address(const std::string& peer);

    // Reliable delivery methods
    // Send an ACK for a received packet
    seastar::future<> send_ack(const seastar::socket_address& peer, uint32_t seq_num);

    // Handle a received ACK
    void handle_ack(const seastar::socket_address& peer, uint32_t seq_num);

    // Check if a sequence number is a duplicate (and update the window)
    bool is_duplicate(const seastar::socket_address& peer, uint32_t seq_num);

    // Process pending retries (called by timer)
    void process_retries();

    // Get next sequence number for a peer
    uint32_t next_seq_num(const seastar::socket_address& peer);

    // Calculate backoff delay for retry
    std::chrono::milliseconds calculate_backoff(uint32_t retry_count) const;
};

}  // namespace ranvier
