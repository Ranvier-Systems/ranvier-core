// Ranvier Core - Gossip Transport Module Implementation
//
// Manages UDP communication and DTLS encryption for gossip.

#include "gossip_transport.hpp"

#include <cstring>
#include <span>

#include <seastar/core/coroutine.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/thread.hh>
#include <seastar/net/inet_address.hh>

namespace ranvier {

GossipTransport::GossipTransport(const ClusterConfig& config)
    : _config(config) {
}

seastar::future<> GossipTransport::start(uint16_t port) {
    // Only shard 0 manages the physical UDP channel
    if (seastar::this_shard_id() != 0) {
        co_return;
    }

    log_gossip_transport().info("Starting gossip transport on port {}", port);

    seastar::socket_address bind_addr(seastar::ipv4_addr("0.0.0.0", port));

    _channel = seastar::engine().net().make_bound_datagram_channel(bind_addr);
    log_gossip_transport().info("Gossip UDP channel opened on port {}", port);

    // Initialize DTLS if enabled
    if (_config.tls.enabled) {
        co_await initialize_dtls();
        initialize_crypto_offloader();
    } else {
        log_gossip_transport().warn("Gossip TLS is DISABLED - cluster traffic is unencrypted!");
        log_gossip_transport().warn("This is a SECURITY RISK in production. Enable cluster.tls for encrypted gossip.");
    }
}

seastar::future<> GossipTransport::stop() {
    log_gossip_transport().info("Stopping gossip transport");

    // Close timer gate first
    co_await _timer_gate.close();
    log_gossip_transport().debug("Timer gate closed");

    // Shutdown channel
    if (_channel) {
        _channel->shutdown_input();
        _channel->shutdown_output();
        _channel = std::nullopt;
    }

    // Cancel timers
    _cert_reload_timer.cancel();
    _dtls_session_cleanup_timer.cancel();

    // Stop crypto offloader
    if (_crypto_offloader) {
        sync_crypto_offloader_stats();
        _crypto_offloader->stop();
        log_gossip_transport().debug("Crypto offloader stopped");
    }

    // Close handshake gate
    co_await _handshake_gate.close();

    // Clean up DTLS context
    if (_dtls_context) {
        _dtls_context.reset();
        log_gossip_transport().debug("DTLS context released");
    }
}

seastar::future<> GossipTransport::initialize_dtls() {
    log_gossip_transport().info("Initializing DTLS for gossip encryption");

    _dtls_context = std::make_unique<DtlsContext>(_config.tls);

    auto err = co_await _dtls_context->initialize();
    if (err) {
        log_gossip_transport().error("Failed to initialize DTLS: {}", *err);
        if (!_config.tls.allow_plaintext_fallback) {
            throw std::runtime_error("DTLS initialization failed: " + *err);
        }
        log_gossip_transport().warn("Falling back to plaintext mode (allow_plaintext_fallback=true)");
        _dtls_context.reset();
        co_return;
    }

    log_gossip_transport().info("DTLS initialized successfully");
    log_gossip_transport().info("DTLS peer verification: {}", _config.tls.verify_peer ? "enabled (mTLS)" : "disabled");

    // Set up certificate reload timer with RAII timer safety
    if (_config.tls.cert_reload_interval.count() > 0) {
        log_gossip_transport().info("Certificate hot-reload enabled: interval={}s",
                                    _config.tls.cert_reload_interval.count());
        _cert_reload_timer.set_callback([this] {
            // RAII Timer Safety: Holder must outlive the work
            seastar::gate::holder timer_holder;
            try {
                timer_holder = _timer_gate.hold();
            } catch (const seastar::gate_closed_exception&) {
                return;
            }

            (void)check_cert_reload().handle_exception([](auto ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    log_gossip_transport().error("Certificate reload check failed: {}", e.what());
                }
            });
        });
        _cert_reload_timer.arm_periodic(_config.tls.cert_reload_interval);
    }

    // Set up DTLS session cleanup timer
    _dtls_session_cleanup_timer.set_callback([this] { cleanup_dtls_sessions(); });
    _dtls_session_cleanup_timer.arm_periodic(std::chrono::seconds(60));

    co_return;
}

void GossipTransport::initialize_crypto_offloader() {
    if (!_config.tls.enabled) {
        return;
    }

    CryptoOffloaderConfig offloader_config;
    offloader_config.size_threshold_bytes = CRYPTO_OFFLOAD_BYTES_THRESHOLD;
    offloader_config.stall_threshold_us = 500;
    offloader_config.offload_latency_threshold_us = CRYPTO_STALL_WARNING_US;
    offloader_config.max_queue_depth = 1024;
    offloader_config.enabled = true;
    offloader_config.symmetric_always_inline = true;
    offloader_config.handshake_always_offload = true;

    _crypto_offloader = std::make_unique<CryptoOffloader>(offloader_config);
    _crypto_offloader->start();

    log_gossip_transport().info("Crypto offloader initialized (seastar::async mode)");
}

seastar::future<> GossipTransport::send(const seastar::socket_address& peer,
                                         const std::vector<uint8_t>& data) {
    if (!_channel) {
        return seastar::make_ready_future<>();
    }

    if (!_dtls_context || !_dtls_context->is_enabled()) {
        // Plaintext mode
        seastar::temporary_buffer<char> buf(data.size());
        std::memcpy(buf.get_write(), data.data(), data.size());
        return _channel->send(peer, std::span<seastar::temporary_buffer<char>>(&buf, 1));
    }

    auto* session = _dtls_context->get_or_create_session(peer, false);
    if (!session) {
        log_gossip_transport().debug("Failed to get DTLS session for peer {}", peer);
        return seastar::make_ready_future<>();
    }

    if (!session->is_established()) {
        log_gossip_transport().trace("DTLS handshake not complete for peer {}, cannot send yet", peer);
        return seastar::make_ready_future<>();
    }

    auto peer_copy = peer;
    return encrypt_with_offloading(session, data, peer).then([this, peer_copy](std::vector<uint8_t> encrypted) {
        if (encrypted.empty() || !_channel) {
            return seastar::make_ready_future<>();
        }

        seastar::temporary_buffer<char> buf(encrypted.size());
        std::memcpy(buf.get_write(), encrypted.data(), encrypted.size());
        return _channel->send(peer_copy, std::span<seastar::temporary_buffer<char>>(&buf, 1)).handle_exception([peer_copy](auto ep) {
            log_gossip_transport().debug("Failed to send encrypted data to {}: {}", peer_copy, ep);
        });
    });
}

seastar::future<> GossipTransport::broadcast(const std::vector<seastar::socket_address>& peers,
                                              const std::vector<uint8_t>& data) {
    if (peers.empty() || !_channel) {
        return seastar::make_ready_future<>();
    }

    // Plaintext mode - use parallel_for_each with send()
    if (!_dtls_context || !_dtls_context->is_enabled()) {
        return seastar::do_with(std::vector<uint8_t>(data), [this, &peers](std::vector<uint8_t>& plaintext_ref) {
            return seastar::parallel_for_each(peers, [this, &plaintext_ref](const seastar::socket_address& peer) {
                return send(peer, plaintext_ref);
            });
        });
    }

    // For high fan-out broadcasts, use seastar::async to batch the crypto work
    if (peers.size() > CRYPTO_OFFLOAD_PEER_THRESHOLD) {
        ++_crypto_batch_broadcasts;
        ++_crypto_ops_offloaded;

        return seastar::async([this,
                               plaintext_copy = std::vector<uint8_t>(data),
                               peers_copy = std::vector<seastar::socket_address>(peers)]() {
            if (!_dtls_context || !_dtls_context->is_enabled()) {
                return;
            }

            std::vector<std::pair<seastar::socket_address, std::vector<uint8_t>>> encrypted_packets;
            encrypted_packets.reserve(peers_copy.size());

            auto batch_start = std::chrono::steady_clock::now();

            for (const auto& peer : peers_copy) {
                if (!_dtls_context) {
                    break;
                }

                auto* session = _dtls_context->get_or_create_session(peer, false);
                if (!session || !session->is_established()) {
                    continue;
                }

                std::vector<uint8_t> encrypted;
                auto result = session->encrypt(plaintext_copy.data(), plaintext_copy.size(), encrypted);
                if (result == DtlsResult::SUCCESS && !encrypted.empty()) {
                    ++_dtls_packets_encrypted;
                    encrypted_packets.emplace_back(peer, std::move(encrypted));
                }
            }

            auto batch_elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - batch_start).count();

            uint64_t batch_threshold = CRYPTO_STALL_WARNING_US * peers_copy.size();
            if (static_cast<uint64_t>(batch_elapsed_us) > batch_threshold) {
                ++_crypto_stall_warnings;
                log_gossip_transport().warn("Crypto batch stall: encrypted {} peers in {}us (threshold: {}us)",
                                            encrypted_packets.size(), batch_elapsed_us, batch_threshold);
            }

            for (auto& [peer, encrypted] : encrypted_packets) {
                if (!_channel) {
                    break;
                }
                send_packet_async(peer, encrypted);
                seastar::thread::yield();
            }
        });
    }

    // For small peer counts, use parallel_for_each
    return seastar::do_with(std::vector<uint8_t>(data), [this, &peers](std::vector<uint8_t>& plaintext_ref) {
        return seastar::parallel_for_each(peers, [this, &plaintext_ref](const seastar::socket_address& peer) {
            return send(peer, plaintext_ref);
        });
    });
}

std::optional<std::vector<uint8_t>> GossipTransport::decrypt(const seastar::socket_address& peer,
                                                              const uint8_t* data, size_t len) {
    if (!_dtls_context || !_dtls_context->is_enabled()) {
        return std::vector<uint8_t>(data, data + len);
    }

    auto* session = _dtls_context->get_or_create_session(peer, true);
    if (!session) {
        log_gossip_transport().debug("Failed to get DTLS session for peer {}", peer);
        return std::nullopt;
    }

    // Handle handshake if session not established
    if (!session->is_established()) {
        std::vector<uint8_t> response;
        auto result = session->continue_handshake(data, len, response);

        if (result == DtlsResult::SUCCESS) {
            ++_dtls_handshakes_completed;
            log_gossip_transport().info("DTLS handshake completed with peer {}", peer);
        } else if (result == DtlsResult::ERROR) {
            ++_dtls_handshakes_failed;
            log_gossip_transport().error("DTLS handshake failed with peer {}: {}", peer, session->last_error());
            _dtls_context->remove_session(peer);
            return std::nullopt;
        }

        if (!response.empty()) {
            send_packet_async(peer, response);
        }

        return std::nullopt;
    }

    // Decrypt application data
    std::vector<uint8_t> decrypted;
    auto start = std::chrono::steady_clock::now();
    auto result = session->decrypt(data, len, decrypted);
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();

    if (static_cast<uint64_t>(elapsed_us) > CRYPTO_STALL_WARNING_US) {
        ++_crypto_stall_warnings;
        log_gossip_transport().warn("Crypto stall: decrypt took {}us for {} bytes from peer {}",
                                    elapsed_us, len, peer);
    }

    if (result != DtlsResult::SUCCESS) {
        if (result == DtlsResult::WANT_READ) {
            std::vector<uint8_t> response;
            session->continue_handshake(data, len, response);
            if (!response.empty()) {
                send_packet_async(peer, response);
            }
        }
        return std::nullopt;
    }

    ++_dtls_packets_decrypted;
    return decrypted;
}

seastar::future<> GossipTransport::initiate_handshake(const seastar::socket_address& peer) {
    if (!_dtls_context || !_channel) {
        return seastar::make_ready_future<>();
    }

    auto* session = _dtls_context->get_or_create_session(peer, false);
    if (!session) {
        return seastar::make_ready_future<>();
    }

    std::vector<uint8_t> handshake_data;
    auto result = session->initiate_handshake(handshake_data);

    if (result == DtlsResult::WANT_READ && !handshake_data.empty()) {
        ++_dtls_handshakes_started;
        send_packet_async(peer, handshake_data);
    }

    return seastar::make_ready_future<>();
}

bool GossipTransport::is_dtls_handshake_packet(const uint8_t* data, size_t len) const {
    if (len < DTLS_RECORD_HEADER_SIZE) {
        return false;
    }

    uint8_t content_type = data[0];
    bool is_handshake_type = (content_type == DTLS_CONTENT_CHANGE_CIPHER_SPEC ||
                              content_type == DTLS_CONTENT_ALERT ||
                              content_type == DTLS_CONTENT_HANDSHAKE);
    if (!is_handshake_type) {
        return false;
    }

    if (data[1] != DTLS_VERSION_MARKER) {
        return false;
    }

    return true;
}

bool GossipTransport::should_drop_mtls_lockdown(const seastar::socket_address& peer,
                                                 const uint8_t* data, size_t len) {
    if (!_config.mtls_enabled) {
        return false;
    }

    if (!_dtls_context || !_dtls_context->is_enabled()) {
        log_gossip_transport().warn("mTLS lockdown active but DTLS not initialized - dropping packet from {}", peer);
        ++_dtls_lockdown_drops;
        return true;
    }

    if (is_dtls_handshake_packet(data, len)) {
        return false;
    }

    auto* session = _dtls_context->get_or_create_session(peer, true);
    if (session && session->is_established()) {
        return false;
    }

    log_gossip_transport().debug("mTLS lockdown: dropping non-DTLS packet from {} (no established session)", peer);
    ++_dtls_lockdown_drops;
    return true;
}

void GossipTransport::send_packet_async(const seastar::socket_address& peer,
                                         const std::vector<uint8_t>& data) {
    if (!_channel || data.empty()) {
        return;
    }

    seastar::temporary_buffer<char> buf(data.size());
    std::memcpy(buf.get_write(), data.data(), data.size());

    (void)_channel->send(peer, std::span<seastar::temporary_buffer<char>>(&buf, 1)).handle_exception([peer](auto ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            log_gossip_transport().debug("Failed to send packet to {}: {}", peer, e.what());
        }
    });
}

std::vector<uint8_t> GossipTransport::encrypt_with_timing(DtlsSession* session,
                                                           const uint8_t* data, size_t len,
                                                           const seastar::socket_address& peer) {
    std::vector<uint8_t> encrypted;

    if (!session || !session->is_established()) {
        return encrypted;
    }

    auto start = std::chrono::steady_clock::now();
    auto result = session->encrypt(data, len, encrypted);
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();

    if (static_cast<uint64_t>(elapsed_us) > CRYPTO_STALL_WARNING_US) {
        ++_crypto_stall_warnings;
        log_gossip_transport().warn("Crypto stall: encrypt took {}us for {} bytes to peer {}",
                                    elapsed_us, len, peer);
    }

    if (result != DtlsResult::SUCCESS) {
        log_gossip_transport().debug("Encryption failed for peer {}: {}", peer, session->last_error());
        return {};
    }

    ++_dtls_packets_encrypted;
    return encrypted;
}

seastar::future<std::vector<uint8_t>> GossipTransport::encrypt_with_offloading(
    DtlsSession* session,
    const std::vector<uint8_t>& plaintext,
    const seastar::socket_address& peer) {

    if (!session || !session->is_established()) {
        return seastar::make_ready_future<std::vector<uint8_t>>(std::vector<uint8_t>{});
    }

    if (!_crypto_offloader || !_crypto_offloader->is_running()) {
        auto encrypted = encrypt_with_timing(session, plaintext.data(), plaintext.size(), peer);
        return seastar::make_ready_future<std::vector<uint8_t>>(std::move(encrypted));
    }

    CryptoOpType op_type = CryptoOpType::SYMMETRIC_ENCRYPT;

    if (!_crypto_offloader->should_offload(op_type, plaintext.size())) {
        auto encrypted = encrypt_with_timing(session, plaintext.data(), plaintext.size(), peer);
        return seastar::make_ready_future<std::vector<uint8_t>>(std::move(encrypted));
    }

    ++_crypto_ops_offloaded;

    auto plaintext_copy = seastar::make_lw_shared<std::vector<uint8_t>>(plaintext);
    auto peer_copy = peer;

    return _crypto_offloader->wrap_crypto_op(
        op_type,
        plaintext.size(),
        [this, peer_copy, plaintext_copy]() -> std::vector<uint8_t> {
            if (!_dtls_context || !_dtls_context->is_enabled()) {
                return {};
            }

            auto* session = _dtls_context->get_or_create_session(peer_copy, false);
            if (!session || !session->is_established()) {
                return {};
            }

            std::vector<uint8_t> encrypted;
            auto result = session->encrypt(plaintext_copy->data(), plaintext_copy->size(), encrypted);

            if (result != DtlsResult::SUCCESS) {
                log_gossip_transport().debug("Offloaded encryption failed for peer {}", peer_copy);
                return {};
            }

            return encrypted;
        }
    ).then([this](std::vector<uint8_t> encrypted) {
        if (!encrypted.empty()) {
            ++_dtls_packets_encrypted;
        }
        return seastar::make_ready_future<std::vector<uint8_t>>(std::move(encrypted));
    });
}

void GossipTransport::cleanup_dtls_sessions() {
    // RAII Timer Safety: Holder must outlive the work
    seastar::gate::holder timer_holder;
    try {
        timer_holder = _timer_gate.hold();
    } catch (const seastar::gate_closed_exception&) {
        return;
    }

    if (!_dtls_context) {
        return;
    }

    _dtls_context->cleanup_idle_sessions(std::chrono::seconds(300));
}

seastar::future<> GossipTransport::check_cert_reload() {
    if (!_dtls_context) {
        co_return;
    }

    if (!co_await _dtls_context->check_and_reload_certs()) {
        co_return;
    }

    ++_dtls_cert_reloads;
    log_gossip_transport().info("Certificates reloaded successfully");
    co_return;
}

void GossipTransport::register_metrics(seastar::metrics::metric_groups& metrics) {
    // Metrics are registered by GossipService using our accessors
    // This method is for registering crypto offloader metrics
    if (_crypto_offloader) {
        _crypto_offloader->register_metrics(metrics);
    }
}

void GossipTransport::sync_crypto_offloader_stats() {
    if (!_crypto_offloader) {
        return;
    }

    auto stats = _crypto_offloader->get_stats();
    _crypto_stalls_avoided += stats.stalls_avoided;
    _crypto_stall_warnings += stats.stall_warnings;
}

}  // namespace ranvier
