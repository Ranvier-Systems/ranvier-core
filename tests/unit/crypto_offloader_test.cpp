// Ranvier Core - CryptoOffloader Unit Tests
//
// Tests for the adaptive crypto offloading system.
// Note: These tests run without the Seastar event loop, so they test
// the configuration, decision logic, and stats - not the actual async execution.

#include "crypto_offloader.hpp"

#include <gtest/gtest.h>
#include <chrono>
#include <thread>

namespace ranvier {
namespace {

//------------------------------------------------------------------------------
// CryptoOpType Tests
//------------------------------------------------------------------------------

TEST(CryptoOpTypeTest, NameConversion) {
    EXPECT_STREQ(crypto_op_type_name(CryptoOpType::SYMMETRIC_ENCRYPT), "symmetric_encrypt");
    EXPECT_STREQ(crypto_op_type_name(CryptoOpType::SYMMETRIC_DECRYPT), "symmetric_decrypt");
    EXPECT_STREQ(crypto_op_type_name(CryptoOpType::HANDSHAKE_INITIATE), "handshake_initiate");
    EXPECT_STREQ(crypto_op_type_name(CryptoOpType::HANDSHAKE_CONTINUE), "handshake_continue");
    EXPECT_STREQ(crypto_op_type_name(CryptoOpType::UNKNOWN), "unknown");
}

//------------------------------------------------------------------------------
// CryptoOffloaderConfig Tests
//------------------------------------------------------------------------------

TEST(CryptoOffloaderConfigTest, DefaultValues) {
    CryptoOffloaderConfig config;

    EXPECT_EQ(config.size_threshold_bytes, 1024);
    EXPECT_EQ(config.stall_threshold_us, 500);
    EXPECT_EQ(config.offload_latency_threshold_us, 100);
    EXPECT_EQ(config.max_queue_depth, 1024);
    EXPECT_TRUE(config.enabled);
    EXPECT_FALSE(config.force_offload);
    EXPECT_TRUE(config.symmetric_always_inline);
    EXPECT_TRUE(config.handshake_always_offload);
}

//------------------------------------------------------------------------------
// CryptoOpStats Tests
//------------------------------------------------------------------------------

TEST(CryptoOpStatsTest, DefaultValues) {
    CryptoOpStats stats;

    EXPECT_EQ(stats.total_ops, 0);
    EXPECT_EQ(stats.offloaded_ops, 0);
    EXPECT_EQ(stats.inline_ops, 0);
    EXPECT_EQ(stats.stalls_avoided, 0);
    EXPECT_EQ(stats.stall_warnings, 0);
    EXPECT_EQ(stats.handshakes_offloaded, 0);
    EXPECT_EQ(stats.symmetric_ops_inline, 0);
    EXPECT_EQ(stats.queue_depth, 0);
    EXPECT_EQ(stats.queue_peak, 0);
    EXPECT_EQ(stats.queue_rejected, 0);
}

//------------------------------------------------------------------------------
// CryptoOffloader Construction Tests
//------------------------------------------------------------------------------

TEST(CryptoOffloaderTest, ConstructWithDefaultConfig) {
    CryptoOffloader offloader;

    EXPECT_FALSE(offloader.is_running());
    EXPECT_EQ(offloader.queue_depth(), 0);

    auto stats = offloader.get_stats();
    EXPECT_EQ(stats.total_ops, 0);
}

TEST(CryptoOffloaderTest, ConstructWithCustomConfig) {
    CryptoOffloaderConfig config;
    config.size_threshold_bytes = 2048;
    config.stall_threshold_us = 1000;
    config.enabled = false;

    CryptoOffloader offloader(config);

    EXPECT_FALSE(offloader.is_running());
    EXPECT_EQ(offloader.config().size_threshold_bytes, 2048);
    EXPECT_EQ(offloader.config().stall_threshold_us, 1000);
    EXPECT_FALSE(offloader.config().enabled);
}

//------------------------------------------------------------------------------
// Start/Stop Tests
//------------------------------------------------------------------------------

TEST(CryptoOffloaderTest, StartWhenEnabled) {
    CryptoOffloaderConfig config;
    config.enabled = true;

    CryptoOffloader offloader(config);

    EXPECT_FALSE(offloader.is_running());

    offloader.start();

    EXPECT_TRUE(offloader.is_running());

    offloader.stop();

    EXPECT_FALSE(offloader.is_running());
}

TEST(CryptoOffloaderTest, StartWhenDisabled) {
    CryptoOffloaderConfig config;
    config.enabled = false;

    CryptoOffloader offloader(config);

    offloader.start();

    // is_running() returns false when disabled, even after start()
    EXPECT_FALSE(offloader.is_running());
}

TEST(CryptoOffloaderTest, DoubleStartIsIdempotent) {
    CryptoOffloader offloader;

    offloader.start();
    EXPECT_TRUE(offloader.is_running());

    offloader.start();  // Should be a no-op
    EXPECT_TRUE(offloader.is_running());

    offloader.stop();
}

TEST(CryptoOffloaderTest, DoubleStopIsIdempotent) {
    CryptoOffloader offloader;

    offloader.start();
    offloader.stop();
    EXPECT_FALSE(offloader.is_running());

    offloader.stop();  // Should be a no-op
    EXPECT_FALSE(offloader.is_running());
}

//------------------------------------------------------------------------------
// should_offload() Decision Logic Tests
//------------------------------------------------------------------------------

class ShouldOffloadTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.enabled = true;
        config_.size_threshold_bytes = 1024;
        config_.offload_latency_threshold_us = 100;
        config_.symmetric_always_inline = true;
        config_.handshake_always_offload = true;
        config_.force_offload = false;
    }

    CryptoOffloaderConfig config_;
};

TEST_F(ShouldOffloadTest, DisabledNeverOffloads) {
    config_.enabled = false;
    CryptoOffloader offloader(config_);
    offloader.start();

    EXPECT_FALSE(offloader.should_offload(CryptoOpType::HANDSHAKE_INITIATE, 0));
    EXPECT_FALSE(offloader.should_offload(CryptoOpType::SYMMETRIC_ENCRYPT, 10000));
}

TEST_F(ShouldOffloadTest, NotRunningNeverOffloads) {
    CryptoOffloader offloader(config_);
    // Don't call start()

    EXPECT_FALSE(offloader.should_offload(CryptoOpType::HANDSHAKE_INITIATE, 0));
}

TEST_F(ShouldOffloadTest, ForceOffloadAlwaysOffloads) {
    config_.force_offload = true;
    CryptoOffloader offloader(config_);
    offloader.start();

    EXPECT_TRUE(offloader.should_offload(CryptoOpType::SYMMETRIC_ENCRYPT, 100));
    EXPECT_TRUE(offloader.should_offload(CryptoOpType::SYMMETRIC_DECRYPT, 100));
    EXPECT_TRUE(offloader.should_offload(CryptoOpType::UNKNOWN, 100));

    offloader.stop();
}

TEST_F(ShouldOffloadTest, HandshakesAlwaysOffload) {
    CryptoOffloader offloader(config_);
    offloader.start();

    EXPECT_TRUE(offloader.should_offload(CryptoOpType::HANDSHAKE_INITIATE, 0));
    EXPECT_TRUE(offloader.should_offload(CryptoOpType::HANDSHAKE_CONTINUE, 0));
    EXPECT_TRUE(offloader.should_offload(CryptoOpType::HANDSHAKE_INITIATE, 100));
    EXPECT_TRUE(offloader.should_offload(CryptoOpType::HANDSHAKE_CONTINUE, 100));

    offloader.stop();
}

TEST_F(ShouldOffloadTest, HandshakeOffloadCanBeDisabled) {
    config_.handshake_always_offload = false;
    CryptoOffloader offloader(config_);
    offloader.start();

    // With handshake_always_offload=false, it falls through to latency estimation
    // Handshakes have high estimated latency (2000us, 500us) so should still offload
    EXPECT_TRUE(offloader.should_offload(CryptoOpType::HANDSHAKE_INITIATE, 0));
    EXPECT_TRUE(offloader.should_offload(CryptoOpType::HANDSHAKE_CONTINUE, 0));

    offloader.stop();
}

TEST_F(ShouldOffloadTest, SmallSymmetricOpsInline) {
    CryptoOffloader offloader(config_);
    offloader.start();

    // Small symmetric ops (< 1KB) should run inline
    EXPECT_FALSE(offloader.should_offload(CryptoOpType::SYMMETRIC_ENCRYPT, 100));
    EXPECT_FALSE(offloader.should_offload(CryptoOpType::SYMMETRIC_DECRYPT, 512));
    EXPECT_FALSE(offloader.should_offload(CryptoOpType::SYMMETRIC_ENCRYPT, 1024));

    offloader.stop();
}

TEST_F(ShouldOffloadTest, LargeSymmetricOpsOffload) {
    CryptoOffloader offloader(config_);
    offloader.start();

    // Large symmetric ops (> 1KB) should offload based on latency estimation
    // For 100KB: estimated latency = 5 + 100 = 105us > 100us threshold
    EXPECT_TRUE(offloader.should_offload(CryptoOpType::SYMMETRIC_ENCRYPT, 100 * 1024));
    EXPECT_TRUE(offloader.should_offload(CryptoOpType::SYMMETRIC_DECRYPT, 100 * 1024));

    offloader.stop();
}

TEST_F(ShouldOffloadTest, SymmetricInlineCanBeDisabled) {
    config_.symmetric_always_inline = false;
    CryptoOffloader offloader(config_);
    offloader.start();

    // With symmetric_always_inline=false, even small ops go to latency estimation
    // Small data: estimated latency = 5 + 0 = 5us < 100us threshold
    EXPECT_FALSE(offloader.should_offload(CryptoOpType::SYMMETRIC_ENCRYPT, 100));

    offloader.stop();
}

TEST_F(ShouldOffloadTest, UnknownTypeUsesLatencyEstimation) {
    CryptoOffloader offloader(config_);
    offloader.start();

    // UNKNOWN type uses size-based heuristic: 10 + (data_size * 10 / 1024)
    // For 1KB: 10 + 10 = 20us < 100us threshold
    EXPECT_FALSE(offloader.should_offload(CryptoOpType::UNKNOWN, 1024));

    // For 100KB: 10 + 1000 = 1010us > 100us threshold
    EXPECT_TRUE(offloader.should_offload(CryptoOpType::UNKNOWN, 100 * 1024));

    offloader.stop();
}

//------------------------------------------------------------------------------
// Statistics Tests
//------------------------------------------------------------------------------

TEST(CryptoOffloaderStatsTest, InitialStatsAreZero) {
    CryptoOffloader offloader;
    offloader.start();

    auto stats = offloader.get_stats();
    EXPECT_EQ(stats.total_ops, 0);
    EXPECT_EQ(stats.offloaded_ops, 0);
    EXPECT_EQ(stats.inline_ops, 0);
    EXPECT_EQ(stats.stalls_avoided, 0);
    EXPECT_EQ(stats.stall_warnings, 0);
    EXPECT_EQ(stats.handshakes_offloaded, 0);
    EXPECT_EQ(stats.symmetric_ops_inline, 0);
    EXPECT_EQ(stats.queue_depth, 0);
    EXPECT_EQ(stats.queue_peak, 0);
    EXPECT_EQ(stats.queue_rejected, 0);

    offloader.stop();
}

TEST(CryptoOffloaderStatsTest, QueueDepthTracking) {
    CryptoOffloader offloader;
    offloader.start();

    EXPECT_EQ(offloader.queue_depth(), 0);

    auto stats = offloader.get_stats();
    EXPECT_EQ(stats.queue_depth, 0);
    EXPECT_EQ(stats.queue_peak, 0);

    offloader.stop();
}

//------------------------------------------------------------------------------
// Edge Cases
//------------------------------------------------------------------------------

TEST(CryptoOffloaderEdgeCasesTest, ZeroSizeData) {
    CryptoOffloaderConfig config;
    config.enabled = true;
    CryptoOffloader offloader(config);
    offloader.start();

    // Zero-size symmetric ops should run inline
    EXPECT_FALSE(offloader.should_offload(CryptoOpType::SYMMETRIC_ENCRYPT, 0));

    // Zero-size handshakes should still offload
    EXPECT_TRUE(offloader.should_offload(CryptoOpType::HANDSHAKE_INITIATE, 0));

    offloader.stop();
}

TEST(CryptoOffloaderEdgeCasesTest, VeryLargeData) {
    CryptoOffloaderConfig config;
    config.enabled = true;
    CryptoOffloader offloader(config);
    offloader.start();

    // Very large data should definitely offload
    size_t huge_size = 1024 * 1024 * 100;  // 100MB
    EXPECT_TRUE(offloader.should_offload(CryptoOpType::SYMMETRIC_ENCRYPT, huge_size));
    EXPECT_TRUE(offloader.should_offload(CryptoOpType::UNKNOWN, huge_size));

    offloader.stop();
}

TEST(CryptoOffloaderEdgeCasesTest, BoundaryThreshold) {
    CryptoOffloaderConfig config;
    config.enabled = true;
    config.size_threshold_bytes = 1024;
    config.symmetric_always_inline = true;
    CryptoOffloader offloader(config);
    offloader.start();

    // Exactly at threshold should be inline
    EXPECT_FALSE(offloader.should_offload(CryptoOpType::SYMMETRIC_ENCRYPT, 1024));

    // Just over threshold goes to latency estimation
    // 1025 bytes: latency = 5 + 1 = 6us < 100us threshold
    // So still inline (latency estimate is too low)
    EXPECT_FALSE(offloader.should_offload(CryptoOpType::SYMMETRIC_ENCRYPT, 1025));

    offloader.stop();
}

}  // namespace
}  // namespace ranvier
