// Ranvier Core - Application Bootstrap and Lifecycle Management
//
// The Application class manages the lifecycle of all Ranvier services,
// including initialization, startup, shutdown, and configuration reloading.
// It orchestrates the correct startup/shutdown ordering of:
//   - TokenizerService: BPE tokenization for token parsing
//   - RouterService: Prefix-affinity routing and radix tree
//   - HttpController: HTTP endpoint handlers (sharded)
//   - HealthService: Background health checks
//   - AsyncPersistenceManager: Non-blocking SQLite batching
//   - K8sDiscoveryService: Kubernetes endpoint discovery
//   - GossipService: Cluster state synchronization

#pragma once

#include "async_persistence.hpp"
#include "config.hpp"
#include "health_service.hpp"
#include "http_controller.hpp"
#include "k8s_discovery_service.hpp"
#include "persistence.hpp"
#include "router_service.hpp"
#include "tokenizer_service.hpp"

#include <atomic>
#include <memory>

#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/sharded.hh>
#include <seastar/http/httpd.hh>
#include <seastar/net/tls.hh>

namespace ranvier {

// Application lifecycle states
enum class ApplicationState {
    CREATED,      // Initial state
    STARTING,     // startup() in progress
    RUNNING,      // Services running normally
    DRAINING,     // Graceful shutdown initiated
    STOPPING,     // shutdown() in progress
    STOPPED       // All services stopped
};

// Application class manages the lifecycle of all Ranvier services
class Application {
public:
    // Construct with configuration
    explicit Application(RanvierConfig config, std::string config_path);

    // Non-copyable, non-movable (owns unique resources)
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    // Destructor must be defined in .cpp where GossipService is complete
    ~Application();

    // --- Lifecycle ---

    // Initialize and start all services in the correct order.
    // Uses seastar::gate to ensure startup completes before any shutdown.
    // Returns a future that completes when all services are running.
    seastar::future<> startup();

    // Gracefully shut down all services in reverse order.
    // First drains in-flight requests, then stops services.
    // Safe to call even if startup() hasn't completed (waits for gate).
    seastar::future<> shutdown();

    // --- Server Loop ---

    // Start HTTP servers and wait for shutdown signal.
    // This is the main server loop that blocks until SIGINT/SIGTERM.
    // Call after startup() completes.
    seastar::future<> run();

    // Signal graceful shutdown (e.g., from signal handler).
    // This starts the drain sequence and eventually causes run() to return.
    void signal_shutdown();

    // --- Hot Reload ---

    // Reload configuration from the config file.
    // Called on SIGHUP signal.
    seastar::future<> reload_config();

    // --- Accessors ---

    // Get current application state
    ApplicationState state() const { return _state; }

    // Check if application is running
    bool is_running() const { return _state == ApplicationState::RUNNING; }

    // Check if shutdown has been signaled
    bool is_shutting_down() const {
        return _state == ApplicationState::DRAINING ||
               _state == ApplicationState::STOPPING ||
               _state == ApplicationState::STOPPED;
    }

    // Get the configuration (for read-only access)
    const RanvierConfig& config() const { return _config; }

    // Get the controller (for route registration in run())
    seastar::sharded<HttpController>& controller() { return _controller; }

    // Get the router service (only valid after startup() succeeds)
    RouterService* router() { return _router.get(); }

private:
    // --- Configuration ---
    RanvierConfig _config;
    std::string _config_path;

    // --- State ---
    ApplicationState _state = ApplicationState::CREATED;

    // Track which services were successfully started (for safe shutdown)
    bool _controller_started = false;

    // Gate to ensure startup completes before shutdown
    seastar::gate _lifecycle_gate;

    // Promise/future for signaling shutdown
    std::shared_ptr<seastar::promise<>> _stop_signal;

    // Counter for SIGINT signals - second SIGINT triggers hard kill.
    // Atomic for robustness, though Seastar signals run on shard 0.
    std::atomic<int> _sigint_count{0};

    // --- Services (owned by Application) ---

    // Infrastructure layer
    TokenizerService _tokenizer;

    // Domain layer
    std::unique_ptr<RouterService> _router;

    // Presentation layer (sharded for multi-core)
    seastar::sharded<HttpController> _controller;

    // Health monitoring
    std::unique_ptr<HealthService> _health_checker;

    // Persistence layer
    std::unique_ptr<PersistenceStore> _persistence;
    std::unique_ptr<AsyncPersistenceManager> _async_persistence;

    // Service discovery
    std::unique_ptr<K8sDiscoveryService> _k8s_discovery;

    // HTTP servers (owned during run())
    std::unique_ptr<seastar::httpd::http_server_control> _api_server;
    std::unique_ptr<seastar::httpd::http_server_control> _metrics_server;

    // TLS credentials (if TLS enabled)
    seastar::shared_ptr<seastar::tls::server_credentials> _tls_creds;

    // --- Private Helpers: Initialization ---

    // Initialize tokenizer from JSON file
    void init_tokenizer();

    // Initialize health checker service
    void init_health_checker();

    // Initialize K8s discovery service (if enabled)
    void init_k8s_discovery();

    // Initialize persistence layer
    seastar::future<> init_persistence();

    // Load persisted state (backends and routes) from SQLite
    seastar::future<> load_persisted_state();

    // --- Private Helpers: Configuration ---

    // Build HttpControllerConfig from RanvierConfig
    HttpControllerConfig build_controller_config() const;

    // Build K8sDiscoveryConfig from RanvierConfig
    K8sDiscoveryConfig build_k8s_config() const;

    // Build HealthServiceConfig from RanvierConfig
    HealthServiceConfig build_health_config() const;

    // Build AsyncPersistenceConfig from RanvierConfig
    AsyncPersistenceConfig build_persistence_config() const;

    // --- Private Helpers: Server Lifecycle ---

    // Setup TLS credentials (if enabled)
    seastar::future<> setup_tls();

    // Start HTTP servers
    seastar::future<> start_servers();

    // Stop HTTP servers
    seastar::future<> stop_servers();

    // Setup signal handlers (SIGHUP, SIGINT, SIGTERM)
    void setup_signal_handlers();

    // --- Private Helpers: Shutdown ---

    // Drain in-flight requests on all controller shards
    seastar::future<> drain_requests();

    // Stop all services in reverse order
    seastar::future<> stop_services();

    // Cleanup on final shutdown
    void cleanup();
};

}  // namespace ranvier
