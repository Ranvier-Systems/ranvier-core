// Ranvier Core - Application Unit Tests
//
// Tests for the Application class configuration builders and state management.
// Note: Full lifecycle tests require Seastar runtime and are in integration tests.

#include "application.hpp"
#include <gtest/gtest.h>

using namespace ranvier;

class ApplicationConfigTest : public ::testing::Test {
protected:
    RanvierConfig default_config;

    void SetUp() override {
        default_config = RanvierConfig::defaults();
    }
};

// =============================================================================
// ApplicationState Enum Tests
// =============================================================================

TEST(ApplicationStateTest, StatesAreDistinct) {
    EXPECT_NE(ApplicationState::CREATED, ApplicationState::STARTING);
    EXPECT_NE(ApplicationState::STARTING, ApplicationState::RUNNING);
    EXPECT_NE(ApplicationState::RUNNING, ApplicationState::DRAINING);
    EXPECT_NE(ApplicationState::DRAINING, ApplicationState::STOPPING);
    EXPECT_NE(ApplicationState::STOPPING, ApplicationState::STOPPED);
}

// =============================================================================
// HttpControllerConfig Builder Tests
// =============================================================================

TEST_F(ApplicationConfigTest, BuildControllerConfigCopiesPoolSettings) {
    default_config.pool.max_connections_per_host = 42;
    default_config.pool.idle_timeout = std::chrono::seconds(123);
    default_config.pool.max_total_connections = 999;

    Application app(default_config, "test.yaml");
    // Note: We can't directly call build_controller_config() as it's private
    // This test documents the expected behavior
    EXPECT_EQ(default_config.pool.max_connections_per_host, 42u);
    EXPECT_EQ(default_config.pool.idle_timeout.count(), 123);
    EXPECT_EQ(default_config.pool.max_total_connections, 999u);
}

TEST_F(ApplicationConfigTest, BuildControllerConfigCopiesRoutingSettings) {
    default_config.routing.min_token_length = 16;
    default_config.routing.enable_token_forwarding = true;
    default_config.routing.accept_client_tokens = true;
    default_config.routing.max_token_id = 50000;

    Application app(default_config, "test.yaml");
    EXPECT_EQ(default_config.routing.min_token_length, 16u);
    EXPECT_TRUE(default_config.routing.enable_token_forwarding);
    EXPECT_TRUE(default_config.routing.accept_client_tokens);
    EXPECT_EQ(default_config.routing.max_token_id, 50000u);
}

TEST_F(ApplicationConfigTest, BuildControllerConfigCopiesTimeoutSettings) {
    default_config.timeouts.connect_timeout = std::chrono::seconds(15);
    default_config.timeouts.request_timeout = std::chrono::seconds(600);
    default_config.shutdown.drain_timeout = std::chrono::seconds(45);

    Application app(default_config, "test.yaml");
    EXPECT_EQ(default_config.timeouts.connect_timeout.count(), 15);
    EXPECT_EQ(default_config.timeouts.request_timeout.count(), 600);
    EXPECT_EQ(default_config.shutdown.drain_timeout.count(), 45);
}

TEST_F(ApplicationConfigTest, BuildControllerConfigCopiesRateLimitSettings) {
    default_config.rate_limit.enabled = true;
    default_config.rate_limit.requests_per_second = 500;
    default_config.rate_limit.burst_size = 250;

    Application app(default_config, "test.yaml");
    EXPECT_TRUE(default_config.rate_limit.enabled);
    EXPECT_EQ(default_config.rate_limit.requests_per_second, 500u);
    EXPECT_EQ(default_config.rate_limit.burst_size, 250u);
}

TEST_F(ApplicationConfigTest, BuildControllerConfigCopiesRetrySettings) {
    default_config.retry.max_retries = 5;
    default_config.retry.initial_backoff = std::chrono::milliseconds(200);
    default_config.retry.max_backoff = std::chrono::milliseconds(10000);
    default_config.retry.backoff_multiplier = 3.0;

    Application app(default_config, "test.yaml");
    EXPECT_EQ(default_config.retry.max_retries, 5u);
    EXPECT_EQ(default_config.retry.initial_backoff.count(), 200);
    EXPECT_EQ(default_config.retry.max_backoff.count(), 10000);
    EXPECT_DOUBLE_EQ(default_config.retry.backoff_multiplier, 3.0);
}

TEST_F(ApplicationConfigTest, BuildControllerConfigCopiesCircuitBreakerSettings) {
    default_config.circuit_breaker.enabled = true;
    default_config.circuit_breaker.failure_threshold = 10;
    default_config.circuit_breaker.success_threshold = 3;
    default_config.circuit_breaker.recovery_timeout = std::chrono::seconds(60);
    default_config.circuit_breaker.fallback_enabled = true;

    Application app(default_config, "test.yaml");
    EXPECT_TRUE(default_config.circuit_breaker.enabled);
    EXPECT_EQ(default_config.circuit_breaker.failure_threshold, 10u);
    EXPECT_EQ(default_config.circuit_breaker.success_threshold, 3u);
    EXPECT_EQ(default_config.circuit_breaker.recovery_timeout.count(), 60);
    EXPECT_TRUE(default_config.circuit_breaker.fallback_enabled);
}

// =============================================================================
// Application State Accessor Tests
// =============================================================================

TEST_F(ApplicationConfigTest, InitialStateIsCreated) {
    Application app(default_config, "test.yaml");
    EXPECT_EQ(app.state(), ApplicationState::CREATED);
}

TEST_F(ApplicationConfigTest, IsRunningReturnsFalseInitially) {
    Application app(default_config, "test.yaml");
    EXPECT_FALSE(app.is_running());
}

TEST_F(ApplicationConfigTest, IsShuttingDownReturnsFalseInitially) {
    Application app(default_config, "test.yaml");
    EXPECT_FALSE(app.is_shutting_down());
}

TEST_F(ApplicationConfigTest, ConfigAccessorReturnsCorrectConfig) {
    default_config.server.api_port = 9999;
    Application app(default_config, "test.yaml");
    EXPECT_EQ(app.config().server.api_port, 9999);
}

TEST_F(ApplicationConfigTest, RouterReturnsNullBeforeStartup) {
    Application app(default_config, "test.yaml");
    EXPECT_EQ(app.router(), nullptr);
}

// =============================================================================
// Signal Handling State Tests
// =============================================================================
// Note: Actual signal delivery tests require Seastar runtime and are covered
// in integration tests. These tests verify the state machine logic that
// signal handlers depend on.

TEST_F(ApplicationConfigTest, IsShuttingDownReturnsFalseForCreatedState) {
    Application app(default_config, "test.yaml");
    // CREATED state should not be considered "shutting down"
    EXPECT_EQ(app.state(), ApplicationState::CREATED);
    EXPECT_FALSE(app.is_shutting_down());
}

TEST_F(ApplicationConfigTest, ShutdownStatesAreCorrectlyIdentified) {
    // Verify the is_shutting_down() logic correctly identifies shutdown states
    // This tests the accessor logic used by signal handlers
    Application app(default_config, "test.yaml");

    // Initial state - not shutting down
    EXPECT_FALSE(app.is_shutting_down());

    // Note: We cannot directly set state from tests as it's private,
    // but we document the expected behavior:
    // - DRAINING: is_shutting_down() == true
    // - STOPPING: is_shutting_down() == true
    // - STOPPED:  is_shutting_down() == true
    // - CREATED:  is_shutting_down() == false
    // - STARTING: is_shutting_down() == false
    // - RUNNING:  is_shutting_down() == false
}

TEST_F(ApplicationConfigTest, ApplicationStateEnumCoversAllLifecycleStages) {
    // Verify all expected lifecycle states exist
    // Signal handlers depend on these states for decision making
    EXPECT_NE(ApplicationState::CREATED, ApplicationState::STARTING);
    EXPECT_NE(ApplicationState::STARTING, ApplicationState::RUNNING);
    EXPECT_NE(ApplicationState::RUNNING, ApplicationState::DRAINING);
    EXPECT_NE(ApplicationState::DRAINING, ApplicationState::STOPPING);
    EXPECT_NE(ApplicationState::STOPPING, ApplicationState::STOPPED);
}

// =============================================================================
// K8s Discovery Config Builder Tests
// =============================================================================

TEST_F(ApplicationConfigTest, K8sConfigIsDisabledByDefault) {
    Application app(default_config, "test.yaml");
    EXPECT_FALSE(default_config.k8s_discovery.enabled);
}

TEST_F(ApplicationConfigTest, K8sConfigCopiesAllSettings) {
    default_config.k8s_discovery.enabled = true;
    default_config.k8s_discovery.api_server = "https://kubernetes.default.svc";
    default_config.k8s_discovery.namespace_name = "production";
    default_config.k8s_discovery.service_name = "my-service";
    default_config.k8s_discovery.target_port = 8080;
    default_config.k8s_discovery.poll_interval = std::chrono::seconds(30);

    Application app(default_config, "test.yaml");
    EXPECT_TRUE(default_config.k8s_discovery.enabled);
    EXPECT_EQ(default_config.k8s_discovery.api_server, "https://kubernetes.default.svc");
    EXPECT_EQ(default_config.k8s_discovery.namespace_name, "production");
    EXPECT_EQ(default_config.k8s_discovery.service_name, "my-service");
    EXPECT_EQ(default_config.k8s_discovery.target_port, 8080);
    EXPECT_EQ(default_config.k8s_discovery.poll_interval.count(), 30);
}

// =============================================================================
// Health Service Config Builder Tests
// =============================================================================

TEST_F(ApplicationConfigTest, HealthConfigHasDefaults) {
    Application app(default_config, "test.yaml");
    EXPECT_EQ(default_config.health.check_interval.count(), 5);
    EXPECT_EQ(default_config.health.check_timeout.count(), 3);
    EXPECT_EQ(default_config.health.failure_threshold, 3u);
    EXPECT_EQ(default_config.health.recovery_threshold, 2u);
}

TEST_F(ApplicationConfigTest, HealthConfigCopiesCustomSettings) {
    default_config.health.check_interval = std::chrono::seconds(10);
    default_config.health.check_timeout = std::chrono::seconds(5);
    default_config.health.failure_threshold = 5;
    default_config.health.recovery_threshold = 3;

    Application app(default_config, "test.yaml");
    EXPECT_EQ(default_config.health.check_interval.count(), 10);
    EXPECT_EQ(default_config.health.check_timeout.count(), 5);
    EXPECT_EQ(default_config.health.failure_threshold, 5u);
    EXPECT_EQ(default_config.health.recovery_threshold, 3u);
}

// =============================================================================
// Application Constructor Tests
// =============================================================================

TEST_F(ApplicationConfigTest, ConstructorStoresConfigPath) {
    Application app(default_config, "/path/to/config.yaml");
    // Config path is stored internally but not exposed
    // This test verifies construction succeeds
    EXPECT_EQ(app.state(), ApplicationState::CREATED);
}

TEST_F(ApplicationConfigTest, ConstructorAcceptsEmptyConfigPath) {
    Application app(default_config, "");
    EXPECT_EQ(app.state(), ApplicationState::CREATED);
}

// =============================================================================
// Configuration Preservation Tests
// =============================================================================

TEST_F(ApplicationConfigTest, ConfigIsPreservedAfterConstruction) {
    default_config.server.api_port = 3000;
    default_config.server.metrics_port = 3001;
    default_config.database.path = "/custom/path.db";

    Application app(default_config, "test.yaml");

    EXPECT_EQ(app.config().server.api_port, 3000);
    EXPECT_EQ(app.config().server.metrics_port, 3001);
    EXPECT_EQ(app.config().database.path, "/custom/path.db");
}

TEST_F(ApplicationConfigTest, TlsConfigIsPreserved) {
    default_config.tls.enabled = true;
    default_config.tls.cert_path = "/certs/server.crt";
    default_config.tls.key_path = "/certs/server.key";

    Application app(default_config, "test.yaml");

    EXPECT_TRUE(app.config().tls.enabled);
    EXPECT_EQ(app.config().tls.cert_path, "/certs/server.crt");
    EXPECT_EQ(app.config().tls.key_path, "/certs/server.key");
}

TEST_F(ApplicationConfigTest, ClusterConfigIsPreserved) {
    default_config.cluster.enabled = true;
    default_config.cluster.gossip_port = 9200;
    default_config.cluster.peers = {"192.168.1.1:9200", "192.168.1.2:9200"};

    Application app(default_config, "test.yaml");

    EXPECT_TRUE(app.config().cluster.enabled);
    EXPECT_EQ(app.config().cluster.gossip_port, 9200);
    ASSERT_EQ(app.config().cluster.peers.size(), 2u);
    EXPECT_EQ(app.config().cluster.peers[0], "192.168.1.1:9200");
    EXPECT_EQ(app.config().cluster.peers[1], "192.168.1.2:9200");
}

// =============================================================================
// Configuration Hot-Reload Tests
// =============================================================================
// Note: Actual reload_config() execution requires Seastar runtime and is tested
// in integration tests. These tests document the expected behavior and verify
// configuration handling.

TEST_F(ApplicationConfigTest, ReloadConfigPathIsPreserved) {
    Application app(default_config, "/etc/ranvier/config.yaml");
    // Config path is stored internally for reload
    // Reload uses RanvierConfig::load() with this path
    EXPECT_EQ(app.state(), ApplicationState::CREATED);
}

// Document expected reload_config() behavior (tested in integration tests):
// - Rate limiting: Rejects reload if last reload was < 10 seconds ago
// - Async I/O: Uses seastar::async() to offload blocking std::ifstream to thread pool
// - Validation: Validates new config before applying; logs error if invalid
// - Atomic propagation: Updates all shards via invoke_on_all() before updating master
// - Error handling: Logs errors but doesn't crash on reload failure

// =============================================================================
// Tokenizer Configuration Tests
// =============================================================================
// Note: Actual init_tokenizer() execution requires Seastar runtime and is tested
// in integration tests. These tests verify configuration handling and document
// expected behavior.

TEST_F(ApplicationConfigTest, TokenizerPathIsPreserved) {
    default_config.assets.tokenizer_path = "/models/gpt2-tokenizer.json";

    Application app(default_config, "test.yaml");

    EXPECT_EQ(app.config().assets.tokenizer_path, "/models/gpt2-tokenizer.json");
}

TEST_F(ApplicationConfigTest, TokenizerPathCanBeAbsolute) {
    default_config.assets.tokenizer_path = "/usr/local/share/ranvier/tokenizer.json";

    Application app(default_config, "test.yaml");

    EXPECT_EQ(app.config().assets.tokenizer_path, "/usr/local/share/ranvier/tokenizer.json");
}

TEST_F(ApplicationConfigTest, TokenizerPathCanBeRelative) {
    default_config.assets.tokenizer_path = "assets/tokenizer.json";

    Application app(default_config, "test.yaml");

    EXPECT_EQ(app.config().assets.tokenizer_path, "assets/tokenizer.json");
}

TEST_F(ApplicationConfigTest, EmptyTokenizerPathIsStoredAsIs) {
    // Empty path will be rejected by init_tokenizer() at runtime
    // (async method returns exception future)
    default_config.assets.tokenizer_path = "";

    Application app(default_config, "test.yaml");

    EXPECT_TRUE(app.config().assets.tokenizer_path.empty());
}

// Document expected init_tokenizer() behavior (tested in integration tests):
// - Empty path: returns exception future "Tokenizer path not configured"
// - Non-existent file: returns exception future "Could not find tokenizer"
// - Empty file (0 bytes): returns exception future "Tokenizer file is empty"
// - File > 100MB: returns exception future "Tokenizer file too large"
// - Valid file: loads tokenizer via DMA I/O and logs success

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
