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
    }

    void TearDown() override {
        // Clean up test files
        std::remove("test_config.yaml");
        std::remove("test_partial.yaml");
        std::remove("test_invalid.yaml");
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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
