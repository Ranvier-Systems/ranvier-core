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

    return std::nullopt;  // Valid
}

}  // namespace ranvier
