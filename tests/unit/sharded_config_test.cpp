// Ranvier Core - Sharded Configuration Unit Tests
//
// Tests for the ShardedConfig wrapper class that enables
// per-core configuration distribution via seastar::sharded<>

#include "sharded_config.hpp"
#include <gtest/gtest.h>

using namespace ranvier;

class ShardedConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a test configuration
        test_config_ = RanvierConfig::defaults();
        test_config_.server.api_port = 9999;
        test_config_.server.bind_address = "127.0.0.1";
        test_config_.routing.min_token_length = 64;
    }

    RanvierConfig test_config_;
};

// =============================================================================
// Construction Tests
// =============================================================================

TEST_F(ShardedConfigTest, DefaultConstructorCreatesDefaultConfig) {
    ShardedConfig sharded;

    // Should have default config values
    const auto& cfg = sharded.config();
    EXPECT_EQ(cfg.server.api_port, 8080);  // Default value
    EXPECT_EQ(cfg.server.bind_address, "0.0.0.0");
}

TEST_F(ShardedConfigTest, ExplicitConstructorStoresConfig) {
    ShardedConfig sharded(test_config_);

    const auto& cfg = sharded.config();
    EXPECT_EQ(cfg.server.api_port, 9999);
    EXPECT_EQ(cfg.server.bind_address, "127.0.0.1");
    EXPECT_EQ(cfg.routing.min_token_length, 64u);
}

TEST_F(ShardedConfigTest, MoveConstructorWorks) {
    RanvierConfig config_copy = test_config_;
    ShardedConfig sharded(std::move(config_copy));

    const auto& cfg = sharded.config();
    EXPECT_EQ(cfg.server.api_port, 9999);
}

// =============================================================================
// Copy/Move Semantics Tests
// =============================================================================

TEST_F(ShardedConfigTest, CopyConstructorWorks) {
    ShardedConfig original(test_config_);
    ShardedConfig copy(original);

    EXPECT_EQ(copy.config().server.api_port, 9999);
    EXPECT_EQ(original.config().server.api_port, 9999);
}

TEST_F(ShardedConfigTest, CopyAssignmentWorks) {
    ShardedConfig original(test_config_);
    ShardedConfig copy;

    copy = original;

    EXPECT_EQ(copy.config().server.api_port, 9999);
}

TEST_F(ShardedConfigTest, MoveAssignmentWorks) {
    ShardedConfig original(test_config_);
    ShardedConfig target;

    target = std::move(original);

    EXPECT_EQ(target.config().server.api_port, 9999);
}

// =============================================================================
// Config Access Tests
// =============================================================================

TEST_F(ShardedConfigTest, ConfigAccessorReturnsConstReference) {
    ShardedConfig sharded(test_config_);

    const auto& cfg = sharded.config();

    // Verify it's the same config
    EXPECT_EQ(&cfg, &sharded.config());
}

// =============================================================================
// Update Tests
// =============================================================================

TEST_F(ShardedConfigTest, UpdateReplacesConfig) {
    ShardedConfig sharded(test_config_);
    EXPECT_EQ(sharded.config().server.api_port, 9999);

    // Create new config
    RanvierConfig new_config = RanvierConfig::defaults();
    new_config.server.api_port = 1234;
    new_config.routing.min_token_length = 128;

    // Update
    sharded.update(new_config);

    // Verify update
    EXPECT_EQ(sharded.config().server.api_port, 1234);
    EXPECT_EQ(sharded.config().routing.min_token_length, 128u);
}

TEST_F(ShardedConfigTest, UpdateWithMoveWorks) {
    ShardedConfig sharded(test_config_);

    RanvierConfig new_config = RanvierConfig::defaults();
    new_config.server.api_port = 5678;

    sharded.update(std::move(new_config));

    EXPECT_EQ(sharded.config().server.api_port, 5678);
}

TEST_F(ShardedConfigTest, UpdatePreservesAllFields) {
    ShardedConfig sharded;

    // Create comprehensive config
    RanvierConfig new_config;
    new_config.server.api_port = 9000;
    new_config.server.metrics_port = 9181;
    new_config.server.bind_address = "192.168.1.1";
    new_config.database.path = "/data/test.db";
    new_config.routing.min_token_length = 32;
    new_config.routing.backend_retry_limit = 10;
    new_config.pool.max_connections_per_host = 50;
    new_config.health.failure_threshold = 5;
    new_config.tls.enabled = true;
    new_config.tls.cert_path = "/certs/test.crt";
    new_config.rate_limit.enabled = true;
    new_config.rate_limit.requests_per_second = 500;

    sharded.update(new_config);

    const auto& cfg = sharded.config();
    EXPECT_EQ(cfg.server.api_port, 9000);
    EXPECT_EQ(cfg.server.metrics_port, 9181);
    EXPECT_EQ(cfg.server.bind_address, "192.168.1.1");
    EXPECT_EQ(cfg.database.path, "/data/test.db");
    EXPECT_EQ(cfg.routing.min_token_length, 32u);
    EXPECT_EQ(cfg.routing.backend_retry_limit, 10u);
    EXPECT_EQ(cfg.pool.max_connections_per_host, 50u);
    EXPECT_EQ(cfg.health.failure_threshold, 5u);
    EXPECT_TRUE(cfg.tls.enabled);
    EXPECT_EQ(cfg.tls.cert_path, "/certs/test.crt");
    EXPECT_TRUE(cfg.rate_limit.enabled);
    EXPECT_EQ(cfg.rate_limit.requests_per_second, 500u);
}

TEST_F(ShardedConfigTest, MultipleUpdatesWork) {
    ShardedConfig sharded;

    for (int i = 1; i <= 5; i++) {
        RanvierConfig cfg;
        cfg.server.api_port = static_cast<uint16_t>(8000 + i);
        sharded.update(cfg);
        EXPECT_EQ(sharded.config().server.api_port, 8000 + i);
    }
}

// =============================================================================
// Stop Method Tests (Seastar compatibility)
// =============================================================================

// Note: The stop() method requires Seastar reactor to run.
// It's tested indirectly in integration tests.
// Here we just verify it compiles and is callable.
TEST_F(ShardedConfigTest, StopMethodExists) {
    ShardedConfig sharded;

    // Verify stop() is callable (returns seastar::future<>)
    // We can't actually call it without a Seastar reactor
    (void)&ShardedConfig::stop;  // Just verify the method exists
}

// =============================================================================
// Noexcept Tests
// =============================================================================

TEST_F(ShardedConfigTest, MoveConstructorIsNoexcept) {
    EXPECT_TRUE(std::is_nothrow_move_constructible_v<ShardedConfig>);
}

TEST_F(ShardedConfigTest, MoveAssignmentIsNoexcept) {
    EXPECT_TRUE(std::is_nothrow_move_assignable_v<ShardedConfig>);
}

TEST_F(ShardedConfigTest, ConfigAccessorIsNoexcept) {
    ShardedConfig sharded;
    EXPECT_TRUE(noexcept(sharded.config()));
}

// =============================================================================
// Nodiscard Tests
// =============================================================================

TEST_F(ShardedConfigTest, ConfigAccessorReturnsValue) {
    ShardedConfig sharded(test_config_);

    // [[nodiscard]] means the return value should be used
    // This test verifies we can capture and use the result
    const RanvierConfig& cfg = sharded.config();
    EXPECT_EQ(cfg.server.api_port, 9999);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
