// Ranvier Core - Application Bootstrap and Lifecycle Implementation

#include "application.hpp"
#include "gossip_service.hpp"
#include "logging.hpp"
#include "metrics_service.hpp"
#include "sqlite_persistence.hpp"
#include "tracing_service.hpp"

#include <csignal>
#include <fstream>

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

void Application::init_tokenizer() {
    std::ifstream t(_config.assets.tokenizer_path);
    if (!t.is_open()) {
        throw std::runtime_error("Could not find tokenizer: " + _config.assets.tokenizer_path);
    }
    std::string json_str((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());
    _tokenizer.load_from_json(json_str);
    log_main.info("Services initialized (Tokenizer: {})", _config.assets.tokenizer_path);
}

HttpControllerConfig Application::build_controller_config() const {
    HttpControllerConfig ctrl_config;
    ctrl_config.pool.max_connections_per_host = _config.pool.max_connections_per_host;
    ctrl_config.pool.idle_timeout = _config.pool.idle_timeout;
    ctrl_config.pool.max_total_connections = _config.pool.max_total_connections;
    ctrl_config.min_token_length = _config.routing.min_token_length;
    ctrl_config.connect_timeout = _config.timeouts.connect_timeout;
    ctrl_config.request_timeout = _config.timeouts.request_timeout;
    ctrl_config.auth = _config.auth;
    ctrl_config.rate_limit.enabled = _config.rate_limit.enabled;
    ctrl_config.rate_limit.requests_per_second = _config.rate_limit.requests_per_second;
    ctrl_config.rate_limit.burst_size = _config.rate_limit.burst_size;
    ctrl_config.retry.max_retries = _config.retry.max_retries;
    ctrl_config.retry.initial_backoff = _config.retry.initial_backoff;
    ctrl_config.retry.max_backoff = _config.retry.max_backoff;
    ctrl_config.retry.backoff_multiplier = _config.retry.backoff_multiplier;
    ctrl_config.circuit_breaker.enabled = _config.circuit_breaker.enabled;
    ctrl_config.circuit_breaker.failure_threshold = _config.circuit_breaker.failure_threshold;
    ctrl_config.circuit_breaker.success_threshold = _config.circuit_breaker.success_threshold;
    ctrl_config.circuit_breaker.recovery_timeout = _config.circuit_breaker.recovery_timeout;
    ctrl_config.circuit_breaker.fallback_enabled = _config.circuit_breaker.fallback_enabled;
    ctrl_config.drain_timeout = _config.shutdown.drain_timeout;
    ctrl_config.enable_token_forwarding = _config.routing.enable_token_forwarding;
    ctrl_config.accept_client_tokens = _config.routing.accept_client_tokens;
    ctrl_config.max_token_id = _config.routing.max_token_id;
    ctrl_config.routing_mode = _config.routing.routing_mode;
    return ctrl_config;
}

seastar::future<> Application::init_persistence() {
    _persistence = create_persistence_store();
    if (_persistence->open(_config.database.path)) {
        log_main.info("Persistence initialized (SQLite: {})", _config.database.path);

        // Checkpoint WAL on startup to ensure clean state after potential crash
        if (_persistence->checkpoint()) {
            log_main.debug("Persistence WAL checkpoint complete");
        } else {
            log_main.warn("Persistence WAL checkpoint failed - continuing anyway");
        }

        // Create async persistence manager
        AsyncPersistenceConfig async_config;
        async_config.flush_interval = std::chrono::milliseconds(100);
        async_config.max_batch_size = 1000;
        async_config.max_queue_depth = 100000;
        async_config.enable_stats_logging = true;
        async_config.stats_interval = std::chrono::seconds(60);

        _async_persistence = std::make_unique<AsyncPersistenceManager>(async_config);
        _async_persistence->set_persistence_store(_persistence.get());

        // Start the async persistence background flush loop
        return _async_persistence->start().then([this] {
            // Set async persistence on all shards
            return _controller.invoke_on_all([this](HttpController& c) {
                c.set_persistence(_async_persistence.get());
            });
        });
    } else {
        log_main.warn("Failed to open persistence store - running without persistence");
        return seastar::make_ready_future<>();
    }
}

seastar::future<> Application::load_persisted_state() {
    if (!_persistence || !_persistence->is_open()) {
        log_main.info("No persistence store - starting with empty state");
        return seastar::make_ready_future<>();
    }

    // Step 1: Verify database integrity
    if (!_persistence->verify_integrity()) {
        log_main.error("Persistence integrity check failed - clearing corrupted state");
        _persistence->clear_all();
        log_main.info("Persistence store cleared - starting fresh");
        return seastar::make_ready_future<>();
    }

    // Step 2: Load backends and routes
    auto backends = _persistence->load_backends();
    auto routes = _persistence->load_routes();

    size_t skipped = _persistence->last_load_skipped_count();
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
    HealthServiceConfig health_config;
    health_config.check_interval = _config.health.check_interval;
    health_config.check_timeout = _config.health.check_timeout;
    health_config.failure_threshold = _config.health.failure_threshold;
    health_config.recovery_threshold = _config.health.recovery_threshold;
    _health_checker = std::make_unique<HealthService>(*_router, health_config);
    _health_checker->start();
}

void Application::init_k8s_discovery() {
    if (!_config.k8s_discovery.enabled) {
        return;
    }

    K8sDiscoveryConfig k8s_config;
    k8s_config.enabled = _config.k8s_discovery.enabled;
    k8s_config.api_server = _config.k8s_discovery.api_server;
    k8s_config.namespace_name = _config.k8s_discovery.namespace_name;
    k8s_config.service_name = _config.k8s_discovery.service_name;
    k8s_config.target_port = _config.k8s_discovery.target_port;
    k8s_config.token_path = _config.k8s_discovery.token_path;
    k8s_config.ca_cert_path = _config.k8s_discovery.ca_cert_path;
    k8s_config.poll_interval = _config.k8s_discovery.poll_interval;
    k8s_config.watch_timeout = _config.k8s_discovery.watch_timeout;
    k8s_config.watch_reconnect_delay = _config.k8s_discovery.watch_reconnect_delay;
    k8s_config.watch_reconnect_max_delay = _config.k8s_discovery.watch_reconnect_max_delay;
    k8s_config.verify_tls = _config.k8s_discovery.verify_tls;
    k8s_config.label_selector = _config.k8s_discovery.label_selector;

    _k8s_discovery = std::make_unique<K8sDiscoveryService>(k8s_config);

    // Connect K8s discovery to router
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

seastar::future<> Application::startup() {
    if (_state != ApplicationState::CREATED) {
        return seastar::make_exception_future<>(
            std::runtime_error("startup() can only be called once"));
    }

    _state = ApplicationState::STARTING;

    // Use the gate to track startup completion
    return seastar::try_with_gate(_lifecycle_gate, [this] {
        // 1. Initialize tokenizer (synchronous)
        try {
            init_tokenizer();
        } catch (const std::exception& e) {
            log_main.error("Service init failed: {}", e.what());
            return seastar::make_ready_future<>();
        }

        // 2. Initialize OpenTelemetry tracing
        TracingService::init(_config.telemetry);
        if (_config.telemetry.enabled) {
            log_main.info("OpenTelemetry tracing enabled (endpoint: {}, sample_rate: {:.2f})",
                _config.telemetry.otlp_endpoint, _config.telemetry.sample_rate);
        } else {
            log_main.info("OpenTelemetry tracing disabled");
        }

        // 3. Initialize router
        _router = std::make_unique<RouterService>(_config.routing, _config.cluster);
        if (_config.cluster.enabled) {
            log_main.info("Cluster mode: {} peers on port {}",
                _config.cluster.peers.size(), _config.cluster.gossip_port);
        }

        // 4. Build controller config
        auto ctrl_config = build_controller_config();

        // 5. Initialize router's thread-local data on all shards
        return _router->initialize_shards().then([this, ctrl_config] {
            // 6. Start HttpController on all shards
            return _controller.start(std::ref(_tokenizer), std::ref(*_router), ctrl_config);
        }).then([this] {
            // 7. Initialize metrics on ALL shards
            return seastar::smp::invoke_on_all([] {
                init_metrics();
            });
        }).then([this] {
            // 8. Initialize persistence
            return init_persistence();
        }).then([this] {
            // 9. Initialize health checker
            init_health_checker();

            // 10. Initialize K8s discovery (if enabled)
            init_k8s_discovery();

            // 11. Start gossip service if cluster mode is enabled
            return _router->start_gossip();
        }).then([this] {
            // 12. Load persisted state
            return load_persisted_state();
        }).then([this] {
            // 13. Start K8s discovery service if enabled
            if (_k8s_discovery && _k8s_discovery->is_enabled()) {
                return _k8s_discovery->start();
            }
            return seastar::make_ready_future<>();
        }).then([this] {
            _state = ApplicationState::RUNNING;
            log_main.info("Application startup complete");
        });
    });
}

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
    // SIGHUP handler for config hot-reload
    seastar::engine().handle_signal(SIGHUP, [this] {
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

    // Graceful shutdown handler
    auto graceful_shutdown = [this]() {
        signal_shutdown();
    };

    seastar::engine().handle_signal(SIGINT, graceful_shutdown);
    seastar::engine().handle_signal(SIGTERM, graceful_shutdown);
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
    return _controller.invoke_on_all([](HttpController& c) {
        c.start_draining();
    }).then([this] {
        return _controller.invoke_on_all([](HttpController& c) {
            return c.wait_for_drain();
        });
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

seastar::future<> Application::stop_services() {
    _state = ApplicationState::STOPPING;

    // Stop Health Checker FIRST
    return _health_checker->stop().then([this] {
        // Stop K8s discovery service
        if (_k8s_discovery && _k8s_discovery->is_enabled()) {
            return _k8s_discovery->stop();
        }
        return seastar::make_ready_future<>();
    }).then([this] {
        // Stop gossip service
        return _router->stop_gossip();
    }).then([this] {
        // Stop HTTP servers
        return stop_servers();
    }).then([this] {
        // Stop the sharded HttpController
        return _controller.stop();
    }).then([this] {
        // Stop async persistence manager
        if (_async_persistence) {
            return _async_persistence->stop().then([] {
                log_main.info("Async persistence manager stopped (queue flushed)");
            });
        }
        return seastar::make_ready_future<>();
    }).finally([this] {
        cleanup();
    });
}

void Application::cleanup() {
    // Log shutdown summary and close persistence
    if (_persistence && _persistence->is_open()) {
        log_main.info("Shutdown summary:");
        log_main.info("  Persisted backends: {}", _persistence->backend_count());
        log_main.info("  Persisted routes:   {}", _persistence->route_count());

        // Checkpoint WAL before closing
        if (_persistence->checkpoint()) {
            log_main.debug("Final WAL checkpoint complete");
        } else {
            log_main.warn("Final WAL checkpoint failed - some data may be in WAL");
        }

        _persistence->close();
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

seastar::future<> Application::reload_config() {
    log_main.info("SIGHUP received - reloading configuration from {}", _config_path);

    try {
        // Load new configuration
        auto new_config = RanvierConfig::load(_config_path);

        // Validate the new configuration
        auto validation_error = RanvierConfig::validate(new_config);
        if (validation_error) {
            log_main.error("Config reload failed - validation error: {}", *validation_error);
            return seastar::make_ready_future<>();
        }

        // Log warnings for settings that cannot be hot-reloaded
        if (new_config.server.api_port != _config.server.api_port ||
            new_config.server.metrics_port != _config.server.metrics_port ||
            new_config.server.bind_address != _config.server.bind_address) {
            log_main.warn("Config reload: server.* changes require restart to take effect");
        }
        if (new_config.database.path != _config.database.path) {
            log_main.warn("Config reload: database.path changes require restart to take effect");
        }
        if (new_config.tls.enabled != _config.tls.enabled ||
            new_config.tls.cert_path != _config.tls.cert_path ||
            new_config.tls.key_path != _config.tls.key_path) {
            log_main.warn("Config reload: tls.* changes require restart to take effect");
        }
        if (new_config.assets.tokenizer_path != _config.assets.tokenizer_path) {
            log_main.warn("Config reload: assets.tokenizer_path changes require restart to take effect");
        }

        // Update stored config
        _config = new_config;

        // Build updated controller config
        auto ctrl_config = build_controller_config();

        // Update HttpController config on all shards
        return _controller.invoke_on_all([ctrl_config](HttpController& c) {
            c.update_config(ctrl_config);
        }).then([this] {
            // Update routing config on all shards
            return _router->update_routing_config(_config.routing);
        }).then([] {
            log_main.info("Configuration reloaded successfully");
        });

    } catch (const std::exception& e) {
        log_main.error("Config reload failed: {}", e.what());
        return seastar::make_ready_future<>();
    }
}

}  // namespace ranvier
