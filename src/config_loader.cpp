// Ranvier Core - Configuration Loader Implementation
//
// YAML parsing, environment variable overrides, and validation logic.
// Note: This file uses std::ifstream which is blocking I/O.
// Config loading happens before Seastar reactor starts, so this is acceptable.

#include "config_loader.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <yaml-cpp/yaml.h>

namespace ranvier {

// =============================================================================
// Environment Variable Helpers
// =============================================================================

std::optional<std::string> RanvierConfig::get_env(const char* name) {
    const char* val = std::getenv(name);
    if (val && val[0] != '\0') {
        return std::string(val);
    }
    return std::nullopt;
}

template<typename T>
std::optional<T> RanvierConfig::get_env_as(const char* name) {
    auto val = get_env(name);
    if (!val) return std::nullopt;

    std::istringstream iss(*val);
    T result;
    if (iss >> result) {
        return result;
    }
    return std::nullopt;
}

// Explicit template instantiations for common types
template std::optional<int> RanvierConfig::get_env_as<int>(const char* name);
template std::optional<uint16_t> RanvierConfig::get_env_as<uint16_t>(const char* name);
template std::optional<uint32_t> RanvierConfig::get_env_as<uint32_t>(const char* name);
template std::optional<int32_t> RanvierConfig::get_env_as<int32_t>(const char* name);
template std::optional<size_t> RanvierConfig::get_env_as<size_t>(const char* name);

// =============================================================================
// Environment Variable Overrides
// =============================================================================

void RanvierConfig::apply_env_overrides() {
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
    if (auto v = get_env_as<size_t>("RANVIER_MAX_ROUTE_TOKENS")) {
        routing.max_route_tokens = *v;
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
    // Legacy: RANVIER_PREFIX_AFFINITY_ENABLED (true=prefix, false=random)
    if (auto v = get_env("RANVIER_PREFIX_AFFINITY_ENABLED")) {
        if (*v == "1" || *v == "true" || *v == "yes") {
            routing.routing_mode = RoutingConfig::RoutingMode::PREFIX;
        } else {
            routing.routing_mode = RoutingConfig::RoutingMode::RANDOM;
        }
    }
    // RANVIER_ROUTING_MODE: "prefix", "hash", or "random"
    if (auto v = get_env("RANVIER_ROUTING_MODE")) {
        if (*v == "prefix") {
            routing.routing_mode = RoutingConfig::RoutingMode::PREFIX;
        } else if (*v == "hash") {
            routing.routing_mode = RoutingConfig::RoutingMode::HASH;
        } else if (*v == "random" || *v == "round_robin") {
            // "round_robin" accepted as alias for backward compatibility
            routing.routing_mode = RoutingConfig::RoutingMode::RANDOM;
        }
    }
    if (auto v = get_env_as<size_t>("RANVIER_PREFIX_TOKEN_LENGTH")) {
        routing.prefix_token_length = *v;
    }
    if (auto v = get_env("RANVIER_ENABLE_PREFIX_BOUNDARY")) {
        routing.enable_prefix_boundary = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env_as<size_t>("RANVIER_MIN_PREFIX_BOUNDARY_TOKENS")) {
        routing.min_prefix_boundary_tokens = *v;
    }
    if (auto v = get_env("RANVIER_ACCEPT_CLIENT_PREFIX_BOUNDARY")) {
        routing.accept_client_prefix_boundary = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_ENABLE_MULTI_DEPTH_ROUTING")) {
        routing.enable_multi_depth_routing = (*v == "1" || *v == "true" || *v == "yes");
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
    if (auto v = get_env("RANVIER_TOKENIZATION_CACHE_ENABLED")) {
        assets.tokenization_cache_enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env_as<size_t>("RANVIER_TOKENIZATION_CACHE_SIZE")) {
        assets.tokenization_cache_size = *v;
    }
    if (auto v = get_env_as<size_t>("RANVIER_TOKENIZATION_CACHE_MAX_TEXT")) {
        assets.tokenization_cache_max_text = *v;
    }
    // Tokenizer thread pool overrides
    if (auto v = get_env("RANVIER_TOKENIZER_THREAD_POOL_ENABLED")) {
        assets.tokenizer_thread_pool_enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env_as<size_t>("RANVIER_TOKENIZER_THREAD_POOL_QUEUE_SIZE")) {
        assets.tokenizer_thread_pool_queue_size = *v;
    }
    if (auto v = get_env_as<size_t>("RANVIER_TOKENIZER_THREAD_POOL_MIN_TEXT")) {
        assets.tokenizer_thread_pool_min_text = *v;
    }
    if (auto v = get_env_as<size_t>("RANVIER_TOKENIZER_THREAD_POOL_MAX_TEXT")) {
        assets.tokenizer_thread_pool_max_text = *v;
    }

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

    // Backpressure overrides
    if (auto v = get_env_as<size_t>("RANVIER_BACKPRESSURE_MAX_CONCURRENT_REQUESTS")) {
        backpressure.max_concurrent_requests = *v;
    }
    if (auto v = get_env("RANVIER_BACKPRESSURE_ENABLE_PERSISTENCE")) {
        backpressure.enable_persistence_backpressure = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_BACKPRESSURE_PERSISTENCE_THRESHOLD")) {
        try {
            backpressure.persistence_queue_threshold = std::stod(*v);
            if (backpressure.persistence_queue_threshold < 0.0) backpressure.persistence_queue_threshold = 0.0;
            if (backpressure.persistence_queue_threshold > 1.0) backpressure.persistence_queue_threshold = 1.0;
        } catch (...) {
            // Ignore invalid values
        }
    }
    if (auto v = get_env_as<uint32_t>("RANVIER_BACKPRESSURE_RETRY_AFTER")) {
        backpressure.retry_after_seconds = *v;
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
    // Reliable delivery overrides
    if (auto v = get_env("RANVIER_CLUSTER_GOSSIP_RELIABLE_DELIVERY")) {
        cluster.gossip_reliable_delivery = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env_as<int>("RANVIER_CLUSTER_GOSSIP_ACK_TIMEOUT_MS")) {
        cluster.gossip_ack_timeout = std::chrono::milliseconds(*v);
    }
    if (auto v = get_env_as<uint32_t>("RANVIER_CLUSTER_GOSSIP_MAX_RETRIES")) {
        cluster.gossip_max_retries = *v;
    }
    if (auto v = get_env_as<size_t>("RANVIER_CLUSTER_GOSSIP_DEDUP_WINDOW")) {
        cluster.gossip_dedup_window = *v;
    }
    // Gossip TLS/DTLS overrides
    if (auto v = get_env("RANVIER_CLUSTER_TLS_ENABLED")) {
        cluster.tls.enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_CLUSTER_TLS_CERT_PATH")) {
        cluster.tls.cert_path = *v;
    }
    if (auto v = get_env("RANVIER_CLUSTER_TLS_KEY_PATH")) {
        cluster.tls.key_path = *v;
    }
    if (auto v = get_env("RANVIER_CLUSTER_TLS_CA_PATH")) {
        cluster.tls.ca_path = *v;
    }
    if (auto v = get_env("RANVIER_CLUSTER_TLS_VERIFY_PEER")) {
        cluster.tls.verify_peer = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env_as<int>("RANVIER_CLUSTER_TLS_CERT_RELOAD_INTERVAL")) {
        cluster.tls.cert_reload_interval = std::chrono::seconds(*v);
    }
    if (auto v = get_env("RANVIER_CLUSTER_TLS_ALLOW_PLAINTEXT_FALLBACK")) {
        cluster.tls.allow_plaintext_fallback = (*v == "1" || *v == "true" || *v == "yes");
    }
    // Quorum/split-brain detection overrides
    if (auto v = get_env("RANVIER_CLUSTER_QUORUM_ENABLED")) {
        cluster.quorum_enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_CLUSTER_QUORUM_THRESHOLD")) {
        try {
            cluster.quorum_threshold = std::stod(*v);
            if (cluster.quorum_threshold < 0.0) cluster.quorum_threshold = 0.0;
            if (cluster.quorum_threshold > 1.0) cluster.quorum_threshold = 1.0;
        } catch (...) {
            // Ignore invalid values
        }
    }
    if (auto v = get_env("RANVIER_CLUSTER_REJECT_ROUTES_ON_QUORUM_LOSS")) {
        cluster.reject_routes_on_quorum_loss = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env_as<uint32_t>("RANVIER_CLUSTER_QUORUM_WARNING_THRESHOLD")) {
        cluster.quorum_warning_threshold = *v;
    }
    if (auto v = get_env_as<int>("RANVIER_CLUSTER_QUORUM_CHECK_WINDOW")) {
        cluster.quorum_check_window = std::chrono::seconds(*v);
    }
    if (auto v = get_env("RANVIER_CLUSTER_MTLS_ENABLED")) {
        cluster.mtls_enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_CLUSTER_FAIL_OPEN_ON_QUORUM_LOSS")) {
        cluster.fail_open_on_quorum_loss = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_CLUSTER_ACCEPT_GOSSIP_ON_QUORUM_LOSS")) {
        cluster.accept_gossip_on_quorum_loss = (*v == "1" || *v == "true" || *v == "yes");
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
    if (auto v = get_env_as<int>("RANVIER_K8S_DNS_TIMEOUT")) {
        k8s_discovery.dns_timeout = std::chrono::seconds(*v);
    }
    if (auto v = get_env_as<uint32_t>("RANVIER_K8S_DNS_MAX_RETRIES")) {
        k8s_discovery.dns_max_retries = *v;
    }
    if (auto v = get_env_as<int>("RANVIER_K8S_DNS_INITIAL_BACKOFF_MS")) {
        k8s_discovery.dns_initial_backoff = std::chrono::milliseconds(*v);
    }

    // Telemetry overrides
    if (auto v = get_env("RANVIER_TELEMETRY_ENABLED")) {
        telemetry.enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_OTLP_ENDPOINT")) {
        telemetry.otlp_endpoint = *v;
    }
    if (auto v = get_env("RANVIER_SERVICE_NAME")) {
        telemetry.service_name = *v;
    }
    if (auto v = get_env("RANVIER_TELEMETRY_SAMPLE_RATE")) {
        try {
            telemetry.sample_rate = std::stod(*v);
            if (telemetry.sample_rate < 0.0) telemetry.sample_rate = 0.0;
            if (telemetry.sample_rate > 1.0) telemetry.sample_rate = 1.0;
        } catch (...) {
            // Ignore invalid values
        }
    }
    if (auto v = get_env_as<int>("RANVIER_TELEMETRY_EXPORT_INTERVAL_MS")) {
        telemetry.export_interval = std::chrono::milliseconds(*v);
    }
    if (auto v = get_env_as<size_t>("RANVIER_TELEMETRY_MAX_QUEUE_SIZE")) {
        telemetry.max_queue_size = *v;
    }
    if (auto v = get_env_as<size_t>("RANVIER_TELEMETRY_MAX_EXPORT_BATCH_SIZE")) {
        telemetry.max_export_batch_size = *v;
    }
}

// =============================================================================
// Load Defaults
// =============================================================================

RanvierConfig RanvierConfig::defaults() {
    RanvierConfig config;
    config.apply_env_overrides();
    return config;
}

// =============================================================================
// YAML Loading
// =============================================================================

RanvierConfig RanvierConfig::load(const std::string& config_path) {
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
            if (r["max_route_tokens"]) {
                config.routing.max_route_tokens = r["max_route_tokens"].as<size_t>();
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
            // Legacy: prefix_affinity_enabled (bool)
            if (r["prefix_affinity_enabled"]) {
                if (r["prefix_affinity_enabled"].as<bool>()) {
                    config.routing.routing_mode = RoutingConfig::RoutingMode::PREFIX;
                } else {
                    config.routing.routing_mode = RoutingConfig::RoutingMode::RANDOM;
                }
            }
            // routing_mode: "prefix", "hash", or "random"
            if (r["routing_mode"]) {
                std::string mode = r["routing_mode"].as<std::string>();
                if (mode == "prefix") {
                    config.routing.routing_mode = RoutingConfig::RoutingMode::PREFIX;
                } else if (mode == "hash") {
                    config.routing.routing_mode = RoutingConfig::RoutingMode::HASH;
                } else if (mode == "random" || mode == "round_robin") {
                    // "round_robin" accepted as alias for backward compatibility
                    config.routing.routing_mode = RoutingConfig::RoutingMode::RANDOM;
                }
            }
            if (r["prefix_token_length"]) {
                config.routing.prefix_token_length = r["prefix_token_length"].as<size_t>();
            }
            if (r["enable_prefix_boundary"]) {
                config.routing.enable_prefix_boundary = r["enable_prefix_boundary"].as<bool>();
            }
            if (r["min_prefix_boundary_tokens"]) {
                config.routing.min_prefix_boundary_tokens = r["min_prefix_boundary_tokens"].as<size_t>();
            }
            if (r["accept_client_prefix_boundary"]) {
                config.routing.accept_client_prefix_boundary = r["accept_client_prefix_boundary"].as<bool>();
            }
            if (r["enable_multi_depth_routing"]) {
                config.routing.enable_multi_depth_routing = r["enable_multi_depth_routing"].as<bool>();
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
            // Tokenization cache settings
            if (a["tokenization_cache_enabled"]) {
                config.assets.tokenization_cache_enabled = a["tokenization_cache_enabled"].as<bool>();
            }
            if (a["tokenization_cache_size"]) {
                config.assets.tokenization_cache_size = a["tokenization_cache_size"].as<size_t>();
            }
            if (a["tokenization_cache_max_text"]) {
                config.assets.tokenization_cache_max_text = a["tokenization_cache_max_text"].as<size_t>();
            }
            // Tokenizer thread pool settings (P3 feature)
            if (a["tokenizer_thread_pool_enabled"]) {
                config.assets.tokenizer_thread_pool_enabled = a["tokenizer_thread_pool_enabled"].as<bool>();
            }
            if (a["tokenizer_thread_pool_queue_size"]) {
                config.assets.tokenizer_thread_pool_queue_size = a["tokenizer_thread_pool_queue_size"].as<size_t>();
            }
            if (a["tokenizer_thread_pool_min_text"]) {
                config.assets.tokenizer_thread_pool_min_text = a["tokenizer_thread_pool_min_text"].as<size_t>();
            }
            if (a["tokenizer_thread_pool_max_text"]) {
                config.assets.tokenizer_thread_pool_max_text = a["tokenizer_thread_pool_max_text"].as<size_t>();
            }
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
            // Legacy single key (backward compatibility)
            if (a["admin_api_key"]) config.auth.admin_api_key = a["admin_api_key"].as<std::string>();

            // New multi-key format with metadata
            if (a["api_keys"]) {
                config.auth.api_keys.clear();
                for (const auto& key_node : a["api_keys"]) {
                    ApiKey api_key;
                    if (key_node["key"]) api_key.key = key_node["key"].as<std::string>();
                    if (key_node["name"]) api_key.name = key_node["name"].as<std::string>();
                    if (key_node["created"]) api_key.created = key_node["created"].as<std::string>();
                    if (key_node["expires"]) api_key.expires = key_node["expires"].as<std::string>();
                    if (key_node["roles"]) {
                        for (const auto& role : key_node["roles"]) {
                            api_key.roles.push_back(role.as<std::string>());
                        }
                    }
                    // Only add keys that have a value
                    if (!api_key.key.empty()) {
                        config.auth.api_keys.push_back(std::move(api_key));
                    }
                }
            }
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

        // Backpressure section
        if (yaml["backpressure"]) {
            YAML::Node bp = yaml["backpressure"];
            if (bp["max_concurrent_requests"]) {
                config.backpressure.max_concurrent_requests = bp["max_concurrent_requests"].as<size_t>();
            }
            if (bp["enable_persistence_backpressure"]) {
                config.backpressure.enable_persistence_backpressure = bp["enable_persistence_backpressure"].as<bool>();
            }
            if (bp["persistence_queue_threshold"]) {
                double threshold = bp["persistence_queue_threshold"].as<double>();
                config.backpressure.persistence_queue_threshold = std::max(0.0, std::min(1.0, threshold));
            }
            if (bp["retry_after_seconds"]) {
                config.backpressure.retry_after_seconds = bp["retry_after_seconds"].as<uint32_t>();
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
            // Reliable delivery settings
            if (c["gossip_reliable_delivery"]) {
                config.cluster.gossip_reliable_delivery = c["gossip_reliable_delivery"].as<bool>();
            }
            if (c["gossip_ack_timeout_ms"]) {
                config.cluster.gossip_ack_timeout = std::chrono::milliseconds(c["gossip_ack_timeout_ms"].as<int>());
            }
            if (c["gossip_max_retries"]) {
                config.cluster.gossip_max_retries = c["gossip_max_retries"].as<uint32_t>();
            }
            if (c["gossip_dedup_window"]) {
                config.cluster.gossip_dedup_window = c["gossip_dedup_window"].as<size_t>();
            }
            // TLS/DTLS settings for gossip encryption
            if (c["tls"]) {
                YAML::Node t = c["tls"];
                if (t["enabled"]) {
                    config.cluster.tls.enabled = t["enabled"].as<bool>();
                }
                if (t["cert_path"]) {
                    config.cluster.tls.cert_path = t["cert_path"].as<std::string>();
                }
                if (t["key_path"]) {
                    config.cluster.tls.key_path = t["key_path"].as<std::string>();
                }
                if (t["ca_path"]) {
                    config.cluster.tls.ca_path = t["ca_path"].as<std::string>();
                }
                if (t["verify_peer"]) {
                    config.cluster.tls.verify_peer = t["verify_peer"].as<bool>();
                }
                if (t["cert_reload_interval_seconds"]) {
                    config.cluster.tls.cert_reload_interval = std::chrono::seconds(t["cert_reload_interval_seconds"].as<int>());
                }
                if (t["allow_plaintext_fallback"]) {
                    config.cluster.tls.allow_plaintext_fallback = t["allow_plaintext_fallback"].as<bool>();
                }
            }
            // Split-brain detection / Quorum settings
            if (c["quorum_enabled"]) {
                config.cluster.quorum_enabled = c["quorum_enabled"].as<bool>();
            }
            if (c["quorum_threshold"]) {
                config.cluster.quorum_threshold = c["quorum_threshold"].as<double>();
            }
            if (c["reject_routes_on_quorum_loss"]) {
                config.cluster.reject_routes_on_quorum_loss = c["reject_routes_on_quorum_loss"].as<bool>();
            }
            if (c["quorum_warning_threshold"]) {
                config.cluster.quorum_warning_threshold = c["quorum_warning_threshold"].as<uint32_t>();
            }
            if (c["quorum_check_window_seconds"]) {
                config.cluster.quorum_check_window = std::chrono::seconds(c["quorum_check_window_seconds"].as<int>());
            }
            if (c["mtls_enabled"]) {
                config.cluster.mtls_enabled = c["mtls_enabled"].as<bool>();
            }
            if (c["fail_open_on_quorum_loss"]) {
                config.cluster.fail_open_on_quorum_loss = c["fail_open_on_quorum_loss"].as<bool>();
            }
            if (c["accept_gossip_on_quorum_loss"]) {
                config.cluster.accept_gossip_on_quorum_loss = c["accept_gossip_on_quorum_loss"].as<bool>();
            }
            if (c["self_backend_id"]) {
                config.cluster.self_backend_id = c["self_backend_id"].as<int32_t>();
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
            if (k["dns_timeout_seconds"]) {
                config.k8s_discovery.dns_timeout = std::chrono::seconds(k["dns_timeout_seconds"].as<int>());
            }
            if (k["dns_max_retries"]) {
                config.k8s_discovery.dns_max_retries = k["dns_max_retries"].as<uint32_t>();
            }
            if (k["dns_initial_backoff_ms"]) {
                config.k8s_discovery.dns_initial_backoff = std::chrono::milliseconds(k["dns_initial_backoff_ms"].as<int>());
            }
        }

        // Telemetry section (OpenTelemetry distributed tracing)
        if (yaml["telemetry"]) {
            YAML::Node t = yaml["telemetry"];
            if (t["enabled"]) config.telemetry.enabled = t["enabled"].as<bool>();
            if (t["otlp_endpoint"]) config.telemetry.otlp_endpoint = t["otlp_endpoint"].as<std::string>();
            if (t["service_name"]) config.telemetry.service_name = t["service_name"].as<std::string>();
            if (t["sample_rate"]) {
                double rate = t["sample_rate"].as<double>();
                config.telemetry.sample_rate = std::max(0.0, std::min(1.0, rate));
            }
            if (t["export_interval_ms"]) {
                config.telemetry.export_interval = std::chrono::milliseconds(t["export_interval_ms"].as<int>());
            }
            if (t["max_queue_size"]) {
                config.telemetry.max_queue_size = t["max_queue_size"].as<size_t>();
            }
            if (t["max_export_batch_size"]) {
                config.telemetry.max_export_batch_size = t["max_export_batch_size"].as<size_t>();
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

// =============================================================================
// Validation
// =============================================================================

std::optional<std::string> RanvierConfig::validate(const RanvierConfig& config) {
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

    // Validate backpressure settings
    if (config.backpressure.persistence_queue_threshold < 0.0 ||
        config.backpressure.persistence_queue_threshold > 1.0) {
        return "backpressure.persistence_queue_threshold must be between 0.0 and 1.0";
    }
    if (config.backpressure.retry_after_seconds == 0) {
        return "backpressure.retry_after_seconds must be positive";
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
        // Validate reliable delivery settings
        if (config.cluster.gossip_reliable_delivery) {
            if (config.cluster.gossip_ack_timeout.count() < 10) {
                return "cluster.gossip_ack_timeout must be at least 10ms";
            }
            if (config.cluster.gossip_dedup_window == 0) {
                return "cluster.gossip_dedup_window must be positive";
            }
        }
        // Validate gossip TLS settings
        if (config.cluster.tls.enabled) {
            if (config.cluster.tls.cert_path.empty()) {
                return "cluster.tls.cert_path is required when gossip TLS is enabled";
            }
            if (config.cluster.tls.key_path.empty()) {
                return "cluster.tls.key_path is required when gossip TLS is enabled";
            }
            if (config.cluster.tls.ca_path.empty()) {
                return "cluster.tls.ca_path is required when gossip TLS is enabled (needed for peer verification)";
            }
            if (config.cluster.tls.cert_reload_interval.count() < 0) {
                return "cluster.tls.cert_reload_interval must be non-negative";
            }
        }
        // Validate quorum settings
        if (config.cluster.quorum_enabled) {
            if (config.cluster.quorum_threshold < 0.0 || config.cluster.quorum_threshold > 1.0) {
                return "cluster.quorum_threshold must be between 0.0 and 1.0";
            }
            if (config.cluster.quorum_check_window.count() < 5) {
                return "cluster.quorum_check_window must be at least 5 seconds";
            }
        }
        // Validate mTLS lockdown settings
        if (config.cluster.mtls_enabled && !config.cluster.tls.enabled) {
            return "cluster.mtls_enabled requires cluster.tls.enabled to be true";
        }
        // Note: self_backend_id=0 is allowed but will log a warning at startup
        // It's needed for graceful shutdown notifications to work correctly
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

    // Validate telemetry settings (if enabled)
    if (config.telemetry.enabled) {
        if (config.telemetry.otlp_endpoint.empty()) {
            return "telemetry.otlp_endpoint must be non-empty when telemetry is enabled";
        }
        if (config.telemetry.service_name.empty()) {
            return "telemetry.service_name must be non-empty when telemetry is enabled";
        }
        if (config.telemetry.sample_rate < 0.0 || config.telemetry.sample_rate > 1.0) {
            return "telemetry.sample_rate must be between 0.0 and 1.0";
        }
        if (config.telemetry.max_queue_size == 0) {
            return "telemetry.max_queue_size must be positive";
        }
        if (config.telemetry.max_export_batch_size == 0) {
            return "telemetry.max_export_batch_size must be positive";
        }
    }

    return std::nullopt;  // Valid
}

}  // namespace ranvier
