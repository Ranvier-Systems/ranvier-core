// Ranvier Core - Content-aware Layer 7+ Load Balancer for LLM Inference
//
// Architecture:
// 1. Infrastructure Layer (TokenizerService): Handles tokenization
// 2. Domain Layer (RouterService): Handles routing logic (Radix Tree, Broadcasting)
// 3. Presentation Layer (HttpController): Handles HTTP endpoints
// 4. Persistence Layer (SqlitePersistence): Handles durable storage of routes/backends

#include "config.hpp"
#include "health_service.hpp"
#include "http_controller.hpp"
#include "logging.hpp"
#include "router_service.hpp"
#include "sqlite_persistence.hpp"
#include "tokenizer_service.hpp"

#include <fstream>
#include <streambuf>

#include <seastar/core/app-template.hh>
#include <seastar/core/prometheus.hh>
#include <seastar/core/reactor.hh>
#include <seastar/http/httpd.hh>
#include <seastar/net/inet_address.hh>

using namespace seastar;

// Global configuration (loaded before Seastar starts)
static ranvier::RanvierConfig g_config;

// Services (Global/Static Scope for MVP)
ranvier::TokenizerService tokenizer;
ranvier::RouterService router;

// These are initialized in run() after config is loaded
std::unique_ptr<ranvier::HttpController> controller;
std::unique_ptr<ranvier::HealthService> health_checker;
std::unique_ptr<ranvier::PersistenceStore> persistence;

// Helper to load persisted state into the router
future<> load_persisted_state() {
    if (!persistence || !persistence->is_open()) {
        ranvier::log_main.info("No persistence store - starting with empty state");
        return make_ready_future<>();
    }

    // Load backends first
    auto backends = persistence->load_backends();
    auto routes = persistence->load_routes();

    if (backends.empty() && routes.empty()) {
        ranvier::log_main.info("Persistence store is empty - starting fresh");
        return make_ready_future<>();
    }

    ranvier::log_main.info("Restoring state from persistence:");
    ranvier::log_main.info("  Backends: {}", backends.size());
    ranvier::log_main.info("  Routes:   {}", routes.size());

    // Log each backend at info level for visibility
    for (const auto& rec : backends) {
        ranvier::log_main.info("  - Backend {} -> {}:{}", rec.id, rec.ip, rec.port);
    }

    return do_with(std::move(backends), std::move(routes), [](auto& backends, auto& routes) {
        return do_for_each(backends, [](const ranvier::BackendRecord& rec) {
            socket_address addr(ipv4_addr(rec.ip, rec.port));
            return router.register_backend_global(rec.id, addr);
        }).then([&routes] {
            return do_for_each(routes, [](const ranvier::RouteRecord& rec) {
                return router.learn_route_global(rec.tokens, rec.backend_id);
            });
        });
    }).then([] {
        ranvier::log_main.info("State restoration complete");
    });
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

    // 2. Init Controller with config
    ranvier::HttpControllerConfig ctrl_config;
    ctrl_config.pool.max_connections_per_host = g_config.pool.max_connections_per_host;
    ctrl_config.pool.idle_timeout = g_config.pool.idle_timeout;
    ctrl_config.pool.max_total_connections = g_config.pool.max_total_connections;
    ctrl_config.min_token_length = g_config.routing.min_token_length;
    ctrl_config.connect_timeout = g_config.timeouts.connect_timeout;
    ctrl_config.request_timeout = g_config.timeouts.request_timeout;
    controller = std::make_unique<ranvier::HttpController>(tokenizer, router, ctrl_config);

    // 3. Init Persistence
    persistence = ranvier::create_persistence_store();
    if (persistence->open(g_config.database.path)) {
        ranvier::log_main.info("Persistence initialized (SQLite: {})", g_config.database.path);
        controller->set_persistence(persistence.get());
    } else {
        ranvier::log_main.warn("Failed to open persistence store - running without persistence");
    }

    // 4. Init Health Checker with config
    ranvier::HealthServiceConfig health_config;
    health_config.check_interval = g_config.health.check_interval;
    health_config.check_timeout = g_config.health.check_timeout;
    health_config.failure_threshold = g_config.health.failure_threshold;
    health_config.recovery_threshold = g_config.health.recovery_threshold;
    health_checker = std::make_unique<ranvier::HealthService>(router, health_config);
    health_checker->start();

    // 5. Load persisted state, then start servers
    return load_persisted_state().then([] {
        // 6. Start Servers (Metrics + API)
        return do_with(seastar::httpd::http_server_control(), seastar::httpd::http_server_control(), [](auto& prom_server, auto& api_server) {

            // A. Setup Prometheus Server
            return prom_server.start().then([&prom_server] {
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
                    controller->register_routes(r);
                });
            }).then([&api_server] {
                auto addr = socket_address(ipv4_addr(g_config.server.bind_address, g_config.server.api_port));
                return api_server.listen(addr);
            }).then([] {
                ranvier::log_main.info("Ranvier listening on {}:{}",
                    g_config.server.bind_address, g_config.server.api_port);

                // Wait Loop
                auto stop_signal = std::make_shared<promise<>>();
                engine().handle_signal(SIGINT, [stop_signal] { stop_signal->set_value(); });
                engine().handle_signal(SIGTERM, [stop_signal] { stop_signal->set_value(); });
                return stop_signal->get_future();
            }).then([&api_server, &prom_server] {
                ranvier::log_main.info("Stopping Ranvier...");

                // Stop Health Checker FIRST
                return health_checker->stop().then([&api_server, &prom_server] {
                    return api_server.stop().then([&prom_server] {
                        return prom_server.stop();
                    });
                });
            }).finally([] {
                // Log shutdown summary and close persistence
                if (persistence && persistence->is_open()) {
                    ranvier::log_main.info("Shutdown summary:");
                    ranvier::log_main.info("  Persisted backends: {}", persistence->backend_count());
                    ranvier::log_main.info("  Persisted routes:   {}", persistence->route_count());
                    persistence->close();
                    ranvier::log_main.info("Persistence store closed");
                }
            });
        });
    });
}

int main(int argc, char** argv) {
    // Load configuration BEFORE Seastar starts
    // This allows us to use config values for server initialization
    std::string config_path = "ranvier.yaml";

    // Check for --config argument
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[i + 1];
        } else if (arg.rfind("--config=", 0) == 0) {
            config_path = arg.substr(9);
        }
    }

    try {
        g_config = ranvier::RanvierConfig::load(config_path);

        // Log config summary (before Seastar logger is available)
        std::cout << "Ranvier Core - Configuration loaded\n";
        std::cout << "  API Port:     " << g_config.server.api_port << "\n";
        std::cout << "  Metrics Port: " << g_config.server.metrics_port << "\n";
        std::cout << "  Database:     " << g_config.database.path << "\n";
        std::cout << "  Health Check: " << g_config.health.check_interval.count() << "s interval\n";
        std::cout << "  Pool Size:    " << g_config.pool.max_connections_per_host << " per host, "
                  << g_config.pool.max_total_connections << " total\n";
        std::cout << "  Timeouts:     " << g_config.timeouts.connect_timeout.count() << "s connect, "
                  << g_config.timeouts.request_timeout.count() << "s request\n";
        std::cout << "  Min Tokens:   " << g_config.routing.min_token_length << "\n";
    } catch (const std::exception& e) {
        std::cerr << "Failed to load config: " << e.what() << "\n";
        return 1;
    }

    app_template app;
    return app.run(argc, argv, run);
}
