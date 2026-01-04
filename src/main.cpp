// Ranvier Core - Content-aware Layer 7+ Load Balancer for LLM Inference
//
// Architecture:
// 1. Infrastructure Layer (TokenizerService): Handles tokenization
// 2. Domain Layer (RouterService): Handles routing logic (Radix Tree, Broadcasting)
// 3. Presentation Layer (HttpController): Handles HTTP endpoints
// 4. Persistence Layer (SqlitePersistence): Handles durable storage of routes/backends

#include "config.hpp"
#include "gossip_service.hpp"
#include "health_service.hpp"
#include "http_controller.hpp"
#include "k8s_discovery_service.hpp"
#include "logging.hpp"
#include "metrics_service.hpp"
#include "router_service.hpp"
#include "sqlite_persistence.hpp"
#include "tokenizer_service.hpp"
#include "tracing_service.hpp"

#include <csignal>
#include <fstream>
#include <streambuf>
#include <sys/stat.h>
#include <unistd.h>

#include <boost/program_options.hpp>
#include <seastar/core/app-template.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/smp.hh>
#include <seastar/http/httpd.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/tls.hh>

using namespace seastar;

// Global configuration (loaded before Seastar starts)
static ranvier::RanvierConfig g_config;
static std::string g_config_path;

// Services (Global/Static Scope for MVP)
ranvier::TokenizerService tokenizer;
std::unique_ptr<ranvier::RouterService> router;

// These are initialized in run() after config is loaded
// HttpController MUST be sharded to avoid cross-shard memory access when
// handling concurrent requests in multi-shard mode (--smp > 1).
// Each shard needs its own ConnectionPool, RateLimiter, and request gate.
seastar::sharded<ranvier::HttpController> controller;
std::unique_ptr<ranvier::HealthService> health_checker;
std::unique_ptr<ranvier::PersistenceStore> persistence;
std::unique_ptr<ranvier::K8sDiscoveryService> k8s_discovery;

// Helper to load persisted state into the router
// Includes crash recovery: validates integrity, skips corrupted records, handles exceptions
future<> load_persisted_state() {
    if (!persistence || !persistence->is_open()) {
        ranvier::log_main.info("No persistence store - starting with empty state");
        return make_ready_future<>();
    }

    // Step 1: Verify database integrity before loading
    // This catches SQLite corruption from incomplete WAL recovery
    if (!persistence->verify_integrity()) {
        ranvier::log_main.error("Persistence integrity check failed - clearing corrupted state");
        persistence->clear_all();
        ranvier::log_main.info("Persistence store cleared - starting fresh");
        return make_ready_future<>();
    }

    // Step 2: Load backends and routes (corrupted records are skipped internally)
    auto backends = persistence->load_backends();
    auto routes = persistence->load_routes();

    // Check if any records were skipped due to corruption
    size_t skipped = persistence->last_load_skipped_count();
    if (skipped > 0) {
        ranvier::log_main.warn("Skipped {} corrupted route records during load", skipped);
    }

    if (backends.empty() && routes.empty()) {
        ranvier::log_main.info("Persistence store is empty - starting fresh");
        return make_ready_future<>();
    }

    ranvier::log_main.info("Restoring state from persistence:");
    ranvier::log_main.info("  Backends: {}", backends.size());
    ranvier::log_main.info("  Routes:   {} (skipped {} corrupted)", routes.size(), skipped);

    // Log each backend at info level for visibility
    for (const auto& rec : backends) {
        ranvier::log_main.info("  - Backend {} -> {}:{} (weight={}, priority={})",
            rec.id, rec.ip, rec.port, rec.weight, rec.priority);
    }

    // Step 3: Restore backends first, then routes
    // Each step has exception handling to continue on partial failures
    return do_with(std::move(backends), std::move(routes),
                   size_t(0), size_t(0),  // Counters for failed backends/routes
        [](auto& backends, auto& routes, size_t& failed_backends, size_t& failed_routes) {
            // Restore backends with individual exception handling
            return do_for_each(backends, [&failed_backends](const ranvier::BackendRecord& rec) {
                socket_address addr(ipv4_addr(rec.ip, rec.port));
                return router->register_backend_global(rec.id, addr, rec.weight, rec.priority)
                    .handle_exception([&failed_backends, id = rec.id](auto ep) {
                        failed_backends++;
                        try {
                            std::rethrow_exception(ep);
                        } catch (const std::exception& e) {
                            ranvier::log_main.warn("Failed to restore backend {}: {}", id, e.what());
                        }
                        return make_ready_future<>();
                    });
            }).then([&routes, &failed_routes] {
                // Restore routes with individual exception handling
                // Routes are less critical - they can be relearned from traffic
                return do_for_each(routes, [&failed_routes](const ranvier::RouteRecord& rec) {
                    // Copy tokens to avoid lifetime issues in async context
                    // The do_with inside learn_route_global needs its own copy
                    auto tokens_copy = rec.tokens;
                    return router->learn_route_global(std::move(tokens_copy), rec.backend_id)
                        .handle_exception([&failed_routes](auto ep) {
                            failed_routes++;
                            // Log at debug level - route failures are common and recoverable
                            try {
                                std::rethrow_exception(ep);
                            } catch (const std::exception& e) {
                                ranvier::log_main.debug("Failed to restore route: {}", e.what());
                            }
                            return make_ready_future<>();
                        });
                });
            }).then([&failed_backends, &failed_routes] {
                if (failed_backends > 0 || failed_routes > 0) {
                    ranvier::log_main.warn("State restoration completed with errors: "
                                           "{} backend failures, {} route failures",
                                           failed_backends, failed_routes);
                } else {
                    ranvier::log_main.info("State restoration complete");
                }
            });
        }).handle_exception([](auto ep) {
            // Catch-all for unexpected errors during restoration
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                ranvier::log_main.error("State restoration failed: {} - starting with empty state", e.what());
            }
            // Continue startup even if restoration fails completely
            return make_ready_future<>();
        });
}

// Hot-reload configuration on SIGHUP
future<> reload_config() {
    ranvier::log_main.info("SIGHUP received - reloading configuration from {}", g_config_path);

    try {
        // Load new configuration
        auto new_config = ranvier::RanvierConfig::load(g_config_path);

        // Validate the new configuration
        auto validation_error = ranvier::RanvierConfig::validate(new_config);
        if (validation_error) {
            ranvier::log_main.error("Config reload failed - validation error: {}", *validation_error);
            return make_ready_future<>();
        }

        // Log warnings for settings that cannot be hot-reloaded
        if (new_config.server.api_port != g_config.server.api_port ||
            new_config.server.metrics_port != g_config.server.metrics_port ||
            new_config.server.bind_address != g_config.server.bind_address) {
            ranvier::log_main.warn("Config reload: server.* changes require restart to take effect");
        }
        if (new_config.database.path != g_config.database.path) {
            ranvier::log_main.warn("Config reload: database.path changes require restart to take effect");
        }
        if (new_config.tls.enabled != g_config.tls.enabled ||
            new_config.tls.cert_path != g_config.tls.cert_path ||
            new_config.tls.key_path != g_config.tls.key_path) {
            ranvier::log_main.warn("Config reload: tls.* changes require restart to take effect");
        }
        if (new_config.assets.tokenizer_path != g_config.assets.tokenizer_path) {
            ranvier::log_main.warn("Config reload: assets.tokenizer_path changes require restart to take effect");
        }

        // Update global config
        g_config = new_config;

        // Update HttpController config (runs on current shard, controller handles thread-safety)
        ranvier::HttpControllerConfig ctrl_config;
        ctrl_config.pool.max_connections_per_host = g_config.pool.max_connections_per_host;
        ctrl_config.pool.idle_timeout = g_config.pool.idle_timeout;
        ctrl_config.pool.max_total_connections = g_config.pool.max_total_connections;
        ctrl_config.min_token_length = g_config.routing.min_token_length;
        ctrl_config.connect_timeout = g_config.timeouts.connect_timeout;
        ctrl_config.request_timeout = g_config.timeouts.request_timeout;
        ctrl_config.auth = g_config.auth;  // Full auth config with multi-key support
        ctrl_config.rate_limit.enabled = g_config.rate_limit.enabled;
        ctrl_config.rate_limit.requests_per_second = g_config.rate_limit.requests_per_second;
        ctrl_config.rate_limit.burst_size = g_config.rate_limit.burst_size;
        ctrl_config.retry.max_retries = g_config.retry.max_retries;
        ctrl_config.retry.initial_backoff = g_config.retry.initial_backoff;
        ctrl_config.retry.max_backoff = g_config.retry.max_backoff;
        ctrl_config.retry.backoff_multiplier = g_config.retry.backoff_multiplier;
        ctrl_config.circuit_breaker.enabled = g_config.circuit_breaker.enabled;
        ctrl_config.circuit_breaker.failure_threshold = g_config.circuit_breaker.failure_threshold;
        ctrl_config.circuit_breaker.success_threshold = g_config.circuit_breaker.success_threshold;
        ctrl_config.circuit_breaker.recovery_timeout = g_config.circuit_breaker.recovery_timeout;
        ctrl_config.circuit_breaker.fallback_enabled = g_config.circuit_breaker.fallback_enabled;
        ctrl_config.drain_timeout = g_config.shutdown.drain_timeout;
        ctrl_config.enable_token_forwarding = g_config.routing.enable_token_forwarding;
        ctrl_config.accept_client_tokens = g_config.routing.accept_client_tokens;
        ctrl_config.max_token_id = g_config.routing.max_token_id;
        ctrl_config.routing_mode = g_config.routing.routing_mode;

        // Update HttpController config on all shards (controller is sharded)
        return controller.invoke_on_all([ctrl_config](ranvier::HttpController& c) {
            c.update_config(ctrl_config);
        }).then([] {
            // Update routing config on all shards
            return router->update_routing_config(g_config.routing);
        }).then([] {
            ranvier::log_main.info("Configuration reloaded successfully");
        });

    } catch (const std::exception& e) {
        ranvier::log_main.error("Config reload failed: {}", e.what());
        return make_ready_future<>();
    }
}

// Dry-run validation - checks configuration, tokenizer, database, and TLS without starting services
// Returns 0 if all checks pass, 1 if any fail
int run_dry_run_validation(const std::string& config_path, const ranvier::RanvierConfig& config) {
    int error_count = 0;

    std::cout << "\nRanvier Core - Dry Run Validation\n\n";

    // ============================================================
    // Configuration validation
    // ============================================================
    std::cout << "Configuration: " << config_path << "\n";

    // Check if config file actually exists
    std::ifstream config_file(config_path);
    if (config_file.is_open()) {
        config_file.close();
        std::cout << "  \xE2\x9C\x93 Config file parsed successfully\n";
    } else {
        std::cout << "  ! Config file not found, using defaults\n";
    }
    std::cout << "  \xE2\x9C\x93 API port: " << config.server.api_port << "\n";
    std::cout << "  \xE2\x9C\x93 Metrics port: " << config.server.metrics_port << "\n";

    // Run config validation
    auto validation_error = ranvier::RanvierConfig::validate(config);
    if (validation_error) {
        std::cout << "  \xE2\x9C\x97 Validation error: " << *validation_error << "\n";
        error_count++;
    } else {
        std::cout << "  \xE2\x9C\x93 Configuration validation passed\n";
    }

    // ============================================================
    // Tokenizer validation
    // ============================================================
    std::cout << "\nTokenizers:\n";

    // Check tokenizer file exists
    std::ifstream tokenizer_file(config.assets.tokenizer_path);
    if (!tokenizer_file.is_open()) {
        std::cout << "  \xE2\x9C\x97 " << config.assets.tokenizer_path << " (file not found)\n";
        error_count++;
    } else {
        // Try to read and parse the JSON
        try {
            std::string json_str((std::istreambuf_iterator<char>(tokenizer_file)),
                                 std::istreambuf_iterator<char>());
            tokenizer_file.close();

            // Try to load the tokenizer to validate the JSON
            ranvier::TokenizerService test_tokenizer;
            test_tokenizer.load_from_json(json_str);

            if (test_tokenizer.is_loaded()) {
                std::cout << "  \xE2\x9C\x93 " << config.assets.tokenizer_path << " (valid)\n";
            } else {
                std::cout << "  \xE2\x9C\x97 " << config.assets.tokenizer_path << " (failed to parse)\n";
                error_count++;
            }
        } catch (const std::exception& e) {
            std::cout << "  \xE2\x9C\x97 " << config.assets.tokenizer_path << " (invalid: " << e.what() << ")\n";
            error_count++;
        }
    }

    // ============================================================
    // Database validation
    // ============================================================
    std::cout << "\nDatabase:\n";

    const std::string& db_path = config.database.path;

    // Check if the database file exists
    struct stat db_stat;
    if (stat(db_path.c_str(), &db_stat) == 0) {
        // File exists, check if it's writable
        if (access(db_path.c_str(), W_OK) == 0) {
            std::cout << "  \xE2\x9C\x93 " << db_path << " (writable)\n";
        } else {
            std::cout << "  \xE2\x9C\x97 " << db_path << " (not writable)\n";
            error_count++;
        }
    } else {
        // File doesn't exist, check if parent directory is writable
        std::string parent_dir = ".";
        size_t last_slash = db_path.find_last_of('/');
        if (last_slash != std::string::npos) {
            parent_dir = db_path.substr(0, last_slash);
            if (parent_dir.empty()) {
                parent_dir = "/";
            }
        }

        if (access(parent_dir.c_str(), W_OK) == 0) {
            std::cout << "  \xE2\x9C\x93 " << db_path << " (can be created)\n";
        } else {
            std::cout << "  \xE2\x9C\x97 " << db_path << " (parent directory not writable: " << parent_dir << ")\n";
            error_count++;
        }
    }

    // ============================================================
    // TLS validation
    // ============================================================
    std::cout << "\nTLS:\n";

    if (!config.tls.enabled) {
        std::cout << "  - TLS disabled\n";
    } else {
        // Check certificate file
        if (config.tls.cert_path.empty()) {
            std::cout << "  \xE2\x9C\x97 Certificate: (path not configured)\n";
            error_count++;
        } else if (access(config.tls.cert_path.c_str(), R_OK) == 0) {
            std::cout << "  \xE2\x9C\x93 Certificate: " << config.tls.cert_path << "\n";
        } else {
            std::cout << "  \xE2\x9C\x97 Certificate: " << config.tls.cert_path << " (not readable)\n";
            error_count++;
        }

        // Check private key file
        if (config.tls.key_path.empty()) {
            std::cout << "  \xE2\x9C\x97 Private key: (path not configured)\n";
            error_count++;
        } else if (access(config.tls.key_path.c_str(), R_OK) == 0) {
            std::cout << "  \xE2\x9C\x93 Private key: " << config.tls.key_path << "\n";
        } else {
            std::cout << "  \xE2\x9C\x97 Private key: " << config.tls.key_path << " (not readable)\n";
            error_count++;
        }
    }

    // ============================================================
    // Summary
    // ============================================================
    std::cout << "\n";
    if (error_count == 0) {
        std::cout << "Result: PASSED\n";
        return 0;
    } else {
        std::cout << "Result: FAILED (" << error_count << " error" << (error_count == 1 ? "" : "s") << ")\n";
        return 1;
    }
}

future<> run() {
    // 1. Init Infrastructure (Tokenizer)
    try {
        std::ifstream t(g_config.assets.tokenizer_path);
        if (!t.is_open()) {
            throw std::runtime_error("Could not find tokenizer: " + g_config.assets.tokenizer_path);
        }
        std::string json_str((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());
        tokenizer.load_from_json(json_str);
        ranvier::log_main.info("Services initialized (Tokenizer: {})", g_config.assets.tokenizer_path);
    } catch (const std::exception& e) {
        ranvier::log_main.error("Service init failed: {}", e.what());
        return make_ready_future<>();
    }

    // 1b. Init OpenTelemetry tracing
    ranvier::TracingService::init(g_config.telemetry);
    if (g_config.telemetry.enabled) {
        ranvier::log_main.info("OpenTelemetry tracing enabled (endpoint: {}, sample_rate: {:.2f})",
            g_config.telemetry.otlp_endpoint, g_config.telemetry.sample_rate);
    } else {
        ranvier::log_main.info("OpenTelemetry tracing disabled");
    }

    // 1a. Init Router with routing and cluster config
    router = std::make_unique<ranvier::RouterService>(g_config.routing, g_config.cluster);
    if (g_config.cluster.enabled) {
        ranvier::log_main.info("Cluster mode: {} peers on port {}",
            g_config.cluster.peers.size(), g_config.cluster.gossip_port);
    }

    // 2. Build HttpController config
    // HttpController must be sharded because it contains shard-local state:
    // - ConnectionPool: manages TCP connections per-shard
    // - RateLimiter: tracks per-client request rates
    // - request_gate: tracks in-flight requests for graceful shutdown
    // Without sharding, requests on shard N would access shard 0's memory → segfault
    ranvier::HttpControllerConfig ctrl_config;
    ctrl_config.pool.max_connections_per_host = g_config.pool.max_connections_per_host;
    ctrl_config.pool.idle_timeout = g_config.pool.idle_timeout;
    ctrl_config.pool.max_total_connections = g_config.pool.max_total_connections;
    ctrl_config.min_token_length = g_config.routing.min_token_length;
    ctrl_config.connect_timeout = g_config.timeouts.connect_timeout;
    ctrl_config.request_timeout = g_config.timeouts.request_timeout;
    ctrl_config.auth = g_config.auth;  // Full auth config with multi-key support
    ctrl_config.rate_limit.enabled = g_config.rate_limit.enabled;
    ctrl_config.rate_limit.requests_per_second = g_config.rate_limit.requests_per_second;
    ctrl_config.rate_limit.burst_size = g_config.rate_limit.burst_size;
    ctrl_config.retry.max_retries = g_config.retry.max_retries;
    ctrl_config.retry.initial_backoff = g_config.retry.initial_backoff;
    ctrl_config.retry.max_backoff = g_config.retry.max_backoff;
    ctrl_config.retry.backoff_multiplier = g_config.retry.backoff_multiplier;
    ctrl_config.circuit_breaker.enabled = g_config.circuit_breaker.enabled;
    ctrl_config.circuit_breaker.failure_threshold = g_config.circuit_breaker.failure_threshold;
    ctrl_config.circuit_breaker.success_threshold = g_config.circuit_breaker.success_threshold;
    ctrl_config.circuit_breaker.recovery_timeout = g_config.circuit_breaker.recovery_timeout;
    ctrl_config.circuit_breaker.fallback_enabled = g_config.circuit_breaker.fallback_enabled;
    ctrl_config.drain_timeout = g_config.shutdown.drain_timeout;
    ctrl_config.enable_token_forwarding = g_config.routing.enable_token_forwarding;
    ctrl_config.accept_client_tokens = g_config.routing.accept_client_tokens;
    ctrl_config.max_token_id = g_config.routing.max_token_id;
    ctrl_config.routing_mode = g_config.routing.routing_mode;

    // 3. Initialize router's thread-local data on all shards, then start services
    // Each shard needs its own RadixTree and backend maps for the shared-nothing architecture
    return router->initialize_shards().then([ctrl_config] {
        // Start HttpController on all shards - each shard gets its own instance
        return controller.start(std::ref(tokenizer), std::ref(*router), ctrl_config);
    }).then([] {
        // Initialize metrics on ALL shards (not just shard 0)
        // Each shard needs its own MetricsService for the shared-nothing architecture
        return seastar::smp::invoke_on_all([] {
            ranvier::init_metrics();
        });
    }).then([] {
        // 3. Init Persistence
        persistence = ranvier::create_persistence_store();
        if (persistence->open(g_config.database.path)) {
            ranvier::log_main.info("Persistence initialized (SQLite: {})", g_config.database.path);

            // Checkpoint WAL on startup to ensure clean state after potential crash
            // This flushes any pending WAL entries to the main database file
            if (persistence->checkpoint()) {
                ranvier::log_main.debug("Persistence WAL checkpoint complete");
            } else {
                ranvier::log_main.warn("Persistence WAL checkpoint failed - continuing anyway");
            }

            // Set persistence on all shards - persistence store is thread-safe for writes
            return controller.invoke_on_all([](ranvier::HttpController& c) {
                c.set_persistence(persistence.get());
            });
        } else {
            ranvier::log_main.warn("Failed to open persistence store - running without persistence");
            return make_ready_future<>();
        }
    }).then([] {
        // 4. Init Health Checker with config
        ranvier::HealthServiceConfig health_config;
        health_config.check_interval = g_config.health.check_interval;
        health_config.check_timeout = g_config.health.check_timeout;
        health_config.failure_threshold = g_config.health.failure_threshold;
        health_config.recovery_threshold = g_config.health.recovery_threshold;
        health_checker = std::make_unique<ranvier::HealthService>(*router, health_config);
        health_checker->start();

        // 4a. Init K8s Discovery Service if enabled
        if (g_config.k8s_discovery.enabled) {
            ranvier::K8sDiscoveryConfig k8s_config;
            k8s_config.enabled = g_config.k8s_discovery.enabled;
            k8s_config.api_server = g_config.k8s_discovery.api_server;
            k8s_config.namespace_name = g_config.k8s_discovery.namespace_name;
            k8s_config.service_name = g_config.k8s_discovery.service_name;
            k8s_config.target_port = g_config.k8s_discovery.target_port;
            k8s_config.token_path = g_config.k8s_discovery.token_path;
            k8s_config.ca_cert_path = g_config.k8s_discovery.ca_cert_path;
            k8s_config.poll_interval = g_config.k8s_discovery.poll_interval;
            k8s_config.watch_timeout = g_config.k8s_discovery.watch_timeout;
            k8s_config.watch_reconnect_delay = g_config.k8s_discovery.watch_reconnect_delay;
            k8s_config.watch_reconnect_max_delay = g_config.k8s_discovery.watch_reconnect_max_delay;
            k8s_config.verify_tls = g_config.k8s_discovery.verify_tls;
            k8s_config.label_selector = g_config.k8s_discovery.label_selector;

            k8s_discovery = std::make_unique<ranvier::K8sDiscoveryService>(k8s_config);

            // Connect K8s discovery to router
            k8s_discovery->set_register_callback(
                [](ranvier::BackendId id, socket_address addr, uint32_t weight, uint32_t priority) {
                    return router->register_backend_global(id, addr, weight, priority);
                });
            k8s_discovery->set_drain_callback(
                [](ranvier::BackendId id) {
                    return router->drain_backend_global(id);
                });

            ranvier::log_main.info("K8s discovery configured for {}/{}",
                g_config.k8s_discovery.namespace_name, g_config.k8s_discovery.service_name);
        }

        // 5. Start gossip service if cluster mode is enabled
        return router->start_gossip();
    }).then([] {
        // 6. Load persisted state
        return load_persisted_state();
    }).then([] {
        // 6a. Start K8s discovery service if enabled
        if (k8s_discovery && k8s_discovery->is_enabled()) {
            return k8s_discovery->start();
        }
        return make_ready_future<>();
    }).then([] {
        // 7. Start Servers (Metrics + API)
        return do_with(seastar::httpd::http_server_control(), seastar::httpd::http_server_control(),
            seastar::shared_ptr<tls::server_credentials>(),
            [](auto& prom_server, auto& api_server, auto& tls_creds) {

            // Setup TLS credentials if enabled
            future<> tls_setup = make_ready_future<>();
            if (g_config.tls.enabled) {
                if (g_config.tls.cert_path.empty() || g_config.tls.key_path.empty()) {
                    ranvier::log_main.error("TLS enabled but cert_path or key_path not configured");
                    return make_exception_future<>(std::runtime_error("TLS configuration incomplete"));
                }

                auto creds_builder = seastar::make_shared<tls::credentials_builder>();
                tls_setup = creds_builder->set_x509_key_file(
                    g_config.tls.cert_path,
                    g_config.tls.key_path,
                    tls::x509_crt_format::PEM
                ).then([creds_builder, &tls_creds] {
                    tls_creds = creds_builder->build_server_credentials();
                    ranvier::log_main.info("TLS credentials loaded successfully");
                }).handle_exception([](auto ep) {
                    ranvier::log_main.error("Failed to load TLS credentials");
                    return make_exception_future<>(ep);
                });
            }

            return tls_setup.then([&prom_server] {
                // A. Setup Prometheus Server
                return prom_server.start();
            }).then([&prom_server] {
                seastar::prometheus::config pconf;
                pconf.metric_help = "Ranvier AI Router";
                return seastar::prometheus::start(prom_server, pconf);
            }).then([&prom_server] {
                auto addr = socket_address(ipv4_addr(g_config.server.bind_address, g_config.server.metrics_port));
                return prom_server.listen(addr);
            }).then([] {
                ranvier::log_main.info("Prometheus metrics listening on {}:{}",
                    g_config.server.bind_address, g_config.server.metrics_port);
                return make_ready_future<>();
            }).then([&api_server] {
                return api_server.start();
            }).then([&api_server] {
                return api_server.set_routes([](seastar::httpd::routes& r) {
                    // Use controller.local() to get the shard-local HttpController instance
                    // This is critical: each shard's routes must use that shard's controller
                    controller.local().register_routes(r);
                });
            }).then([&api_server, &tls_creds] {
                auto addr = socket_address(ipv4_addr(g_config.server.bind_address, g_config.server.api_port));
                if (tls_creds) {
                    // Pass TLS credentials directly to listen()
                    return api_server.listen(addr, tls_creds);
                }
                return api_server.listen(addr);
            }).then([] {
                auto protocol = g_config.tls.enabled ? "https" : "http";
                ranvier::log_main.info("Ranvier listening on {}://{}:{}",
                    protocol, g_config.server.bind_address, g_config.server.api_port);

                // Setup SIGHUP handler for config hot-reload
                engine().handle_signal(SIGHUP, [] {
                    // Spawn reload as a background task (fire-and-forget with error handling)
                    (void)reload_config().handle_exception([](auto ep) {
                        try {
                            std::rethrow_exception(ep);
                        } catch (const std::exception& e) {
                            ranvier::log_main.error("Config reload exception: {}", e.what());
                        }
                    });
                });
                ranvier::log_main.info("SIGHUP handler registered for config hot-reload");

                // Set up config reload callback for /admin/keys/reload endpoint
                // The callback triggers SIGHUP to reuse existing reload logic
                (void)controller.invoke_on_all([](ranvier::HttpController& c) {
                    c.set_config_reload_callback([]() {
                        // Trigger SIGHUP to self - this is async but reliable
                        if (raise(SIGHUP) == 0) {
                            ranvier::log_main.info("Config reload triggered via API endpoint");
                            return true;
                        }
                        ranvier::log_main.error("Failed to send SIGHUP signal");
                        return false;
                    });
                });
                ranvier::log_main.info("Config reload callback registered for /admin/keys/reload endpoint");

                // Wait Loop with Graceful Shutdown
                auto stop_signal = std::make_shared<promise<>>();

                // Graceful shutdown handler: drain then signal stop
                auto graceful_shutdown = [stop_signal]() {
                    ranvier::log_main.info("Shutdown signal received - initiating graceful shutdown");

                    // Start draining on ALL shards (reject new requests)
                    (void)controller.invoke_on_all([](ranvier::HttpController& c) {
                        c.start_draining();
                    }).then([] {
                        // Wait for in-flight requests on ALL shards to complete
                        return controller.invoke_on_all([](ranvier::HttpController& c) {
                            return c.wait_for_drain();
                        });
                    }).then([stop_signal] {
                        stop_signal->set_value();
                    }).handle_exception([stop_signal](auto ep) {
                        try {
                            std::rethrow_exception(ep);
                        } catch (const std::exception& e) {
                            ranvier::log_main.error("Error during drain: {}", e.what());
                        }
                        // Still proceed with shutdown even on error
                        stop_signal->set_value();
                    });
                };

                engine().handle_signal(SIGINT, graceful_shutdown);
                engine().handle_signal(SIGTERM, graceful_shutdown);
                return stop_signal->get_future();
            }).then([&api_server, &prom_server] {
                ranvier::log_main.info("Drain complete - stopping Ranvier...");

                // Stop Health Checker FIRST
                return health_checker->stop().then([&api_server, &prom_server] {
                    // Stop K8s discovery service
                    if (k8s_discovery && k8s_discovery->is_enabled()) {
                        return k8s_discovery->stop().then([&api_server, &prom_server] {
                            // Stop gossip service
                            return router->stop_gossip().then([&api_server, &prom_server] {
                                return api_server.stop().then([&prom_server] {
                                    return prom_server.stop();
                                });
                            });
                        });
                    }
                    // Stop gossip service
                    return router->stop_gossip().then([&api_server, &prom_server] {
                        return api_server.stop().then([&prom_server] {
                            return prom_server.stop();
                        });
                    });
                });
            }).then([] {
                // Stop the sharded HttpController (cleans up per-shard resources)
                return controller.stop();
            }).finally([] {
                // Log shutdown summary and close persistence
                if (persistence && persistence->is_open()) {
                    ranvier::log_main.info("Shutdown summary:");
                    ranvier::log_main.info("  Persisted backends: {}", persistence->backend_count());
                    ranvier::log_main.info("  Persisted routes:   {}", persistence->route_count());

                    // Checkpoint WAL before closing to ensure all data is durable
                    // This prevents data loss if the process is killed shortly after
                    if (persistence->checkpoint()) {
                        ranvier::log_main.debug("Final WAL checkpoint complete");
                    } else {
                        ranvier::log_main.warn("Final WAL checkpoint failed - some data may be in WAL");
                    }

                    persistence->close();
                    ranvier::log_main.info("Persistence store closed");
                }

                // Shutdown OpenTelemetry tracing (flush pending spans)
                if (ranvier::TracingService::is_enabled()) {
                    ranvier::TracingService::shutdown();
                    ranvier::log_main.info("OpenTelemetry tracing shutdown complete");
                }
            });
        });
    });
}

int main(int argc, char** argv) {
    // Check for --help BEFORE loading config (avoids config errors blocking help)
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << R"(Ranvier Core - Content-aware Layer 7+ Load Balancer for LLM Inference

USAGE:
    ranvier_server [OPTIONS]

DESCRIPTION:
    Ranvier routes LLM requests based on token prefixes rather than
    connection availability, reducing GPU cache thrashing by directing
    requests to backends that already hold relevant KV cache state.

OPTIONS:
    -h, --help              Print this help message and exit
    --help-seastar          Show Seastar framework options
    --help-loggers          Print available logger names
    --config <PATH>         Path to configuration file (default: ranvier.yaml,
                            falls back to built-in defaults if not found)
    --dry-run               Validate configuration and exit (no server start)
    --smp <N>               Number of CPU cores to use
    --memory <SIZE>         Memory to allocate (e.g., 4G)

SIGNALS:
    SIGHUP                  Reload configuration (hot-reload)
    SIGINT, SIGTERM         Graceful shutdown with connection draining

EXAMPLES:
    ranvier_server
        Start with ranvier.yaml if present, otherwise use built-in defaults

    ranvier_server --config /etc/ranvier/config.yaml
        Start with custom config file

    ranvier_server --dry-run
        Validate configuration without starting the server

    ranvier_server --smp 4 --memory 8G
        Start with 4 CPU cores and 8GB memory

For more information, see: https://github.com/ranvier-systems/ranvier-core
)";
            return 0;
        }
    }

    // Load configuration BEFORE Seastar starts
    // This allows us to use config values for server initialization
    std::string config_path = "ranvier.yaml";
    bool dry_run = false;

    // Check for --config and --dry-run arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[i + 1];
        } else if (arg.rfind("--config=", 0) == 0) {
            config_path = arg.substr(9);
        } else if (arg == "--dry-run") {
            dry_run = true;
        }
    }

    try {
        g_config = ranvier::RanvierConfig::load(config_path);
        g_config_path = config_path;  // Store for hot-reload

        // Run dry-run validation if requested
        if (dry_run) {
            return run_dry_run_validation(config_path, g_config);
        }

        // Log config summary (before Seastar logger is available)
        // Check if config file actually exists to report accurately
        std::ifstream config_check(config_path);
        if (config_check.is_open()) {
            config_check.close();
            std::cout << "Ranvier Core - Configuration loaded from " << config_path << "\n";
        } else {
            std::cout << "Ranvier Core - Using built-in defaults (" << config_path << " not found)\n";
        }
        std::cout << "  API Port:     " << g_config.server.api_port << "\n";
        std::cout << "  Metrics Port: " << g_config.server.metrics_port << "\n";
        std::cout << "  Database:     " << g_config.database.path << "\n";
        std::cout << "  Health Check: " << g_config.health.check_interval.count() << "s interval\n";
        std::cout << "  Pool Size:    " << g_config.pool.max_connections_per_host << " per host, "
                  << g_config.pool.max_total_connections << " total\n";
        std::cout << "  Timeouts:     " << g_config.timeouts.connect_timeout.count() << "s connect, "
                  << g_config.timeouts.request_timeout.count() << "s request\n";
        std::cout << "  Min Tokens:   " << g_config.routing.min_token_length << "\n";
        if (g_config.tls.enabled) {
            std::cout << "  TLS:          enabled (cert: " << g_config.tls.cert_path << ")\n";
        } else {
            std::cout << "  TLS:          disabled\n";
        }
        if (!g_config.auth.admin_api_key.empty()) {
            std::cout << "  Admin Auth:   enabled (API key configured)\n";
        } else {
            std::cout << "  Admin Auth:   disabled (no API key)\n";
        }
        if (g_config.rate_limit.enabled) {
            std::cout << "  Rate Limit:   " << g_config.rate_limit.requests_per_second
                      << " req/s, burst " << g_config.rate_limit.burst_size << "\n";
        } else {
            std::cout << "  Rate Limit:   disabled\n";
        }
        if (g_config.retry.max_retries > 0) {
            std::cout << "  Retry:        " << g_config.retry.max_retries << " retries, "
                      << g_config.retry.initial_backoff.count() << "-"
                      << g_config.retry.max_backoff.count() << "ms backoff\n";
        } else {
            std::cout << "  Retry:        disabled\n";
        }
        if (g_config.circuit_breaker.enabled) {
            std::cout << "  Circuit:      " << g_config.circuit_breaker.failure_threshold << " failures, "
                      << g_config.circuit_breaker.recovery_timeout.count() << "s recovery";
            if (g_config.circuit_breaker.fallback_enabled) {
                std::cout << " (fallback on)";
            }
            std::cout << "\n";
        } else {
            std::cout << "  Circuit:      disabled\n";
        }
        std::cout << "  Drain Timeout: " << g_config.shutdown.drain_timeout.count() << "s\n";
        if (g_config.cluster.enabled) {
            std::cout << "  Cluster:      port " << g_config.cluster.gossip_port
                      << ", " << g_config.cluster.peers.size() << " peers\n";
        } else {
            std::cout << "  Cluster:      disabled (standalone mode)\n";
        }
        if (g_config.k8s_discovery.enabled) {
            std::cout << "  K8s Discovery: " << g_config.k8s_discovery.namespace_name
                      << "/" << g_config.k8s_discovery.service_name
                      << " (port " << g_config.k8s_discovery.target_port << ")\n";
        } else {
            std::cout << "  K8s Discovery: disabled\n";
        }
        if (g_config.telemetry.enabled) {
            std::cout << "  Telemetry:    " << g_config.telemetry.otlp_endpoint
                      << " (sample_rate: " << g_config.telemetry.sample_rate << ")\n";
        } else {
            std::cout << "  Telemetry:    disabled\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to load config: " << e.what() << "\n";
        return 1;
    }

    app_template app;

    // Register our custom options with Seastar so they're recognized
    // (We already parsed them above, but Seastar needs to know they exist)
    app.add_options()
        ("config", boost::program_options::value<std::string>()->default_value("ranvier.yaml"),
         "Path to configuration file")
        ("dry-run", "Validate configuration and exit (no server start)");

    return app.run(argc, argv, run);
}
