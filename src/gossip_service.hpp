// Ranvier Core - Gossip Service for Distributed State Synchronization
//
// Implements a simple gossip protocol for cluster state sync:
// - UDP-based route announcements between cluster nodes
// - Stateless packet format for route propagation
// - Thread-local service following Seastar's shared-nothing model
// - Optional DTLS encryption for secure cluster communication (mTLS)

#pragma once

#include "config.hpp"
#include "crypto_offloader.hpp"
#include "dtls_context.hpp"
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
#include <seastar/core/gate.hh>
#include <seastar/core/timer.hh>
#include <seastar/net/api.hh>
#include <seastar/net/dns.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/net/udp.hh>

namespace ranvier {

// Gossip logger
inline seastar::logger log_gossip("ranvier.gossip");

// Cluster quorum state for split-brain detection
// Healthy: N/2+1 peers are reachable, full cluster operations allowed
// Degraded: Lost quorum, reject new route writes but serve existing routes
enum class QuorumState : uint8_t {
    HEALTHY = 1,   // Quorum maintained, full operations
    DEGRADED = 0,  // Quorum lost, read-only mode for routes
};

// Packet types for gossip protocol
enum class GossipPacketType : uint8_t {
    ROUTE_ANNOUNCEMENT = 0x01,  // New route learned
    HEARTBEAT = 0x02,           // Keep-alive
    ROUTE_ACK = 0x03,           // Acknowledgment for route announcement
    NODE_STATE = 0x04,          // Node state change (e.g., draining)
};

// Node state values for cluster-wide notifications
enum class NodeState : uint8_t {
    ACTIVE = 0x00,    // Normal operation
    DRAINING = 0x01,  // Shutting down gracefully, stop sending traffic
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

// Wire format for node state notifications
// Format: [type:1][version:1][node_state:1][backend_id:4]
// Peers receiving DRAINING state should set weight of routes to that backend to 0
struct NodeStatePacket {
    static constexpr uint8_t PROTOCOL_VERSION = 2;
    static constexpr size_t PACKET_SIZE = 7;  // type + version + state + backend_id

    GossipPacketType type = GossipPacketType::NODE_STATE;
    uint8_t version = PROTOCOL_VERSION;
    NodeState state = NodeState::ACTIVE;
    BackendId backend_id = 0;  // The backend ID this node represents

    // Serialize to bytes for UDP transmission
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buffer;
        buffer.reserve(PACKET_SIZE);

        buffer.push_back(static_cast<uint8_t>(type));
        buffer.push_back(version);
        buffer.push_back(static_cast<uint8_t>(state));

        // Backend ID (big-endian)
        buffer.push_back((backend_id >> 24) & 0xFF);
        buffer.push_back((backend_id >> 16) & 0xFF);
        buffer.push_back((backend_id >> 8) & 0xFF);
        buffer.push_back(backend_id & 0xFF);

        return buffer;
    }

    // Deserialize from bytes received via UDP
    static std::optional<NodeStatePacket> deserialize(const uint8_t* data, size_t len) {
        if (len != PACKET_SIZE) {
            return std::nullopt;
        }

        NodeStatePacket pkt;
        pkt.type = static_cast<GossipPacketType>(data[0]);
        pkt.version = data[1];

        if (pkt.type != GossipPacketType::NODE_STATE || pkt.version != PROTOCOL_VERSION) {
            return std::nullopt;
        }

        pkt.state = static_cast<NodeState>(data[2]);

        // Backend ID (big-endian)
        pkt.backend_id = (static_cast<BackendId>(data[3]) << 24) |
                         (static_cast<BackendId>(data[4]) << 16) |
                         (static_cast<BackendId>(data[5]) << 8) |
                         static_cast<BackendId>(data[6]);

        return pkt;
    }
};

// Callback for handling received route announcements
// Called with (tokens, backend_id) when a route announcement is received
using RouteLearnCallback = std::function<seastar::future<>(std::vector<TokenId>, BackendId)>;

// Callback for handling node state changes (e.g., draining notifications)
// Called with (backend_id, state) when a peer broadcasts a state change
// The callback should handle setting the backend weight to 0 for DRAINING
using NodeStateCallback = std::function<seastar::future<>(BackendId, NodeState)>;

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

    // Set callback for handling node state changes (e.g., DRAINING notifications)
    void set_node_state_callback(NodeStateCallback callback);

    // Set the local backend ID that this node represents
    // Used when broadcasting node state changes (e.g., DRAINING on shutdown)
    void set_local_backend_id(BackendId id) { _local_backend_id = id; }

    // Get the local backend ID
    BackendId local_backend_id() const { return _local_backend_id; }

    // Broadcast a node state change to all peers
    // Call with NodeState::DRAINING on SIGTERM to notify peers to stop sending traffic
    seastar::future<> broadcast_node_state(NodeState state);

    // Check if this node is in draining state
    bool is_draining() const { return _draining.load(std::memory_order_relaxed); }

    // Broadcast a route announcement to all peers
    // Called by RouterService when a new route is learned locally
    seastar::future<> broadcast_route(const std::vector<TokenId>& tokens, BackendId backend);

    // Check if gossip is enabled
    bool is_enabled() const { return _config.enabled; }

    // Quorum state accessors for split-brain detection
    // NOTE: These accessors only return valid data on shard 0.
    // On other shards, they return initial/stale values.
    // Use submit_to(0, ...) if you need to query from another shard.
    QuorumState quorum_state() const { return _quorum_state; }
    bool has_quorum() const { return _quorum_state == QuorumState::HEALTHY; }
    bool is_degraded() const { return _quorum_state == QuorumState::DEGRADED; }

    // Fail-open mode: when enabled and quorum is lost, requests should be
    // routed randomly to healthy backends instead of being rejected.
    // RouterService queries this to decide routing behavior during split-brain.
    bool is_fail_open_mode() const {
        return _config.quorum_enabled &&
               _config.fail_open_on_quorum_loss &&
               is_degraded();
    }

    // Get quorum status for external queries (e.g., health checks, metrics)
    // NOTE: Only valid on shard 0 where peer table is maintained.
    size_t quorum_required() const;  // N/2+1
    size_t peers_alive_count() const { return _stats_cluster_peers_alive; }
    size_t total_peers_count() const { return _peer_table.size(); }
    size_t peers_recently_seen_count() const { return _stats_peers_recently_seen; }

    // ==========================================================================
    // Admin API - Cluster State Inspection
    // ==========================================================================

    // Peer state for admin API
    struct PeerInfo {
        std::string address;
        uint16_t port;
        bool is_alive;
        int64_t last_seen_ms;  // Milliseconds since epoch
        std::optional<BackendId> associated_backend;
    };

    // Cluster state for admin API
    struct ClusterState {
        std::string quorum_state;  // "HEALTHY" or "DEGRADED"
        size_t quorum_required;
        size_t peers_alive;
        size_t total_peers;
        size_t peers_recently_seen;
        bool is_draining;
        BackendId local_backend_id;
        std::vector<PeerInfo> peers;
    };

    // Get current cluster state for admin inspection
    ClusterState get_cluster_state() const;

    // Callback type for pruning routes
    using RoutePruneCallback = std::function<seastar::future<>(BackendId)>;

    void set_route_prune_callback(RoutePruneCallback callback) {
        _route_prune_callback = std::move(callback);
    }

    // Gossip protection: check if service is accepting new tasks
    // Returns false during shutdown or re-sync
    bool is_accepting_tasks() const {
        return _running && !_resyncing.load(std::memory_order_relaxed);
    }

    // Start re-sync mode: rejects new gossip tasks while flushing existing ones
    // Call when recovering from a network partition or cluster split
    void start_resync();

    // End re-sync mode: resume normal gossip operations
    void end_resync();

private:
    ClusterConfig _config;
    RouteLearnCallback _route_learn_callback;
    NodeStateCallback _node_state_callback;

    // Local backend ID - identifies which backend this node represents
    // Used when broadcasting node state changes (e.g., DRAINING)
    BackendId _local_backend_id = 0;

    // Draining flag - set when this node is shutting down
    std::atomic<bool> _draining{false};

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

    // Quorum state for split-brain detection
    QuorumState _quorum_state = QuorumState::HEALTHY;
    uint64_t _stats_quorum_state = 1;  // 1=healthy, 0=degraded (for Prometheus gauge)
    uint64_t _quorum_transitions = 0;  // Count of state transitions
    uint64_t _routes_rejected_degraded = 0;  // Routes rejected due to degraded state
    uint64_t _routes_rejected_incoming_degraded = 0;  // Incoming routes rejected due to degraded state
    uint64_t _stats_peers_recently_seen = 0;  // Count of peers seen within quorum check window
    bool _quorum_warning_active = false;  // Track if we've already logged a warning (rate limiting)

    // Fail-open mode metrics
    uint64_t _routes_allowed_fail_open = 0;  // Routes broadcast allowed due to fail-open mode
    uint64_t _gossip_accepted_fail_open = 0;  // Incoming gossip accepted due to fail-open mode

    // DTLS lockdown metrics
    uint64_t _dtls_lockdown_drops = 0;  // Packets dropped due to mTLS lockdown (non-DTLS when mTLS required)

    // Node state notification metrics
    uint64_t _node_state_sent = 0;       // Node state packets sent (e.g., DRAINING)
    uint64_t _node_state_received = 0;   // Node state packets received from peers

    // Reliable delivery metrics
    uint64_t _acks_sent = 0;
    uint64_t _acks_received = 0;
    uint64_t _retries_sent = 0;
    uint64_t _duplicates_suppressed = 0;
    uint64_t _max_retries_exceeded = 0;
    uint64_t _dedup_peers_overflow = 0;  // Times dedup peer limit was hit (Rule #4)
    uint64_t _pending_acks_overflow = 0;  // Times pending acks limit was hit (Rule #4)
    uint64_t _stats_pending_acks_count = 0;  // Current count of pending acks (gauge)

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

    // DTLS encryption context (shard 0 only)
    std::unique_ptr<DtlsContext> _dtls_context;
    seastar::timer<> _cert_reload_timer;
    seastar::timer<> _dtls_session_cleanup_timer;

    // Adaptive crypto offloader (shard 0 only)
    // Manages thread pool for expensive crypto operations to avoid reactor stalls
    std::unique_ptr<CryptoOffloader> _crypto_offloader;

    // DTLS metrics
    uint64_t _dtls_handshakes_started = 0;
    uint64_t _dtls_handshakes_completed = 0;
    uint64_t _dtls_handshakes_failed = 0;
    uint64_t _dtls_packets_encrypted = 0;
    uint64_t _dtls_packets_decrypted = 0;
    uint64_t _dtls_cert_reloads = 0;

    // Crypto stall watchdog metrics
    uint64_t _crypto_stall_warnings = 0;       // Count of crypto ops exceeding stall threshold
    uint64_t _crypto_ops_offloaded = 0;        // Count of crypto ops offloaded to thread pool
    uint64_t _crypto_batch_broadcasts = 0;     // Count of batched broadcast operations
    uint64_t _crypto_stalls_avoided = 0;       // Count of reactor stalls avoided via offloading
    uint64_t _crypto_handshakes_offloaded = 0; // Count of handshake operations offloaded

    // Gate for cert reload handshakes - coordinates with stop() for graceful shutdown
    seastar::gate _handshake_gate;

    // Gate for gossip tasks - ensures no new gossip tasks are accepted during shutdown/re-sync
    // This provides backpressure protection for the gossip subsystem
    seastar::gate _gossip_task_gate;

    // TIMER CALLBACK SAFETY (RAII Guard Pattern):
    // Timer callbacks capture `this`, creating a potential use-after-free if the
    // callback executes after destruction begins. The race window is:
    //   1. Timer fires, callback is queued on reactor
    //   2. stop() is called, cancels timer (but callback is already queued)
    //   3. stop() returns, destructor can begin
    //   4. Queued callback executes with dangling `this`
    //
    // Solution: _timer_gate ensures timer callbacks cannot execute during shutdown.
    // - Timer callbacks acquire a gate::holder at entry (fails if gate is closed)
    // - stop() closes the gate FIRST (waits for in-flight callbacks to complete)
    // - Only then are timers cancelled and resources freed
    //
    // This guarantees: No timer callback can access `this` after stop() returns.
    // Protected callbacks: heartbeat, liveness check, retry, discovery, cert reload,
    //                      DTLS session cleanup
    seastar::gate _timer_gate;

    // Flag to indicate gossip is re-syncing (e.g., after network partition recovery)
    std::atomic<bool> _resyncing{false};

    // Crypto offloading thresholds - tune based on your hardware and latency requirements
    // These control when crypto operations are offloaded to seastar::thread to avoid reactor stalls
    static constexpr size_t CRYPTO_OFFLOAD_PEER_THRESHOLD = 10;      // Use batch mode if > N peers
    static constexpr size_t CRYPTO_OFFLOAD_BYTES_THRESHOLD = 1024;   // Offload if packet > N bytes
    static constexpr uint64_t CRYPTO_STALL_WARNING_US = 100;         // Warn if single op > 100μs

    // Deduplication limits (Rule #4: bounded containers)
    // Prevents OOM from malicious peers flooding unique source addresses
    static constexpr size_t MAX_DEDUP_PEERS = 10000;  // Max unique peers to track for dedup

    // Pending ACKs limits (Rule #4: bounded containers)
    // Prevents OOM if peers become unresponsive faster than retries expire
    // 1000 allows ~100 peers * 10 in-flight messages each
    static constexpr size_t MAX_PENDING_ACKS = 1000;

    void update_peer_liveness(const seastar::socket_address& addr);
    void check_liveness();

    // Quorum state management (two strategies):
    // - update_quorum_state(): Uses alive peer count (less strict, for startup)
    // - check_quorum(): Uses recently-seen count within window (strict, for runtime)
    void update_quorum_state();
    void check_quorum();

    //--------------------------------------------------------------------------
    // DTLS Lockdown Helpers
    //--------------------------------------------------------------------------

    // DTLS record layer content type constants (RFC 6347)
    static constexpr uint8_t DTLS_CONTENT_CHANGE_CIPHER_SPEC = 20;
    static constexpr uint8_t DTLS_CONTENT_ALERT = 21;
    static constexpr uint8_t DTLS_CONTENT_HANDSHAKE = 22;
    static constexpr uint8_t DTLS_CONTENT_APPLICATION_DATA = 23;
    static constexpr uint8_t DTLS_VERSION_MARKER = 0xFE;  // First byte of DTLS version
    static constexpr size_t DTLS_RECORD_HEADER_SIZE = 13;

    // Check if packet is a DTLS handshake/alert packet (allows through mTLS lockdown)
    bool is_dtls_handshake_packet(const uint8_t* data, size_t len) const;

    // Check if packet should be dropped by mTLS lockdown policy
    bool should_drop_packet_mtls_lockdown(const seastar::socket_address& peer, const uint8_t* data, size_t len);

    //--------------------------------------------------------------------------
    // DTLS Helper Methods
    //--------------------------------------------------------------------------

    // Lifecycle
    seastar::future<> initialize_dtls();
    void cleanup_dtls_sessions();
    seastar::future<> check_cert_reload();

    // Encryption/Decryption
    seastar::future<> send_encrypted(const seastar::socket_address& peer,
                                      const std::vector<uint8_t>& plaintext);
    std::optional<std::vector<uint8_t>> decrypt_packet(const seastar::socket_address& peer,
                                                        const uint8_t* data, size_t len);

    // Handshake handling
    seastar::future<> handle_dtls_handshake(const seastar::socket_address& peer,
                                             const uint8_t* data, size_t len);
    seastar::future<> initiate_peer_handshake(const seastar::socket_address& peer);

    // Broadcast operations
    seastar::future<> broadcast_encrypted(const std::vector<seastar::socket_address>& peers,
                                           const std::vector<uint8_t>& plaintext);

    //--------------------------------------------------------------------------
    // Low-level Helpers (reduce code duplication)
    //--------------------------------------------------------------------------

    // Send raw bytes to a peer (creates owned buffer, handles exceptions)
    void send_packet_async(const seastar::socket_address& peer,
                           const std::vector<uint8_t>& data);

    // Encrypt data for a peer, returns empty vector on failure
    // Records timing metrics and logs stall warnings
    std::vector<uint8_t> encrypt_with_timing(DtlsSession* session,
                                              const uint8_t* data, size_t len,
                                              const seastar::socket_address& peer);

    // Check if DTLS context is valid and enabled (for use in async blocks)
    bool is_dtls_ready() const {
        return _dtls_context && _dtls_context->is_enabled();
    }

    // Stall watchdog: measure and log crypto operation latency
    template<typename Func>
    auto with_stall_watchdog(const char* op_name, Func&& func) -> decltype(func()) {
        auto start = std::chrono::steady_clock::now();
        auto result = func();
        auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (static_cast<uint64_t>(elapsed_us) > CRYPTO_STALL_WARNING_US) {
            ++_crypto_stall_warnings;
            log_gossip.warn("Crypto stall detected: {} took {}μs (threshold: {}μs)",
                           op_name, elapsed_us, CRYPTO_STALL_WARNING_US);
        }
        return result;
    }

    //--------------------------------------------------------------------------
    // Adaptive Crypto Offloading
    //--------------------------------------------------------------------------

    // Initialize the crypto offloader (called during start())
    void initialize_crypto_offloader();

    // Encrypt data using adaptive offloading
    // Returns future that resolves to encrypted data, or empty vector on failure
    seastar::future<std::vector<uint8_t>> encrypt_with_offloading(
        DtlsSession* session,
        const std::vector<uint8_t>& plaintext,
        const seastar::socket_address& peer);

    // Perform DTLS handshake with adaptive offloading
    // Handshakes are always offloaded as they involve expensive RSA/ECDH operations
    seastar::future<DtlsResult> handshake_with_offloading(
        DtlsSession* session,
        const uint8_t* data,
        size_t len,
        std::vector<uint8_t>& response);

    // Update crypto offloader stats from local metrics
    void sync_crypto_offloader_stats();

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
