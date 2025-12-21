// Ranvier Core - Content-aware Layer 7+ Load Balancer for LLM Inference
//
// Architecture:
// 1. Infrastructure Layer (TokenizerService): Handles tokenization
// 2. Domain Layer (RouterService): Handles routing logic (Radix Tree, Broadcasting)
// 3. Presentation Layer (HttpController): Handles HTTP endpoints
// 4. Persistence Layer (SqlitePersistence): Handles durable storage of routes/backends

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
using namespace seastar::httpd;

// Services (Global/Static Scope for MVP)
ranvier::TokenizerService tokenizer;
ranvier::RouterService router;
ranvier::HttpController controller{tokenizer, router};
ranvier::HealthService health_checker{router};
std::unique_ptr<ranvier::PersistenceStore> persistence;

// Helper to load persisted state into the router
future<> load_persisted_state() {
    if (!persistence || !persistence->is_open()) {
        return make_ready_future<>();
    }

    // Load backends first
    auto backends = persistence->load_backends();
    ranvier::log_main.info("Loading {} backends from persistence", backends.size());

    return do_with(std::move(backends), [](auto& backends) {
        return do_for_each(backends, [](const ranvier::BackendRecord& rec) {
            socket_address addr(ipv4_addr(rec.ip, rec.port));
            ranvier::log_main.debug("Restoring backend {} -> {}:{}", rec.id, rec.ip, rec.port);
            return router.register_backend_global(rec.id, addr);
        });
    }).then([] {
        // Then load routes
        auto routes = persistence->load_routes();
        ranvier::log_main.info("Loading {} routes from persistence", routes.size());

        return do_with(std::move(routes), [](auto& routes) {
            return do_for_each(routes, [](const ranvier::RouteRecord& rec) {
                ranvier::log_main.debug("Restoring route ({} tokens) -> backend {}", rec.tokens.size(), rec.backend_id);
                return router.learn_route_global(rec.tokens, rec.backend_id);
            });
        });
    });
}

future<> run() {
    // 1. Init Infrastructure
    try {
        std::ifstream t("assets/gpt2.json");
        if (!t.is_open()) {
            throw std::runtime_error("Could not find assets/gpt2.json");
        }
        std::string json_str((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());
        tokenizer.load_from_json(json_str);
        ranvier::log_main.info("Services initialized (GPT-2 Tokenizer)");
    } catch (const std::exception& e) {
        ranvier::log_main.error("Service init failed: {}", e.what());
        return make_ready_future<>();
    }

    // 2. Init Persistence
    persistence = ranvier::create_persistence_store();
    if (persistence->open("ranvier.db")) {
        ranvier::log_main.info("Persistence initialized (SQLite: ranvier.db)");
        controller.set_persistence(persistence.get());
    } else {
        ranvier::log_main.warn("Failed to open persistence store - running without persistence");
    }

    // Start Health Checker
    health_checker.start();

    // 3. Load persisted state, then start servers
    return load_persisted_state().then([] {
        // 4. Start Servers (Metrics + API)
        return do_with(http_server_control(), http_server_control(), [](auto& prom_server, auto& api_server) {

            // A. Setup Prometheus Server (Port 9180)
            return prom_server.start().then([&prom_server] {
                seastar::prometheus::config pconf;
                pconf.metric_help = "Ranvier AI Router";
                return seastar::prometheus::start(prom_server, pconf);
            }).then([&prom_server] {
                return prom_server.listen(socket_address(ipv4_addr("0.0.0.0", 9180)));
            }).then([] {
                ranvier::log_main.info("Prometheus metrics listening on port 9180");
                return make_ready_future<>();
            }).then([&api_server] {
                return api_server.start();
            }).then([&api_server] {
                return api_server.set_routes([](routes& r) {
                    controller.register_routes(r);
                });
            }).then([&api_server] {
                return api_server.listen(socket_address(ipv4_addr("0.0.0.0", 8080)));
            }).then([] {
                ranvier::log_main.info("Ranvier listening on port 8080");

                // Wait Loop
                auto stop_signal = std::make_shared<promise<>>();
                engine().handle_signal(SIGINT, [stop_signal] { stop_signal->set_value(); });
                engine().handle_signal(SIGTERM, [stop_signal] { stop_signal->set_value(); });
                return stop_signal->get_future();
            }).then([&api_server, &prom_server] {
                ranvier::log_main.info("Stopping Ranvier...");

                // Stop Health Checker FIRST
                return health_checker.stop().then([&api_server, &prom_server] {
                    return api_server.stop().then([&prom_server] {
                        return prom_server.stop();
                    });
                });
            }).finally([] {
                // Close persistence store on shutdown
                if (persistence) {
                    persistence->close();
                    ranvier::log_main.info("Persistence store closed");
                }
            });
        });
    });
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, run);
}
