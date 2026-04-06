// Ranvier Core - Gossip Protocol Module
//
// Manages message handling, reliable delivery, and DNS discovery for gossip.
// Extracted from GossipService for better modularity and testability.
//
// Responsibilities:
// - Packet serialization/deserialization
// - Reliable delivery with ACKs and retries
// - Duplicate suppression (replay attack prevention)
// - DNS-based peer discovery
// - Heartbeat broadcasting

#pragma once

#include "config.hpp"
#include "gossip_consensus.hpp"
#include "gossip_transport.hpp"
#include "logging.hpp"
#include "types.hpp"

#include <chrono>
#include <deque>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/timer.hh>
#include <seastar/net/dns.hh>
#include <seastar/net/socket_defs.hh>

namespace ranvier {

// Forward declaration for logger
inline seastar::logger& log_gossip_protocol() {
    static seastar::logger logger("ranvier.gossip.protocol");
    return logger;
}

// Packet types for gossip protocol
enum class GossipPacketType : uint8_t {
    ROUTE_ANNOUNCEMENT = 0x01,
    HEARTBEAT = 0x02,
    ROUTE_ACK = 0x03,
    NODE_STATE = 0x04,
    CACHE_EVICTION = 0x05,
};

// Node state values
enum class NodeState : uint8_t {
    ACTIVE = 0x00,
    DRAINING = 0x01,
};

// Callback types
using RouteLearnCallback = std::function<seastar::future<>(std::vector<TokenId>, BackendId)>;
using NodeStateCallback = std::function<seastar::future<>(BackendId, NodeState)>;
using CacheEvictionCallback = std::function<seastar::future<>(uint64_t prefix_hash, BackendId backend_id)>;

// Wire format for route announcements (v2 with sequence numbers)
struct RouteAnnouncementPacket {
    static constexpr uint8_t PROTOCOL_VERSION = 2;
    static constexpr size_t HEADER_SIZE = 12;
    static constexpr size_t MAX_TOKENS = 256;
    static constexpr size_t MAX_PACKET_SIZE = HEADER_SIZE + (MAX_TOKENS * sizeof(TokenId));

    GossipPacketType type = GossipPacketType::ROUTE_ANNOUNCEMENT;
    uint8_t version = PROTOCOL_VERSION;
    uint32_t seq_num = 0;
    BackendId backend_id = 0;
    uint16_t token_count = 0;
    std::vector<TokenId> tokens;

    std::vector<uint8_t> serialize() const;
    static std::optional<RouteAnnouncementPacket> deserialize(const uint8_t* data, size_t len);
};

// Wire format for route acknowledgments
struct RouteAckPacket {
    static constexpr uint8_t PROTOCOL_VERSION = 2;
    static constexpr size_t PACKET_SIZE = 6;

    GossipPacketType type = GossipPacketType::ROUTE_ACK;
    uint8_t version = PROTOCOL_VERSION;
    uint32_t seq_num = 0;

    std::vector<uint8_t> serialize() const;
    static std::optional<RouteAckPacket> deserialize(const uint8_t* data, size_t len);
};

// Wire format for node state notifications
struct NodeStatePacket {
    static constexpr uint8_t PROTOCOL_VERSION = 2;
    static constexpr size_t PACKET_SIZE = 7;

    GossipPacketType type = GossipPacketType::NODE_STATE;
    uint8_t version = PROTOCOL_VERSION;
    NodeState state = NodeState::ACTIVE;
    BackendId backend_id = 0;

    std::vector<uint8_t> serialize() const;
    static std::optional<NodeStatePacket> deserialize(const uint8_t* data, size_t len);
};

// Wire format for cache eviction notifications (Phase 2: cluster propagation)
// Format: [type:1][version:1][seq_num:4][backend_id:4][prefix_hash:8] = 18 bytes
struct CacheEvictionPacket {
    static constexpr uint8_t PROTOCOL_VERSION = 2;
    static constexpr size_t PACKET_SIZE = 18;

    GossipPacketType type = GossipPacketType::CACHE_EVICTION;
    uint8_t version = PROTOCOL_VERSION;
    uint32_t seq_num = 0;
    BackendId backend_id = 0;
    uint64_t prefix_hash = 0;

    std::vector<uint8_t> serialize() const;
    static std::optional<CacheEvictionPacket> deserialize(const uint8_t* data, size_t len);
};

// GossipProtocol: Manages message handling and reliable delivery
//
// This class handles the protocol logic:
// - Packet parsing and dispatch
// - ACKs, retries, and duplicate suppression
// - DNS-based peer discovery
// - Heartbeat generation
//
// Threading: This class is shard-local. Only shard 0 handles the actual
// protocol logic. Other shards can call broadcast methods safely.
class GossipProtocol {
public:
    explicit GossipProtocol(const ClusterConfig& config);
    ~GossipProtocol() = default;

    // Non-copyable, non-movable
    GossipProtocol(const GossipProtocol&) = delete;
    GossipProtocol& operator=(const GossipProtocol&) = delete;

    // Lifecycle
    seastar::future<> start(GossipTransport* transport, GossipConsensus* consensus,
                            std::vector<seastar::socket_address>* peer_addresses);
    seastar::future<> stop();

    // Callbacks
    void set_route_learn_callback(RouteLearnCallback callback) {
        _route_learn_callback = std::move(callback);
    }
    void set_node_state_callback(NodeStateCallback callback) {
        _node_state_callback = std::move(callback);
    }
    void set_cache_eviction_callback(CacheEvictionCallback callback) {
        _cache_eviction_callback = std::move(callback);
    }

    // Broadcast methods
    seastar::future<> broadcast_route(const std::vector<TokenId>& tokens, BackendId backend);
    seastar::future<> broadcast_node_state(NodeState state, BackendId local_backend_id);
    seastar::future<> broadcast_cache_eviction(uint64_t prefix_hash, BackendId backend_id);
    seastar::future<> broadcast_heartbeat();

    // Handle incoming packet (called from receive loop)
    seastar::future<> handle_packet(seastar::net::udp_datagram&& dgram);

    // DNS discovery
    seastar::future<> refresh_peers();

    // Metrics accessors
    uint64_t packets_sent() const { return _packets_sent; }
    uint64_t packets_received() const { return _packets_received; }
    uint64_t packets_invalid() const { return _packets_invalid; }
    uint64_t packets_untrusted() const { return _packets_untrusted; }
    uint64_t dns_discovery_success() const { return _dns_discovery_success; }
    uint64_t dns_discovery_failure() const { return _dns_discovery_failure; }
    uint64_t acks_sent() const { return _acks_sent; }
    uint64_t acks_received() const { return _acks_received; }
    uint64_t retries_sent() const { return _retries_sent; }
    uint64_t duplicates_suppressed() const { return _duplicates_suppressed; }
    uint64_t max_retries_exceeded() const { return _max_retries_exceeded; }
    uint64_t dedup_peers_overflow() const { return _dedup_peers_overflow; }
    uint64_t pending_acks_overflow() const { return _pending_acks_overflow; }
    uint64_t pending_acks_count() const { return _stats_pending_acks_count; }
    uint64_t node_state_sent() const { return _node_state_sent; }
    uint64_t node_state_received() const { return _node_state_received; }
    uint64_t cache_evictions_sent() const { return _cache_evictions_sent; }
    uint64_t cache_evictions_received() const { return _cache_evictions_received; }

    // Clear reliable delivery state (used during resync/shutdown)
    void clear_pending_acks();

    // Parse peer address string to socket_address
    static std::optional<seastar::socket_address> parse_peer_address(const std::string& peer);

private:
    const ClusterConfig& _config;
    bool _running = false;

    // References to other modules (set during start)
    GossipTransport* _transport = nullptr;
    GossipConsensus* _consensus = nullptr;
    std::vector<seastar::socket_address>* _peer_addresses = nullptr;
    std::unordered_set<seastar::socket_address> _peer_address_set;  // O(1) lookup for handle_packet()

    // Callbacks
    RouteLearnCallback _route_learn_callback;
    NodeStateCallback _node_state_callback;
    CacheEvictionCallback _cache_eviction_callback;

    // Timers
    seastar::timer<> _heartbeat_timer;
    seastar::timer<> _retry_timer;
    seastar::timer<> _discovery_timer;

    // DNS resolver
    seastar::net::dns_resolver _dns_resolver;
    bool _discovery_enabled = false;
    seastar::future<> _discovery_future;

    // Reliable delivery state
    struct PendingAck {
        uint32_t seq_num;
        std::vector<uint8_t> serialized_packet;
        seastar::lowres_clock::time_point next_retry;
        uint32_t retry_count = 0;
    };

    std::unordered_map<seastar::socket_address, uint32_t> _peer_seq_counters;
    std::unordered_map<seastar::socket_address, std::unordered_map<uint32_t, PendingAck>> _pending_acks;
    std::unordered_map<seastar::socket_address, std::deque<uint32_t>> _received_seq_windows;
    std::unordered_map<seastar::socket_address, std::unordered_set<uint32_t>> _received_seq_sets;

    // Deduplication limits (Rule #4: bounded containers)
    static constexpr size_t MAX_DEDUP_PEERS = 10000;
    static constexpr size_t MAX_PENDING_ACKS = 1000;

    // Metrics
    uint64_t _packets_sent = 0;
    uint64_t _packets_received = 0;
    uint64_t _packets_invalid = 0;
    uint64_t _packets_untrusted = 0;
    uint64_t _dns_discovery_success = 0;
    uint64_t _dns_discovery_failure = 0;
    uint64_t _acks_sent = 0;
    uint64_t _acks_received = 0;
    uint64_t _retries_sent = 0;
    uint64_t _duplicates_suppressed = 0;
    uint64_t _max_retries_exceeded = 0;
    uint64_t _dedup_peers_overflow = 0;
    uint64_t _pending_acks_overflow = 0;
    uint64_t _stats_pending_acks_count = 0;
    uint64_t _node_state_sent = 0;
    uint64_t _node_state_received = 0;
    uint64_t _cache_evictions_sent = 0;
    uint64_t _cache_evictions_received = 0;

    // Internal methods
    seastar::future<> send_ack(const seastar::socket_address& peer, uint32_t seq_num);
    void handle_ack(const seastar::socket_address& peer, uint32_t seq_num);
    bool is_duplicate(const seastar::socket_address& peer, uint32_t seq_num);
    seastar::future<> process_retries();
    uint32_t next_seq_num(const seastar::socket_address& peer);
    std::chrono::milliseconds calculate_backoff(uint32_t retry_count) const;

    // Cleanup state for a removed peer
    void cleanup_peer_state(const seastar::socket_address& peer);
};

}  // namespace ranvier
