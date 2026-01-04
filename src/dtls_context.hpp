// Ranvier Core - DTLS Context for Gossip Protocol Encryption
//
// Provides DTLS 1.2/1.3 encryption for UDP-based gossip communication.
// Uses OpenSSL for DTLS support with mutual TLS (mTLS) authentication.
//
// Key features:
// - Per-peer DTLS sessions with connection-oriented handshake
// - Certificate hot-reloading without restart
// - Async-compatible with Seastar (uses memory BIOs, non-blocking)
// - Supports certificate verification and client authentication

#pragma once

#include "config.hpp"
#include "logging.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/bio.h>

#include <seastar/core/future.hh>
#include <seastar/core/timer.hh>
#include <seastar/net/socket_defs.hh>

namespace ranvier {

// Logger for DTLS operations
inline seastar::logger log_dtls("ranvier.dtls");

// Result of a DTLS operation
enum class DtlsResult {
    SUCCESS,           // Operation completed successfully
    WANT_READ,         // Need more data from peer (handshake in progress)
    WANT_WRITE,        // Need to send data to peer (handshake in progress)
    ERROR,             // Fatal error occurred
    HANDSHAKE_NEEDED,  // Handshake not yet complete
    CLOSED             // Connection was closed
};

// DTLS session state
enum class DtlsSessionState {
    NEW,           // Session just created
    HANDSHAKING,   // Handshake in progress
    ESTABLISHED,   // Handshake complete, ready for data
    ERROR,         // Error occurred
    CLOSED         // Session closed
};

// Forward declaration
class DtlsContext;

// Represents a DTLS session with a single peer
// Uses OpenSSL's memory BIOs for async I/O compatibility
class DtlsSession {
public:
    DtlsSession(SSL* ssl, bool is_server);
    ~DtlsSession();

    // Non-copyable, movable
    DtlsSession(const DtlsSession&) = delete;
    DtlsSession& operator=(const DtlsSession&) = delete;
    DtlsSession(DtlsSession&& other) noexcept;
    DtlsSession& operator=(DtlsSession&& other) noexcept;

    // Get current session state
    DtlsSessionState state() const { return _state; }

    // Process incoming encrypted data from the network
    // Returns any data that needs to be sent to the peer
    DtlsResult process_incoming(const uint8_t* data, size_t len,
                                 std::vector<uint8_t>& outgoing);

    // Encrypt application data for sending
    // Writes encrypted data to 'encrypted' vector
    DtlsResult encrypt(const uint8_t* plaintext, size_t len,
                        std::vector<uint8_t>& encrypted);

    // Decrypt application data received from peer
    // Writes decrypted data to 'decrypted' vector
    DtlsResult decrypt(const uint8_t* ciphertext, size_t len,
                        std::vector<uint8_t>& decrypted);

    // Initiate handshake (for client role)
    // Returns data to send to peer
    DtlsResult initiate_handshake(std::vector<uint8_t>& outgoing);

    // Continue handshake with data from peer
    // Returns data to send to peer (if any)
    DtlsResult continue_handshake(const uint8_t* data, size_t len,
                                   std::vector<uint8_t>& outgoing);

    // Check if handshake is complete
    bool is_established() const { return _state == DtlsSessionState::ESTABLISHED; }

    // Get peer certificate common name (for logging)
    std::string peer_common_name() const;

    // Get last error message
    const std::string& last_error() const { return _last_error; }

    // Get time of last activity
    std::chrono::steady_clock::time_point last_activity() const { return _last_activity; }

    // Update last activity time
    void touch() { _last_activity = std::chrono::steady_clock::now(); }

    // Get OpenSSL error string (static utility)
    static std::string get_ssl_error();

private:
    SSL* _ssl = nullptr;
    BIO* _read_bio = nullptr;   // For incoming encrypted data
    BIO* _write_bio = nullptr;  // For outgoing encrypted data
    DtlsSessionState _state = DtlsSessionState::NEW;
    bool _is_server = false;
    std::string _last_error;
    std::chrono::steady_clock::time_point _last_activity;

    // Read any pending data from write BIO
    std::vector<uint8_t> drain_write_bio();

    // Set error state with message
    void set_error(const std::string& msg);
};

// DTLS context manager for gossip encryption
// Manages SSL_CTX and per-peer sessions
class DtlsContext {
public:
    explicit DtlsContext(const GossipTlsConfig& config);
    ~DtlsContext();

    // Non-copyable
    DtlsContext(const DtlsContext&) = delete;
    DtlsContext& operator=(const DtlsContext&) = delete;

    // Initialize the context (load certificates)
    // Returns error message on failure, nullopt on success
    std::optional<std::string> initialize();

    // Check if DTLS is enabled and initialized
    bool is_enabled() const { return _enabled && _initialized; }

    // Get or create a session for a peer
    // is_server: true if we received data first (server role)
    DtlsSession* get_or_create_session(const seastar::socket_address& peer, bool is_server);

    // Remove a session for a peer
    void remove_session(const seastar::socket_address& peer);

    // Check and reload certificates if changed
    // Returns true if certificates were reloaded
    bool check_and_reload_certs();

    // Get session count (for metrics)
    size_t session_count() const;

    // Clean up idle sessions older than timeout
    void cleanup_idle_sessions(std::chrono::seconds timeout);

    // Get configuration
    const GossipTlsConfig& config() const { return _config; }

private:
    GossipTlsConfig _config;
    SSL_CTX* _ctx = nullptr;
    bool _enabled = false;
    bool _initialized = false;

    // Per-peer sessions
    std::unordered_map<seastar::socket_address, std::unique_ptr<DtlsSession>> _sessions;

    // Certificate file modification times for hot reload
    std::chrono::system_clock::time_point _cert_mtime;
    std::chrono::system_clock::time_point _key_mtime;
    std::chrono::system_clock::time_point _ca_mtime;

    // Create a new SSL object for a session
    SSL* create_ssl(bool is_server);

    // Load certificates into context
    std::optional<std::string> load_certificates();

    // Get file modification time
    static std::optional<std::chrono::system_clock::time_point> get_file_mtime(const std::string& path);
};

// RAII wrapper for OpenSSL initialization
class OpenSSLInit {
public:
    OpenSSLInit();
    ~OpenSSLInit();

    // Non-copyable
    OpenSSLInit(const OpenSSLInit&) = delete;
    OpenSSLInit& operator=(const OpenSSLInit&) = delete;

private:
    static std::once_flag _init_flag;
    static std::once_flag _cleanup_flag;
};

}  // namespace ranvier
