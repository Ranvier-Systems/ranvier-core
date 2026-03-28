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
#include "router_service.hpp"
#include "shard_load_balancer.hpp"
#include "sharded_config.hpp"
#include "tokenizer_service.hpp"
#include "tokenizer_thread_pool.hpp"

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

    // Get the master configuration (for read-only access)
    const RanvierConfig& config() const { return _config; }

    // Get the sharded config for the local shard (for services to use)
    // This provides lock-free, per-core access to configuration
    const RanvierConfig& local_config() const { return _sharded_config.local().config(); }

    // Get the sharded config container (for invoke_on_all operations)
    seastar::sharded<ShardedConfig>& sharded_config() { return _sharded_config; }

    // Get the controller (for route registration in run())
    seastar::sharded<HttpController>& controller() { return _controller; }

    // Get the router service (only valid after startup() succeeds)
    RouterService* router() { return _router.get(); }

private:
    // --- Configuration ---
    // Master config (used for initial loading and reload operations)
    RanvierConfig _config;
    std::string _config_path;

    // Sharded config - one copy per CPU core for lock-free access
    // Services can receive const RanvierConfig& from their local shard
    seastar::sharded<ShardedConfig> _sharded_config;

    // --- State ---
    ApplicationState _state = ApplicationState::CREATED;

    // Track which services were successfully started (for safe shutdown)
    bool _sharded_config_started = false;
    bool _controller_started = false;

    // Gate to ensure startup completes before shutdown
    seastar::gate _lifecycle_gate;

    // Promise/future for signaling shutdown (unique_ptr: single owner, no sharing needed)
    std::unique_ptr<seastar::promise<>> _stop_signal;

    // Counter for SIGINT signals - second SIGINT triggers hard kill.
    // Atomic for robustness, though Seastar signals run on shard 0.
    std::atomic<int> _sigint_count{0};

    // Atomic flag to ensure signal_shutdown() is idempotent and race-safe.
    // Prevents concurrent execution from multiple signal handlers.
    std::atomic<bool> _shutdown_initiated{false};

    // Shutdown timing for metrics/debugging
    std::chrono::steady_clock::time_point _shutdown_start_time;

    // Rate limiting for config hot-reload (prevents reload storms from rapid SIGHUPs)
    std::chrono::steady_clock::time_point _last_reload_time;

    // --- Services (owned by Application) ---

    // Infrastructure layer - sharded for thread safety
    // Each shard has its own TokenizerService instance because tokenizers-cpp
    // is NOT thread-safe for concurrent Encode() calls on the same instance
    seastar::sharded<TokenizerService> _tokenizer;
    bool _tokenizer_started = false;
    std::string _tokenizer_json;  // Cached JSON for loading on each shard

    // Tokenizer thread pool for non-blocking FFI offload
    // Each shard has its own worker thread with dedicated tokenizer instance
    seastar::sharded<TokenizerThreadPool> _tokenizer_thread_pool;
    bool _tokenizer_thread_pool_started = false;

    // Domain layer
    std::unique_ptr<RouterService> _router;

    // Presentation layer (sharded for multi-core)
    seastar::sharded<HttpController> _controller;

    // Shard load balancer (P2C algorithm for cross-shard dispatch)
    seastar::sharded<ShardLoadBalancer> _load_balancer;
    bool _load_balancer_started = false;

    // Health monitoring
    std::unique_ptr<HealthService> _health_checker;

    // Persistence layer (AsyncPersistenceManager owns the underlying SQLite store)
    std::unique_ptr<AsyncPersistenceManager> _async_persistence;

    // Service discovery
    std::unique_ptr<K8sDiscoveryService> _k8s_discovery;

    // HTTP servers (owned during run())
    std::unique_ptr<seastar::httpd::http_server_control> _api_server;
    std::unique_ptr<seastar::httpd::http_server_control> _metrics_server;

    // TLS credentials (if TLS enabled)
    seastar::shared_ptr<seastar::tls::server_credentials> _tls_creds;

    // --- Private Helpers: Local Mode ---

    // Apply local mode overrides to config before services start.
    // When local_mode.enabled, disables clustering and/or persistence
    // based on local_mode sub-flags. Must be called before sharded_config.start().
    void apply_local_mode_overrides();

    // --- Private Helpers: Initialization ---

    // Initialize tokenizer from JSON file (async, uses Seastar DMA file I/O)
    seastar::future<> init_tokenizer();

    // Initialize health checker service
    void init_health_checker();

    // Initialize K8s discovery service (if enabled)
    void init_k8s_discovery();

    // Initialize persistence layer
    seastar::future<> init_persistence();

    // Load persisted state (backends and routes) from SQLite
    seastar::future<> load_persisted_state();

    // --- Private Helpers: Configuration ---

    // Build HttpControllerConfig from member _config
    HttpControllerConfig build_controller_config() const;

    // Build HttpControllerConfig from a specific config (for hot-reload)
    static HttpControllerConfig build_controller_config_from(const RanvierConfig& config);

    // Build K8sDiscoveryConfig from RanvierConfig
    K8sDiscoveryConfig build_k8s_config() const;

    // Build HealthServiceConfig from RanvierConfig
    HealthServiceConfig build_health_config() const;

    // Build AsyncPersistenceConfig from RanvierConfig
    AsyncPersistenceConfig build_persistence_config() const;

    // Log warnings for config changes that require restart
    void log_non_reloadable_changes(const RanvierConfig& new_config) const;

    // Auto-configure max_token_id from tokenizer vocabulary size
    // Called during startup and optionally during config reload
    // Updates both _config and _sharded_config for consistency
    seastar::future<> apply_vocab_size_config();

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

    // Phase 1: Broadcast DRAINING state to cluster peers (with timeout)
    seastar::future<> phase_broadcast_draining();

    // Phase 2: Drain in-flight requests on all controller shards (with timeout)
    seastar::future<> phase_drain_requests();

    // Phase 3: Stop all services in reverse order
    seastar::future<> phase_stop_services();

    // Legacy: Called by phase_drain_requests
    seastar::future<> drain_requests();

    // Stop all services in reverse order
    seastar::future<> stop_services();

    // Cleanup on final shutdown
    void cleanup();

    // Log shutdown phase completion with timing
    void log_phase_complete(const char* phase_name,
                            std::chrono::steady_clock::time_point phase_start) const;
};

}  // namespace ranvier
