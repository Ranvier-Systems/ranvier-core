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

    // Load configuration from YAML file
    static RanvierConfig load(const std::string& config_path);

    // Load with defaults (no file)
    static RanvierConfig defaults();

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

}  // namespace ranvier
