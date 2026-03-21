// Ranvier Core - Application Bootstrap and Lifecycle Implementation

#include "application.hpp"
#include "gossip_service.hpp"
#include "logging.hpp"
#include "metrics_service.hpp"
#include "shard_load_metrics.hpp"
#include "tracing_service.hpp"

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>

#include <seastar/core/fstream.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/net/inet_address.hh>

namespace ranvier {

Application::Application(RanvierConfig config, std::string config_path)
    : _config(std::move(config))
    , _config_path(std::move(config_path))
    , _state(ApplicationState::CREATED)
    , _stop_signal(std::make_unique<seastar::promise<>>()) {
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

                // Cache JSON for loading on each shard
                _tokenizer_json = std::string(buf.get(), buf.size());
                log_main.info("Tokenizer JSON loaded: {} bytes", _tokenizer_json.size());
                auto tpl_fmt = parse_chat_template_format(_config.assets.chat_template_format);
                if (tpl_fmt != ChatTemplateFormat::none) {
                    log_main.info("Chat template format: {} (tokenization will match vLLM)",
                                  chat_template_format_name(tpl_fmt));
                }

                // Start sharded tokenizer service (one instance per core for thread safety)
                return _tokenizer.start().then([this] {
                    _tokenizer_started = true;
                    // Configure tokenization cache from config (before loading tokenizer)
                    TokenizationCacheConfig cache_cfg;
                    cache_cfg.enabled = _config.assets.tokenization_cache_enabled;
                    cache_cfg.max_entries = _config.assets.tokenization_cache_size;
                    cache_cfg.max_text_length = _config.assets.tokenization_cache_max_text;
                    // Load tokenizer and configure cache on each shard from cached JSON
                    return _tokenizer.invoke_on_all([this, cache_cfg](TokenizerService& t) {
                        t.configure_cache(cache_cfg);
                        t.load_from_json(_tokenizer_json);
                    });
                }).then([this] {
                    log_main.info("Tokenizer initialized on {} shards (cache: {}, max_entries: {})",
                                  seastar::smp::count,
                                  _config.assets.tokenization_cache_enabled ? "enabled" : "disabled",
                                  _config.assets.tokenization_cache_size);
                    // Note: _tokenizer_json is kept for thread pool worker initialization
                    // It will be cleared after thread pool workers are started
                });
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
    cfg.block_alignment = config.routing.block_alignment;
    cfg.enable_prefix_boundary = config.routing.enable_prefix_boundary;
    cfg.min_prefix_boundary_tokens = config.routing.min_prefix_boundary_tokens;
    cfg.accept_client_prefix_boundary = config.routing.accept_client_prefix_boundary;
    cfg.enable_multi_depth_routing = config.routing.enable_multi_depth_routing;
    cfg.chat_template = ChatTemplate(parse_chat_template_format(config.assets.chat_template_format));
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
    cfg.max_stale_retries = config.retry.max_stale_retries;
    // Circuit breaker
    cfg.circuit_breaker.enabled = config.circuit_breaker.enabled;
    cfg.circuit_breaker.failure_threshold = config.circuit_breaker.failure_threshold;
    cfg.circuit_breaker.success_threshold = config.circuit_breaker.success_threshold;
    cfg.circuit_breaker.recovery_timeout = config.circuit_breaker.recovery_timeout;
    cfg.circuit_breaker.fallback_enabled = config.circuit_breaker.fallback_enabled;
    // Request body size limit and DNS timeout
    cfg.max_request_body_bytes = config.server.max_request_body_bytes;
    cfg.dns_resolution_timeout_seconds = config.server.dns_resolution_timeout_seconds;
    // Backpressure settings
    cfg.backpressure.max_concurrent_requests = config.backpressure.max_concurrent_requests;
    cfg.backpressure.enable_persistence_backpressure = config.backpressure.enable_persistence_backpressure;
    cfg.backpressure.persistence_queue_threshold = config.backpressure.persistence_queue_threshold;
    cfg.backpressure.retry_after_seconds = config.backpressure.retry_after_seconds;
    // Load balancing settings (P2C algorithm)
    cfg.load_balancing.enabled = config.load_balancing.enabled;
    cfg.load_balancing.min_load_difference = config.load_balancing.min_load_difference;
    cfg.load_balancing.local_processing_threshold = config.load_balancing.local_processing_threshold;
    cfg.load_balancing.snapshot_refresh_interval_us = config.load_balancing.snapshot_refresh_interval_us;
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

seastar::future<> Application::apply_vocab_size_config() {
    // Rule 22: coroutine converts any pre-future throw into a failed future
    // Auto-configure max_token_id from tokenizer vocabulary size
    // This ensures client-provided tokens are validated against the actual tokenizer
    size_t vocab_size = _tokenizer.local().vocab_size();

    if (vocab_size == 0) {
        // Tokenizer may not have loaded correctly, or vocab size unavailable
        log_main.warn("Tokenizer vocab_size returned 0 - using configured max_token_id: {}",
                      _config.routing.max_token_id);
        co_return;
    }

    // Bounds check: vocab_size must fit in int32_t (max ~2.1 billion)
    // Current LLMs have vocab sizes up to ~256k, so this is a sanity check
    constexpr size_t MAX_SAFE_VOCAB = static_cast<size_t>(std::numeric_limits<int32_t>::max());
    if (vocab_size > MAX_SAFE_VOCAB) {
        log_main.warn("Tokenizer vocab_size ({}) exceeds int32_t max - clamping to {}",
                      vocab_size, MAX_SAFE_VOCAB);
        vocab_size = MAX_SAFE_VOCAB;
    }

    auto current_max = static_cast<size_t>(_config.routing.max_token_id);
    if (current_max < vocab_size) {
        _config.routing.max_token_id = static_cast<int32_t>(vocab_size);
        log_main.info("Auto-configured max_token_id from tokenizer vocab size: {}", vocab_size);

        // Update ShardedConfig so all shards have consistent config
        // This ensures Application::local_config() returns the correct value
        co_await _sharded_config.invoke_on_all([max_id = _config.routing.max_token_id](ShardedConfig& cfg) {
            // Get a mutable copy, update max_token_id, and apply
            RanvierConfig updated = cfg.config();
            updated.routing.max_token_id = max_id;
            cfg.update(std::move(updated));
        });
    } else {
        log_main.debug("max_token_id ({}) already >= vocab_size ({}), no auto-config needed",
                       current_max, vocab_size);
    }
}

// =============================================================================
// Service Initialization
// =============================================================================

seastar::future<> Application::init_persistence() {
    // Rule 22: coroutine converts any pre-future throw into a failed future
    // Create async persistence manager (which creates and owns the underlying SQLite store)
    _async_persistence = std::make_unique<AsyncPersistenceManager>(build_persistence_config());

    // Open the database
    if (!_async_persistence->open(_config.database.path)) {
        log_main.warn("Failed to open persistence store - running without persistence");
        _async_persistence.reset();
        co_return;
    }

    log_main.info("Persistence initialized (SQLite: {})", _config.database.path);

    // Checkpoint WAL on startup to ensure clean state after potential crash
    if (_async_persistence->checkpoint()) {
        log_main.debug("Persistence WAL checkpoint complete");
    } else {
        log_main.warn("Persistence WAL checkpoint failed - continuing anyway");
    }

    // Start async persistence manager (arms flush timer)
    co_await _async_persistence->start();

    // Distribute async persistence manager to all HttpController shards
    co_await _controller.invoke_on_all([this](HttpController& c) {
        c.set_persistence(_async_persistence.get());
    });
}

seastar::future<> Application::load_persisted_state() {
    // Rule 22: coroutine converts any pre-future throw into a failed future
    if (!_async_persistence || !_async_persistence->is_open()) {
        log_main.info("No persistence store - starting with empty state");
        co_return;
    }

    // Step 1: Verify database integrity
    if (!_async_persistence->verify_integrity()) {
        log_main.error("Persistence integrity check failed - clearing corrupted state");
        _async_persistence->clear_all();
        log_main.info("Persistence store cleared - starting fresh");
        co_return;
    }

    // Step 2: Load backends and routes
    auto backends = _async_persistence->load_backends();
    auto routes = _async_persistence->load_routes();

    size_t skipped = _async_persistence->last_load_skipped_count();
    if (skipped > 0) {
        log_main.warn("Skipped {} corrupted route records during load", skipped);
    }

    // Step 2b: Filter out routes with invalid backend_id (business validation)
    // Persistence returns raw data; the service layer validates.
    size_t invalid_backend_count = std::erase_if(routes, [](const RouteRecord& r) {
        return r.backend_id <= 0;
    });
    if (invalid_backend_count > 0) {
        log_main.warn("Discarded {} routes with invalid backend_id <= 0", invalid_backend_count);
    }

    if (backends.empty() && routes.empty()) {
        log_main.info("Persistence store is empty - starting fresh");
        co_return;
    }

    log_main.info("Restoring state from persistence:");
    log_main.info("  Backends: {}", backends.size());
    log_main.info("  Routes:   {} (skipped {} corrupted, {} invalid backend_id)",
                  routes.size(), skipped, invalid_backend_count);

    for (const auto& rec : backends) {
        log_main.info("  - Backend {} -> {}:{} (weight={}, priority={})",
            rec.id, rec.ip, rec.port, rec.weight, rec.priority);
    }

    // Step 3: Restore backends first, then routes
    try {
        size_t failed_backends = 0;
        for (const auto& rec : backends) {
            try {
                seastar::socket_address addr(seastar::ipv4_addr(rec.ip, rec.port));
                co_await _router->register_backend_global(rec.id, addr, rec.weight, rec.priority);
            } catch (...) {
                failed_backends++;
                try {
                    throw;
                } catch (const std::exception& e) {
                    log_main.warn("Failed to restore backend {}: {}", rec.id, e.what());
                }
            }
        }

        size_t failed_routes = 0;
        for (const auto& rec : routes) {
            try {
                auto tokens_copy = rec.tokens;
                co_await _router->learn_route_global(std::move(tokens_copy), rec.backend_id);
            } catch (...) {
                failed_routes++;
                try {
                    throw;
                } catch (const std::exception& e) {
                    log_main.debug("Failed to restore route: {}", e.what());
                }
            }
        }

        if (failed_backends > 0 || failed_routes > 0) {
            log_main.warn("State restoration completed with errors: "
                          "{} backend failures, {} route failures",
                          failed_backends, failed_routes);
        } else {
            log_main.info("State restoration complete");
        }
    } catch (...) {
        try {
            throw;
        } catch (const std::exception& e) {
            log_main.error("State restoration failed: {} - starting with empty state", e.what());
        }
    }
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
            // 2b. Auto-configure max_token_id from tokenizer vocabulary size
            // This ensures client-provided tokens are validated against the actual tokenizer
            return apply_vocab_size_config();
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
            // 7b. Register circuit cleanup callback on all shards
            // This enables RouterService to clean up CircuitBreaker entries when backends
            // are unregistered (Rule #4: bounded container cleanup)
            return _controller.invoke_on_all([](HttpController& c) {
                RouterService::set_circuit_cleanup_callback([&c](BackendId id) {
                    c.remove_circuit(id);
                    // Record metric for Prometheus (circuit_breaker_circuits_removed_total)
                    if (g_metrics) {
                        metrics().record_circuit_removed();
                    }
                });
            });
        }).then([this] {
            // 8. Initialize metrics and shard load metrics on ALL shards
            return seastar::smp::invoke_on_all([] {
                init_metrics();
                init_shard_load_metrics();
            });
        }).then([this] {
            // 9. Start shard load balancer (P2C algorithm for cross-shard dispatch)
            ShardLoadBalancerConfig lb_config;
            lb_config.enabled = _config.load_balancing.enabled;
            lb_config.min_load_difference = _config.load_balancing.min_load_difference;
            lb_config.local_processing_threshold = _config.load_balancing.local_processing_threshold;
            lb_config.snapshot_refresh_interval_us = _config.load_balancing.snapshot_refresh_interval_us;
            return _load_balancer.start(lb_config);
        }).then([this] {
            _load_balancer_started = true;
            // Connect load balancer to HttpController on all shards
            return _controller.invoke_on_all([this](HttpController& c) {
                c.set_load_balancer(&_load_balancer);
            });
        }).then([this] {
            // Register load balancer metrics on all shards
            return _load_balancer.invoke_on_all([](ShardLoadBalancer& lb) {
                lb.register_metrics();
            });
        }).then([this] {
            if (_config.load_balancing.enabled) {
                log_main.info("Shard load balancer initialized (P2C algorithm, {} shards)",
                    seastar::smp::count);
            } else {
                log_main.info("Shard load balancer disabled (requests processed locally)");
            }
        }).then([this] {
            // Set cross-shard refs for tokenizer (enables async cross-shard dispatch on cache miss)
            if (_tokenizer_started && _load_balancer_started) {
                return _tokenizer.invoke_on_all([this](TokenizerService& t) {
                    t.set_cross_shard_refs(&_load_balancer, &_tokenizer);
                });
            }
            return seastar::make_ready_future<>();
        }).then([this] {
            // Start tokenizer thread pool (P3: disabled by default, enable via config)
            // Thread pool provides truly non-blocking tokenization by offloading FFI
            // to dedicated OS threads outside Seastar's reactor.
            if (!_tokenizer_started) {
                return seastar::make_ready_future<>();
            }

            // Build thread pool config from application config
            ThreadPoolTokenizationConfig pool_cfg;
            pool_cfg.enabled = _config.assets.tokenizer_thread_pool_enabled;
            pool_cfg.max_queue_size = _config.assets.tokenizer_thread_pool_queue_size;
            pool_cfg.min_text_length = _config.assets.tokenizer_thread_pool_min_text;
            pool_cfg.max_text_length = _config.assets.tokenizer_thread_pool_max_text;

            return _tokenizer_thread_pool.start(pool_cfg).then([this, pool_cfg] {
                _tokenizer_thread_pool_started = true;

                if (!pool_cfg.enabled) {
                    log_main.debug("Tokenizer thread pool disabled");
                    return seastar::make_ready_future<>();
                }

                // Load tokenizer and start workers on each shard
                return _tokenizer_thread_pool.invoke_on_all([this](TokenizerThreadPool& pool) {
                    pool.load_tokenizer(_tokenizer_json);
                    // Get alien instance for cross-thread signaling
                    // Note: default_instance is a pointer, dereference to get reference
                    pool.start_worker(*seastar::alien::internal::default_instance);
                    pool.register_metrics();
                });
            }).then([this, pool_cfg] {
                if (pool_cfg.enabled) {
                    log_main.info("Tokenizer thread pool started on {} shards "
                                  "(queue_size={}, min_text={}, max_text={})",
                                  seastar::smp::count,
                                  pool_cfg.max_queue_size,
                                  pool_cfg.min_text_length,
                                  pool_cfg.max_text_length);
                }

                // Set thread pool ref on tokenizer service
                return _tokenizer.invoke_on_all([this](TokenizerService& t) {
                    t.set_thread_pool_ref(&_tokenizer_thread_pool);
                });
            }).then([this] {
                // Configure local fallback semaphore and register metrics
                auto max_concurrent = _config.assets.tokenizer_local_fallback_max_concurrent;
                return _tokenizer.invoke_on_all(
                    [max_concurrent](TokenizerService& t) {
                        t.configure_local_fallback(max_concurrent);
                        t.register_metrics();
                    });
            }).then([this] {
                // Now we can clear the cached JSON
                _tokenizer_json.clear();
                _tokenizer_json.shrink_to_fit();
            });
        }).then([this] {
            // 10. Initialize persistence
            return init_persistence();
        }).then([this] {
            // 10. Initialize health checker
            init_health_checker();

            // 11. Initialize K8s discovery (if enabled)
            init_k8s_discovery();

            // 12. Start gossip service if cluster mode is enabled
            return _router->start_gossip();
        }).then([this] {
            // 13. Start draining backend reaper timer
            _router->start_draining_reaper();
            return seastar::make_ready_future<>();
        }).then([this] {
            // 14. Load persisted state
            return load_persisted_state();
        }).then([this] {
            // 15. Start K8s discovery service if enabled
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
    // Rule 22: coroutine converts any pre-future throw into a failed future
    _api_server = std::make_unique<seastar::httpd::http_server_control>();
    _metrics_server = std::make_unique<seastar::httpd::http_server_control>();

    co_await setup_tls();

    // Start Prometheus metrics server
    co_await _metrics_server->start();

    seastar::prometheus::config pconf;
    co_await seastar::prometheus::start(*_metrics_server, pconf);

    auto metrics_addr = seastar::socket_address(
        seastar::ipv4_addr(_config.server.bind_address, _config.server.metrics_port));
    co_await _metrics_server->listen(metrics_addr);

    log_main.info("Prometheus metrics listening on {}:{}",
        _config.server.bind_address, _config.server.metrics_port);

    // Start API server
    co_await _api_server->start();

    co_await _api_server->set_routes([this](seastar::httpd::routes& r) {
        _controller.local().register_routes(r);
    });

    auto api_addr = seastar::socket_address(
        seastar::ipv4_addr(_config.server.bind_address, _config.server.api_port));
    if (_tls_creds) {
        co_await _api_server->listen(api_addr, _tls_creds);
    } else {
        co_await _api_server->listen(api_addr);
    }

    auto protocol = _config.tls.enabled ? "https" : "http";
    log_main.info("Ranvier listening on {}://{}:{}",
        protocol, _config.server.bind_address, _config.server.api_port);
}

seastar::future<> Application::stop_servers() {
    // Rule 22: coroutine converts any pre-future throw into a failed future
    seastar::future<> api_stop = seastar::make_ready_future<>();
    seastar::future<> metrics_stop = seastar::make_ready_future<>();

    if (_api_server) {
        api_stop = _api_server->stop();
    }
    if (_metrics_server) {
        metrics_stop = _metrics_server->stop();
    }

    co_await seastar::when_all(std::move(api_stop), std::move(metrics_stop)).discard_result();
}

void Application::setup_signal_handlers() {
    // Suppress deprecation: reactor::handle_signal is deprecated in favor of
    // seastar::handle_signal() free function, but the free function is not
    // available in our Seastar version yet.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

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
    }).handle_exception([](auto ep) {
        log_main.warn("Config reload callback broadcast failed: {}", ep);
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

#pragma GCC diagnostic pop
}

void Application::signal_shutdown() {
    // Use atomic compare-exchange to ensure exactly one thread executes shutdown.
    // This handles the race between SIGINT and SIGTERM arriving simultaneously.
    bool expected = false;
    if (!_shutdown_initiated.compare_exchange_strong(expected, true,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_acquire)) {
        log_main.debug("Shutdown already initiated - ignoring duplicate signal");
        return;
    }

    // Also check state (belt-and-suspenders for code paths that set state directly)
    if (_state == ApplicationState::DRAINING ||
        _state == ApplicationState::STOPPING ||
        _state == ApplicationState::STOPPED) {
        return;
    }

    _shutdown_start_time = std::chrono::steady_clock::now();
    log_main.info("=== GRACEFUL SHUTDOWN INITIATED ===");
    log_main.info("Shutdown timeout: {}s, Drain timeout: {}s",
                  _config.shutdown.shutdown_timeout.count(),
                  _config.shutdown.drain_timeout.count());
    _state = ApplicationState::DRAINING;

    // Execute shutdown phases with overall timeout protection
    auto shutdown_deadline = seastar::lowres_clock::now() + _config.shutdown.shutdown_timeout;

    // Chain the shutdown phases
    auto shutdown_future = phase_broadcast_draining()
        .then([this] {
            return phase_drain_requests();
        })
        .then([this] {
            log_main.info("Drain complete - signaling main loop to stop");
            _stop_signal->set_value();
        })
        .handle_exception([this](auto ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                log_main.error("Error during shutdown sequence: {}", e.what());
            }
            // Always signal stop, even on error - we must not hang
            try {
                _stop_signal->set_value();
            } catch (const std::exception& e) {
                // Rule #9: Promise may already be fulfilled - debug level since this is
                // expected during shutdown race conditions, not an error condition
                log_main.debug("Stop signal already set during shutdown: {}", e.what());
            }
        });

    // Apply overall shutdown timeout
    (void)seastar::with_timeout(shutdown_deadline, std::move(shutdown_future))
        .handle_exception_type([this](const seastar::timed_out_error&) {
            log_main.error("Shutdown timeout ({}s) exceeded - forcing stop signal",
                          _config.shutdown.shutdown_timeout.count());
            try {
                _stop_signal->set_value();
            } catch (const std::exception& e) {
                // Rule #9: Promise may already be fulfilled - debug level since this is
                // expected during shutdown timeout handling
                log_main.debug("Stop signal already set during timeout handler: {}", e.what());
            }
        });
}

void Application::log_phase_complete(const char* phase_name,
                                      std::chrono::steady_clock::time_point phase_start) const {
    auto elapsed = std::chrono::steady_clock::now() - phase_start;
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    log_main.info("  [{}] completed in {}ms", phase_name, elapsed_ms);
}

seastar::future<> Application::phase_broadcast_draining() {
    auto phase_start = std::chrono::steady_clock::now();
    log_main.info("Phase 1/3: Broadcasting DRAINING state to cluster...");

    // Skip if clustering is not enabled or gossip not available
    if (!_router || !_router->gossip_service() || !_router->gossip_service()->is_enabled()) {
        log_main.info("  Cluster mode disabled - skipping gossip broadcast");
        log_phase_complete("BROADCAST", phase_start);
        return seastar::make_ready_future<>();
    }

    // Broadcast with timeout to avoid blocking shutdown on network issues
    auto broadcast_deadline = seastar::lowres_clock::now() + _config.shutdown.gossip_broadcast_timeout;

    return seastar::with_timeout(
        broadcast_deadline,
        _router->gossip_service()->broadcast_node_state(NodeState::DRAINING)
    ).then([this, phase_start] {
        log_main.info("  Successfully notified cluster peers");
        log_phase_complete("BROADCAST", phase_start);
    }).handle_exception_type([this, phase_start](const seastar::timed_out_error&) {
        log_main.warn("  Gossip broadcast timed out after {}ms - continuing shutdown",
                     _config.shutdown.gossip_broadcast_timeout.count());
        log_phase_complete("BROADCAST", phase_start);
    }).handle_exception([this, phase_start](auto ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            log_main.warn("  Gossip broadcast failed: {} - continuing shutdown", e.what());
        }
        log_phase_complete("BROADCAST", phase_start);
    });
}

seastar::future<> Application::phase_drain_requests() {
    auto phase_start = std::chrono::steady_clock::now();
    log_main.info("Phase 2/3: Draining in-flight requests (timeout: {}s)...",
                  _config.shutdown.drain_timeout.count());

    return drain_requests().then([this, phase_start] {
        log_phase_complete("DRAIN", phase_start);
    }).handle_exception([this, phase_start](auto ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            log_main.warn("  Drain encountered error: {} - continuing shutdown", e.what());
        }
        log_phase_complete("DRAIN", phase_start);
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

        // Wait for shutdown signal (set by signal_shutdown())
        return _stop_signal->get_future();
    }).then([this] {
        // Phase 3 is logged inside stop_services()
        return shutdown();
    });
}

// =============================================================================
// Shutdown Management
// =============================================================================

seastar::future<> Application::stop_services() {
    auto phase_start = std::chrono::steady_clock::now();
    log_main.info("Phase 3/3: Stopping services in reverse order...");
    _state = ApplicationState::STOPPING;

    // -------------------------------------------------------------------------
    // Step 1: Stop services that don't need request handling (parallel)
    // Health checker and K8s discovery can stop independently
    // -------------------------------------------------------------------------
    std::vector<seastar::future<>> parallel_stops;

    if (_health_checker) {
        parallel_stops.push_back(
            _health_checker->stop().then([] {
                log_main.debug("  Health checker stopped");
            }).handle_exception([](auto ep) {
                log_main.warn("  Health checker stop error (ignored)");
            })
        );
    }

    if (_k8s_discovery && _k8s_discovery->is_enabled()) {
        parallel_stops.push_back(
            _k8s_discovery->stop().then([] {
                log_main.debug("  K8s discovery stopped");
            }).handle_exception([](auto ep) {
                log_main.warn("  K8s discovery stop error (ignored)");
            })
        );
    }

    return seastar::when_all_succeed(parallel_stops.begin(), parallel_stops.end())
        .discard_result()
        .then([this] {
            // -------------------------------------------------------------------------
            // Step 2: Stop gossip service and draining reaper
            // Must happen after health checker (which may use routing info)
            // -------------------------------------------------------------------------
            if (_router) {
                _router->stop_draining_reaper();
                return _router->stop_gossip().then([] {
                    log_main.debug("  Gossip service stopped");
                }).handle_exception([](auto ep) {
                    log_main.warn("  Gossip service stop error (ignored)");
                    return seastar::make_ready_future<>();
                });
            }
            return seastar::make_ready_future<>();
        })
        .then([this] {
            // -------------------------------------------------------------------------
            // Step 3: Stop HTTP servers (parallel - API and metrics are independent)
            // -------------------------------------------------------------------------
            return stop_servers().then([] {
                log_main.debug("  HTTP servers stopped");
            });
        })
        .then([this] {
            // -------------------------------------------------------------------------
            // Step 4: Stop sharded services (sequential - correct shutdown order)
            // -------------------------------------------------------------------------
            if (!_controller_started) {
                return seastar::make_ready_future<>();
            }
            return _controller.stop().then([] {
                log_main.debug("  HttpController stopped on all shards");
            });
        })
        .then([this] {
            // -------------------------------------------------------------------------
            // Step 4a: Stop tokenizer thread pool workers (blocks until threads exit)
            // -------------------------------------------------------------------------
            if (!_tokenizer_thread_pool_started) {
                return seastar::make_ready_future<>();
            }
            // First stop all worker threads (this blocks until each thread exits)
            return _tokenizer_thread_pool.invoke_on_all([](TokenizerThreadPool& pool) {
                pool.stop_worker();
            }).then([this] {
                // Then stop the sharded service
                return _tokenizer_thread_pool.stop();
            }).then([] {
                log_main.debug("  TokenizerThreadPool stopped on all shards");
            });
        })
        .then([this] {
            if (!_load_balancer_started) {
                return seastar::make_ready_future<>();
            }
            return _load_balancer.stop().then([] {
                log_main.debug("  Shard load balancer stopped");
            });
        })
        .then([this] {
            // -------------------------------------------------------------------------
            // Step 4b: Stop sharded tokenizer service
            // -------------------------------------------------------------------------
            if (!_tokenizer_started) {
                return seastar::make_ready_future<>();
            }
            return _tokenizer.stop().then([] {
                log_main.debug("  TokenizerService stopped on all shards");
            });
        })
        .then([this] {
            // -------------------------------------------------------------------------
            // Step 5: Stop persistence (flushes pending writes)
            // -------------------------------------------------------------------------
            if (!_async_persistence) {
                return seastar::make_ready_future<>();
            }
            return _async_persistence->stop().then([] {
                log_main.debug("  Async persistence stopped (queue flushed)");
            });
        })
        .then([this] {
            // -------------------------------------------------------------------------
            // Step 6: Stop sharded config (last infrastructure component)
            // -------------------------------------------------------------------------
            if (!_sharded_config_started) {
                return seastar::make_ready_future<>();
            }
            return _sharded_config.stop().then([] {
                log_main.debug("  Sharded config stopped");
            });
        })
        .finally([this, phase_start] {
            log_phase_complete("STOP_SERVICES", phase_start);
            cleanup();
        });
}

void Application::cleanup() {
    // Log shutdown summary and close persistence
    if (_async_persistence && _async_persistence->is_open()) {
        log_main.info("Persistence shutdown summary:");
        log_main.info("  Persisted backends: {}", _async_persistence->backend_count());
        log_main.info("  Persisted routes:   {}", _async_persistence->route_count());

        // Checkpoint WAL before closing
        if (_async_persistence->checkpoint()) {
            log_main.debug("Final WAL checkpoint complete");
        } else {
            log_main.warn("Final WAL checkpoint failed - some data may be in WAL");
        }

        _async_persistence->close();
        log_main.debug("Persistence store closed");
    }

    // Shutdown OpenTelemetry tracing
    if (TracingService::is_enabled()) {
        TracingService::shutdown();
        log_main.debug("OpenTelemetry tracing shutdown complete");
    }

    _state = ApplicationState::STOPPED;

    // Log total shutdown time if we have a valid start time
    if (_shutdown_start_time.time_since_epoch().count() > 0) {
        auto total_elapsed = std::chrono::steady_clock::now() - _shutdown_start_time;
        auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(total_elapsed).count();
        log_main.info("=== GRACEFUL SHUTDOWN COMPLETE (total: {}ms) ===", total_ms);
    } else {
        log_main.info("=== SHUTDOWN COMPLETE ===");
    }
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
    // Rate limiting: reject reload if last reload was < 10 seconds ago
    static constexpr auto RELOAD_COOLDOWN = std::chrono::seconds(10);

    auto now = std::chrono::steady_clock::now();
    if (_last_reload_time.time_since_epoch().count() > 0) {
        auto elapsed = now - _last_reload_time;
        if (elapsed < RELOAD_COOLDOWN) {
            auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                RELOAD_COOLDOWN - elapsed).count();
            log_main.warn("Config reload rate-limited: {} seconds remaining before next reload allowed",
                          remaining);
            return seastar::make_ready_future<>();
        }
    }

    log_main.info("Reloading configuration from {}", _config_path);

    // Load configuration asynchronously to avoid blocking the reactor.
    // seastar::async() offloads the blocking std::ifstream I/O to the thread pool.
    // This is the ONLY safe way to reload config after the Seastar reactor starts.
    return seastar::async([path = _config_path] {
        return RanvierConfig::load(path);
    }).then([this, now](RanvierConfig new_config) {
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
        }).then([this, config_ptr, now] {
            // Step 5: Only update master config after all shards succeeded
            // This ensures consistency - if any step fails, master config is unchanged
            _config = *config_ptr;
            _last_reload_time = now;

            // Step 6: Re-apply auto-configure for max_token_id from tokenizer vocab size
            // This ensures the tokenizer-derived value is preserved across reloads
            return apply_vocab_size_config();
        }).then([] {
            log_main.info("Configuration reloaded successfully on all cores");
        });
    }).handle_exception([](auto ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            log_main.error("Config reload failed: {}", e.what());
        }
        // Don't re-throw - config reload failure shouldn't crash the server
    });
}

}  // namespace ranvier
