// Ranvier Core - DTLS Context Implementation
//
// OpenSSL-based DTLS implementation for gossip protocol encryption

#include "dtls_context.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sys/stat.h>

namespace ranvier {

// OpenSSL initialization
std::once_flag OpenSSLInit::_init_flag;
std::once_flag OpenSSLInit::_cleanup_flag;

OpenSSLInit::OpenSSLInit() {
    std::call_once(_init_flag, []() {
        // OpenSSL 1.1.0+ automatically initializes itself
        // But we still need to make sure error strings are loaded
        SSL_load_error_strings();
        OpenSSL_add_ssl_algorithms();
        log_dtls.info("OpenSSL initialized: {}", OpenSSL_version(OPENSSL_VERSION));
    });
}

OpenSSLInit::~OpenSSLInit() {
    // Note: Don't cleanup OpenSSL here as other parts may still use it
    // EVP_cleanup() and ERR_free_strings() are deprecated in OpenSSL 1.1.0+
}

// Static OpenSSL initializer (constructed before main)
static OpenSSLInit openssl_init;

//------------------------------------------------------------------------------
// DtlsSession Implementation
//------------------------------------------------------------------------------

DtlsSession::DtlsSession(SSL* ssl, bool is_server)
    : _ssl(ssl), _is_server(is_server), _last_activity(std::chrono::steady_clock::now()) {

    // Create memory BIOs for non-blocking I/O
    _read_bio = BIO_new(BIO_s_mem());
    _write_bio = BIO_new(BIO_s_mem());

    if (!_read_bio || !_write_bio) {
        set_error("Failed to create memory BIOs");
        return;
    }

    // Set BIOs to non-blocking mode
    BIO_set_nbio(_read_bio, 1);
    BIO_set_nbio(_write_bio, 1);

    // Attach BIOs to SSL object
    // SSL_set_bio takes ownership of the BIOs
    SSL_set_bio(_ssl, _read_bio, _write_bio);

    // Set mode based on role
    if (_is_server) {
        SSL_set_accept_state(_ssl);
    } else {
        SSL_set_connect_state(_ssl);
    }

    _state = DtlsSessionState::NEW;
}

DtlsSession::~DtlsSession() {
    if (_ssl) {
        // SSL_free also frees the attached BIOs
        SSL_free(_ssl);
        _ssl = nullptr;
        _read_bio = nullptr;
        _write_bio = nullptr;
    }
}

DtlsSession::DtlsSession(DtlsSession&& other) noexcept
    : _ssl(other._ssl),
      _read_bio(other._read_bio),
      _write_bio(other._write_bio),
      _state(other._state),
      _is_server(other._is_server),
      _last_error(std::move(other._last_error)),
      _last_activity(other._last_activity) {
    other._ssl = nullptr;
    other._read_bio = nullptr;
    other._write_bio = nullptr;
    other._state = DtlsSessionState::CLOSED;
}

DtlsSession& DtlsSession::operator=(DtlsSession&& other) noexcept {
    if (this != &other) {
        if (_ssl) {
            SSL_free(_ssl);
        }
        _ssl = other._ssl;
        _read_bio = other._read_bio;
        _write_bio = other._write_bio;
        _state = other._state;
        _is_server = other._is_server;
        _last_error = std::move(other._last_error);
        _last_activity = other._last_activity;

        other._ssl = nullptr;
        other._read_bio = nullptr;
        other._write_bio = nullptr;
        other._state = DtlsSessionState::CLOSED;
    }
    return *this;
}

std::vector<uint8_t> DtlsSession::drain_write_bio() {
    std::vector<uint8_t> data;
    char buf[4096];
    int len;

    while ((len = BIO_read(_write_bio, buf, sizeof(buf))) > 0) {
        data.insert(data.end(), buf, buf + len);
    }

    return data;
}

void DtlsSession::set_error(const std::string& msg) {
    _state = DtlsSessionState::ERROR;
    _last_error = msg + ": " + get_ssl_error();
    log_dtls.error("DTLS session error: {}", _last_error);
}

std::string DtlsSession::get_ssl_error() {
    char buf[256];
    unsigned long err = ERR_get_error();
    if (err == 0) {
        return "No error";
    }
    ERR_error_string_n(err, buf, sizeof(buf));
    return std::string(buf);
}

DtlsResult DtlsSession::process_incoming(const uint8_t* data, size_t len,
                                          std::vector<uint8_t>& outgoing) {
    if (!_ssl || _state == DtlsSessionState::ERROR || _state == DtlsSessionState::CLOSED) {
        return DtlsResult::ERROR;
    }

    touch();

    // Write incoming data to read BIO
    int written = BIO_write(_read_bio, data, static_cast<int>(len));
    if (written <= 0) {
        set_error("Failed to write to read BIO");
        return DtlsResult::ERROR;
    }

    // If not yet established, continue handshake
    if (_state != DtlsSessionState::ESTABLISHED) {
        return continue_handshake(nullptr, 0, outgoing);
    }

    return DtlsResult::SUCCESS;
}

DtlsResult DtlsSession::encrypt(const uint8_t* plaintext, size_t len,
                                 std::vector<uint8_t>& encrypted) {
    if (!_ssl || _state != DtlsSessionState::ESTABLISHED) {
        return DtlsResult::HANDSHAKE_NEEDED;
    }

    touch();

    int written = SSL_write(_ssl, plaintext, static_cast<int>(len));
    if (written <= 0) {
        int err = SSL_get_error(_ssl, written);
        if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
            return DtlsResult::WANT_WRITE;
        }
        set_error("SSL_write failed");
        return DtlsResult::ERROR;
    }

    // Drain encrypted data from write BIO
    encrypted = drain_write_bio();
    return DtlsResult::SUCCESS;
}

DtlsResult DtlsSession::decrypt(const uint8_t* ciphertext, size_t len,
                                 std::vector<uint8_t>& decrypted) {
    if (!_ssl) {
        return DtlsResult::ERROR;
    }

    touch();

    // Write ciphertext to read BIO
    int written = BIO_write(_read_bio, ciphertext, static_cast<int>(len));
    if (written <= 0) {
        set_error("Failed to write ciphertext to read BIO");
        return DtlsResult::ERROR;
    }

    // If not established, this might be handshake data
    if (_state != DtlsSessionState::ESTABLISHED) {
        std::vector<uint8_t> outgoing;
        auto result = continue_handshake(nullptr, 0, outgoing);
        // Note: outgoing data needs to be sent by caller
        return result;
    }

    // Read decrypted data
    char buf[4096];
    int read_len = SSL_read(_ssl, buf, sizeof(buf));
    if (read_len > 0) {
        decrypted.insert(decrypted.end(), buf, buf + read_len);
        return DtlsResult::SUCCESS;
    }

    int err = SSL_get_error(_ssl, read_len);
    if (err == SSL_ERROR_WANT_READ) {
        return DtlsResult::WANT_READ;
    }
    if (err == SSL_ERROR_ZERO_RETURN) {
        _state = DtlsSessionState::CLOSED;
        return DtlsResult::CLOSED;
    }

    set_error("SSL_read failed");
    return DtlsResult::ERROR;
}

DtlsResult DtlsSession::initiate_handshake(std::vector<uint8_t>& outgoing) {
    if (!_ssl || _is_server) {
        return DtlsResult::ERROR;
    }

    _state = DtlsSessionState::HANDSHAKING;

    int ret = SSL_do_handshake(_ssl);
    if (ret == 1) {
        // Handshake complete (unlikely on first call)
        _state = DtlsSessionState::ESTABLISHED;
        log_dtls.info("DTLS handshake completed (client)");
        outgoing = drain_write_bio();
        return DtlsResult::SUCCESS;
    }

    int err = SSL_get_error(_ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        // Need to send ClientHello
        outgoing = drain_write_bio();
        return DtlsResult::WANT_READ;
    }

    set_error("SSL_do_handshake failed");
    return DtlsResult::ERROR;
}

DtlsResult DtlsSession::continue_handshake(const uint8_t* data, size_t len,
                                            std::vector<uint8_t>& outgoing) {
    if (!_ssl) {
        return DtlsResult::ERROR;
    }

    // If data provided, write to read BIO
    if (data && len > 0) {
        int written = BIO_write(_read_bio, data, static_cast<int>(len));
        if (written <= 0) {
            set_error("Failed to write handshake data to BIO");
            return DtlsResult::ERROR;
        }
    }

    _state = DtlsSessionState::HANDSHAKING;

    int ret = SSL_do_handshake(_ssl);
    if (ret == 1) {
        // Handshake complete
        _state = DtlsSessionState::ESTABLISHED;
        log_dtls.info("DTLS handshake completed ({})", _is_server ? "server" : "client");

        // Log peer certificate info
        std::string peer_cn = peer_common_name();
        if (!peer_cn.empty()) {
            log_dtls.info("DTLS peer certificate CN: {}", peer_cn);
        }

        outgoing = drain_write_bio();
        return DtlsResult::SUCCESS;
    }

    int err = SSL_get_error(_ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        // Handshake in progress, may have data to send
        outgoing = drain_write_bio();
        return DtlsResult::WANT_READ;
    }

    set_error("SSL_do_handshake failed during continue");
    return DtlsResult::ERROR;
}

std::string DtlsSession::peer_common_name() const {
    if (!_ssl) {
        return "";
    }

    X509* peer_cert = SSL_get_peer_certificate(_ssl);
    if (!peer_cert) {
        return "";
    }

    char cn[256] = {0};
    X509_NAME* subject = X509_get_subject_name(peer_cert);
    if (subject) {
        X509_NAME_get_text_by_NID(subject, NID_commonName, cn, sizeof(cn));
    }

    X509_free(peer_cert);
    return std::string(cn);
}

//------------------------------------------------------------------------------
// DtlsContext Implementation
//------------------------------------------------------------------------------

DtlsContext::DtlsContext(const GossipTlsConfig& config)
    : _config(config), _enabled(config.enabled) {
}

DtlsContext::~DtlsContext() {
    // Clear sessions first (they reference _ctx)
    _sessions.clear();

    if (_ctx) {
        SSL_CTX_free(_ctx);
        _ctx = nullptr;
    }
}

std::optional<std::string> DtlsContext::initialize() {
    if (!_enabled) {
        log_dtls.info("DTLS disabled");
        return std::nullopt;
    }

    log_dtls.info("Initializing DTLS context");

    // Create DTLS context
    // Use DTLS_method() for DTLS 1.0+ (negotiates highest available)
    const SSL_METHOD* method = DTLS_method();
    if (!method) {
        return "Failed to get DTLS method: " + DtlsSession::get_ssl_error();
    }

    _ctx = SSL_CTX_new(method);
    if (!_ctx) {
        return "Failed to create SSL_CTX: " + DtlsSession::get_ssl_error();
    }

    // Set minimum protocol version to DTLS 1.2
    if (!SSL_CTX_set_min_proto_version(_ctx, DTLS1_2_VERSION)) {
        SSL_CTX_free(_ctx);
        _ctx = nullptr;
        return "Failed to set minimum DTLS version: " + DtlsSession::get_ssl_error();
    }

    // Enable DTLS cookie exchange for DoS protection
    SSL_CTX_set_options(_ctx, SSL_OP_COOKIE_EXCHANGE);

    // Set verification mode based on config
    if (_config.verify_peer) {
        SSL_CTX_set_verify(_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
        log_dtls.info("DTLS peer verification enabled (mTLS)");
    } else {
        SSL_CTX_set_verify(_ctx, SSL_VERIFY_NONE, nullptr);
        log_dtls.warn("DTLS peer verification disabled - NOT RECOMMENDED for production");
    }

    // Load certificates
    auto err = load_certificates();
    if (err) {
        SSL_CTX_free(_ctx);
        _ctx = nullptr;
        return err;
    }

    _initialized = true;
    log_dtls.info("DTLS context initialized successfully");

    // Warn if plaintext mode is enabled
    if (!_enabled) {
        log_dtls.warn("Gossip TLS is DISABLED - traffic is unencrypted!");
        log_dtls.warn("Enable cluster.tls.enabled for production deployments");
    }

    return std::nullopt;
}

std::optional<std::string> DtlsContext::load_certificates() {
    // Load certificate
    if (SSL_CTX_use_certificate_file(_ctx, _config.cert_path.c_str(), SSL_FILETYPE_PEM) <= 0) {
        return "Failed to load certificate from " + _config.cert_path + ": " + DtlsSession::get_ssl_error();
    }
    log_dtls.debug("Loaded certificate: {}", _config.cert_path);

    // Load private key
    if (SSL_CTX_use_PrivateKey_file(_ctx, _config.key_path.c_str(), SSL_FILETYPE_PEM) <= 0) {
        return "Failed to load private key from " + _config.key_path + ": " + DtlsSession::get_ssl_error();
    }
    log_dtls.debug("Loaded private key: {}", _config.key_path);

    // Verify key matches certificate
    if (!SSL_CTX_check_private_key(_ctx)) {
        return "Private key does not match certificate: " + DtlsSession::get_ssl_error();
    }

    // Load CA certificate for peer verification
    if (SSL_CTX_load_verify_locations(_ctx, _config.ca_path.c_str(), nullptr) <= 0) {
        return "Failed to load CA certificate from " + _config.ca_path + ": " + DtlsSession::get_ssl_error();
    }
    log_dtls.debug("Loaded CA certificate: {}", _config.ca_path);

    // Store modification times for hot reload
    _cert_mtime = get_file_mtime(_config.cert_path).value_or(std::chrono::system_clock::time_point{});
    _key_mtime = get_file_mtime(_config.key_path).value_or(std::chrono::system_clock::time_point{});
    _ca_mtime = get_file_mtime(_config.ca_path).value_or(std::chrono::system_clock::time_point{});

    return std::nullopt;
}

std::optional<std::chrono::system_clock::time_point> DtlsContext::get_file_mtime(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return std::nullopt;
    }
    return std::chrono::system_clock::from_time_t(st.st_mtime);
}

bool DtlsContext::check_and_reload_certs() {
    if (!_initialized || _config.cert_reload_interval.count() == 0) {
        return false;
    }

    auto cert_mtime = get_file_mtime(_config.cert_path);
    auto key_mtime = get_file_mtime(_config.key_path);
    auto ca_mtime = get_file_mtime(_config.ca_path);

    bool changed = false;
    if (cert_mtime && *cert_mtime != _cert_mtime) {
        changed = true;
    }
    if (key_mtime && *key_mtime != _key_mtime) {
        changed = true;
    }
    if (ca_mtime && *ca_mtime != _ca_mtime) {
        changed = true;
    }

    if (!changed) {
        return false;
    }

    log_dtls.info("Certificate files changed, reloading...");

    // Create a new context with new certificates
    SSL_CTX* new_ctx = SSL_CTX_new(DTLS_method());
    if (!new_ctx) {
        log_dtls.error("Failed to create new SSL_CTX for reload");
        return false;
    }

    // Configure new context
    SSL_CTX_set_min_proto_version(new_ctx, DTLS1_2_VERSION);
    SSL_CTX_set_options(new_ctx, SSL_OP_COOKIE_EXCHANGE);

    if (_config.verify_peer) {
        SSL_CTX_set_verify(new_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
    }

    // Load certificates into new context
    if (SSL_CTX_use_certificate_file(new_ctx, _config.cert_path.c_str(), SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(new_ctx, _config.key_path.c_str(), SSL_FILETYPE_PEM) <= 0 ||
        !SSL_CTX_check_private_key(new_ctx) ||
        SSL_CTX_load_verify_locations(new_ctx, _config.ca_path.c_str(), nullptr) <= 0) {
        log_dtls.error("Failed to load certificates during reload");
        SSL_CTX_free(new_ctx);
        return false;
    }

    // Success - swap contexts
    SSL_CTX* old_ctx = _ctx;
    _ctx = new_ctx;

    // Update modification times
    _cert_mtime = cert_mtime.value_or(std::chrono::system_clock::time_point{});
    _key_mtime = key_mtime.value_or(std::chrono::system_clock::time_point{});
    _ca_mtime = ca_mtime.value_or(std::chrono::system_clock::time_point{});

    // Clear existing sessions (they use old context)
    size_t session_count = _sessions.size();
    _sessions.clear();

    // Free old context
    SSL_CTX_free(old_ctx);

    log_dtls.info("Certificates reloaded successfully, cleared {} sessions", session_count);
    return true;
}

SSL* DtlsContext::create_ssl(bool is_server) {
    if (!_ctx) {
        return nullptr;
    }

    SSL* ssl = SSL_new(_ctx);
    if (!ssl) {
        log_dtls.error("Failed to create SSL object: {}", DtlsSession::get_ssl_error());
        return nullptr;
    }

    return ssl;
}

DtlsSession* DtlsContext::get_or_create_session(const seastar::socket_address& peer, bool is_server) {
    auto it = _sessions.find(peer);
    if (it != _sessions.end()) {
        return it->second.get();
    }

    // Create new session
    SSL* ssl = create_ssl(is_server);
    if (!ssl) {
        return nullptr;
    }

    auto session = std::make_unique<DtlsSession>(ssl, is_server);
    DtlsSession* ptr = session.get();
    _sessions[peer] = std::move(session);

    log_dtls.debug("Created DTLS session for peer {} ({})", peer, is_server ? "server" : "client");
    return ptr;
}

void DtlsContext::remove_session(const seastar::socket_address& peer) {
    auto it = _sessions.find(peer);
    if (it != _sessions.end()) {
        log_dtls.debug("Removing DTLS session for peer {}", peer);
        _sessions.erase(it);
    }
}

size_t DtlsContext::session_count() const {
    return _sessions.size();
}

void DtlsContext::cleanup_idle_sessions(std::chrono::seconds timeout) {
    auto now = std::chrono::steady_clock::now();
    size_t removed = 0;

    for (auto it = _sessions.begin(); it != _sessions.end(); ) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second->last_activity());
        if (age > timeout) {
            log_dtls.debug("Cleaning up idle DTLS session (idle for {}s)", age.count());
            it = _sessions.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }

    if (removed > 0) {
        log_dtls.info("Cleaned up {} idle DTLS sessions", removed);
    }
}

}  // namespace ranvier
