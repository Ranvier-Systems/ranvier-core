// Ranvier Core - Application Bootstrap and Lifecycle Implementation

#include "application.hpp"
#include "gossip_service.hpp"
#include "logging.hpp"
#include "metrics_service.hpp"
#include "tracing_service.hpp"

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>

#include <seastar/core/fstream.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/when_all.hh>
#include <seastar/net/inet_address.hh>

namespace ranvier {

Application::Application(RanvierConfig config, std::string config_path)
    : _config(std::move(config))
    , _config_path(std::move(config_path))
    , _state(ApplicationState::CREATED)
    , _stop_signal(std::make_shared<seastar::promise<>>()) {
}

// Destructor defined here where GossipService is complete (via gossip_service.hpp)
Application::~Application() = default;

seastar::future<> Application::init_tokenizer() {
    // Maximum tokenizer file size (100MB) - prevents OOM from misconfiguration
    static constexpr uint64_t MAX_TOKENIZER_SIZE = 100 * 1024 * 1024;
    const auto& path = _config.assets.tokenizer_path;

    // Early validation - fail fast on empty path
    if (path.empty()) {
        return seastar::make_exception_future<>(std::runtime_error("Tokenizer path is empty"));
    }

    // Use DMA for non-blocking disk I/O
    return seastar::open_file_dma(path, seastar::open_flags::ro).then([this, path](seastar::file f) {
        return f.size().then([this, f, path](size_t size) mutable {
            // Sanity check: Empty file
            if (size == 0) {
                return seastar::make_exception_future<>(
                    std::runtime_error("Tokenizer file is empty: " + path)
                );
            }

            // Safety check: Memory limit (100MB is a safe default for tokenizers)
            if (size > MAX_TOKENIZER_SIZE) {
                return seastar::make_exception_future<>(
                    std::runtime_error("Tokenizer file exceeds maximum allowed size (100MB): " + path)
                );
            }

            // Perform the optimized read
            return f.dma_read_bulk<char>(0, size).then([this, path](seastar::temporary_buffer<char> buf) {
                // Double check the buffer isn't empty (redundant but safe)
                if (buf.empty()) {
                    throw std::runtime_error("Failed to read data from tokenizer: " + path);
                }

                // Final hand-off to the service
                _tokenizer.load_from_json(std::string(buf.get(), buf.size()));
                log_main.info("Tokenizer loaded: {} bytes", buf.size());
                return seastar::make_ready_future<>();
            });
        }).finally([f]() mutable {
            return f.close();
        });
    }).handle_exception([path](auto ep) {
        log_main.error("Failed to load tokenizer from {}: {}", path, ep);
        return seastar::make_exception_future<>(ep);
    });
}

// =============================================================================
// Configuration Builders
// =============================================================================

// Static helper that can work with any RanvierConfig (used during hot-reload)
HttpControllerConfig Application::build_controller_config_from(const RanvierConfig& config) {
    HttpControllerConfig cfg;
    // Connection pool settings
    cfg.pool.max_connections_per_host = config.pool.max_connections_per_host;
    cfg.pool.idle_timeout = config.pool.idle_timeout;
    cfg.pool.max_total_connections = config.pool.max_total_connections;
    // Routing settings
    cfg.min_token_length = config.routing.min_token_length;
    cfg.enable_token_forwarding = config.routing.enable_token_forwarding;
    cfg.accept_client_tokens = config.routing.accept_client_tokens;
    cfg.max_token_id = config.routing.max_token_id;
    cfg.routing_mode = config.routing.routing_mode;
    // Timeout settings
    cfg.connect_timeout = config.timeouts.connect_timeout;
    cfg.request_timeout = config.timeouts.request_timeout;
    cfg.drain_timeout = config.shutdown.drain_timeout;
    // Auth settings
    cfg.auth = config.auth;
    // Rate limiting
    cfg.rate_limit.enabled = config.rate_limit.enabled;
    cfg.rate_limit.requests_per_second = config.rate_limit.requests_per_second;
    cfg.rate_limit.burst_size = config.rate_limit.burst_size;
    // Retry settings
    cfg.retry.max_retries = config.retry.max_retries;
    cfg.retry.initial_backoff = config.retry.initial_backoff;
    cfg.retry.max_backoff = config.retry.max_backoff;
    cfg.retry.backoff_multiplier = config.retry.backoff_multiplier;
    // Circuit breaker
    cfg.circuit_breaker.enabled = config.circuit_breaker.enabled;
    cfg.circuit_breaker.failure_threshold = config.circuit_breaker.failure_threshold;
    cfg.circuit_breaker.success_threshold = config.circuit_breaker.success_threshold;
    cfg.circuit_breaker.recovery_timeout = config.circuit_breaker.recovery_timeout;
    cfg.circuit_breaker.fallback_enabled = config.circuit_breaker.fallback_enabled;
    return cfg;
}

// Instance method delegates to static helper
HttpControllerConfig Application::build_controller_config() const {
    return build_controller_config_from(_config);
}

K8sDiscoveryConfig Application::build_k8s_config() const {
    K8sDiscoveryConfig cfg;
    cfg.enabled = _config.k8s_discovery.enabled;
    cfg.api_server = _config.k8s_discovery.api_server;
    cfg.namespace_name = _config.k8s_discovery.namespace_name;
    cfg.service_name = _config.k8s_discovery.service_name;
    cfg.target_port = _config.k8s_discovery.target_port;
    cfg.token_path = _config.k8s_discovery.token_path;
    cfg.ca_cert_path = _config.k8s_discovery.ca_cert_path;
    cfg.poll_interval = _config.k8s_discovery.poll_interval;
    cfg.watch_timeout = _config.k8s_discovery.watch_timeout;
    cfg.watch_reconnect_delay = _config.k8s_discovery.watch_reconnect_delay;
    cfg.watch_reconnect_max_delay = _config.k8s_discovery.watch_reconnect_max_delay;
    cfg.verify_tls = _config.k8s_discovery.verify_tls;
    cfg.label_selector = _config.k8s_discovery.label_selector;
    return cfg;
}

HealthServiceConfig Application::build_health_config() const {
    HealthServiceConfig cfg;
    cfg.check_interval = _config.health.check_interval;
    cfg.check_timeout = _config.health.check_timeout;
    cfg.failure_threshold = _config.health.failure_threshold;
    cfg.recovery_threshold = _config.health.recovery_threshold;
    return cfg;
}

AsyncPersistenceConfig Application::build_persistence_config() const {
    AsyncPersistenceConfig cfg;
    cfg.flush_interval = std::chrono::milliseconds(100);
    cfg.max_batch_size = 1000;
    cfg.max_queue_depth = 100000;
    cfg.enable_stats_logging = true;
    cfg.stats_interval = std::chrono::seconds(60);
    return cfg;
}

void Application::log_non_reloadable_changes(const RanvierConfig& new_config) const {
    // Server settings require restart (port bindings)
    if (new_config.server.api_port != _config.server.api_port ||
        new_config.server.metrics_port != _config.server.metrics_port ||
        new_config.server.bind_address != _config.server.bind_address) {
        log_main.warn("Config reload: server.* changes require restart to take effect");
    }

    // Database path cannot be changed at runtime
    if (new_config.database.path != _config.database.path) {
        log_main.warn("Config reload: database.path changes require restart to take effect");
    }

    // TLS settings require server restart
    if (new_config.tls.enabled != _config.tls.enabled ||
        new_config.tls.cert_path != _config.tls.cert_path ||
        new_config.tls.key_path != _config.tls.key_path) {
        log_main.warn("Config reload: tls.* changes require restart to take effect");
    }

    // Tokenizer is loaded once at startup
    if (new_config.assets.tokenizer_path != _config.assets.tokenizer_path) {
        log_main.warn("Config reload: assets.tokenizer_path changes require restart to take effect");
    }

    // Cluster mode cannot be toggled at runtime
    if (new_config.cluster.enabled != _config.cluster.enabled) {
        log_main.warn("Config reload: cluster.enabled changes require restart to take effect");
    }

    // Telemetry initialization is one-time
    if (new_config.telemetry.enabled != _config.telemetry.enabled) {
        log_main.warn("Config reload: telemetry.enabled changes require restart to take effect");
    }
}

// =============================================================================
// Service Initialization
// =============================================================================

seastar::future<> Application::init_persistence() {
    // Create async persistence manager (which creates and owns the underlying SQLite store)
    _async_persistence = std::make_unique<AsyncPersistenceManager>(build_persistence_config());

    // Open the database
    if (!_async_persistence->open(_config.database.path)) {
        log_main.warn("Failed to open persistence store - running without persistence");
        _async_persistence.reset();
        return seastar::make_ready_future<>();
    }

    log_main.info("Persistence initialized (SQLite: {})", _config.database.path);

    // Checkpoint WAL on startup to ensure clean state after potential crash
    if (_async_persistence->checkpoint()) {
        log_main.debug("Persistence WAL checkpoint complete");
    } else {
        log_main.warn("Persistence WAL checkpoint failed - continuing anyway");
    }

    // Start async persistence manager (arms flush timer)
    return _async_persistence->start().then([this] {
        // Distribute async persistence manager to all HttpController shards
        return _controller.invoke_on_all([this](HttpController& c) {
            c.set_persistence(_async_persistence.get());
        });
    });
}

seastar::future<> Application::load_persisted_state() {
    if (!_async_persistence || !_async_persistence->is_open()) {
        log_main.info("No persistence store - starting with empty state");
        return seastar::make_ready_future<>();
    }

    // Step 1: Verify database integrity
    if (!_async_persistence->verify_integrity()) {
        log_main.error("Persistence integrity check failed - clearing corrupted state");
        _async_persistence->clear_all();
        log_main.info("Persistence store cleared - starting fresh");
        return seastar::make_ready_future<>();
    }

    // Step 2: Load backends and routes
    auto backends = _async_persistence->load_backends();
    auto routes = _async_persistence->load_routes();

    size_t skipped = _async_persistence->last_load_skipped_count();
    if (skipped > 0) {
        log_main.warn("Skipped {} corrupted route records during load", skipped);
    }

    if (backends.empty() && routes.empty()) {
        log_main.info("Persistence store is empty - starting fresh");
        return seastar::make_ready_future<>();
    }

    log_main.info("Restoring state from persistence:");
    log_main.info("  Backends: {}", backends.size());
    log_main.info("  Routes:   {} (skipped {} corrupted)", routes.size(), skipped);

    for (const auto& rec : backends) {
        log_main.info("  - Backend {} -> {}:{} (weight={}, priority={})",
            rec.id, rec.ip, rec.port, rec.weight, rec.priority);
    }

    // Step 3: Restore backends first, then routes
    return seastar::do_with(std::move(backends), std::move(routes),
                            size_t(0), size_t(0),
        [this](auto& backends, auto& routes, size_t& failed_backends, size_t& failed_routes) {
            return seastar::do_for_each(backends, [this, &failed_backends](const BackendRecord& rec) {
                seastar::socket_address addr(seastar::ipv4_addr(rec.ip, rec.port));
                return _router->register_backend_global(rec.id, addr, rec.weight, rec.priority)
                    .handle_exception([&failed_backends, id = rec.id](auto ep) {
                        failed_backends++;
                        try {
                            std::rethrow_exception(ep);
                        } catch (const std::exception& e) {
                            log_main.warn("Failed to restore backend {}: {}", id, e.what());
                        }
                        return seastar::make_ready_future<>();
                    });
            }).then([this, &routes, &failed_routes] {
                return seastar::do_for_each(routes, [this, &failed_routes](const RouteRecord& rec) {
                    auto tokens_copy = rec.tokens;
                    return _router->learn_route_global(std::move(tokens_copy), rec.backend_id)
                        .handle_exception([&failed_routes](auto ep) {
                            failed_routes++;
                            try {
                                std::rethrow_exception(ep);
                            } catch (const std::exception& e) {
                                log_main.debug("Failed to restore route: {}", e.what());
                            }
                            return seastar::make_ready_future<>();
                        });
                });
            }).then([&failed_backends, &failed_routes] {
                if (failed_backends > 0 || failed_routes > 0) {
                    log_main.warn("State restoration completed with errors: "
                                  "{} backend failures, {} route failures",
                                  failed_backends, failed_routes);
                } else {
                    log_main.info("State restoration complete");
                }
            });
        }).handle_exception([](auto ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                log_main.error("State restoration failed: {} - starting with empty state", e.what());
            }
            return seastar::make_ready_future<>();
        });
}

void Application::init_health_checker() {
    _health_checker = std::make_unique<HealthService>(*_router, build_health_config());
    _health_checker->start();
}

void Application::init_k8s_discovery() {
    if (!_config.k8s_discovery.enabled) {
        return;
    }

    _k8s_discovery = std::make_unique<K8sDiscoveryService>(build_k8s_config());

    // Connect K8s discovery to router via callbacks
    _k8s_discovery->set_register_callback(
        [this](BackendId id, seastar::socket_address addr, uint32_t weight, uint32_t priority) {
            return _router->register_backend_global(id, addr, weight, priority);
        });
    _k8s_discovery->set_drain_callback(
        [this](BackendId id) {
            return _router->drain_backend_global(id);
        });

    log_main.info("K8s discovery configured for {}/{}",
        _config.k8s_discovery.namespace_name, _config.k8s_discovery.service_name);
}

seastar::future<> Application::setup_tls() {
    if (!_config.tls.enabled) {
        return seastar::make_ready_future<>();
    }

    if (_config.tls.cert_path.empty() || _config.tls.key_path.empty()) {
        log_main.error("TLS enabled but cert_path or key_path not configured");
        return seastar::make_exception_future<>(std::runtime_error("TLS configuration incomplete"));
    }

    auto creds_builder = seastar::make_shared<seastar::tls::credentials_builder>();
    return creds_builder->set_x509_key_file(
        _config.tls.cert_path,
        _config.tls.key_path,
        seastar::tls::x509_crt_format::PEM
    ).then([this, creds_builder] {
        _tls_creds = creds_builder->build_server_credentials();
        log_main.info("TLS credentials loaded successfully");
    }).handle_exception([](auto ep) {
        log_main.error("Failed to load TLS credentials");
        return seastar::make_exception_future<>(ep);
    });
}

// =============================================================================
// Lifecycle Management
// =============================================================================

seastar::future<> Application::startup() {
    if (_state != ApplicationState::CREATED) {
        return seastar::make_exception_future<>(
            std::runtime_error("startup() can only be called once"));
    }

    _state = ApplicationState::STARTING;

    // Use the gate to track startup completion
    return seastar::try_with_gate(_lifecycle_gate, [this] {
        // 1. Initialize sharded config - distribute config to all CPU cores
        // This provides lock-free per-core access to configuration
        return _sharded_config.start(ShardedConfig(_config)).then([this] {
            _sharded_config_started = true;
            log_main.info("Sharded config initialized on {} cores", seastar::smp::count);
        }).then([this] {
            // 2. Initialize tokenizer (async with DMA file I/O) - failure is fatal
            return init_tokenizer();
        }).then([this] {
            // 3. Initialize OpenTelemetry tracing
            TracingService::init(_config.telemetry);
            if (_config.telemetry.enabled) {
                log_main.info("OpenTelemetry tracing enabled (endpoint: {}, sample_rate: {:.2f})",
                    _config.telemetry.otlp_endpoint, _config.telemetry.sample_rate);
            } else {
                log_main.info("OpenTelemetry tracing disabled");
            }

            // 4. Initialize router
            _router = std::make_unique<RouterService>(_config.routing, _config.cluster);
            if (_config.cluster.enabled) {
                log_main.info("Cluster mode: {} peers on port {}",
                    _config.cluster.peers.size(), _config.cluster.gossip_port);
            }

            // 5. Build controller config
            auto ctrl_config = build_controller_config();

            // 6. Initialize router's thread-local data on all shards
            return _router->initialize_shards().then([this, ctrl_config] {
                // 7. Start HttpController on all shards with config reference
                return _controller.start(std::ref(_tokenizer), std::ref(*_router), ctrl_config);
            });
        }).then([this] {
            _controller_started = true;
        }).then([this] {
            // 8. Initialize metrics on ALL shards
            return seastar::smp::invoke_on_all([] {
                init_metrics();
            });
        }).then([this] {
            // 9. Initialize persistence
            return init_persistence();
        }).then([this] {
            // 10. Initialize health checker
            init_health_checker();

            // 11. Initialize K8s discovery (if enabled)
            init_k8s_discovery();

            // 12. Start gossip service if cluster mode is enabled
            return _router->start_gossip();
        }).then([this] {
            // 13. Load persisted state
            return load_persisted_state();
        }).then([this] {
            // 14. Start K8s discovery service if enabled
            if (_k8s_discovery && _k8s_discovery->is_enabled()) {
                return _k8s_discovery->start();
            }
            return seastar::make_ready_future<>();
        }).then([this] {
            _state = ApplicationState::RUNNING;
            log_main.info("Application startup complete");
        }).handle_exception([this](auto ep) {
            // Startup failed - log and re-throw
            // State remains STARTING, which signals partial initialization
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                log_main.error("Application startup failed: {}", e.what());
            }
            return seastar::make_exception_future<>(ep);
        });
    });
}

// =============================================================================
// HTTP Server Management
// =============================================================================

seastar::future<> Application::start_servers() {
    _api_server = std::make_unique<seastar::httpd::http_server_control>();
    _metrics_server = std::make_unique<seastar::httpd::http_server_control>();

    return setup_tls().then([this] {
        // Start Prometheus metrics server
        return _metrics_server->start();
    }).then([this] {
        seastar::prometheus::config pconf;
        pconf.metric_help = "Ranvier AI Router";
        return seastar::prometheus::start(*_metrics_server, pconf);
    }).then([this] {
        auto addr = seastar::socket_address(
            seastar::ipv4_addr(_config.server.bind_address, _config.server.metrics_port));
        return _metrics_server->listen(addr);
    }).then([this] {
        log_main.info("Prometheus metrics listening on {}:{}",
            _config.server.bind_address, _config.server.metrics_port);

        // Start API server
        return _api_server->start();
    }).then([this] {
        return _api_server->set_routes([this](seastar::httpd::routes& r) {
            _controller.local().register_routes(r);
        });
    }).then([this] {
        auto addr = seastar::socket_address(
            seastar::ipv4_addr(_config.server.bind_address, _config.server.api_port));
        if (_tls_creds) {
            return _api_server->listen(addr, _tls_creds);
        }
        return _api_server->listen(addr);
    }).then([this] {
        auto protocol = _config.tls.enabled ? "https" : "http";
        log_main.info("Ranvier listening on {}://{}:{}",
            protocol, _config.server.bind_address, _config.server.api_port);
    });
}

seastar::future<> Application::stop_servers() {
    seastar::future<> api_stop = seastar::make_ready_future<>();
    seastar::future<> metrics_stop = seastar::make_ready_future<>();

    if (_api_server) {
        api_stop = _api_server->stop();
    }
    if (_metrics_server) {
        metrics_stop = _metrics_server->stop();
    }

    return seastar::when_all(std::move(api_stop), std::move(metrics_stop)).discard_result();
}

void Application::setup_signal_handlers() {
    // =========================================================================
    // SIGHUP handler - Configuration hot-reload
    // =========================================================================
    // Uses Seastar-native signal handling which integrates with the reactor.
    // The handler runs within the Seastar event loop context, allowing us to
    // return futures and use async operations safely.
    seastar::engine().handle_signal(SIGHUP, [this] {
        log_main.info("SIGHUP received - triggering configuration reload");
        // reload_config() uses sharded<HttpController>::invoke_on_all to
        // propagate configuration changes across all CPU cores
        (void)reload_config().handle_exception([](auto ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                log_main.error("Config reload exception: {}", e.what());
            }
        });
    });
    log_main.info("SIGHUP handler registered for config hot-reload");

    // Set up config reload callback for /admin/keys/reload endpoint
    (void)_controller.invoke_on_all([](HttpController& c) {
        c.set_config_reload_callback([]() {
            if (raise(SIGHUP) == 0) {
                log_main.info("Config reload triggered via API endpoint");
                return true;
            }
            log_main.error("Failed to send SIGHUP signal");
            return false;
        });
    });
    log_main.info("Config reload callback registered for /admin/keys/reload endpoint");

    // =========================================================================
    // SIGINT handler - Graceful shutdown with hard kill on second signal
    // =========================================================================
    // First SIGINT: Initiates graceful shutdown sequence
    //   - Sets state to DRAINING
    //   - Drains in-flight requests
    //   - Resolves _stop_signal promise to unblock run()
    // Second SIGINT: Hard kill (immediate exit)
    //   - For when graceful shutdown is stuck or taking too long
    //   - Uses std::exit(1) to terminate immediately
    // Note: If already shutting down (e.g., from SIGTERM), first SIGINT
    //       triggers hard kill since graceful shutdown is already in progress.
    seastar::engine().handle_signal(SIGINT, [this] {
        int count = _sigint_count.fetch_add(1, std::memory_order_relaxed) + 1;

        if (count == 1 && !is_shutting_down()) {
            // First SIGINT and not already shutting down - graceful shutdown
            log_main.info("SIGINT received - initiating graceful shutdown");
            log_main.info("Press Ctrl+C again to force immediate termination");
            signal_shutdown();
        } else {
            // Either: second+ SIGINT, or first SIGINT but already shutting down
            // In both cases, user wants immediate termination
            log_main.warn("Forcing immediate termination (signal count: {}, shutting_down: {})",
                          count, is_shutting_down());
            // Flush output streams to ensure log message is visible
            std::cerr.flush();
            std::cout.flush();
            std::exit(1);
        }
    });
    log_main.info("SIGINT handler registered (graceful shutdown, double for hard kill)");

    // =========================================================================
    // SIGTERM handler - Graceful shutdown (no hard kill option)
    // =========================================================================
    // SIGTERM is typically sent by process managers (systemd, k8s) and should
    // always trigger a graceful shutdown. Unlike SIGINT, we don't support
    // hard kill on repeated SIGTERM since process managers have their own
    // escalation (SIGKILL after timeout).
    seastar::engine().handle_signal(SIGTERM, [this] {
        log_main.info("SIGTERM received - initiating graceful shutdown");
        signal_shutdown();
    });
    log_main.info("SIGTERM handler registered (graceful shutdown)");
}

void Application::signal_shutdown() {
    if (_state == ApplicationState::DRAINING ||
        _state == ApplicationState::STOPPING ||
        _state == ApplicationState::STOPPED) {
        return;  // Already shutting down
    }

    log_main.info("Shutdown signal received - initiating graceful shutdown");
    _state = ApplicationState::DRAINING;

    // Start draining on ALL shards (reject new requests)
    (void)drain_requests().then([this] {
        _stop_signal->set_value();
    }).handle_exception([this](auto ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            log_main.error("Error during drain: {}", e.what());
        }
        // Still proceed with shutdown even on error
        _stop_signal->set_value();
    });
}

seastar::future<> Application::drain_requests() {
    // Only drain if controller was successfully started
    if (!_controller_started) {
        return seastar::make_ready_future<>();
    }

    return _controller.invoke_on_all([](HttpController& c) {
        c.start_draining();
    }).then([this] {
        return _controller.invoke_on_all([](HttpController& c) {
            return c.wait_for_drain();
        });
    }).handle_exception([](auto ep) {
        // Log but don't fail - we're shutting down anyway
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            log_main.warn("Error during request drain: {}", e.what());
        }
        return seastar::make_ready_future<>();
    });
}

seastar::future<> Application::run() {
    if (_state != ApplicationState::RUNNING) {
        return seastar::make_exception_future<>(
            std::runtime_error("run() can only be called after startup() completes"));
    }

    return start_servers().then([this] {
        setup_signal_handlers();

        // Wait for shutdown signal
        return _stop_signal->get_future();
    }).then([this] {
        log_main.info("Drain complete - stopping Ranvier...");
        return shutdown();
    });
}

// =============================================================================
// Shutdown Management
// =============================================================================

seastar::future<> Application::stop_services() {
    _state = ApplicationState::STOPPING;

    // Build the shutdown chain with null checks for partially-initialized state
    seastar::future<> chain = seastar::make_ready_future<>();

    // Stop Health Checker FIRST (if initialized)
    if (_health_checker) {
        chain = chain.then([this] {
            return _health_checker->stop();
        });
    }

    // Stop K8s discovery service (if initialized and enabled)
    chain = chain.then([this] {
        if (_k8s_discovery && _k8s_discovery->is_enabled()) {
            return _k8s_discovery->stop();
        }
        return seastar::make_ready_future<>();
    });

    // Stop gossip service (if router initialized)
    chain = chain.then([this] {
        if (_router) {
            return _router->stop_gossip();
        }
        return seastar::make_ready_future<>();
    });

    // Stop HTTP servers
    chain = chain.then([this] {
        return stop_servers();
    });

    // Stop the sharded HttpController (only if it was started)
    if (_controller_started) {
        chain = chain.then([this] {
            return _controller.stop();
        });
    }

    // Stop async persistence manager
    chain = chain.then([this] {
        if (_async_persistence) {
            return _async_persistence->stop().then([] {
                log_main.info("Async persistence manager stopped (queue flushed)");
            });
        }
        return seastar::make_ready_future<>();
    });

    // Stop sharded config (if started)
    if (_sharded_config_started) {
        chain = chain.then([this] {
            return _sharded_config.stop().then([] {
                log_main.info("Sharded config stopped");
            });
        });
    }

    // Final cleanup
    return chain.finally([this] {
        cleanup();
    });
}

void Application::cleanup() {
    // Log shutdown summary and close persistence
    if (_async_persistence && _async_persistence->is_open()) {
        log_main.info("Shutdown summary:");
        log_main.info("  Persisted backends: {}", _async_persistence->backend_count());
        log_main.info("  Persisted routes:   {}", _async_persistence->route_count());

        // Checkpoint WAL before closing
        if (_async_persistence->checkpoint()) {
            log_main.debug("Final WAL checkpoint complete");
        } else {
            log_main.warn("Final WAL checkpoint failed - some data may be in WAL");
        }

        _async_persistence->close();
        log_main.info("Persistence store closed");
    }

    // Shutdown OpenTelemetry tracing
    if (TracingService::is_enabled()) {
        TracingService::shutdown();
        log_main.info("OpenTelemetry tracing shutdown complete");
    }

    _state = ApplicationState::STOPPED;
}

seastar::future<> Application::shutdown() {
    // Wait for startup to complete before shutting down
    return _lifecycle_gate.close().then([this] {
        return stop_services();
    });
}

// =============================================================================
// Configuration Hot-Reload
// =============================================================================

seastar::future<> Application::reload_config() {
    log_main.info("Reloading configuration from {}", _config_path);

    // Load and validate new configuration synchronously
    // This happens before any state is modified, so failures are safe
    RanvierConfig new_config;
    try {
        new_config = RanvierConfig::load(_config_path);
    } catch (const std::exception& e) {
        log_main.error("Config reload failed - could not load file: {}", e.what());
        return seastar::make_ready_future<>();
    }

    // Validate the new configuration
    auto validation_error = RanvierConfig::validate(new_config);
    if (validation_error) {
        log_main.error("Config reload failed - validation error: {}", *validation_error);
        return seastar::make_ready_future<>();
    }

    // Log warnings for settings that cannot be hot-reloaded
    log_non_reloadable_changes(new_config);

    // Use shared_ptr to avoid copying config N times for N shards
    auto config_ptr = std::make_shared<RanvierConfig>(std::move(new_config));

    // Step 1: Update the sharded config across all cores using invoke_on_all
    // This ensures each shard has the updated configuration for lock-free access
    return _sharded_config.invoke_on_all([config_ptr](ShardedConfig& cfg) {
        cfg.update(*config_ptr);
    }).then([this, config_ptr] {
        log_main.debug("Sharded config updated on all {} cores", seastar::smp::count);

        // Step 2: Build updated controller config from the new config
        auto ctrl_config = build_controller_config_from(*config_ptr);

        // Step 3: Update HttpController config on all shards
        return _controller.invoke_on_all([ctrl_config](HttpController& c) {
            c.update_config(ctrl_config);
        });
    }).then([this, config_ptr] {
        // Step 4: Update routing config on all shards via RouterService
        return _router->update_routing_config(config_ptr->routing);
    }).then([this, config_ptr] {
        // Step 5: Only update master config after all shards succeeded
        // This ensures consistency - if any step fails, master config is unchanged
        _config = *config_ptr;
        log_main.info("Configuration reloaded successfully on all cores");
    }).handle_exception([](auto ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            log_main.error("Config reload failed during propagation: {}", e.what());
        }
        // Don't re-throw - config reload failure shouldn't crash the server
    });
}

}  // namespace ranvier
