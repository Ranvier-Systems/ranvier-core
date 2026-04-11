// Ranvier Core - Configuration Loader Implementation
//
// YAML parsing, environment variable overrides, and validation logic.
// Note: This file uses std::ifstream which is blocking I/O.
// Config loading happens before Seastar reactor starts, so this is acceptable.

#include "config_loader.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
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

// =============================================================================
// Environment Variable Overrides
// =============================================================================

void RanvierConfig::apply_env_overrides() {
    // Server overrides
    if (auto v = get_env("RANVIER_BIND_ADDRESS")) server.bind_address = *v;
    if (auto v = get_env_as<uint16_t>("RANVIER_API_PORT")) server.api_port = *v;
    if (auto v = get_env_as<uint16_t>("RANVIER_METRICS_PORT")) server.metrics_port = *v;
    if (auto v = get_env_as<size_t>("RANVIER_MAX_REQUEST_BODY_BYTES")) server.max_request_body_bytes = *v;
    if (auto v = get_env_as<uint32_t>("RANVIER_DNS_RESOLUTION_TIMEOUT_SECONDS")) server.dns_resolution_timeout_seconds = *v;

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
    if (auto v = get_env("RANVIER_HEALTH_ENABLE_VLLM_METRICS")) {
        health.enable_vllm_metrics = (*v == "true" || *v == "1");
    }
    if (auto v = get_env_as<int>("RANVIER_HEALTH_VLLM_METRICS_TIMEOUT_MS")) {
        health.vllm_metrics_timeout = std::chrono::milliseconds(*v);
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
    if (auto v = get_env("RANVIER_PARTIAL_TOKENIZATION")) {
        routing.enable_partial_tokenization = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env_as<size_t>("RANVIER_PARTIAL_TOKENIZE_BYTES_PER_TOKEN")) {
        routing.partial_tokenize_bytes_per_token = *v;
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
    // Load-aware routing configuration
    if (auto v = get_env("RANVIER_LOAD_AWARE_ROUTING")) {
        routing.load_aware_routing = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env_as<double>("RANVIER_LOAD_IMBALANCE_FACTOR")) {
        routing.load_imbalance_factor = *v;
    }
    if (auto v = get_env_as<uint64_t>("RANVIER_LOAD_IMBALANCE_FLOOR")) {
        routing.load_imbalance_floor = *v;
    }
    // GPU load integration (vLLM-aware routing)
    if (auto v = get_env_as<double>("RANVIER_ROUTING_GPU_LOAD_WEIGHT")) {
        routing.gpu_load_weight = *v;
    }
    if (auto v = get_env_as<int>("RANVIER_ROUTING_GPU_LOAD_CACHE_TTL")) {
        routing.gpu_load_cache_ttl = std::chrono::seconds(*v);
    }
    // Compression-aware load scoring
    if (auto v = get_env_as<double>("RANVIER_DEFAULT_COMPRESSION_RATIO")) {
        routing.default_compression_ratio = *v;
    }
    // Capacity-aware hash fallback
    if (auto v = get_env_as<double>("RANVIER_CAPACITY_HEADROOM_WEIGHT")) {
        routing.capacity_headroom_weight = *v;
    }
    // Compression-aware route TTL
    if (auto v = get_env_as<double>("RANVIER_MAX_TTL_MULTIPLIER")) {
        routing.max_ttl_multiplier = *v;
    }
    // Cross-shard load synchronization
    if (auto v = get_env("RANVIER_CROSS_SHARD_LOAD_SYNC")) {
        routing.cross_shard_load_sync = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env_as<uint64_t>("RANVIER_CROSS_SHARD_LOAD_SYNC_INTERVAL_MS")) {
        routing.cross_shard_load_sync_interval = std::chrono::milliseconds(*v);
    }
    // Route batch flush interval
    if (auto v = get_env_as<uint32_t>("RANVIER_ROUTE_BATCH_FLUSH_INTERVAL_MS")) {
        if (*v < 1 || *v > 1000) {
            std::cerr << "[WARN] RANVIER_ROUTE_BATCH_FLUSH_INTERVAL_MS=" << *v
                      << " out of range [1, 1000], using default 10ms\n";
        } else {
            routing.route_batch_flush_interval = std::chrono::milliseconds(*v);
            if (*v < 2) {
                std::cerr << "[WARN] RANVIER_ROUTE_BATCH_FLUSH_INTERVAL_MS=" << *v
                          << ": values below 2ms may cause high SMP overhead on multi-core systems\n";
            } else if (*v > 50) {
                std::cerr << "[WARN] RANVIER_ROUTE_BATCH_FLUSH_INTERVAL_MS=" << *v
                          << ": values above 50ms may cause stale routes and reduced cache hit rates\n";
            }
        }
    }
    // Hash strategy configuration
    if (auto v = get_env("RANVIER_HASH_STRATEGY")) {
        if (*v == "jump") {
            routing.hash_strategy = RoutingConfig::HashStrategy::JUMP;
        } else if (*v == "bounded_load") {
            routing.hash_strategy = RoutingConfig::HashStrategy::BOUNDED_LOAD;
        } else if (*v == "p2c") {
            routing.hash_strategy = RoutingConfig::HashStrategy::P2C;
        } else if (*v == "modular") {
            routing.hash_strategy = RoutingConfig::HashStrategy::MODULAR;
        }
    }
    if (auto v = get_env_as<double>("RANVIER_BOUNDED_LOAD_EPSILON")) {
        routing.bounded_load_epsilon = *v;
    }
    if (auto v = get_env_as<uint64_t>("RANVIER_P2C_LOAD_BIAS")) {
        routing.p2c_load_bias = *v;
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
    if (auto v = get_env("RANVIER_CHAT_TEMPLATE_FORMAT")) assets.chat_template_format = *v;
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
    // Local fallback semaphore override
    if (auto v = get_env_as<size_t>("RANVIER_TOKENIZER_LOCAL_FALLBACK_MAX_CONCURRENT")) {
        assets.tokenizer_local_fallback_max_concurrent = *v;
    }

    // TLS overrides
    if (auto v = get_env("RANVIER_TLS_ENABLED")) {
        tls.enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_TLS_CERT_PATH")) tls.cert_path = *v;
    if (auto v = get_env("RANVIER_TLS_KEY_PATH")) tls.key_path = *v;

    // Auth overrides
    if (auto v = get_env("RANVIER_ADMIN_API_KEY")) auth.admin_api_key = *v;

    // Metrics endpoint security overrides
    if (auto v = get_env("RANVIER_METRICS_AUTH_TOKEN")) metrics.auth_token = *v;
    if (auto v = get_env("RANVIER_METRICS_ALLOWED_IPS")) {
        // Parse comma-separated IP list
        metrics.allowed_ips.clear();
        std::istringstream iss(*v);
        std::string ip;
        while (std::getline(iss, ip, ',')) {
            size_t start = ip.find_first_not_of(" \t");
            size_t end = ip.find_last_not_of(" \t");
            if (start != std::string::npos && end != std::string::npos) {
                metrics.allowed_ips.push_back(ip.substr(start, end - start + 1));
            }
        }
        if (metrics.allowed_ips.size() > MetricsConfig::MAX_ALLOWED_IPS) {
            std::cerr << "[WARN] RANVIER_METRICS_ALLOWED_IPS has " << metrics.allowed_ips.size()
                      << " entries, truncating to " << MetricsConfig::MAX_ALLOWED_IPS << "\n";
            metrics.allowed_ips.resize(MetricsConfig::MAX_ALLOWED_IPS);
        }
    }

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
    if (auto v = get_env_as<uint32_t>("RANVIER_RETRY_MAX_STALE")) {
        retry.max_stale_retries = *v;
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
        } catch (const std::exception& e) {
            // Rule #9: Log at warn level (pre-Seastar, use std::cerr)
            std::cerr << "[WARN] Invalid RANVIER_BACKPRESSURE_PERSISTENCE_THRESHOLD value '"
                      << *v << "': " << e.what() << " - using default\n";
        }
    }
    if (auto v = get_env_as<uint32_t>("RANVIER_BACKPRESSURE_RETRY_AFTER")) {
        backpressure.retry_after_seconds = *v;
    }
    if (auto v = get_env("RANVIER_BACKPRESSURE_ENABLE_PRIORITY_QUEUE")) {
        backpressure.enable_priority_queue = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_BACKPRESSURE_TIER_CAPACITY")) {
        // Parse comma-separated list: "64,128,256,512"
        std::istringstream iss(*v);
        std::string token;
        size_t idx = 0;
        while (std::getline(iss, token, ',') && idx < 4) {
            try {
                backpressure.tier_capacity[idx] = static_cast<uint32_t>(std::stoul(token));
            } catch (const std::exception& e) {
                // Rule #9: Log at warn level (pre-Seastar, use std::cerr)
                std::cerr << "[WARN] Invalid RANVIER_BACKPRESSURE_TIER_CAPACITY element '"
                          << token << "': " << e.what() << " - using default\n";
            }
            ++idx;
        }
    }
    if (auto v = get_env_as<uint32_t>("RANVIER_BACKPRESSURE_MAX_PER_AGENT_QUEUED")) {
        backpressure.max_per_agent_queued = *v;
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
        } catch (const std::exception& e) {
            // Rule #9: Log at warn level (pre-Seastar, use std::cerr)
            std::cerr << "[WARN] Invalid RANVIER_CLUSTER_QUORUM_THRESHOLD value '"
                      << *v << "': " << e.what() << " - using default\n";
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
        } catch (const std::exception& e) {
            // Rule #9: Log at warn level (pre-Seastar, use std::cerr)
            std::cerr << "[WARN] Invalid RANVIER_TELEMETRY_SAMPLE_RATE value '"
                      << *v << "': " << e.what() << " - using default\n";
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

    // Cost estimation overrides
    if (auto v = get_env("RANVIER_COST_ESTIMATION_ENABLED")) {
        cost_estimation.enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env_as<double>("RANVIER_COST_ESTIMATION_OUTPUT_MULTIPLIER")) {
        cost_estimation.default_output_multiplier = *v;
    }
    if (auto v = get_env_as<uint64_t>("RANVIER_COST_ESTIMATION_MAX_TOKENS")) {
        cost_estimation.max_estimated_tokens = *v;
    }

    // Priority tier overrides
    if (auto v = get_env("RANVIER_PRIORITY_TIER_ENABLED")) {
        priority_tier.enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_PRIORITY_TIER_DEFAULT")) {
        priority_tier.default_priority = *v;
    }
    if (auto v = get_env_as<double>("RANVIER_PRIORITY_TIER_COST_THRESHOLD_HIGH")) {
        priority_tier.cost_threshold_high = *v;
    }
    if (auto v = get_env_as<double>("RANVIER_PRIORITY_TIER_COST_THRESHOLD_LOW")) {
        priority_tier.cost_threshold_low = *v;
    }
    if (auto v = get_env("RANVIER_PRIORITY_TIER_RESPECT_HEADER")) {
        priority_tier.respect_header = (*v == "1" || *v == "true" || *v == "yes");
    }

    // Local mode overrides
    if (auto v = get_env("RANVIER_LOCAL_MODE")) {
        local_mode.enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_LOCAL_DISABLE_CLUSTERING")) {
        local_mode.disable_clustering = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_LOCAL_DISABLE_PERSISTENCE")) {
        local_mode.disable_persistence = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_LOCAL_AUTO_DISCOVER")) {
        local_mode.auto_discover_backends = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_LOCAL_DISCOVERY_PORTS")) {
        // Parse comma-separated port list (same pattern as RANVIER_CLUSTER_PEERS)
        local_mode.discovery_ports.clear();
        std::istringstream iss(*v);
        std::string token;
        size_t count = 0;
        while (std::getline(iss, token, ',')) {
            if (count >= LocalModeConfig::MAX_DISCOVERY_PORTS) {
                std::cerr << "[WARN] RANVIER_LOCAL_DISCOVERY_PORTS has more than "
                          << LocalModeConfig::MAX_DISCOVERY_PORTS
                          << " entries, truncating (Rule #4)\n";
                break;
            }
            // Trim whitespace
            size_t start = token.find_first_not_of(" \t");
            size_t end = token.find_last_not_of(" \t");
            if (start != std::string::npos && end != std::string::npos) {
                try {
                    auto port = static_cast<uint16_t>(std::stoul(token.substr(start, end - start + 1)));
                    local_mode.discovery_ports.push_back(port);
                } catch (const std::exception& e) {
                    // Rule #9: Log at warn level (pre-Seastar, use std::cerr)
                    std::cerr << "[WARN] Invalid port in RANVIER_LOCAL_DISCOVERY_PORTS: '"
                              << token << "': " << e.what() << " - skipping\n";
                }
            }
            ++count;
        }
    }

    // Local discovery timing overrides
    if (auto v = get_env("RANVIER_LOCAL_DISCOVERY_SCAN_INTERVAL")) {
        try {
            local_mode.discovery_scan_interval = std::chrono::seconds(std::stoul(*v));
        } catch (const std::exception& e) {
            std::cerr << "[WARN] Invalid RANVIER_LOCAL_DISCOVERY_SCAN_INTERVAL: '"
                      << *v << "': " << e.what() << " - using default\n";
        }
    }
    if (auto v = get_env("RANVIER_LOCAL_DISCOVERY_PROBE_TIMEOUT_MS")) {
        try {
            local_mode.discovery_probe_timeout = std::chrono::milliseconds(std::stoul(*v));
        } catch (const std::exception& e) {
            std::cerr << "[WARN] Invalid RANVIER_LOCAL_DISCOVERY_PROBE_TIMEOUT_MS: '"
                      << *v << "': " << e.what() << " - using default\n";
        }
    }
    if (auto v = get_env("RANVIER_LOCAL_DISCOVERY_CONNECT_TIMEOUT_MS")) {
        try {
            local_mode.discovery_connect_timeout = std::chrono::milliseconds(std::stoul(*v));
        } catch (const std::exception& e) {
            std::cerr << "[WARN] Invalid RANVIER_LOCAL_DISCOVERY_CONNECT_TIMEOUT_MS: '"
                      << *v << "': " << e.what() << " - using default\n";
        }
    }

    // Intent classification overrides
    if (auto v = get_env("RANVIER_INTENT_CLASSIFICATION_ENABLED")) {
        intent_classification.enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_INTENT_FIM_FIELDS")) {
        intent_classification.fim_fields.clear();
        std::istringstream iss(*v);
        std::string token;
        size_t count = 0;
        while (std::getline(iss, token, ',')) {
            if (count >= IntentClassificationConfig::MAX_FIM_FIELDS) {
                std::cerr << "[WARN] RANVIER_INTENT_FIM_FIELDS has more than "
                          << IntentClassificationConfig::MAX_FIM_FIELDS
                          << " entries, truncating (Rule #4)\n";
                break;
            }
            // Trim whitespace
            size_t start = token.find_first_not_of(" \t");
            size_t end = token.find_last_not_of(" \t");
            if (start != std::string::npos && end != std::string::npos) {
                intent_classification.fim_fields.push_back(token.substr(start, end - start + 1));
            }
            ++count;
        }
    }
    if (auto v = get_env("RANVIER_INTENT_EDIT_KEYWORDS")) {
        intent_classification.edit_system_keywords.clear();
        std::istringstream iss(*v);
        std::string token;
        size_t count = 0;
        while (std::getline(iss, token, ',')) {
            if (count >= IntentClassificationConfig::MAX_EDIT_SYSTEM_KEYWORDS) {
                std::cerr << "[WARN] RANVIER_INTENT_EDIT_KEYWORDS has more than "
                          << IntentClassificationConfig::MAX_EDIT_SYSTEM_KEYWORDS
                          << " entries, truncating (Rule #4)\n";
                break;
            }
            size_t start = token.find_first_not_of(" \t");
            size_t end = token.find_last_not_of(" \t");
            if (start != std::string::npos && end != std::string::npos) {
                intent_classification.edit_system_keywords.push_back(token.substr(start, end - start + 1));
            }
            ++count;
        }
    }

    // Agent registry overrides
    if (auto v = get_env("RANVIER_AGENT_REGISTRY_ENABLED")) {
        agent_registry.enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_AGENT_AUTO_DETECT")) {
        agent_registry.auto_detect_agents = (*v == "1" || *v == "true" || *v == "yes");
    }

    // Cost-based routing overrides
    if (auto v = get_env("RANVIER_COST_ROUTING_ENABLED")) {
        routing.cost_routing.enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env_as<double>("RANVIER_COST_ROUTING_MAX_COST")) {
        routing.cost_routing.max_cost_per_backend = *v;
    }
    if (auto v = get_env_as<double>("RANVIER_COST_ROUTING_SMALL_THRESHOLD")) {
        routing.cost_routing.small_request_threshold = *v;
    }
    if (auto v = get_env("RANVIER_COST_ROUTING_FAST_LANE")) {
        routing.cost_routing.enable_fast_lane = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env_as<double>("RANVIER_COST_ROUTING_IMBALANCE_FACTOR")) {
        routing.cost_routing.cost_imbalance_factor = *v;
    }

    // Cache events overrides
    if (auto v = get_env("RANVIER_CACHE_EVENTS_ENABLED")) {
        cache_events.enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_CACHE_EVENTS_AUTH_TOKEN")) {
        cache_events.auth_token = *v;
    }
    if (auto v = get_env_as<uint32_t>("RANVIER_CACHE_EVENTS_MAX_EVENTS")) {
        cache_events.max_events_per_request = *v;
    }
    if (auto v = get_env_as<uint32_t>("RANVIER_CACHE_EVENTS_MAX_AGE")) {
        cache_events.max_event_age_seconds = *v;
    }
    if (auto v = get_env("RANVIER_CACHE_EVENTS_INJECT_HEADER")) {
        cache_events.inject_prefix_hash_header = (*v == "1" || *v == "true" || *v == "yes");
    }

    // Dashboard overrides
    if (auto v = get_env("RANVIER_DASHBOARD_ENABLED")) {
        dashboard.enabled = (*v == "1" || *v == "true" || *v == "yes");
    }
    if (auto v = get_env("RANVIER_DASHBOARD_CORS")) {
        dashboard.enable_cors = (*v == "1" || *v == "true" || *v == "yes");
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
            if (s["max_request_body_bytes"]) config.server.max_request_body_bytes = s["max_request_body_bytes"].as<size_t>();
            if (s["dns_resolution_timeout_seconds"]) config.server.dns_resolution_timeout_seconds = s["dns_resolution_timeout_seconds"].as<uint32_t>();
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
            if (h["enable_vllm_metrics"]) {
                config.health.enable_vllm_metrics = h["enable_vllm_metrics"].as<bool>();
            }
            if (h["vllm_metrics_timeout_ms"]) {
                config.health.vllm_metrics_timeout = std::chrono::milliseconds(
                    h["vllm_metrics_timeout_ms"].as<int>());
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
            if (r["enable_partial_tokenization"]) {
                config.routing.enable_partial_tokenization = r["enable_partial_tokenization"].as<bool>();
            }
            if (r["partial_tokenize_bytes_per_token"]) {
                config.routing.partial_tokenize_bytes_per_token = r["partial_tokenize_bytes_per_token"].as<size_t>();
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
            // Load-aware routing
            if (r["load_aware_routing"]) {
                config.routing.load_aware_routing = r["load_aware_routing"].as<bool>();
            }
            if (r["load_imbalance_factor"]) {
                config.routing.load_imbalance_factor = r["load_imbalance_factor"].as<double>();
            }
            if (r["load_imbalance_floor"]) {
                config.routing.load_imbalance_floor = r["load_imbalance_floor"].as<uint64_t>();
            }
            // Cross-shard load synchronization
            if (r["cross_shard_load_sync"]) {
                config.routing.cross_shard_load_sync = r["cross_shard_load_sync"].as<bool>();
            }
            if (r["cross_shard_load_sync_interval_ms"]) {
                config.routing.cross_shard_load_sync_interval =
                    std::chrono::milliseconds(r["cross_shard_load_sync_interval_ms"].as<uint64_t>());
            }
            if (r["route_batch_flush_interval_ms"]) {
                config.routing.route_batch_flush_interval =
                    std::chrono::milliseconds(r["route_batch_flush_interval_ms"].as<uint32_t>());
            }
            // Hash strategy
            if (r["hash_strategy"]) {
                std::string strategy = r["hash_strategy"].as<std::string>();
                if (strategy == "jump") {
                    config.routing.hash_strategy = RoutingConfig::HashStrategy::JUMP;
                } else if (strategy == "bounded_load") {
                    config.routing.hash_strategy = RoutingConfig::HashStrategy::BOUNDED_LOAD;
                } else if (strategy == "p2c") {
                    config.routing.hash_strategy = RoutingConfig::HashStrategy::P2C;
                } else if (strategy == "modular") {
                    config.routing.hash_strategy = RoutingConfig::HashStrategy::MODULAR;
                }
            }
            if (r["bounded_load_epsilon"]) {
                config.routing.bounded_load_epsilon = r["bounded_load_epsilon"].as<double>();
            }
            if (r["p2c_load_bias"]) {
                config.routing.p2c_load_bias = r["p2c_load_bias"].as<uint64_t>();
            }
            // GPU load integration (vLLM-aware routing)
            if (r["gpu_load_weight"]) {
                config.routing.gpu_load_weight = r["gpu_load_weight"].as<double>();
            }
            if (r["gpu_load_cache_ttl"]) {
                config.routing.gpu_load_cache_ttl =
                    std::chrono::seconds(r["gpu_load_cache_ttl"].as<int>());
            }
            // Compression-aware load scoring
            if (r["default_compression_ratio"]) {
                config.routing.default_compression_ratio = r["default_compression_ratio"].as<double>();
            }
            // Capacity-aware hash fallback
            if (r["capacity_headroom_weight"]) {
                config.routing.capacity_headroom_weight = r["capacity_headroom_weight"].as<double>();
            }
            // Compression-aware route TTL
            if (r["max_ttl_multiplier"]) {
                config.routing.max_ttl_multiplier = r["max_ttl_multiplier"].as<double>();
            }
            // Cost-based routing sub-section (nested under routing)
            if (r["cost_routing"]) {
                YAML::Node cr = r["cost_routing"];
                if (cr["enabled"]) config.routing.cost_routing.enabled = cr["enabled"].as<bool>();
                if (cr["max_cost_per_backend"]) {
                    config.routing.cost_routing.max_cost_per_backend = cr["max_cost_per_backend"].as<double>();
                }
                if (cr["small_request_threshold"]) {
                    config.routing.cost_routing.small_request_threshold = cr["small_request_threshold"].as<double>();
                }
                if (cr["enable_fast_lane"]) {
                    config.routing.cost_routing.enable_fast_lane = cr["enable_fast_lane"].as<bool>();
                }
                if (cr["cost_imbalance_factor"]) {
                    config.routing.cost_routing.cost_imbalance_factor = cr["cost_imbalance_factor"].as<double>();
                }
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
            if (a["chat_template_format"]) config.assets.chat_template_format = a["chat_template_format"].as<std::string>();
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
            if (a["tokenizer_local_fallback_max_concurrent"]) {
                config.assets.tokenizer_local_fallback_max_concurrent = a["tokenizer_local_fallback_max_concurrent"].as<size_t>();
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

        // Metrics endpoint security section
        if (yaml["metrics"]) {
            YAML::Node m = yaml["metrics"];
            if (m["auth_token"]) config.metrics.auth_token = m["auth_token"].as<std::string>();
            if (m["allowed_ips"]) {
                config.metrics.allowed_ips.clear();
                for (const auto& ip : m["allowed_ips"]) {
                    config.metrics.allowed_ips.push_back(ip.as<std::string>());
                }
                if (config.metrics.allowed_ips.size() > MetricsConfig::MAX_ALLOWED_IPS) {
                    std::cerr << "[WARN] metrics.allowed_ips has " << config.metrics.allowed_ips.size()
                              << " entries, truncating to " << MetricsConfig::MAX_ALLOWED_IPS << "\n";
                    config.metrics.allowed_ips.resize(MetricsConfig::MAX_ALLOWED_IPS);
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
            if (r["max_stale_retries"]) config.retry.max_stale_retries = r["max_stale_retries"].as<uint32_t>();
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
            if (bp["enable_priority_queue"]) {
                config.backpressure.enable_priority_queue = bp["enable_priority_queue"].as<bool>();
            }
            if (bp["tier_capacity"]) {
                auto tc = bp["tier_capacity"];
                if (tc.IsSequence() && tc.size() == 4) {
                    for (size_t i = 0; i < 4; ++i) {
                        config.backpressure.tier_capacity[i] = tc[i].as<uint32_t>();
                    }
                }
            }
            if (bp["max_per_agent_queued"]) {
                config.backpressure.max_per_agent_queued = bp["max_per_agent_queued"].as<uint32_t>();
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
        // Cost estimation section
        if (yaml["cost_estimation"]) {
            YAML::Node ce = yaml["cost_estimation"];
            if (ce["enabled"]) config.cost_estimation.enabled = ce["enabled"].as<bool>();
            if (ce["default_output_multiplier"]) {
                config.cost_estimation.default_output_multiplier = ce["default_output_multiplier"].as<double>();
            }
            if (ce["max_estimated_tokens"]) {
                config.cost_estimation.max_estimated_tokens = ce["max_estimated_tokens"].as<uint64_t>();
            }
        }

        // Priority tier section
        if (yaml["priority_tier"]) {
            YAML::Node pt = yaml["priority_tier"];
            if (pt["enabled"]) config.priority_tier.enabled = pt["enabled"].as<bool>();
            if (pt["default_priority"]) config.priority_tier.default_priority = pt["default_priority"].as<std::string>();
            if (pt["cost_threshold_high"]) config.priority_tier.cost_threshold_high = pt["cost_threshold_high"].as<double>();
            if (pt["cost_threshold_low"]) config.priority_tier.cost_threshold_low = pt["cost_threshold_low"].as<double>();
            if (pt["respect_header"]) config.priority_tier.respect_header = pt["respect_header"].as<bool>();
            if (pt["known_user_agents"]) {
                YAML::Node agents = pt["known_user_agents"];
                if (agents.IsSequence()) {
                    config.priority_tier.known_user_agents.clear();
                    size_t count = 0;
                    for (const auto& entry : agents) {
                        if (count >= PriorityTierConfig::MAX_KNOWN_USER_AGENTS) {
                            std::cerr << "[WARN] priority_tier.known_user_agents has more than "
                                      << PriorityTierConfig::MAX_KNOWN_USER_AGENTS
                                      << " entries, truncating (Rule #4)\n";
                            break;
                        }
                        PriorityTierUserAgentEntry ua;
                        if (entry["pattern"]) ua.pattern = entry["pattern"].as<std::string>();
                        if (entry["priority"]) {
                            std::string p = entry["priority"].as<std::string>();
                            if (p == "critical")      ua.priority = 0;
                            else if (p == "high")     ua.priority = 1;
                            else if (p == "normal")   ua.priority = 2;
                            else if (p == "low")      ua.priority = 3;
                            else {
                                std::cerr << "[WARN] Unknown priority '" << p
                                          << "' in known_user_agents, defaulting to normal\n";
                                ua.priority = 2;
                            }
                        }
                        config.priority_tier.known_user_agents.push_back(std::move(ua));
                        ++count;
                    }
                }
            }
        }
        // Intent classification section
        if (yaml["intent_classification"]) {
            YAML::Node ic = yaml["intent_classification"];
            if (ic["enabled"]) config.intent_classification.enabled = ic["enabled"].as<bool>();
            if (ic["fim_fields"]) {
                YAML::Node ff = ic["fim_fields"];
                if (ff.IsSequence()) {
                    config.intent_classification.fim_fields.clear();
                    size_t count = 0;
                    for (const auto& entry : ff) {
                        if (count >= IntentClassificationConfig::MAX_FIM_FIELDS) {
                            std::cerr << "[WARN] intent_classification.fim_fields has more than "
                                      << IntentClassificationConfig::MAX_FIM_FIELDS
                                      << " entries, truncating (Rule #4)\n";
                            break;
                        }
                        config.intent_classification.fim_fields.push_back(entry.as<std::string>());
                        ++count;
                    }
                }
            }
            if (ic["edit_system_keywords"]) {
                YAML::Node kw = ic["edit_system_keywords"];
                if (kw.IsSequence()) {
                    config.intent_classification.edit_system_keywords.clear();
                    size_t count = 0;
                    for (const auto& entry : kw) {
                        if (count >= IntentClassificationConfig::MAX_EDIT_SYSTEM_KEYWORDS) {
                            std::cerr << "[WARN] intent_classification.edit_system_keywords has more than "
                                      << IntentClassificationConfig::MAX_EDIT_SYSTEM_KEYWORDS
                                      << " entries, truncating (Rule #4)\n";
                            break;
                        }
                        config.intent_classification.edit_system_keywords.push_back(entry.as<std::string>());
                        ++count;
                    }
                }
            }
            if (ic["edit_tag_patterns"]) {
                YAML::Node tp = ic["edit_tag_patterns"];
                if (tp.IsSequence()) {
                    config.intent_classification.edit_tag_patterns.clear();
                    size_t count = 0;
                    for (const auto& entry : tp) {
                        if (count >= IntentClassificationConfig::MAX_EDIT_TAG_PATTERNS) {
                            std::cerr << "[WARN] intent_classification.edit_tag_patterns has more than "
                                      << IntentClassificationConfig::MAX_EDIT_TAG_PATTERNS
                                      << " entries, truncating (Rule #4)\n";
                            break;
                        }
                        config.intent_classification.edit_tag_patterns.push_back(entry.as<std::string>());
                        ++count;
                    }
                }
            }
        }
        // Local mode section
        if (yaml["local_mode"]) {
            YAML::Node lm = yaml["local_mode"];
            if (lm["enabled"]) config.local_mode.enabled = lm["enabled"].as<bool>();
            if (lm["disable_clustering"]) config.local_mode.disable_clustering = lm["disable_clustering"].as<bool>();
            if (lm["disable_persistence"]) config.local_mode.disable_persistence = lm["disable_persistence"].as<bool>();
            if (lm["auto_discover_backends"]) config.local_mode.auto_discover_backends = lm["auto_discover_backends"].as<bool>();
            if (lm["discovery_ports"]) {
                YAML::Node dp = lm["discovery_ports"];
                if (dp.IsSequence()) {
                    config.local_mode.discovery_ports.clear();
                    size_t count = 0;
                    for (const auto& entry : dp) {
                        if (count >= LocalModeConfig::MAX_DISCOVERY_PORTS) {
                            std::cerr << "[WARN] local_mode.discovery_ports has more than "
                                      << LocalModeConfig::MAX_DISCOVERY_PORTS
                                      << " entries, truncating (Rule #4)\n";
                            break;
                        }
                        config.local_mode.discovery_ports.push_back(entry.as<uint16_t>());
                        ++count;
                    }
                }
            }
            if (lm["discovery_scan_interval_seconds"]) {
                config.local_mode.discovery_scan_interval =
                    std::chrono::seconds(lm["discovery_scan_interval_seconds"].as<unsigned>());
            }
            if (lm["discovery_probe_timeout_ms"]) {
                config.local_mode.discovery_probe_timeout =
                    std::chrono::milliseconds(lm["discovery_probe_timeout_ms"].as<unsigned>());
            }
            if (lm["discovery_connect_timeout_ms"]) {
                config.local_mode.discovery_connect_timeout =
                    std::chrono::milliseconds(lm["discovery_connect_timeout_ms"].as<unsigned>());
            }
        }
        // Agent registry section
        if (yaml["agent_registry"]) {
            YAML::Node ar = yaml["agent_registry"];
            if (ar["enabled"]) config.agent_registry.enabled = ar["enabled"].as<bool>();
            if (ar["auto_detect_agents"]) config.agent_registry.auto_detect_agents = ar["auto_detect_agents"].as<bool>();
            if (ar["known_agents"]) {
                YAML::Node agents = ar["known_agents"];
                if (agents.IsSequence()) {
                    config.agent_registry.known_agents.clear();
                    size_t count = 0;
                    for (const auto& entry : agents) {
                        if (count >= AgentRegistryConfig::MAX_KNOWN_AGENTS) {
                            std::cerr << "[WARN] agent_registry.known_agents has more than "
                                      << AgentRegistryConfig::MAX_KNOWN_AGENTS
                                      << " entries, truncating (Rule #4)\n";
                            break;
                        }
                        AgentConfig ac;
                        if (entry["pattern"]) ac.pattern = entry["pattern"].as<std::string>();
                        if (entry["name"]) ac.name = entry["name"].as<std::string>();
                        if (entry["default_priority"]) {
                            std::string p = entry["default_priority"].as<std::string>();
                            if (p == "critical")      ac.default_priority = 0;
                            else if (p == "high")     ac.default_priority = 1;
                            else if (p == "normal")   ac.default_priority = 2;
                            else if (p == "low")      ac.default_priority = 3;
                            else {
                                std::cerr << "[WARN] Unknown priority '" << p
                                          << "' in agent_registry.known_agents, defaulting to normal\n";
                                ac.default_priority = 2;
                            }
                        }
                        if (entry["allow_pause"]) ac.allow_pause = entry["allow_pause"].as<bool>();
                        config.agent_registry.known_agents.push_back(std::move(ac));
                        ++count;
                    }
                }
            }
        }

        // Cache events section
        if (yaml["cache_events"]) {
            YAML::Node ce = yaml["cache_events"];
            if (ce["enabled"]) config.cache_events.enabled = ce["enabled"].as<bool>();
            if (ce["auth_token"]) config.cache_events.auth_token = ce["auth_token"].as<std::string>();
            if (ce["max_events_per_request"]) config.cache_events.max_events_per_request = ce["max_events_per_request"].as<uint32_t>();
            if (ce["max_event_age_seconds"]) config.cache_events.max_event_age_seconds = ce["max_event_age_seconds"].as<uint32_t>();
            if (ce["propagate_via_gossip"]) config.cache_events.propagate_via_gossip = ce["propagate_via_gossip"].as<bool>();
            if (ce["inject_prefix_hash_header"]) config.cache_events.inject_prefix_hash_header = ce["inject_prefix_hash_header"].as<bool>();
        }

        // Dashboard section
        if (yaml["dashboard"]) {
            YAML::Node db = yaml["dashboard"];
            if (db["enabled"]) config.dashboard.enabled = db["enabled"].as<bool>();
            if (db["enable_cors"]) config.dashboard.enable_cors = db["enable_cors"].as<bool>();
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

    // Validate hash strategy settings
    if (config.routing.bounded_load_epsilon < 0.0 || config.routing.bounded_load_epsilon > 10.0) {
        return "routing.bounded_load_epsilon must be between 0.0 and 10.0";
    }
    if (config.routing.capacity_headroom_weight < 0.0 || config.routing.capacity_headroom_weight > 100.0) {
        return "routing.capacity_headroom_weight must be between 0.0 and 100.0";
    }
    if (config.routing.max_ttl_multiplier < 1.0 || config.routing.max_ttl_multiplier > 10.0) {
        return "routing.max_ttl_multiplier must be between 1.0 and 10.0";
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

    // Validate cost estimation settings
    if (config.cost_estimation.default_output_multiplier < 0.0) {
        return "cost_estimation.default_output_multiplier must be non-negative";
    }
    if (config.cost_estimation.max_estimated_tokens == 0) {
        return "cost_estimation.max_estimated_tokens must be positive";
    }

    // Validate priority tier settings
    if (config.priority_tier.enabled) {
        const auto& dp = config.priority_tier.default_priority;
        if (dp != "critical" && dp != "high" && dp != "normal" && dp != "low") {
            return "priority_tier.default_priority must be one of: critical, high, normal, low";
        }
        if (config.priority_tier.cost_threshold_high < 0.0) {
            return "priority_tier.cost_threshold_high must be non-negative";
        }
        if (config.priority_tier.cost_threshold_low < 0.0) {
            return "priority_tier.cost_threshold_low must be non-negative";
        }
        if (config.priority_tier.cost_threshold_low > config.priority_tier.cost_threshold_high) {
            return "priority_tier.cost_threshold_low must not exceed cost_threshold_high";
        }
        if (config.priority_tier.known_user_agents.size() > PriorityTierConfig::MAX_KNOWN_USER_AGENTS) {
            return "priority_tier.known_user_agents exceeds maximum of 64 entries (Rule #4)";
        }
    }

    // Validate local mode settings
    if (config.local_mode.enabled) {
        if (config.cluster.enabled) {
            // Warn but don't error — local_mode wins at startup via apply_local_mode_overrides()
            std::cerr << "[WARN] local_mode.enabled overrides cluster.enabled — "
                      << "clustering will be disabled\n";
        }
        if (config.local_mode.auto_discover_backends && config.local_mode.discovery_ports.empty()) {
            std::cerr << "[WARN] No discovery ports configured, auto-discovery will find nothing\n";
        }
    }
    // Validate discovery_ports regardless of enabled state (catch config errors early)
    for (auto port : config.local_mode.discovery_ports) {
        if (port == 0) {
            return "local_mode.discovery_ports contains invalid port 0 (must be 1-65535)";
        }
        // uint16_t max is 65535, so no need to check > 65535
    }
    if (config.local_mode.discovery_ports.size() > LocalModeConfig::MAX_DISCOVERY_PORTS) {
        return "local_mode.discovery_ports exceeds maximum of 64 entries (Rule #4)";
    }

    // Validate compression-aware load scoring
    if (config.routing.default_compression_ratio < 1.0) {
        return "default_compression_ratio must be >= 1.0 (1.0 = no compression)";
    }

    // Validate cost-based routing settings
    if (config.routing.cost_routing.max_cost_per_backend <= 0.0) {
        return "cost_routing.max_cost_per_backend must be positive";
    }
    if (config.routing.cost_routing.small_request_threshold < 0.0) {
        return "cost_routing.small_request_threshold must be non-negative";
    }
    if (config.routing.cost_routing.cost_imbalance_factor <= 0.0) {
        return "cost_routing.cost_imbalance_factor must be positive";
    }

    // Validate cache events settings
    if (config.cache_events.max_events_per_request == 0) {
        return "cache_events.max_events_per_request must be positive";
    }
    if (config.cache_events.max_event_age_seconds == 0) {
        return "cache_events.max_event_age_seconds must be positive";
    }

    return std::nullopt;  // Valid
}

}  // namespace ranvier
