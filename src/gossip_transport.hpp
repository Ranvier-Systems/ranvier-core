// Ranvier Core - Gossip Transport Module
//
// Manages UDP communication and DTLS encryption for the gossip protocol.
// Extracted from GossipService for better modularity and testability.
//
// Responsibilities:
// - UDP channel management
// - DTLS encryption/decryption
// - Certificate hot-reloading
// - Crypto offloading for expensive operations
// - mTLS lockdown enforcement

#pragma once

#include "config.hpp"
#include "crypto_offloader.hpp"
#include "dtls_context.hpp"
#include "logging.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/timer.hh>
#include <seastar/net/api.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/net/udp.hh>

namespace ranvier {

// Forward declaration for logger
inline seastar::logger& log_gossip_transport() {
    static seastar::logger logger("ranvier.gossip.transport");
    return logger;
}

// GossipTransport: Manages UDP/DTLS communication for gossip
//
// This class handles all network transport concerns:
// - UDP socket management
// - DTLS encryption and handshakes
// - Certificate hot-reloading
// - Crypto operation offloading to avoid reactor stalls
// - mTLS lockdown policy enforcement
//
// Threading: This class is shard-local. Only shard 0 manages the physical
// UDP channel. Other shards can still use the encryption helpers.
class GossipTransport {
public:
    explicit GossipTransport(const ClusterConfig& config);
    ~GossipTransport() = default;

    // Non-copyable, non-movable
    GossipTransport(const GossipTransport&) = delete;
    GossipTransport& operator=(const GossipTransport&) = delete;

    // Lifecycle
    seastar::future<> start(uint16_t port);
    seastar::future<> stop();

    // Check if transport is ready
    bool is_ready() const { return _channel.has_value(); }

    // Send data to a peer (handles encryption transparently)
    seastar::future<> send(const seastar::socket_address& peer,
                           const std::vector<uint8_t>& data);

    // Broadcast data to multiple peers (optimized for large fan-out)
    seastar::future<> broadcast(const std::vector<seastar::socket_address>& peers,
                                const std::vector<uint8_t>& data);

    // Decrypt incoming packet, returns nullopt if handshake or error
    std::optional<std::vector<uint8_t>> decrypt(const seastar::socket_address& peer,
                                                 const uint8_t* data, size_t len);

    // DTLS handshake management
    seastar::future<> initiate_handshake(const seastar::socket_address& peer);

    // mTLS lockdown check
    bool should_drop_mtls_lockdown(const seastar::socket_address& peer,
                                    const uint8_t* data, size_t len);

    // Check if packet is a DTLS handshake packet
    bool is_dtls_handshake_packet(const uint8_t* data, size_t len) const;

    // Direct channel access for receive loop
    std::optional<seastar::net::udp_channel>& channel() { return _channel; }

    // Access timer gate for shared timer safety
    seastar::gate& timer_gate() { return _timer_gate; }

    // Access handshake gate for shared handshake coordination
    seastar::gate& handshake_gate() { return _handshake_gate; }

    // Check if DTLS is enabled and ready
    bool is_dtls_enabled() const {
        return _dtls_context && _dtls_context->is_enabled();
    }

    // Metrics accessors
    uint64_t dtls_handshakes_started() const { return _dtls_handshakes_started; }
    uint64_t dtls_handshakes_completed() const { return _dtls_handshakes_completed; }
    uint64_t dtls_handshakes_failed() const { return _dtls_handshakes_failed; }
    uint64_t dtls_packets_encrypted() const { return _dtls_packets_encrypted; }
    uint64_t dtls_packets_decrypted() const { return _dtls_packets_decrypted; }
    uint64_t dtls_cert_reloads() const { return _dtls_cert_reloads; }
    uint64_t dtls_lockdown_drops() const { return _dtls_lockdown_drops; }
    uint64_t dtls_sessions_rejected() const {
        return _dtls_context ? _dtls_context->sessions_rejected() : 0;
    }
    uint64_t crypto_stall_warnings() const { return _crypto_stall_warnings; }
    uint64_t crypto_ops_offloaded() const { return _crypto_ops_offloaded; }
    uint64_t crypto_batch_broadcasts() const { return _crypto_batch_broadcasts; }
    uint64_t crypto_stalls_avoided() const { return _crypto_stalls_avoided; }
    uint64_t crypto_handshakes_offloaded() const { return _crypto_handshakes_offloaded; }

    // Register metrics with Seastar metrics system
    void register_metrics(seastar::metrics::metric_groups& metrics);

    // Sync stats from crypto offloader
    void sync_crypto_offloader_stats();

private:
    const ClusterConfig& _config;

    // UDP channel
    std::optional<seastar::net::udp_channel> _channel;

    // DTLS context and crypto offloader
    std::unique_ptr<DtlsContext> _dtls_context;
    std::unique_ptr<CryptoOffloader> _crypto_offloader;

    // Timers
    seastar::timer<> _cert_reload_timer;
    seastar::timer<> _dtls_session_cleanup_timer;

    // Gates for coordination
    seastar::gate _timer_gate;
    seastar::gate _handshake_gate;

    // DTLS metrics
    uint64_t _dtls_handshakes_started = 0;
    uint64_t _dtls_handshakes_completed = 0;
    uint64_t _dtls_handshakes_failed = 0;
    uint64_t _dtls_packets_encrypted = 0;
    uint64_t _dtls_packets_decrypted = 0;
    uint64_t _dtls_cert_reloads = 0;
    uint64_t _dtls_lockdown_drops = 0;

    // Crypto stall watchdog metrics
    uint64_t _crypto_stall_warnings = 0;
    uint64_t _crypto_ops_offloaded = 0;
    uint64_t _crypto_batch_broadcasts = 0;
    uint64_t _crypto_stalls_avoided = 0;
    uint64_t _crypto_handshakes_offloaded = 0;

    // Thresholds
    static constexpr size_t CRYPTO_OFFLOAD_PEER_THRESHOLD = 10;
    static constexpr size_t CRYPTO_OFFLOAD_BYTES_THRESHOLD = 1024;
    static constexpr uint64_t CRYPTO_STALL_WARNING_US = 100;

    // DTLS record layer constants (RFC 6347)
    static constexpr uint8_t DTLS_CONTENT_CHANGE_CIPHER_SPEC = 20;
    static constexpr uint8_t DTLS_CONTENT_ALERT = 21;
    static constexpr uint8_t DTLS_CONTENT_HANDSHAKE = 22;
    static constexpr uint8_t DTLS_CONTENT_APPLICATION_DATA = 23;
    static constexpr uint8_t DTLS_VERSION_MARKER = 0xFE;
    static constexpr size_t DTLS_RECORD_HEADER_SIZE = 13;

    // Internal helpers
    seastar::future<> initialize_dtls();
    void initialize_crypto_offloader();
    void cleanup_dtls_sessions();
    seastar::future<> check_cert_reload();

    void send_packet_async(const seastar::socket_address& peer,
                           const std::vector<uint8_t>& data);

    std::vector<uint8_t> encrypt_with_timing(DtlsSession* session,
                                              const uint8_t* data, size_t len,
                                              const seastar::socket_address& peer);

    seastar::future<std::vector<uint8_t>> encrypt_with_offloading(
        DtlsSession* session,
        const std::vector<uint8_t>& plaintext,
        const seastar::socket_address& peer);
};

}  // namespace ranvier
