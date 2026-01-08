// Ranvier Core - Configuration Unit Tests

#include "config.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <cstdlib>

using namespace ranvier;

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clear any environment variables that might interfere
        unsetenv("RANVIER_API_PORT");
        unsetenv("RANVIER_METRICS_PORT");
        unsetenv("RANVIER_BIND_ADDRESS");
        unsetenv("RANVIER_DB_PATH");
        unsetenv("RANVIER_HEALTH_CHECK_INTERVAL");
        unsetenv("RANVIER_MIN_TOKEN_LENGTH");
        unsetenv("RANVIER_POOL_MAX_PER_HOST");
        unsetenv("RANVIER_TOKENIZER_PATH");
        unsetenv("RANVIER_TLS_ENABLED");
        unsetenv("RANVIER_TLS_CERT_PATH");
        unsetenv("RANVIER_TLS_KEY_PATH");
        unsetenv("RANVIER_ADMIN_API_KEY");
        unsetenv("RANVIER_RATE_LIMIT_ENABLED");
        unsetenv("RANVIER_RATE_LIMIT_RPS");
        unsetenv("RANVIER_RATE_LIMIT_BURST");
        unsetenv("RANVIER_RETRY_MAX");
        unsetenv("RANVIER_RETRY_INITIAL_BACKOFF_MS");
        unsetenv("RANVIER_RETRY_MAX_BACKOFF_MS");
        unsetenv("RANVIER_CIRCUIT_BREAKER_ENABLED");
        unsetenv("RANVIER_CIRCUIT_BREAKER_FAILURE_THRESHOLD");
        unsetenv("RANVIER_CIRCUIT_BREAKER_SUCCESS_THRESHOLD");
        unsetenv("RANVIER_CIRCUIT_BREAKER_RECOVERY_TIMEOUT");
        unsetenv("RANVIER_CIRCUIT_BREAKER_FALLBACK");
        // DNS discovery env vars
        unsetenv("RANVIER_CLUSTER_DISCOVERY_DNS_NAME");
        unsetenv("RANVIER_CLUSTER_DISCOVERY_TYPE");
        unsetenv("RANVIER_CLUSTER_DISCOVERY_REFRESH_INTERVAL");
        unsetenv("RANVIER_CLUSTER_ENABLED");
        unsetenv("RANVIER_CLUSTER_GOSSIP_PORT");
        // Client token env vars
        unsetenv("RANVIER_ACCEPT_CLIENT_TOKENS");
        unsetenv("RANVIER_MAX_TOKEN_ID");
        // Gossip TLS env vars
        unsetenv("RANVIER_CLUSTER_TLS_ENABLED");
        unsetenv("RANVIER_CLUSTER_TLS_CERT_PATH");
        unsetenv("RANVIER_CLUSTER_TLS_KEY_PATH");
        unsetenv("RANVIER_CLUSTER_TLS_CA_PATH");
        unsetenv("RANVIER_CLUSTER_TLS_VERIFY_PEER");
        unsetenv("RANVIER_CLUSTER_TLS_CERT_RELOAD_INTERVAL");
        unsetenv("RANVIER_CLUSTER_TLS_ALLOW_PLAINTEXT_FALLBACK");
    }

    void TearDown() override {
        // Clean up test files
        std::remove("test_config.yaml");
        std::remove("test_partial.yaml");
        std::remove("test_invalid.yaml");
        std::remove("test_cluster.yaml");
        std::remove("test_dns_discovery.yaml");
        std::remove("test_gossip_tls.yaml");
    }

    void writeTestConfig(const std::string& filename, const std::string& content) {
        std::ofstream f(filename);
        f << content;
        f.close();
    }
};

// Test that defaults() returns sensible default values
TEST_F(ConfigTest, DefaultsReturnExpectedValues) {
    auto config = RanvierConfig::defaults();

    // Server defaults
    EXPECT_EQ(config.server.bind_address, "0.0.0.0");
    EXPECT_EQ(config.server.api_port, 8080);
    EXPECT_EQ(config.server.metrics_port, 9180);

    // Database defaults
    EXPECT_EQ(config.database.path, "ranvier.db");
    EXPECT_EQ(config.database.journal_mode, "WAL");
    EXPECT_EQ(config.database.synchronous, "NORMAL");

    // Health defaults
    EXPECT_EQ(config.health.check_interval.count(), 5);
    EXPECT_EQ(config.health.check_timeout.count(), 3);
    EXPECT_EQ(config.health.failure_threshold, 3u);
    EXPECT_EQ(config.health.recovery_threshold, 2u);

    // Pool defaults
    EXPECT_EQ(config.pool.max_connections_per_host, 10u);
    EXPECT_EQ(config.pool.idle_timeout.count(), 60);
    EXPECT_EQ(config.pool.max_total_connections, 100u);
    EXPECT_TRUE(config.pool.tcp_nodelay);

    // Routing defaults
    EXPECT_EQ(config.routing.min_token_length, 4u);
    EXPECT_EQ(config.routing.backend_retry_limit, 5u);

    // Timeout defaults
    EXPECT_EQ(config.timeouts.connect_timeout.count(), 5);
    EXPECT_EQ(config.timeouts.request_timeout.count(), 300);

    // Assets defaults
    EXPECT_EQ(config.assets.tokenizer_path, "assets/gpt2.json");

    // TLS defaults
    EXPECT_FALSE(config.tls.enabled);
    EXPECT_EQ(config.tls.cert_path, "");
    EXPECT_EQ(config.tls.key_path, "");

    // Auth defaults
    EXPECT_EQ(config.auth.admin_api_key, "");

    // Rate limit defaults
    EXPECT_FALSE(config.rate_limit.enabled);
    EXPECT_EQ(config.rate_limit.requests_per_second, 100u);
    EXPECT_EQ(config.rate_limit.burst_size, 50u);

    // Retry defaults
    EXPECT_EQ(config.retry.max_retries, 3u);
    EXPECT_EQ(config.retry.initial_backoff.count(), 100);
    EXPECT_EQ(config.retry.max_backoff.count(), 5000);
    EXPECT_DOUBLE_EQ(config.retry.backoff_multiplier, 2.0);

    // Circuit breaker defaults
    EXPECT_TRUE(config.circuit_breaker.enabled);
    EXPECT_EQ(config.circuit_breaker.failure_threshold, 5u);
    EXPECT_EQ(config.circuit_breaker.success_threshold, 2u);
    EXPECT_EQ(config.circuit_breaker.recovery_timeout.count(), 30);
    EXPECT_TRUE(config.circuit_breaker.fallback_enabled);
}

// Test loading a complete YAML config file
TEST_F(ConfigTest, LoadFullConfigFromFile) {
    writeTestConfig("test_config.yaml", R"(
server:
  bind_address: "127.0.0.1"
  api_port: 9000
  metrics_port: 9181

database:
  path: "/data/ranvier.db"
  journal_mode: "DELETE"
  synchronous: "FULL"

health:
  check_interval_seconds: 10
  check_timeout_seconds: 5
  failure_threshold: 5
  recovery_threshold: 3

pool:
  max_connections_per_host: 20
  idle_timeout_seconds: 120
  max_total_connections: 200
  tcp_nodelay: false

routing:
  min_token_length: 64
  backend_retry_limit: 10

timeouts:
  connect_timeout_seconds: 10
  request_timeout_seconds: 600

assets:
  tokenizer_path: "/models/tokenizer.json"

tls:
  enabled: true
  cert_path: "/certs/server.crt"
  key_path: "/certs/server.key"

auth:
  admin_api_key: "test-secret-key-12345"

rate_limit:
  enabled: true
  requests_per_second: 200
  burst_size: 100

retry:
  max_retries: 5
  initial_backoff_ms: 200
  max_backoff_ms: 10000
  backoff_multiplier: 3.0

circuit_breaker:
  enabled: true
  failure_threshold: 10
  success_threshold: 3
  recovery_timeout_seconds: 60
  fallback_enabled: true
)");

    auto config = RanvierConfig::load("test_config.yaml");

    // Server
    EXPECT_EQ(config.server.bind_address, "127.0.0.1");
    EXPECT_EQ(config.server.api_port, 9000);
    EXPECT_EQ(config.server.metrics_port, 9181);

    // Database
    EXPECT_EQ(config.database.path, "/data/ranvier.db");
    EXPECT_EQ(config.database.journal_mode, "DELETE");
    EXPECT_EQ(config.database.synchronous, "FULL");

    // Health
    EXPECT_EQ(config.health.check_interval.count(), 10);
    EXPECT_EQ(config.health.check_timeout.count(), 5);
    EXPECT_EQ(config.health.failure_threshold, 5u);
    EXPECT_EQ(config.health.recovery_threshold, 3u);

    // Pool
    EXPECT_EQ(config.pool.max_connections_per_host, 20u);
    EXPECT_EQ(config.pool.idle_timeout.count(), 120);
    EXPECT_EQ(config.pool.max_total_connections, 200u);
    EXPECT_FALSE(config.pool.tcp_nodelay);

    // Routing
    EXPECT_EQ(config.routing.min_token_length, 64u);
    EXPECT_EQ(config.routing.backend_retry_limit, 10u);

    // Timeouts
    EXPECT_EQ(config.timeouts.connect_timeout.count(), 10);
    EXPECT_EQ(config.timeouts.request_timeout.count(), 600);

    // Assets
    EXPECT_EQ(config.assets.tokenizer_path, "/models/tokenizer.json");

    // TLS
    EXPECT_TRUE(config.tls.enabled);
    EXPECT_EQ(config.tls.cert_path, "/certs/server.crt");
    EXPECT_EQ(config.tls.key_path, "/certs/server.key");

    // Auth
    EXPECT_EQ(config.auth.admin_api_key, "test-secret-key-12345");

    // Rate limit
    EXPECT_TRUE(config.rate_limit.enabled);
    EXPECT_EQ(config.rate_limit.requests_per_second, 200u);
    EXPECT_EQ(config.rate_limit.burst_size, 100u);

    // Retry
    EXPECT_EQ(config.retry.max_retries, 5u);
    EXPECT_EQ(config.retry.initial_backoff.count(), 200);
    EXPECT_EQ(config.retry.max_backoff.count(), 10000);
    EXPECT_DOUBLE_EQ(config.retry.backoff_multiplier, 3.0);

    // Circuit breaker
    EXPECT_TRUE(config.circuit_breaker.enabled);
    EXPECT_EQ(config.circuit_breaker.failure_threshold, 10u);
    EXPECT_EQ(config.circuit_breaker.success_threshold, 3u);
    EXPECT_EQ(config.circuit_breaker.recovery_timeout.count(), 60);
    EXPECT_TRUE(config.circuit_breaker.fallback_enabled);
}

// Test that partial config files work (missing sections use defaults)
TEST_F(ConfigTest, LoadPartialConfigUsesDefaults) {
    writeTestConfig("test_partial.yaml", R"(
server:
  api_port: 3000

routing:
  min_token_length: 32
)");

    auto config = RanvierConfig::load("test_partial.yaml");

    // Specified values
    EXPECT_EQ(config.server.api_port, 3000);
    EXPECT_EQ(config.routing.min_token_length, 32u);

    // Defaults for unspecified values
    EXPECT_EQ(config.server.bind_address, "0.0.0.0");
    EXPECT_EQ(config.server.metrics_port, 9180);
    EXPECT_EQ(config.database.path, "ranvier.db");
    EXPECT_EQ(config.health.check_interval.count(), 5);
    EXPECT_EQ(config.pool.max_connections_per_host, 10u);
}

// Test that missing config file returns defaults
TEST_F(ConfigTest, MissingFileReturnsDefaults) {
    auto config = RanvierConfig::load("nonexistent_file.yaml");

    EXPECT_EQ(config.server.api_port, 8080);
    EXPECT_EQ(config.database.path, "ranvier.db");
}

// Test environment variable overrides
TEST_F(ConfigTest, EnvironmentVariablesOverrideFile) {
    writeTestConfig("test_config.yaml", R"(
server:
  api_port: 9000
  metrics_port: 9181

database:
  path: "/data/ranvier.db"
)");

    // Set environment variables
    setenv("RANVIER_API_PORT", "7777", 1);
    setenv("RANVIER_DB_PATH", "/env/override.db", 1);

    auto config = RanvierConfig::load("test_config.yaml");

    // Env vars should override file settings
    EXPECT_EQ(config.server.api_port, 7777);
    EXPECT_EQ(config.database.path, "/env/override.db");

    // Non-overridden values from file
    EXPECT_EQ(config.server.metrics_port, 9181);
}

// Test environment variable overrides on defaults
TEST_F(ConfigTest, EnvironmentVariablesOverrideDefaults) {
    setenv("RANVIER_API_PORT", "5000", 1);
    setenv("RANVIER_BIND_ADDRESS", "192.168.1.1", 1);
    setenv("RANVIER_HEALTH_CHECK_INTERVAL", "30", 1);
    setenv("RANVIER_MIN_TOKEN_LENGTH", "128", 1);

    auto config = RanvierConfig::defaults();

    EXPECT_EQ(config.server.api_port, 5000);
    EXPECT_EQ(config.server.bind_address, "192.168.1.1");
    EXPECT_EQ(config.health.check_interval.count(), 30);
    EXPECT_EQ(config.routing.min_token_length, 128u);

    // Non-overridden defaults
    EXPECT_EQ(config.server.metrics_port, 9180);
}

// Test invalid environment variables are ignored
TEST_F(ConfigTest, InvalidEnvVarsAreIgnored) {
    setenv("RANVIER_API_PORT", "not_a_number", 1);

    auto config = RanvierConfig::defaults();

    // Should fall back to default since env var is invalid
    EXPECT_EQ(config.server.api_port, 8080);
}

// Test empty environment variables are ignored
TEST_F(ConfigTest, EmptyEnvVarsAreIgnored) {
    setenv("RANVIER_API_PORT", "", 1);

    auto config = RanvierConfig::defaults();

    EXPECT_EQ(config.server.api_port, 8080);
}

// Test invalid YAML throws exception
TEST_F(ConfigTest, InvalidYamlThrows) {
    writeTestConfig("test_invalid.yaml", R"(
server:
  api_port: [this is not valid yaml
    broken: syntax
)");

    EXPECT_THROW(RanvierConfig::load("test_invalid.yaml"), std::runtime_error);
}

// Test config struct initialization
TEST_F(ConfigTest, StructsHaveCorrectDefaults) {
    ServerConfig server;
    EXPECT_EQ(server.bind_address, "0.0.0.0");
    EXPECT_EQ(server.api_port, 8080);

    DatabaseConfig db;
    EXPECT_EQ(db.path, "ranvier.db");
    EXPECT_EQ(db.journal_mode, "WAL");

    HealthConfig health;
    EXPECT_EQ(health.check_interval.count(), 5);

    PoolConfig pool;
    EXPECT_EQ(pool.max_connections_per_host, 10u);
    EXPECT_TRUE(pool.tcp_nodelay);

    RoutingConfig routing;
    EXPECT_EQ(routing.min_token_length, 4u);

    TimeoutConfig timeouts;
    EXPECT_EQ(timeouts.connect_timeout.count(), 5);
    EXPECT_EQ(timeouts.request_timeout.count(), 300);

    AssetsConfig assets;
    EXPECT_EQ(assets.tokenizer_path, "assets/gpt2.json");

    TlsConfig tls;
    EXPECT_FALSE(tls.enabled);
    EXPECT_EQ(tls.cert_path, "");
    EXPECT_EQ(tls.key_path, "");

    AuthConfig auth;
    EXPECT_EQ(auth.admin_api_key, "");

    RateLimitConfig rate_limit;
    EXPECT_FALSE(rate_limit.enabled);
    EXPECT_EQ(rate_limit.requests_per_second, 100u);
    EXPECT_EQ(rate_limit.burst_size, 50u);

    RetryConfig retry;
    EXPECT_EQ(retry.max_retries, 3u);
    EXPECT_EQ(retry.initial_backoff.count(), 100);
    EXPECT_EQ(retry.max_backoff.count(), 5000);
    EXPECT_DOUBLE_EQ(retry.backoff_multiplier, 2.0);

    CircuitBreakerConfig cb;
    EXPECT_TRUE(cb.enabled);
    EXPECT_EQ(cb.failure_threshold, 5u);
    EXPECT_EQ(cb.success_threshold, 2u);
    EXPECT_EQ(cb.recovery_timeout.count(), 30);
    EXPECT_TRUE(cb.fallback_enabled);
}

// Test TLS environment variable overrides
TEST_F(ConfigTest, TlsEnvironmentVariablesOverride) {
    setenv("RANVIER_TLS_ENABLED", "true", 1);
    setenv("RANVIER_TLS_CERT_PATH", "/env/cert.pem", 1);
    setenv("RANVIER_TLS_KEY_PATH", "/env/key.pem", 1);

    auto config = RanvierConfig::defaults();

    EXPECT_TRUE(config.tls.enabled);
    EXPECT_EQ(config.tls.cert_path, "/env/cert.pem");
    EXPECT_EQ(config.tls.key_path, "/env/key.pem");
}

// Test TLS enabled values (1, true, yes all work)
TEST_F(ConfigTest, TlsEnabledAcceptsMultipleValues) {
    // Test "1"
    setenv("RANVIER_TLS_ENABLED", "1", 1);
    auto config1 = RanvierConfig::defaults();
    EXPECT_TRUE(config1.tls.enabled);

    // Test "yes"
    setenv("RANVIER_TLS_ENABLED", "yes", 1);
    auto config2 = RanvierConfig::defaults();
    EXPECT_TRUE(config2.tls.enabled);

    // Test "false" (should remain false)
    setenv("RANVIER_TLS_ENABLED", "false", 1);
    auto config3 = RanvierConfig::defaults();
    EXPECT_FALSE(config3.tls.enabled);
}

// Test auth environment variable override
TEST_F(ConfigTest, AuthEnvironmentVariableOverride) {
    setenv("RANVIER_ADMIN_API_KEY", "env-secret-key", 1);

    auto config = RanvierConfig::defaults();

    EXPECT_EQ(config.auth.admin_api_key, "env-secret-key");
}

// Test rate limit environment variable overrides
TEST_F(ConfigTest, RateLimitEnvironmentVariablesOverride) {
    setenv("RANVIER_RATE_LIMIT_ENABLED", "true", 1);
    setenv("RANVIER_RATE_LIMIT_RPS", "500", 1);
    setenv("RANVIER_RATE_LIMIT_BURST", "250", 1);

    auto config = RanvierConfig::defaults();

    EXPECT_TRUE(config.rate_limit.enabled);
    EXPECT_EQ(config.rate_limit.requests_per_second, 500u);
    EXPECT_EQ(config.rate_limit.burst_size, 250u);
}

// Test retry environment variable overrides
TEST_F(ConfigTest, RetryEnvironmentVariablesOverride) {
    setenv("RANVIER_RETRY_MAX", "10", 1);
    setenv("RANVIER_RETRY_INITIAL_BACKOFF_MS", "500", 1);
    setenv("RANVIER_RETRY_MAX_BACKOFF_MS", "30000", 1);

    auto config = RanvierConfig::defaults();

    EXPECT_EQ(config.retry.max_retries, 10u);
    EXPECT_EQ(config.retry.initial_backoff.count(), 500);
    EXPECT_EQ(config.retry.max_backoff.count(), 30000);
}

// Test retry with zero max_retries (disabled)
TEST_F(ConfigTest, RetryDisabledWithZeroMaxRetries) {
    setenv("RANVIER_RETRY_MAX", "0", 1);

    auto config = RanvierConfig::defaults();

    EXPECT_EQ(config.retry.max_retries, 0u);
}

// Test circuit breaker environment variable overrides
TEST_F(ConfigTest, CircuitBreakerEnvironmentVariablesOverride) {
    setenv("RANVIER_CIRCUIT_BREAKER_ENABLED", "false", 1);
    setenv("RANVIER_CIRCUIT_BREAKER_FAILURE_THRESHOLD", "10", 1);
    setenv("RANVIER_CIRCUIT_BREAKER_SUCCESS_THRESHOLD", "5", 1);
    setenv("RANVIER_CIRCUIT_BREAKER_RECOVERY_TIMEOUT", "120", 1);
    setenv("RANVIER_CIRCUIT_BREAKER_FALLBACK", "false", 1);

    auto config = RanvierConfig::defaults();

    EXPECT_FALSE(config.circuit_breaker.enabled);
    EXPECT_EQ(config.circuit_breaker.failure_threshold, 10u);
    EXPECT_EQ(config.circuit_breaker.success_threshold, 5u);
    EXPECT_EQ(config.circuit_breaker.recovery_timeout.count(), 120);
    EXPECT_FALSE(config.circuit_breaker.fallback_enabled);
}

// Test circuit breaker disabled via env
TEST_F(ConfigTest, CircuitBreakerDisabledViaEnv) {
    setenv("RANVIER_CIRCUIT_BREAKER_ENABLED", "0", 1);

    auto config = RanvierConfig::defaults();

    EXPECT_FALSE(config.circuit_breaker.enabled);
}

// =============================================================================
// Cluster Configuration Tests
// =============================================================================

TEST_F(ConfigTest, ClusterConfigDefaults) {
    auto config = RanvierConfig::defaults();

    EXPECT_FALSE(config.cluster.enabled);
    EXPECT_EQ(config.cluster.gossip_port, 9190);
    EXPECT_TRUE(config.cluster.peers.empty());
    EXPECT_EQ(config.cluster.gossip_interval.count(), 1000);
    EXPECT_EQ(config.cluster.gossip_heartbeat_interval.count(), 5);
    EXPECT_EQ(config.cluster.gossip_peer_timeout.count(), 15);

    // DNS discovery defaults
    EXPECT_TRUE(config.cluster.discovery_dns_name.empty());
    EXPECT_EQ(config.cluster.discovery_type, DiscoveryType::STATIC);
    EXPECT_EQ(config.cluster.discovery_refresh_interval.count(), 30);
}

TEST_F(ConfigTest, ClusterConfigFromYaml) {
    writeTestConfig("test_cluster.yaml", R"(
cluster:
  enabled: true
  gossip_port: 9200
  peers:
    - "192.168.1.100:9200"
    - "192.168.1.101:9200"
  gossip_interval_ms: 2000
  gossip_heartbeat_interval_seconds: 10
  gossip_peer_timeout_seconds: 30
)");

    auto config = RanvierConfig::load("test_cluster.yaml");

    EXPECT_TRUE(config.cluster.enabled);
    EXPECT_EQ(config.cluster.gossip_port, 9200);
    ASSERT_EQ(config.cluster.peers.size(), 2);
    EXPECT_EQ(config.cluster.peers[0], "192.168.1.100:9200");
    EXPECT_EQ(config.cluster.peers[1], "192.168.1.101:9200");
    EXPECT_EQ(config.cluster.gossip_interval.count(), 2000);
    EXPECT_EQ(config.cluster.gossip_heartbeat_interval.count(), 10);
    EXPECT_EQ(config.cluster.gossip_peer_timeout.count(), 30);
}

// =============================================================================
// DNS Discovery Configuration Tests
// =============================================================================

TEST_F(ConfigTest, DnsDiscoveryDefaults) {
    ClusterConfig cluster;

    EXPECT_TRUE(cluster.discovery_dns_name.empty());
    EXPECT_EQ(cluster.discovery_type, DiscoveryType::STATIC);
    EXPECT_EQ(cluster.discovery_refresh_interval.count(), 30);
}

TEST_F(ConfigTest, DnsDiscoveryFromYamlTypeA) {
    writeTestConfig("test_dns_discovery.yaml", R"(
cluster:
  enabled: true
  gossip_port: 9190
  discovery_dns_name: "ranvier-headless.default.svc.cluster.local"
  discovery_type: "A"
  discovery_refresh_interval_seconds: 60
)");

    auto config = RanvierConfig::load("test_dns_discovery.yaml");

    EXPECT_TRUE(config.cluster.enabled);
    EXPECT_EQ(config.cluster.discovery_dns_name, "ranvier-headless.default.svc.cluster.local");
    EXPECT_EQ(config.cluster.discovery_type, DiscoveryType::A);
    EXPECT_EQ(config.cluster.discovery_refresh_interval.count(), 60);
}

TEST_F(ConfigTest, DnsDiscoveryFromYamlTypeSRV) {
    writeTestConfig("test_dns_discovery.yaml", R"(
cluster:
  enabled: true
  gossip_port: 9190
  discovery_dns_name: "ranvier.example.com"
  discovery_type: "SRV"
  discovery_refresh_interval_seconds: 45
)");

    auto config = RanvierConfig::load("test_dns_discovery.yaml");

    EXPECT_EQ(config.cluster.discovery_type, DiscoveryType::SRV);
    EXPECT_EQ(config.cluster.discovery_refresh_interval.count(), 45);
}

TEST_F(ConfigTest, DnsDiscoveryTypeCaseInsensitive) {
    // Test lowercase "a"
    writeTestConfig("test_dns_discovery.yaml", R"(
cluster:
  enabled: true
  discovery_dns_name: "test.local"
  discovery_type: "a"
)");
    auto config1 = RanvierConfig::load("test_dns_discovery.yaml");
    EXPECT_EQ(config1.cluster.discovery_type, DiscoveryType::A);

    // Test lowercase "srv"
    writeTestConfig("test_dns_discovery.yaml", R"(
cluster:
  enabled: true
  discovery_dns_name: "test.local"
  discovery_type: "srv"
)");
    auto config2 = RanvierConfig::load("test_dns_discovery.yaml");
    EXPECT_EQ(config2.cluster.discovery_type, DiscoveryType::SRV);

    // Test invalid type falls back to STATIC
    writeTestConfig("test_dns_discovery.yaml", R"(
cluster:
  enabled: true
  discovery_type: "invalid"
)");
    auto config3 = RanvierConfig::load("test_dns_discovery.yaml");
    EXPECT_EQ(config3.cluster.discovery_type, DiscoveryType::STATIC);
}

TEST_F(ConfigTest, DnsDiscoveryEnvironmentVariables) {
    setenv("RANVIER_CLUSTER_DISCOVERY_DNS_NAME", "env-discovery.local", 1);
    setenv("RANVIER_CLUSTER_DISCOVERY_TYPE", "A", 1);
    setenv("RANVIER_CLUSTER_DISCOVERY_REFRESH_INTERVAL", "120", 1);

    auto config = RanvierConfig::defaults();

    EXPECT_EQ(config.cluster.discovery_dns_name, "env-discovery.local");
    EXPECT_EQ(config.cluster.discovery_type, DiscoveryType::A);
    EXPECT_EQ(config.cluster.discovery_refresh_interval.count(), 120);
}

TEST_F(ConfigTest, DnsDiscoveryEnvTypeSRV) {
    setenv("RANVIER_CLUSTER_DISCOVERY_TYPE", "SRV", 1);
    auto config = RanvierConfig::defaults();
    EXPECT_EQ(config.cluster.discovery_type, DiscoveryType::SRV);
}

TEST_F(ConfigTest, DnsDiscoveryEnvTypeLowercase) {
    setenv("RANVIER_CLUSTER_DISCOVERY_TYPE", "srv", 1);
    auto config1 = RanvierConfig::defaults();
    EXPECT_EQ(config1.cluster.discovery_type, DiscoveryType::SRV);

    setenv("RANVIER_CLUSTER_DISCOVERY_TYPE", "a", 1);
    auto config2 = RanvierConfig::defaults();
    EXPECT_EQ(config2.cluster.discovery_type, DiscoveryType::A);
}

TEST_F(ConfigTest, DnsDiscoveryEnvOverridesYaml) {
    writeTestConfig("test_dns_discovery.yaml", R"(
cluster:
  enabled: true
  discovery_dns_name: "yaml-discovery.local"
  discovery_type: "A"
  discovery_refresh_interval_seconds: 30
)");

    setenv("RANVIER_CLUSTER_DISCOVERY_DNS_NAME", "env-override.local", 1);
    setenv("RANVIER_CLUSTER_DISCOVERY_TYPE", "SRV", 1);
    setenv("RANVIER_CLUSTER_DISCOVERY_REFRESH_INTERVAL", "90", 1);

    auto config = RanvierConfig::load("test_dns_discovery.yaml");

    // Env vars should override YAML
    EXPECT_EQ(config.cluster.discovery_dns_name, "env-override.local");
    EXPECT_EQ(config.cluster.discovery_type, DiscoveryType::SRV);
    EXPECT_EQ(config.cluster.discovery_refresh_interval.count(), 90);
}

// =============================================================================
// Cluster Configuration Validation Tests
// =============================================================================

TEST_F(ConfigTest, ValidationPassesForValidClusterConfig) {
    RanvierConfig config;
    config.cluster.enabled = true;
    config.cluster.gossip_port = 9190;
    config.cluster.gossip_interval = std::chrono::milliseconds(1000);
    config.cluster.gossip_heartbeat_interval = std::chrono::seconds(5);
    config.cluster.gossip_peer_timeout = std::chrono::seconds(15);

    auto error = RanvierConfig::validate(config);
    EXPECT_FALSE(error.has_value()) << "Unexpected error: " << error.value_or("");
}

TEST_F(ConfigTest, ValidationFailsForZeroGossipPort) {
    RanvierConfig config;
    config.cluster.enabled = true;
    config.cluster.gossip_port = 0;

    auto error = RanvierConfig::validate(config);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("gossip_port"), std::string::npos);
}

TEST_F(ConfigTest, ValidationFailsForShortGossipInterval) {
    RanvierConfig config;
    config.cluster.enabled = true;
    config.cluster.gossip_port = 9190;
    config.cluster.gossip_interval = std::chrono::milliseconds(50);  // Too short

    auto error = RanvierConfig::validate(config);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("gossip_interval"), std::string::npos);
}

TEST_F(ConfigTest, ValidationFailsForPeerTimeoutLessThanTwiceHeartbeat) {
    RanvierConfig config;
    config.cluster.enabled = true;
    config.cluster.gossip_port = 9190;
    config.cluster.gossip_interval = std::chrono::milliseconds(1000);
    config.cluster.gossip_heartbeat_interval = std::chrono::seconds(10);
    config.cluster.gossip_peer_timeout = std::chrono::seconds(15);  // Less than 2x heartbeat

    auto error = RanvierConfig::validate(config);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("peer_timeout"), std::string::npos);
}

TEST_F(ConfigTest, ValidationFailsForDnsDiscoveryWithoutDnsName) {
    RanvierConfig config;
    config.cluster.enabled = true;
    config.cluster.gossip_port = 9190;
    config.cluster.gossip_interval = std::chrono::milliseconds(1000);
    config.cluster.gossip_heartbeat_interval = std::chrono::seconds(5);
    config.cluster.gossip_peer_timeout = std::chrono::seconds(15);
    config.cluster.discovery_type = DiscoveryType::A;
    config.cluster.discovery_dns_name = "";  // Empty!

    auto error = RanvierConfig::validate(config);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("discovery_dns_name"), std::string::npos);
}

TEST_F(ConfigTest, ValidationFailsForShortRefreshInterval) {
    RanvierConfig config;
    config.cluster.enabled = true;
    config.cluster.gossip_port = 9190;
    config.cluster.gossip_interval = std::chrono::milliseconds(1000);
    config.cluster.gossip_heartbeat_interval = std::chrono::seconds(5);
    config.cluster.gossip_peer_timeout = std::chrono::seconds(15);
    config.cluster.discovery_type = DiscoveryType::SRV;
    config.cluster.discovery_dns_name = "ranvier.local";
    config.cluster.discovery_refresh_interval = std::chrono::seconds(3);  // Too short

    auto error = RanvierConfig::validate(config);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("discovery_refresh_interval"), std::string::npos);
}

TEST_F(ConfigTest, ValidationPassesForStaticDiscoveryWithoutDnsName) {
    RanvierConfig config;
    config.cluster.enabled = true;
    config.cluster.gossip_port = 9190;
    config.cluster.gossip_interval = std::chrono::milliseconds(1000);
    config.cluster.gossip_heartbeat_interval = std::chrono::seconds(5);
    config.cluster.gossip_peer_timeout = std::chrono::seconds(15);
    config.cluster.discovery_type = DiscoveryType::STATIC;  // STATIC doesn't require DNS name
    config.cluster.discovery_dns_name = "";

    auto error = RanvierConfig::validate(config);
    EXPECT_FALSE(error.has_value()) << "Unexpected error: " << error.value_or("");
}

TEST_F(ConfigTest, ValidationPassesForValidDnsDiscoveryConfig) {
    RanvierConfig config;
    config.cluster.enabled = true;
    config.cluster.gossip_port = 9190;
    config.cluster.gossip_interval = std::chrono::milliseconds(1000);
    config.cluster.gossip_heartbeat_interval = std::chrono::seconds(5);
    config.cluster.gossip_peer_timeout = std::chrono::seconds(15);
    config.cluster.discovery_type = DiscoveryType::A;
    config.cluster.discovery_dns_name = "ranvier-headless.default.svc.cluster.local";
    config.cluster.discovery_refresh_interval = std::chrono::seconds(30);

    auto error = RanvierConfig::validate(config);
    EXPECT_FALSE(error.has_value()) << "Unexpected error: " << error.value_or("");
}

// =============================================================================
// DiscoveryType Enum Tests
// =============================================================================

TEST_F(ConfigTest, DiscoveryTypeEnumValues) {
    // Ensure enum values are distinct
    EXPECT_NE(DiscoveryType::STATIC, DiscoveryType::A);
    EXPECT_NE(DiscoveryType::STATIC, DiscoveryType::SRV);
    EXPECT_NE(DiscoveryType::A, DiscoveryType::SRV);
}

// =============================================================================
// Pre-tokenized Client Input Configuration Tests
// =============================================================================

TEST_F(ConfigTest, AcceptClientTokensDefaults) {
    auto config = RanvierConfig::defaults();

    EXPECT_FALSE(config.routing.accept_client_tokens);
    EXPECT_EQ(config.routing.max_token_id, 100000);
}

TEST_F(ConfigTest, AcceptClientTokensFromYaml) {
    writeTestConfig("test_config.yaml", R"(
routing:
  accept_client_tokens: true
  max_token_id: 50257
)");

    auto config = RanvierConfig::load("test_config.yaml");

    EXPECT_TRUE(config.routing.accept_client_tokens);
    EXPECT_EQ(config.routing.max_token_id, 50257);
}

TEST_F(ConfigTest, AcceptClientTokensEnvironmentVariables) {
    setenv("RANVIER_ACCEPT_CLIENT_TOKENS", "true", 1);
    setenv("RANVIER_MAX_TOKEN_ID", "32000", 1);

    auto config = RanvierConfig::defaults();

    EXPECT_TRUE(config.routing.accept_client_tokens);
    EXPECT_EQ(config.routing.max_token_id, 32000);

    // Clean up
    unsetenv("RANVIER_ACCEPT_CLIENT_TOKENS");
    unsetenv("RANVIER_MAX_TOKEN_ID");
}

TEST_F(ConfigTest, AcceptClientTokensEnvOverridesYaml) {
    writeTestConfig("test_config.yaml", R"(
routing:
  accept_client_tokens: false
  max_token_id: 50257
)");

    setenv("RANVIER_ACCEPT_CLIENT_TOKENS", "1", 1);
    setenv("RANVIER_MAX_TOKEN_ID", "100000", 1);

    auto config = RanvierConfig::load("test_config.yaml");

    // Env vars should override YAML
    EXPECT_TRUE(config.routing.accept_client_tokens);
    EXPECT_EQ(config.routing.max_token_id, 100000);

    // Clean up
    unsetenv("RANVIER_ACCEPT_CLIENT_TOKENS");
    unsetenv("RANVIER_MAX_TOKEN_ID");
}

TEST_F(ConfigTest, AcceptClientTokensYesValue) {
    setenv("RANVIER_ACCEPT_CLIENT_TOKENS", "yes", 1);

    auto config = RanvierConfig::defaults();

    EXPECT_TRUE(config.routing.accept_client_tokens);

    unsetenv("RANVIER_ACCEPT_CLIENT_TOKENS");
}

TEST_F(ConfigTest, AcceptClientTokensFalseValue) {
    setenv("RANVIER_ACCEPT_CLIENT_TOKENS", "false", 1);

    auto config = RanvierConfig::defaults();

    EXPECT_FALSE(config.routing.accept_client_tokens);

    unsetenv("RANVIER_ACCEPT_CLIENT_TOKENS");
}

TEST_F(ConfigTest, RoutingConfigStructDefaults) {
    RoutingConfig routing;

    EXPECT_FALSE(routing.accept_client_tokens);
    EXPECT_EQ(routing.max_token_id, 100000);
    EXPECT_FALSE(routing.enable_token_forwarding);
}

// =============================================================================
// API Key Rotation Tests
// =============================================================================

TEST_F(ConfigTest, ApiKeyIsExpiredForPastDate) {
    ApiKey key;
    key.key = "test-key";
    key.name = "test";
    key.expires = "2020-01-01";  // Past date

    EXPECT_TRUE(key.is_expired());
}

TEST_F(ConfigTest, ApiKeyIsNotExpiredForFutureDate) {
    ApiKey key;
    key.key = "test-key";
    key.name = "test";
    key.expires = "2099-12-31";  // Far future date

    EXPECT_FALSE(key.is_expired());
}

TEST_F(ConfigTest, ApiKeyIsNotExpiredWithNoExpiry) {
    ApiKey key;
    key.key = "test-key";
    key.name = "test";
    // No expires set (nullopt)

    EXPECT_FALSE(key.is_expired());
}

TEST_F(ConfigTest, ApiKeyIsNotExpiredWithEmptyExpiry) {
    ApiKey key;
    key.key = "test-key";
    key.name = "test";
    key.expires = "";  // Empty string

    EXPECT_FALSE(key.is_expired());
}

TEST_F(ConfigTest, ApiKeyGetLogIdentifierReturnsName) {
    ApiKey key;
    key.key = "rnv_prod_abc123def456ghi789jkl012mno345pqr";
    key.name = "production-deploy";

    EXPECT_EQ(key.get_log_identifier(), "production-deploy");
}

TEST_F(ConfigTest, ApiKeyGetLogIdentifierReturnsTruncatedKeyIfNoName) {
    ApiKey key;
    key.key = "rnv_prod_abc123def456ghi789jkl012mno345pqr";
    key.name = "";  // No name

    std::string log_id = key.get_log_identifier();
    EXPECT_EQ(log_id, "rnv_prod_abc...");  // First 12 chars + "..."
}

TEST_F(ConfigTest, AuthConfigSecureCompareWorks) {
    EXPECT_TRUE(AuthConfig::secure_compare("test", "test"));
    EXPECT_FALSE(AuthConfig::secure_compare("test", "test1"));
    EXPECT_FALSE(AuthConfig::secure_compare("test1", "test"));
    EXPECT_FALSE(AuthConfig::secure_compare("test", "TEST"));
    EXPECT_TRUE(AuthConfig::secure_compare("", ""));
}

TEST_F(ConfigTest, AuthConfigIsEnabledWithLegacyKey) {
    AuthConfig auth;
    auth.admin_api_key = "";
    EXPECT_FALSE(auth.is_enabled());

    auth.admin_api_key = "secret-key";
    EXPECT_TRUE(auth.is_enabled());
}

TEST_F(ConfigTest, AuthConfigIsEnabledWithApiKeys) {
    AuthConfig auth;
    EXPECT_FALSE(auth.is_enabled());

    ApiKey key;
    key.key = "test-key";
    key.name = "test";
    auth.api_keys.push_back(key);
    EXPECT_TRUE(auth.is_enabled());
}

TEST_F(ConfigTest, AuthConfigValidateTokenWithLegacyKey) {
    AuthConfig auth;
    auth.admin_api_key = "secret-key";

    auto [valid1, name1] = auth.validate_token("secret-key");
    EXPECT_TRUE(valid1);
    EXPECT_EQ(name1, "legacy-key");

    auto [valid2, name2] = auth.validate_token("wrong-key");
    EXPECT_FALSE(valid2);
    EXPECT_EQ(name2, "");
}

TEST_F(ConfigTest, AuthConfigValidateTokenWithApiKeys) {
    AuthConfig auth;

    ApiKey key1;
    key1.key = "rnv_prod_key1";
    key1.name = "prod-key";
    key1.expires = "2099-12-31";
    auth.api_keys.push_back(key1);

    ApiKey key2;
    key2.key = "rnv_test_key2";
    key2.name = "test-key";
    auth.api_keys.push_back(key2);

    auto [valid1, name1] = auth.validate_token("rnv_prod_key1");
    EXPECT_TRUE(valid1);
    EXPECT_EQ(name1, "prod-key");

    auto [valid2, name2] = auth.validate_token("rnv_test_key2");
    EXPECT_TRUE(valid2);
    EXPECT_EQ(name2, "test-key");

    auto [valid3, name3] = auth.validate_token("invalid");
    EXPECT_FALSE(valid3);
}

TEST_F(ConfigTest, AuthConfigRejectsExpiredKey) {
    AuthConfig auth;

    ApiKey key;
    key.key = "rnv_expired_key";
    key.name = "expired-key";
    key.expires = "2020-01-01";  // Past date
    auth.api_keys.push_back(key);

    auto [valid, name] = auth.validate_token("rnv_expired_key");
    EXPECT_FALSE(valid);
    EXPECT_NE(name.find("expired"), std::string::npos);
}

TEST_F(ConfigTest, AuthConfigApiKeysTakePrecedenceOverLegacy) {
    AuthConfig auth;
    auth.admin_api_key = "legacy-secret";

    ApiKey key;
    key.key = "rnv_new_key";
    key.name = "new-key";
    auth.api_keys.push_back(key);

    // New key should work
    auto [valid1, name1] = auth.validate_token("rnv_new_key");
    EXPECT_TRUE(valid1);
    EXPECT_EQ(name1, "new-key");

    // Legacy key should also still work (fallback)
    auto [valid2, name2] = auth.validate_token("legacy-secret");
    EXPECT_TRUE(valid2);
    EXPECT_EQ(name2, "legacy-key");
}

TEST_F(ConfigTest, AuthConfigValidKeyCount) {
    AuthConfig auth;
    EXPECT_EQ(auth.valid_key_count(), 0u);

    auth.admin_api_key = "legacy";
    EXPECT_EQ(auth.valid_key_count(), 1u);

    ApiKey key1;
    key1.key = "valid-key";
    key1.name = "valid";
    auth.api_keys.push_back(key1);
    EXPECT_EQ(auth.valid_key_count(), 2u);

    ApiKey key2;
    key2.key = "expired-key";
    key2.name = "expired";
    key2.expires = "2020-01-01";  // Expired
    auth.api_keys.push_back(key2);
    EXPECT_EQ(auth.valid_key_count(), 2u);  // Still 2, expired key doesn't count
}

TEST_F(ConfigTest, ApiKeysFromYaml) {
    writeTestConfig("test_config.yaml", R"(
auth:
  api_keys:
    - key: "rnv_prod_abc123"
      name: "production"
      created: "2025-01-01"
      expires: "2025-12-31"
      roles: ["admin"]
    - key: "rnv_test_xyz789"
      name: "testing"
      created: "2025-01-15"
      roles: ["viewer"]
)");

    auto config = RanvierConfig::load("test_config.yaml");

    ASSERT_EQ(config.auth.api_keys.size(), 2u);

    EXPECT_EQ(config.auth.api_keys[0].key, "rnv_prod_abc123");
    EXPECT_EQ(config.auth.api_keys[0].name, "production");
    EXPECT_EQ(config.auth.api_keys[0].created, "2025-01-01");
    EXPECT_EQ(config.auth.api_keys[0].expires.value(), "2025-12-31");
    ASSERT_EQ(config.auth.api_keys[0].roles.size(), 1u);
    EXPECT_EQ(config.auth.api_keys[0].roles[0], "admin");

    EXPECT_EQ(config.auth.api_keys[1].key, "rnv_test_xyz789");
    EXPECT_EQ(config.auth.api_keys[1].name, "testing");
    EXPECT_FALSE(config.auth.api_keys[1].expires.has_value());
    ASSERT_EQ(config.auth.api_keys[1].roles.size(), 1u);
    EXPECT_EQ(config.auth.api_keys[1].roles[0], "viewer");
}

TEST_F(ConfigTest, ApiKeysAndLegacyKeyFromYaml) {
    writeTestConfig("test_config.yaml", R"(
auth:
  admin_api_key: "legacy-key-123"
  api_keys:
    - key: "rnv_new_key"
      name: "new-key"
)");

    auto config = RanvierConfig::load("test_config.yaml");

    // Both should be set
    EXPECT_EQ(config.auth.admin_api_key, "legacy-key-123");
    ASSERT_EQ(config.auth.api_keys.size(), 1u);
    EXPECT_EQ(config.auth.api_keys[0].key, "rnv_new_key");

    // Auth should be enabled
    EXPECT_TRUE(config.auth.is_enabled());

    // Both keys should be valid
    EXPECT_EQ(config.auth.valid_key_count(), 2u);

    auto [valid1, _1] = config.auth.validate_token("legacy-key-123");
    EXPECT_TRUE(valid1);

    auto [valid2, _2] = config.auth.validate_token("rnv_new_key");
    EXPECT_TRUE(valid2);
}

TEST_F(ConfigTest, LegacySingleKeyStillWorks) {
    writeTestConfig("test_config.yaml", R"(
auth:
  admin_api_key: "simple-key"
)");

    auto config = RanvierConfig::load("test_config.yaml");

    EXPECT_EQ(config.auth.admin_api_key, "simple-key");
    EXPECT_TRUE(config.auth.api_keys.empty());
    EXPECT_TRUE(config.auth.is_enabled());

    auto [valid, name] = config.auth.validate_token("simple-key");
    EXPECT_TRUE(valid);
    EXPECT_EQ(name, "legacy-key");
}

TEST_F(ConfigTest, ApiKeyHotReloadAddsNewKey) {
    // Simulate initial config
    writeTestConfig("test_config.yaml", R"(
auth:
  api_keys:
    - key: "rnv_key1"
      name: "key1"
)");

    auto config1 = RanvierConfig::load("test_config.yaml");
    EXPECT_EQ(config1.auth.api_keys.size(), 1u);

    // Simulate updated config with new key
    writeTestConfig("test_config.yaml", R"(
auth:
  api_keys:
    - key: "rnv_key1"
      name: "key1"
    - key: "rnv_key2"
      name: "key2"
)");

    auto config2 = RanvierConfig::load("test_config.yaml");
    EXPECT_EQ(config2.auth.api_keys.size(), 2u);

    // New key should now be valid
    auto [valid, name] = config2.auth.validate_token("rnv_key2");
    EXPECT_TRUE(valid);
    EXPECT_EQ(name, "key2");
}

// =============================================================================
// Gossip TLS/DTLS Configuration Tests
// =============================================================================

TEST_F(ConfigTest, GossipTlsConfigDefaults) {
    GossipTlsConfig tls;

    EXPECT_FALSE(tls.enabled);
    EXPECT_EQ(tls.cert_path, "");
    EXPECT_EQ(tls.key_path, "");
    EXPECT_EQ(tls.ca_path, "");
    EXPECT_TRUE(tls.verify_peer);
    EXPECT_EQ(tls.cert_reload_interval.count(), 300);
    EXPECT_FALSE(tls.allow_plaintext_fallback);
}

TEST_F(ConfigTest, GossipTlsInClusterConfigDefaults) {
    auto config = RanvierConfig::defaults();

    EXPECT_FALSE(config.cluster.tls.enabled);
    EXPECT_EQ(config.cluster.tls.cert_path, "");
    EXPECT_EQ(config.cluster.tls.key_path, "");
    EXPECT_EQ(config.cluster.tls.ca_path, "");
    EXPECT_TRUE(config.cluster.tls.verify_peer);
    EXPECT_EQ(config.cluster.tls.cert_reload_interval.count(), 300);
    EXPECT_FALSE(config.cluster.tls.allow_plaintext_fallback);
}

TEST_F(ConfigTest, GossipTlsFromYaml) {
    writeTestConfig("test_gossip_tls.yaml", R"(
cluster:
  enabled: true
  gossip_port: 9190
  tls:
    enabled: true
    cert_path: "/certs/node.crt"
    key_path: "/certs/node.key"
    ca_path: "/certs/ca.crt"
    verify_peer: true
    cert_reload_interval_seconds: 600
    allow_plaintext_fallback: false
)");

    auto config = RanvierConfig::load("test_gossip_tls.yaml");

    EXPECT_TRUE(config.cluster.enabled);
    EXPECT_TRUE(config.cluster.tls.enabled);
    EXPECT_EQ(config.cluster.tls.cert_path, "/certs/node.crt");
    EXPECT_EQ(config.cluster.tls.key_path, "/certs/node.key");
    EXPECT_EQ(config.cluster.tls.ca_path, "/certs/ca.crt");
    EXPECT_TRUE(config.cluster.tls.verify_peer);
    EXPECT_EQ(config.cluster.tls.cert_reload_interval.count(), 600);
    EXPECT_FALSE(config.cluster.tls.allow_plaintext_fallback);
}

TEST_F(ConfigTest, GossipTlsWithPlaintextFallback) {
    writeTestConfig("test_gossip_tls.yaml", R"(
cluster:
  enabled: true
  tls:
    enabled: true
    cert_path: "/certs/node.crt"
    key_path: "/certs/node.key"
    ca_path: "/certs/ca.crt"
    verify_peer: false
    allow_plaintext_fallback: true
)");

    auto config = RanvierConfig::load("test_gossip_tls.yaml");

    EXPECT_TRUE(config.cluster.tls.enabled);
    EXPECT_FALSE(config.cluster.tls.verify_peer);
    EXPECT_TRUE(config.cluster.tls.allow_plaintext_fallback);
}

TEST_F(ConfigTest, GossipTlsEnvironmentVariables) {
    setenv("RANVIER_CLUSTER_TLS_ENABLED", "true", 1);
    setenv("RANVIER_CLUSTER_TLS_CERT_PATH", "/env/node.crt", 1);
    setenv("RANVIER_CLUSTER_TLS_KEY_PATH", "/env/node.key", 1);
    setenv("RANVIER_CLUSTER_TLS_CA_PATH", "/env/ca.crt", 1);
    setenv("RANVIER_CLUSTER_TLS_VERIFY_PEER", "false", 1);
    setenv("RANVIER_CLUSTER_TLS_CERT_RELOAD_INTERVAL", "120", 1);
    setenv("RANVIER_CLUSTER_TLS_ALLOW_PLAINTEXT_FALLBACK", "true", 1);

    auto config = RanvierConfig::defaults();

    EXPECT_TRUE(config.cluster.tls.enabled);
    EXPECT_EQ(config.cluster.tls.cert_path, "/env/node.crt");
    EXPECT_EQ(config.cluster.tls.key_path, "/env/node.key");
    EXPECT_EQ(config.cluster.tls.ca_path, "/env/ca.crt");
    EXPECT_FALSE(config.cluster.tls.verify_peer);
    EXPECT_EQ(config.cluster.tls.cert_reload_interval.count(), 120);
    EXPECT_TRUE(config.cluster.tls.allow_plaintext_fallback);
}

TEST_F(ConfigTest, GossipTlsEnvOverridesYaml) {
    writeTestConfig("test_gossip_tls.yaml", R"(
cluster:
  enabled: true
  tls:
    enabled: false
    cert_path: "/yaml/node.crt"
    key_path: "/yaml/node.key"
    ca_path: "/yaml/ca.crt"
)");

    setenv("RANVIER_CLUSTER_TLS_ENABLED", "1", 1);
    setenv("RANVIER_CLUSTER_TLS_CERT_PATH", "/env/override.crt", 1);

    auto config = RanvierConfig::load("test_gossip_tls.yaml");

    // Env vars override YAML
    EXPECT_TRUE(config.cluster.tls.enabled);
    EXPECT_EQ(config.cluster.tls.cert_path, "/env/override.crt");
    // Non-overridden values from YAML
    EXPECT_EQ(config.cluster.tls.key_path, "/yaml/node.key");
    EXPECT_EQ(config.cluster.tls.ca_path, "/yaml/ca.crt");
}

TEST_F(ConfigTest, GossipTlsEnabledAcceptsMultipleValues) {
    // Test "1"
    setenv("RANVIER_CLUSTER_TLS_ENABLED", "1", 1);
    auto config1 = RanvierConfig::defaults();
    EXPECT_TRUE(config1.cluster.tls.enabled);

    // Test "yes"
    setenv("RANVIER_CLUSTER_TLS_ENABLED", "yes", 1);
    auto config2 = RanvierConfig::defaults();
    EXPECT_TRUE(config2.cluster.tls.enabled);

    // Test "true"
    setenv("RANVIER_CLUSTER_TLS_ENABLED", "true", 1);
    auto config3 = RanvierConfig::defaults();
    EXPECT_TRUE(config3.cluster.tls.enabled);

    // Test "false" (should be false)
    setenv("RANVIER_CLUSTER_TLS_ENABLED", "false", 1);
    auto config4 = RanvierConfig::defaults();
    EXPECT_FALSE(config4.cluster.tls.enabled);

    // Test "0" (should be false)
    setenv("RANVIER_CLUSTER_TLS_ENABLED", "0", 1);
    auto config5 = RanvierConfig::defaults();
    EXPECT_FALSE(config5.cluster.tls.enabled);
}

TEST_F(ConfigTest, GossipTlsDisabledByDefault) {
    writeTestConfig("test_gossip_tls.yaml", R"(
cluster:
  enabled: true
  gossip_port: 9190
)");

    auto config = RanvierConfig::load("test_gossip_tls.yaml");

    // TLS should be disabled if not specified
    EXPECT_FALSE(config.cluster.tls.enabled);
}

// =============================================================================
// Gossip TLS Validation Tests
// =============================================================================

TEST_F(ConfigTest, ValidationFailsForGossipTlsWithoutCertPath) {
    RanvierConfig config;
    config.cluster.enabled = true;
    config.cluster.gossip_port = 9190;
    config.cluster.gossip_interval = std::chrono::milliseconds(1000);
    config.cluster.gossip_heartbeat_interval = std::chrono::seconds(5);
    config.cluster.gossip_peer_timeout = std::chrono::seconds(15);
    config.cluster.tls.enabled = true;
    config.cluster.tls.cert_path = "";  // Missing!
    config.cluster.tls.key_path = "/certs/node.key";
    config.cluster.tls.ca_path = "/certs/ca.crt";

    auto error = RanvierConfig::validate(config);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("cert_path"), std::string::npos);
}

TEST_F(ConfigTest, ValidationFailsForGossipTlsWithoutKeyPath) {
    RanvierConfig config;
    config.cluster.enabled = true;
    config.cluster.gossip_port = 9190;
    config.cluster.gossip_interval = std::chrono::milliseconds(1000);
    config.cluster.gossip_heartbeat_interval = std::chrono::seconds(5);
    config.cluster.gossip_peer_timeout = std::chrono::seconds(15);
    config.cluster.tls.enabled = true;
    config.cluster.tls.cert_path = "/certs/node.crt";
    config.cluster.tls.key_path = "";  // Missing!
    config.cluster.tls.ca_path = "/certs/ca.crt";

    auto error = RanvierConfig::validate(config);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("key_path"), std::string::npos);
}

TEST_F(ConfigTest, ValidationFailsForGossipTlsWithoutCaPath) {
    RanvierConfig config;
    config.cluster.enabled = true;
    config.cluster.gossip_port = 9190;
    config.cluster.gossip_interval = std::chrono::milliseconds(1000);
    config.cluster.gossip_heartbeat_interval = std::chrono::seconds(5);
    config.cluster.gossip_peer_timeout = std::chrono::seconds(15);
    config.cluster.tls.enabled = true;
    config.cluster.tls.cert_path = "/certs/node.crt";
    config.cluster.tls.key_path = "/certs/node.key";
    config.cluster.tls.ca_path = "";  // Missing!

    auto error = RanvierConfig::validate(config);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("ca_path"), std::string::npos);
}

TEST_F(ConfigTest, ValidationPassesForValidGossipTlsConfig) {
    RanvierConfig config;
    config.cluster.enabled = true;
    config.cluster.gossip_port = 9190;
    config.cluster.gossip_interval = std::chrono::milliseconds(1000);
    config.cluster.gossip_heartbeat_interval = std::chrono::seconds(5);
    config.cluster.gossip_peer_timeout = std::chrono::seconds(15);
    config.cluster.tls.enabled = true;
    config.cluster.tls.cert_path = "/certs/node.crt";
    config.cluster.tls.key_path = "/certs/node.key";
    config.cluster.tls.ca_path = "/certs/ca.crt";

    auto error = RanvierConfig::validate(config);
    EXPECT_FALSE(error.has_value()) << "Unexpected error: " << error.value_or("");
}

TEST_F(ConfigTest, ValidationPassesForDisabledGossipTls) {
    RanvierConfig config;
    config.cluster.enabled = true;
    config.cluster.gossip_port = 9190;
    config.cluster.gossip_interval = std::chrono::milliseconds(1000);
    config.cluster.gossip_heartbeat_interval = std::chrono::seconds(5);
    config.cluster.gossip_peer_timeout = std::chrono::seconds(15);
    config.cluster.tls.enabled = false;  // Disabled, so paths not required
    config.cluster.tls.cert_path = "";
    config.cluster.tls.key_path = "";
    config.cluster.tls.ca_path = "";

    auto error = RanvierConfig::validate(config);
    EXPECT_FALSE(error.has_value()) << "Unexpected error: " << error.value_or("");
}

TEST_F(ConfigTest, ValidationPassesForZeroCertReloadInterval) {
    RanvierConfig config;
    config.cluster.enabled = true;
    config.cluster.gossip_port = 9190;
    config.cluster.gossip_interval = std::chrono::milliseconds(1000);
    config.cluster.gossip_heartbeat_interval = std::chrono::seconds(5);
    config.cluster.gossip_peer_timeout = std::chrono::seconds(15);
    config.cluster.tls.enabled = true;
    config.cluster.tls.cert_path = "/certs/node.crt";
    config.cluster.tls.key_path = "/certs/node.key";
    config.cluster.tls.ca_path = "/certs/ca.crt";
    config.cluster.tls.cert_reload_interval = std::chrono::seconds(0);  // Disabled reload

    auto error = RanvierConfig::validate(config);
    EXPECT_FALSE(error.has_value()) << "Unexpected error: " << error.value_or("");
}

// =============================================================================
// Backpressure Configuration Validation Tests
// =============================================================================

TEST_F(ConfigTest, ValidationPassesForValidBackpressureConfig) {
    RanvierConfig config;
    config.backpressure.max_concurrent_requests = 1000;
    config.backpressure.enable_persistence_backpressure = true;
    config.backpressure.persistence_queue_threshold = 0.8;
    config.backpressure.retry_after_seconds = 1;

    auto error = RanvierConfig::validate(config);
    EXPECT_FALSE(error.has_value()) << "Unexpected error: " << error.value_or("");
}

TEST_F(ConfigTest, ValidationPassesForUnlimitedConcurrency) {
    RanvierConfig config;
    config.backpressure.max_concurrent_requests = 0;  // 0 means unlimited

    auto error = RanvierConfig::validate(config);
    EXPECT_FALSE(error.has_value()) << "Unexpected error: " << error.value_or("");
}

TEST_F(ConfigTest, ValidationFailsForNegativePersistenceThreshold) {
    RanvierConfig config;
    config.backpressure.persistence_queue_threshold = -0.1;

    auto error = RanvierConfig::validate(config);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("persistence_queue_threshold"), std::string::npos);
}

TEST_F(ConfigTest, ValidationFailsForThresholdGreaterThanOne) {
    RanvierConfig config;
    config.backpressure.persistence_queue_threshold = 1.5;

    auto error = RanvierConfig::validate(config);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("persistence_queue_threshold"), std::string::npos);
}

TEST_F(ConfigTest, ValidationFailsForZeroRetryAfter) {
    RanvierConfig config;
    config.backpressure.retry_after_seconds = 0;

    auto error = RanvierConfig::validate(config);
    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("retry_after_seconds"), std::string::npos);
}

TEST_F(ConfigTest, ValidationPassesForEdgeCaseThresholds) {
    // Test boundary values: 0.0 and 1.0 should be valid
    RanvierConfig config;
    config.backpressure.persistence_queue_threshold = 0.0;
    auto error = RanvierConfig::validate(config);
    EXPECT_FALSE(error.has_value()) << "Threshold 0.0 should be valid";

    config.backpressure.persistence_queue_threshold = 1.0;
    error = RanvierConfig::validate(config);
    EXPECT_FALSE(error.has_value()) << "Threshold 1.0 should be valid";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
