// Ranvier Core - Configuration Management
//
// Supports loading configuration from:
// 1. YAML config file (default: ranvier.yaml)
// 2. Environment variables (override file settings)
// 3. Built-in defaults (fallback)

#pragma once

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include <yaml-cpp/yaml.h>

namespace ranvier {

// Server configuration (HTTP API and Prometheus)
struct ServerConfig {
    std::string bind_address = "0.0.0.0";
    uint16_t api_port = 8080;
    uint16_t metrics_port = 9180;
};

// Database/persistence configuration
struct DatabaseConfig {
    std::string path = "ranvier.db";
    std::string journal_mode = "WAL";
    std::string synchronous = "NORMAL";
};

// Health check configuration
struct HealthConfig {
    std::chrono::seconds check_interval{5};
    std::chrono::seconds check_timeout{3};
    uint32_t failure_threshold = 3;
    uint32_t recovery_threshold = 2;
};

// Connection pool configuration
struct PoolConfig {
    size_t max_connections_per_host = 10;
    std::chrono::seconds idle_timeout{60};
    size_t max_total_connections = 100;
    bool tcp_nodelay = true;
};

// Routing and caching configuration
struct RoutingConfig {
    size_t min_token_length = 4;  // Minimum tokens before caching a route
    uint32_t backend_retry_limit = 5;  // Max attempts to find a live backend
    uint32_t block_alignment = 16;  // vLLM PagedAttention block size for route alignment
    size_t max_routes = 100000;  // Maximum number of routes in the prefix cache (0 = unlimited)
    std::chrono::seconds ttl_seconds{3600};  // TTL for cached routes (1 hour default)
    std::chrono::seconds backend_drain_timeout{60};  // Time to wait before fully removing a draining backend
    bool enable_token_forwarding = false;  // Forward pre-computed token IDs to backends (vLLM prompt_token_ids)
    bool accept_client_tokens = false;  // Accept pre-tokenized prompt_token_ids from clients for routing
    int32_t max_token_id = 100000;  // Maximum valid token ID for validation (security: reject out-of-range tokens)

    // Prefix-affinity routing: route requests with same prefix to same backend for KV cache reuse
    bool prefix_affinity_enabled = true;  // Enable prefix-affinity routing (default: true)
    size_t prefix_token_length = 128;  // Number of tokens to use as routing key (default: 128)
};

// Timeout configuration
struct TimeoutConfig {
    std::chrono::seconds connect_timeout{5};      // Timeout for establishing backend connection
    std::chrono::seconds request_timeout{300};    // Total timeout for entire request (5 min for LLM inference)
};

// Tokenizer/assets configuration
struct AssetsConfig {
    std::string tokenizer_path = "assets/gpt2.json";
};

// TLS configuration
struct TlsConfig {
    bool enabled = false;                     // Enable TLS for API server
    std::string cert_path = "";               // Path to certificate file (PEM)
    std::string key_path = "";                // Path to private key file (PEM)
};

// Authentication configuration
struct AuthConfig {
    std::string admin_api_key = "";           // API key for admin endpoints (empty = no auth)
};

// Rate limiting configuration
struct RateLimitConfig {
    bool enabled = false;                     // Enable rate limiting
    uint32_t requests_per_second = 100;       // Max requests per second per client
    uint32_t burst_size = 50;                 // Allow burst above rate limit
};

// Retry configuration for transient failures
struct RetryConfig {
    uint32_t max_retries = 3;                         // Maximum retry attempts (0 = no retries)
    std::chrono::milliseconds initial_backoff{100};   // Initial backoff delay
    std::chrono::milliseconds max_backoff{5000};      // Maximum backoff delay
    double backoff_multiplier = 2.0;                  // Exponential backoff multiplier
};

// Circuit breaker configuration for graceful degradation
struct CircuitBreakerConfig {
    bool enabled = true;                              // Enable circuit breaker
    uint32_t failure_threshold = 5;                   // Failures before opening circuit
    uint32_t success_threshold = 2;                   // Successes in half-open to close
    std::chrono::seconds recovery_timeout{30};        // Time before trying half-open
    bool fallback_enabled = true;                     // Try alternative backends on failure
};

// Shutdown configuration for graceful drain
struct ShutdownConfig {
    std::chrono::seconds drain_timeout{30};           // Max time to wait for in-flight requests
};

// Discovery type for DNS-based peer discovery
enum class DiscoveryType {
    STATIC,  // Use static peer list only (default)
    A,       // Use DNS A records (IPs only, use default gossip_port)
    SRV      // Use DNS SRV records (IPs and ports)
};

// Cluster configuration for distributed mode (gossip-based state sync)
struct ClusterConfig {
    bool enabled = false;                                  // Enable distributed mode
    uint16_t gossip_port = 9190;                          // UDP port for gossip protocol
    std::vector<std::string> peers;                       // Static list of peer addresses (IP:Port)
    std::chrono::milliseconds gossip_interval{1000};      // Interval between gossip rounds
    std::chrono::seconds gossip_heartbeat_interval{5};    // Interval between heartbeat broadcasts
    std::chrono::seconds gossip_peer_timeout{15};         // Time before marking a peer as dead

    // DNS-based peer discovery
    std::string discovery_dns_name;                        // DNS name to query for peer discovery (empty = disabled)
    DiscoveryType discovery_type = DiscoveryType::STATIC;  // Type of DNS records to query
    std::chrono::seconds discovery_refresh_interval{30};   // Interval between DNS refresh queries
};

// Kubernetes service discovery configuration
struct K8sDiscoveryConfig {
    bool enabled = false;                                  // Enable K8s discovery
    std::string api_server = "https://kubernetes.default.svc";  // K8s API server URL
    std::string namespace_name = "default";                // Namespace to watch
    std::string service_name;                              // Service name to watch (required if enabled)
    uint16_t target_port = 8080;                          // Target port for GPU backends

    // Authentication (uses in-cluster service account by default)
    std::string token_path = "/var/run/secrets/kubernetes.io/serviceaccount/token";
    std::string ca_cert_path = "/var/run/secrets/kubernetes.io/serviceaccount/ca.crt";

    // Polling configuration
    std::chrono::seconds poll_interval{30};                // Interval between full syncs
    std::chrono::seconds watch_timeout{300};               // Watch connection timeout

    // Watch reconnection settings
    std::chrono::seconds watch_reconnect_delay{5};         // Delay before reconnecting watch after failure
    std::chrono::seconds watch_reconnect_max_delay{60};    // Maximum reconnect delay (for exponential backoff)

    // TLS settings
    bool verify_tls = true;                                // Verify server certificate

    // Label selector (optional, filters EndpointSlices)
    std::string label_selector;
};

// Top-level configuration
struct RanvierConfig {
    ServerConfig server;
    DatabaseConfig database;
    HealthConfig health;
    PoolConfig pool;
    RoutingConfig routing;
    TimeoutConfig timeouts;
    AssetsConfig assets;
    TlsConfig tls;
    AuthConfig auth;
    RateLimitConfig rate_limit;
    RetryConfig retry;
    CircuitBreakerConfig circuit_breaker;
    ShutdownConfig shutdown;
    ClusterConfig cluster;
    K8sDiscoveryConfig k8s_discovery;

    // Load configuration from YAML file
    static RanvierConfig load(const std::string& config_path);

    // Load with defaults (no file)
    static RanvierConfig defaults();

    // Validate configuration, returns error message if invalid, nullopt if valid
    static std::optional<std::string> validate(const RanvierConfig& config);

private:
    // Apply environment variable overrides
    void apply_env_overrides();

    // Helper to get env var as optional string
    static std::optional<std::string> get_env(const char* name);

    // Helper to parse numeric env vars
    template<typename T>
    static std::optional<T> get_env_as(const char* name);
};

// Implementation

inline std::optional<std::string> RanvierConfig::get_env(const char* name) {
    const char* val = std::getenv(name);
    if (val && val[0] != '\0') {
        return std::string(val);
    }
    return std::nullopt;
}

template<typename T>
inline std::optional<T> RanvierConfig::get_env_as(const char* name) {
    auto val = get_env(name);
    if (!val) return std::nullopt;

    std::istringstream iss(*val);
    T result;
    if (iss >> result) {
        return result;
    }
    return std::nullopt;
}

inline void RanvierConfig::apply_env_overrides() {
    // Server overrides
    if (auto v = get_env("RANVIER_BIND_ADDRESS")) server.bind_address = *v;
    if (auto v = get_env_as<uint16_t>("RANVIER_API_PORT")) server.api_port = *v;
    if (auto v = get_env_as<uint16_t>("RANVIER_METRICS_PORT")) server.metrics_port = *v;

    // Database overrides
    if (auto v = get_env("RANVIER_DB_PATH")) database.path = *v;
    if (auto v = get_env("RANVIER_DB_JOURNAL_MODE")) database.journal_mode = *v;
    if (auto v = get_env("RANVIER_DB_SYNCHRONOUS")) database.synchronous = *v;

    // Health check overrides
    if (auto v = get_env_as<int>("RANVIER_HEALTH_CHECK_INTERVAL")) {
        health.check_interval = std::chrono::seconds(*v);
    }
    if (auto v = get_env_as<int>("RANVIER_HEALTH_CHECK_TIMEOUT")) {
        health.check_timeout = std::chrono::seconds(*v);
    }
    if (auto v = get_env_as<uint32_t>("RANVIER_HEALTH_FAILURE_THRESHOLD")) {
        health.failure_threshold = *v;
    }

    // Pool overrides
    if (auto v = get_env_as<size_t>("RANVIER_POOL_MAX_PER_HOST")) {
        pool.max_connections_per_host = *v;
    }
    if (auto v = get_env_as<int>("RANVIER_POOL_IDLE_TIMEOUT")) {
        pool.idle_timeout = std::chrono::seconds(*v);
    }
    if (auto v = get_env_as<size_t>("RANVIER_POOL_MAX_TOTAL")) {
        pool.max_total_connections = *v;
    }

    // Routing overrides
    if (auto v = get_env_as<size_t>("RANVIER_MIN_TOKEN_LENGTH")) {
        routing.min_token_length = *v;
    }
    if (auto v = get_env_as<uint32_t>("RANVIER_BLOCK_ALIGNMENT")) {
        routing.block_alignment = *v;
    }
    if (auto v = get_env_as<size_t>("RANVIER_MAX_ROUTES")) {
        routing.max_routes = *v;
    }
    if (auto v = get_env_as<int>("RANVIER_ROUTE_TTL_SECONDS")) {
        routing.ttl_seconds = std::chrono::seconds(*v);
    }
    if (auto v = get_env_as<int>("RANVIER_BACKEND_DRAIN_TIMEOUT")) {
        routing.backend_drain_timeout = std::chrono::seconds(*v);
    }
    if (auto v = get_env("RANVIER_ENABLE_TOKEN_FORWARDING")) {
        routing.enable_token_forwarding = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_ACCEPT_CLIENT_TOKENS")) {
        routing.accept_client_tokens = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env_as<int32_t>("RANVIER_MAX_TOKEN_ID")) {
        routing.max_token_id = *v;
    }
    if (auto v = get_env("RANVIER_PREFIX_AFFINITY_ENABLED")) {
        routing.prefix_affinity_enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    // Also support RANVIER_ROUTING_MODE for benchmark compatibility
    // "prefix" enables prefix-affinity, "round_robin" disables it
    if (auto v = get_env("RANVIER_ROUTING_MODE")) {
        routing.prefix_affinity_enabled = (*v == "prefix");
    }
    if (auto v = get_env_as<size_t>("RANVIER_PREFIX_TOKEN_LENGTH")) {
        routing.prefix_token_length = *v;
    }

    // Timeout overrides
    if (auto v = get_env_as<int>("RANVIER_CONNECT_TIMEOUT")) {
        timeouts.connect_timeout = std::chrono::seconds(*v);
    }
    if (auto v = get_env_as<int>("RANVIER_REQUEST_TIMEOUT")) {
        timeouts.request_timeout = std::chrono::seconds(*v);
    }

    // Assets overrides
    if (auto v = get_env("RANVIER_TOKENIZER_PATH")) assets.tokenizer_path = *v;

    // TLS overrides
    if (auto v = get_env("RANVIER_TLS_ENABLED")) {
        tls.enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_TLS_CERT_PATH")) tls.cert_path = *v;
    if (auto v = get_env("RANVIER_TLS_KEY_PATH")) tls.key_path = *v;

    // Auth overrides
    if (auto v = get_env("RANVIER_ADMIN_API_KEY")) auth.admin_api_key = *v;

    // Rate limit overrides
    if (auto v = get_env("RANVIER_RATE_LIMIT_ENABLED")) {
        rate_limit.enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env_as<uint32_t>("RANVIER_RATE_LIMIT_RPS")) {
        rate_limit.requests_per_second = *v;
    }
    if (auto v = get_env_as<uint32_t>("RANVIER_RATE_LIMIT_BURST")) {
        rate_limit.burst_size = *v;
    }

    // Retry overrides
    if (auto v = get_env_as<uint32_t>("RANVIER_RETRY_MAX")) {
        retry.max_retries = *v;
    }
    if (auto v = get_env_as<int>("RANVIER_RETRY_INITIAL_BACKOFF_MS")) {
        retry.initial_backoff = std::chrono::milliseconds(*v);
    }
    if (auto v = get_env_as<int>("RANVIER_RETRY_MAX_BACKOFF_MS")) {
        retry.max_backoff = std::chrono::milliseconds(*v);
    }

    // Circuit breaker overrides
    if (auto v = get_env("RANVIER_CIRCUIT_BREAKER_ENABLED")) {
        circuit_breaker.enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env_as<uint32_t>("RANVIER_CIRCUIT_BREAKER_FAILURE_THRESHOLD")) {
        circuit_breaker.failure_threshold = *v;
    }
    if (auto v = get_env_as<uint32_t>("RANVIER_CIRCUIT_BREAKER_SUCCESS_THRESHOLD")) {
        circuit_breaker.success_threshold = *v;
    }
    if (auto v = get_env_as<int>("RANVIER_CIRCUIT_BREAKER_RECOVERY_TIMEOUT")) {
        circuit_breaker.recovery_timeout = std::chrono::seconds(*v);
    }
    if (auto v = get_env("RANVIER_CIRCUIT_BREAKER_FALLBACK")) {
        circuit_breaker.fallback_enabled = (*v == "1" || *v == "true" || *v == "yes");
    }

    // Shutdown overrides
    if (auto v = get_env_as<int>("RANVIER_SHUTDOWN_DRAIN_TIMEOUT")) {
        shutdown.drain_timeout = std::chrono::seconds(*v);
    }

    // Cluster overrides
    if (auto v = get_env("RANVIER_CLUSTER_ENABLED")) {
        cluster.enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env_as<uint16_t>("RANVIER_CLUSTER_GOSSIP_PORT")) {
        cluster.gossip_port = *v;
    }
    if (auto v = get_env("RANVIER_CLUSTER_PEERS")) {
        // Parse comma-separated peer list
        cluster.peers.clear();
        std::istringstream iss(*v);
        std::string peer;
        while (std::getline(iss, peer, ',')) {
            // Trim whitespace
            size_t start = peer.find_first_not_of(" \t");
            size_t end = peer.find_last_not_of(" \t");
            if (start != std::string::npos && end != std::string::npos) {
                cluster.peers.push_back(peer.substr(start, end - start + 1));
            }
        }
    }
    if (auto v = get_env_as<int>("RANVIER_CLUSTER_GOSSIP_INTERVAL_MS")) {
        cluster.gossip_interval = std::chrono::milliseconds(*v);
    }
    if (auto v = get_env_as<int>("RANVIER_CLUSTER_GOSSIP_HEARTBEAT_INTERVAL")) {
        cluster.gossip_heartbeat_interval = std::chrono::seconds(*v);
    }
    if (auto v = get_env_as<int>("RANVIER_CLUSTER_GOSSIP_PEER_TIMEOUT")) {
        cluster.gossip_peer_timeout = std::chrono::seconds(*v);
    }
    if (auto v = get_env("RANVIER_CLUSTER_DISCOVERY_DNS_NAME")) {
        cluster.discovery_dns_name = *v;
    }
    if (auto v = get_env("RANVIER_CLUSTER_DISCOVERY_TYPE")) {
        if (*v == "A" || *v == "a") {
            cluster.discovery_type = DiscoveryType::A;
        } else if (*v == "SRV" || *v == "srv") {
            cluster.discovery_type = DiscoveryType::SRV;
        } else {
            cluster.discovery_type = DiscoveryType::STATIC;
        }
    }
    if (auto v = get_env_as<int>("RANVIER_CLUSTER_DISCOVERY_REFRESH_INTERVAL")) {
        cluster.discovery_refresh_interval = std::chrono::seconds(*v);
    }

    // K8s discovery overrides
    if (auto v = get_env("RANVIER_K8S_DISCOVERY_ENABLED")) {
        k8s_discovery.enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_K8S_API_SERVER")) {
        k8s_discovery.api_server = *v;
    }
    if (auto v = get_env("RANVIER_K8S_NAMESPACE")) {
        k8s_discovery.namespace_name = *v;
    }
    if (auto v = get_env("RANVIER_K8S_SERVICE_NAME")) {
        k8s_discovery.service_name = *v;
    }
    if (auto v = get_env_as<uint16_t>("RANVIER_K8S_TARGET_PORT")) {
        k8s_discovery.target_port = *v;
    }
    if (auto v = get_env("RANVIER_K8S_TOKEN_PATH")) {
        k8s_discovery.token_path = *v;
    }
    if (auto v = get_env("RANVIER_K8S_CA_CERT_PATH")) {
        k8s_discovery.ca_cert_path = *v;
    }
    if (auto v = get_env_as<int>("RANVIER_K8S_POLL_INTERVAL")) {
        k8s_discovery.poll_interval = std::chrono::seconds(*v);
    }
    if (auto v = get_env_as<int>("RANVIER_K8S_WATCH_TIMEOUT")) {
        k8s_discovery.watch_timeout = std::chrono::seconds(*v);
    }
    if (auto v = get_env_as<int>("RANVIER_K8S_WATCH_RECONNECT_DELAY")) {
        k8s_discovery.watch_reconnect_delay = std::chrono::seconds(*v);
    }
    if (auto v = get_env_as<int>("RANVIER_K8S_WATCH_RECONNECT_MAX_DELAY")) {
        k8s_discovery.watch_reconnect_max_delay = std::chrono::seconds(*v);
    }
    if (auto v = get_env("RANVIER_K8S_VERIFY_TLS")) {
        k8s_discovery.verify_tls = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_K8S_LABEL_SELECTOR")) {
        k8s_discovery.label_selector = *v;
    }
}

inline RanvierConfig RanvierConfig::defaults() {
    RanvierConfig config;
    config.apply_env_overrides();
    return config;
}

inline RanvierConfig RanvierConfig::load(const std::string& config_path) {
    RanvierConfig config;

    std::ifstream file(config_path);
    if (!file.is_open()) {
        // File not found - use defaults with env overrides
        config.apply_env_overrides();
        return config;
    }

    try {
        YAML::Node yaml = YAML::Load(file);

        // Server section
        if (yaml["server"]) {
            YAML::Node s = yaml["server"];
            if (s["bind_address"]) config.server.bind_address = s["bind_address"].as<std::string>();
            if (s["api_port"]) config.server.api_port = s["api_port"].as<uint16_t>();
            if (s["metrics_port"]) config.server.metrics_port = s["metrics_port"].as<uint16_t>();
        }

        // Database section
        if (yaml["database"]) {
            YAML::Node d = yaml["database"];
            if (d["path"]) config.database.path = d["path"].as<std::string>();
            if (d["journal_mode"]) config.database.journal_mode = d["journal_mode"].as<std::string>();
            if (d["synchronous"]) config.database.synchronous = d["synchronous"].as<std::string>();
        }

        // Health section
        if (yaml["health"]) {
            YAML::Node h = yaml["health"];
            if (h["check_interval_seconds"]) {
                config.health.check_interval = std::chrono::seconds(h["check_interval_seconds"].as<int>());
            }
            if (h["check_timeout_seconds"]) {
                config.health.check_timeout = std::chrono::seconds(h["check_timeout_seconds"].as<int>());
            }
            if (h["failure_threshold"]) {
                config.health.failure_threshold = h["failure_threshold"].as<uint32_t>();
            }
            if (h["recovery_threshold"]) {
                config.health.recovery_threshold = h["recovery_threshold"].as<uint32_t>();
            }
        }

        // Pool section
        if (yaml["pool"]) {
            YAML::Node p = yaml["pool"];
            if (p["max_connections_per_host"]) {
                config.pool.max_connections_per_host = p["max_connections_per_host"].as<size_t>();
            }
            if (p["idle_timeout_seconds"]) {
                config.pool.idle_timeout = std::chrono::seconds(p["idle_timeout_seconds"].as<int>());
            }
            if (p["max_total_connections"]) {
                config.pool.max_total_connections = p["max_total_connections"].as<size_t>();
            }
            if (p["tcp_nodelay"]) {
                config.pool.tcp_nodelay = p["tcp_nodelay"].as<bool>();
            }
        }

        // Routing section
        if (yaml["routing"]) {
            YAML::Node r = yaml["routing"];
            if (r["min_token_length"]) {
                config.routing.min_token_length = r["min_token_length"].as<size_t>();
            }
            if (r["backend_retry_limit"]) {
                config.routing.backend_retry_limit = r["backend_retry_limit"].as<uint32_t>();
            }
            if (r["block_alignment"]) {
                config.routing.block_alignment = r["block_alignment"].as<uint32_t>();
            }
            if (r["max_routes"]) {
                config.routing.max_routes = r["max_routes"].as<size_t>();
            }
            if (r["ttl_seconds"]) {
                config.routing.ttl_seconds = std::chrono::seconds(r["ttl_seconds"].as<int>());
            }
            if (r["backend_drain_timeout_seconds"]) {
                config.routing.backend_drain_timeout = std::chrono::seconds(r["backend_drain_timeout_seconds"].as<int>());
            }
            if (r["enable_token_forwarding"]) {
                config.routing.enable_token_forwarding = r["enable_token_forwarding"].as<bool>();
            }
            if (r["accept_client_tokens"]) {
                config.routing.accept_client_tokens = r["accept_client_tokens"].as<bool>();
            }
            if (r["max_token_id"]) {
                config.routing.max_token_id = r["max_token_id"].as<int32_t>();
            }
            if (r["prefix_affinity_enabled"]) {
                config.routing.prefix_affinity_enabled = r["prefix_affinity_enabled"].as<bool>();
            }
            if (r["prefix_token_length"]) {
                config.routing.prefix_token_length = r["prefix_token_length"].as<size_t>();
            }
        }

        // Timeouts section
        if (yaml["timeouts"]) {
            YAML::Node t = yaml["timeouts"];
            if (t["connect_timeout_seconds"]) {
                config.timeouts.connect_timeout = std::chrono::seconds(t["connect_timeout_seconds"].as<int>());
            }
            if (t["request_timeout_seconds"]) {
                config.timeouts.request_timeout = std::chrono::seconds(t["request_timeout_seconds"].as<int>());
            }
        }

        // Assets section
        if (yaml["assets"]) {
            YAML::Node a = yaml["assets"];
            if (a["tokenizer_path"]) config.assets.tokenizer_path = a["tokenizer_path"].as<std::string>();
        }

        // TLS section
        if (yaml["tls"]) {
            YAML::Node t = yaml["tls"];
            if (t["enabled"]) config.tls.enabled = t["enabled"].as<bool>();
            if (t["cert_path"]) config.tls.cert_path = t["cert_path"].as<std::string>();
            if (t["key_path"]) config.tls.key_path = t["key_path"].as<std::string>();
        }

        // Auth section
        if (yaml["auth"]) {
            YAML::Node a = yaml["auth"];
            if (a["admin_api_key"]) config.auth.admin_api_key = a["admin_api_key"].as<std::string>();
        }

        // Rate limit section
        if (yaml["rate_limit"]) {
            YAML::Node r = yaml["rate_limit"];
            if (r["enabled"]) config.rate_limit.enabled = r["enabled"].as<bool>();
            if (r["requests_per_second"]) config.rate_limit.requests_per_second = r["requests_per_second"].as<uint32_t>();
            if (r["burst_size"]) config.rate_limit.burst_size = r["burst_size"].as<uint32_t>();
        }

        // Retry section
        if (yaml["retry"]) {
            YAML::Node r = yaml["retry"];
            if (r["max_retries"]) config.retry.max_retries = r["max_retries"].as<uint32_t>();
            if (r["initial_backoff_ms"]) {
                config.retry.initial_backoff = std::chrono::milliseconds(r["initial_backoff_ms"].as<int>());
            }
            if (r["max_backoff_ms"]) {
                config.retry.max_backoff = std::chrono::milliseconds(r["max_backoff_ms"].as<int>());
            }
            if (r["backoff_multiplier"]) config.retry.backoff_multiplier = r["backoff_multiplier"].as<double>();
        }

        // Circuit breaker section
        if (yaml["circuit_breaker"]) {
            YAML::Node cb = yaml["circuit_breaker"];
            if (cb["enabled"]) config.circuit_breaker.enabled = cb["enabled"].as<bool>();
            if (cb["failure_threshold"]) config.circuit_breaker.failure_threshold = cb["failure_threshold"].as<uint32_t>();
            if (cb["success_threshold"]) config.circuit_breaker.success_threshold = cb["success_threshold"].as<uint32_t>();
            if (cb["recovery_timeout_seconds"]) {
                config.circuit_breaker.recovery_timeout = std::chrono::seconds(cb["recovery_timeout_seconds"].as<int>());
            }
            if (cb["fallback_enabled"]) config.circuit_breaker.fallback_enabled = cb["fallback_enabled"].as<bool>();
        }

        // Shutdown section
        if (yaml["shutdown"]) {
            YAML::Node s = yaml["shutdown"];
            if (s["drain_timeout_seconds"]) {
                config.shutdown.drain_timeout = std::chrono::seconds(s["drain_timeout_seconds"].as<int>());
            }
        }

        // Cluster section
        if (yaml["cluster"]) {
            YAML::Node c = yaml["cluster"];
            if (c["enabled"]) config.cluster.enabled = c["enabled"].as<bool>();
            if (c["gossip_port"]) config.cluster.gossip_port = c["gossip_port"].as<uint16_t>();
            if (c["gossip_interval_ms"]) {
                config.cluster.gossip_interval = std::chrono::milliseconds(c["gossip_interval_ms"].as<int>());
            }
            if (c["peers"]) {
                config.cluster.peers.clear();
                for (const auto& peer : c["peers"]) {
                    config.cluster.peers.push_back(peer.as<std::string>());
                }
            }
            if (c["gossip_heartbeat_interval_seconds"]) {
                config.cluster.gossip_heartbeat_interval = std::chrono::seconds(c["gossip_heartbeat_interval_seconds"].as<int>());
            }
            if (c["gossip_peer_timeout_seconds"]) {
                config.cluster.gossip_peer_timeout = std::chrono::seconds(c["gossip_peer_timeout_seconds"].as<int>());
            }
            if (c["discovery_dns_name"]) {
                config.cluster.discovery_dns_name = c["discovery_dns_name"].as<std::string>();
            }
            if (c["discovery_type"]) {
                std::string dtype = c["discovery_type"].as<std::string>();
                if (dtype == "A" || dtype == "a") {
                    config.cluster.discovery_type = DiscoveryType::A;
                } else if (dtype == "SRV" || dtype == "srv") {
                    config.cluster.discovery_type = DiscoveryType::SRV;
                } else {
                    config.cluster.discovery_type = DiscoveryType::STATIC;
                }
            }
            if (c["discovery_refresh_interval_seconds"]) {
                config.cluster.discovery_refresh_interval = std::chrono::seconds(c["discovery_refresh_interval_seconds"].as<int>());
            }
        }

        // K8s discovery section
        if (yaml["k8s_discovery"]) {
            YAML::Node k = yaml["k8s_discovery"];
            if (k["enabled"]) config.k8s_discovery.enabled = k["enabled"].as<bool>();
            if (k["api_server"]) config.k8s_discovery.api_server = k["api_server"].as<std::string>();
            if (k["namespace"]) config.k8s_discovery.namespace_name = k["namespace"].as<std::string>();
            if (k["service_name"]) config.k8s_discovery.service_name = k["service_name"].as<std::string>();
            if (k["target_port"]) config.k8s_discovery.target_port = k["target_port"].as<uint16_t>();
            if (k["token_path"]) config.k8s_discovery.token_path = k["token_path"].as<std::string>();
            if (k["ca_cert_path"]) config.k8s_discovery.ca_cert_path = k["ca_cert_path"].as<std::string>();
            if (k["poll_interval_seconds"]) {
                config.k8s_discovery.poll_interval = std::chrono::seconds(k["poll_interval_seconds"].as<int>());
            }
            if (k["watch_timeout_seconds"]) {
                config.k8s_discovery.watch_timeout = std::chrono::seconds(k["watch_timeout_seconds"].as<int>());
            }
            if (k["watch_reconnect_delay_seconds"]) {
                config.k8s_discovery.watch_reconnect_delay = std::chrono::seconds(k["watch_reconnect_delay_seconds"].as<int>());
            }
            if (k["watch_reconnect_max_delay_seconds"]) {
                config.k8s_discovery.watch_reconnect_max_delay = std::chrono::seconds(k["watch_reconnect_max_delay_seconds"].as<int>());
            }
            if (k["verify_tls"]) config.k8s_discovery.verify_tls = k["verify_tls"].as<bool>();
            if (k["label_selector"]) config.k8s_discovery.label_selector = k["label_selector"].as<std::string>();
        }
    } catch (const YAML::Exception& e) {
        // Log error and fall back to defaults
        // Note: Can't use Seastar logger here since config loads before Seastar init
        // In practice, main.cpp will catch and report this
        throw std::runtime_error("Failed to parse config file: " + std::string(e.what()));
    }

    // Environment variables override file settings
    config.apply_env_overrides();
    return config;
}

inline std::optional<std::string> RanvierConfig::validate(const RanvierConfig& config) {
    // Validate server ports
    if (config.server.api_port == 0) {
        return "server.api_port must be non-zero";
    }
    if (config.server.metrics_port == 0) {
        return "server.metrics_port must be non-zero";
    }
    if (config.server.api_port == config.server.metrics_port) {
        return "server.api_port and server.metrics_port must be different";
    }

    // Validate health check settings
    if (config.health.check_interval.count() == 0) {
        return "health.check_interval must be positive";
    }
    if (config.health.check_timeout.count() == 0) {
        return "health.check_timeout must be positive";
    }
    if (config.health.failure_threshold == 0) {
        return "health.failure_threshold must be positive";
    }

    // Validate pool settings
    if (config.pool.max_connections_per_host == 0) {
        return "pool.max_connections_per_host must be positive";
    }
    if (config.pool.max_total_connections == 0) {
        return "pool.max_total_connections must be positive";
    }

    // Validate routing settings
    if (config.routing.block_alignment == 0) {
        return "routing.block_alignment must be positive";
    }
    if (config.routing.backend_drain_timeout.count() == 0) {
        return "routing.backend_drain_timeout must be positive";
    }

    // Validate timeout settings
    if (config.timeouts.connect_timeout.count() == 0) {
        return "timeouts.connect_timeout must be positive";
    }
    if (config.timeouts.request_timeout.count() == 0) {
        return "timeouts.request_timeout must be positive";
    }

    // Validate TLS settings (if enabled)
    if (config.tls.enabled) {
        if (config.tls.cert_path.empty()) {
            return "tls.cert_path is required when TLS is enabled";
        }
        if (config.tls.key_path.empty()) {
            return "tls.key_path is required when TLS is enabled";
        }
    }

    // Validate retry settings
    if (config.retry.backoff_multiplier < 1.0) {
        return "retry.backoff_multiplier must be >= 1.0";
    }

    // Validate circuit breaker settings
    if (config.circuit_breaker.enabled) {
        if (config.circuit_breaker.failure_threshold == 0) {
            return "circuit_breaker.failure_threshold must be positive";
        }
        if (config.circuit_breaker.success_threshold == 0) {
            return "circuit_breaker.success_threshold must be positive";
        }
    }

    // Validate shutdown settings
    if (config.shutdown.drain_timeout.count() == 0) {
        return "shutdown.drain_timeout must be positive";
    }

    // Validate cluster settings (if enabled)
    if (config.cluster.enabled) {
        if (config.cluster.gossip_port == 0) {
            return "cluster.gossip_port must be non-zero";
        }
        if (config.cluster.gossip_port == config.server.api_port) {
            return "cluster.gossip_port must be different from server.api_port";
        }
        if (config.cluster.gossip_port == config.server.metrics_port) {
            return "cluster.gossip_port must be different from server.metrics_port";
        }
        if (config.cluster.gossip_interval.count() < 100) {
            return "cluster.gossip_interval must be at least 100ms";
        }
        if (config.cluster.gossip_heartbeat_interval.count() == 0) {
            return "cluster.gossip_heartbeat_interval must be positive";
        }
        if (config.cluster.gossip_peer_timeout.count() == 0) {
            return "cluster.gossip_peer_timeout must be positive";
        }
        if (config.cluster.gossip_peer_timeout < 2 * config.cluster.gossip_heartbeat_interval) {
            return "cluster.gossip_peer_timeout must be at least twice gossip_heartbeat_interval";
        }
        // Validate DNS discovery settings
        if (config.cluster.discovery_type != DiscoveryType::STATIC) {
            if (config.cluster.discovery_dns_name.empty()) {
                return "cluster.discovery_dns_name is required when discovery_type is not STATIC";
            }
            if (config.cluster.discovery_refresh_interval.count() < 5) {
                return "cluster.discovery_refresh_interval must be at least 5 seconds";
            }
        }
    }

    // Validate K8s discovery settings (if enabled)
    if (config.k8s_discovery.enabled) {
        if (config.k8s_discovery.service_name.empty()) {
            return "k8s_discovery.service_name is required when K8s discovery is enabled";
        }
        if (config.k8s_discovery.namespace_name.empty()) {
            return "k8s_discovery.namespace must be non-empty";
        }
        if (config.k8s_discovery.target_port == 0) {
            return "k8s_discovery.target_port must be non-zero";
        }
        if (config.k8s_discovery.poll_interval.count() < 5) {
            return "k8s_discovery.poll_interval must be at least 5 seconds";
        }
        if (config.k8s_discovery.api_server.empty()) {
            return "k8s_discovery.api_server must be non-empty";
        }
    }

    return std::nullopt;  // Valid
}

}  // namespace ranvier
