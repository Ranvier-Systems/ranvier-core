// Ranvier Core - Shard Load Balancer Unit Tests
//
// Tests for the shard-aware load balancer using the Power of Two Choices (P2C)
// algorithm for distributing requests across CPU cores.

#include "shard_load_metrics.hpp"
#include "shard_load_balancer.hpp"
#include "cross_shard_request.hpp"
#include <gtest/gtest.h>
#include <string>
#include <string_view>

using namespace ranvier;

// =============================================================================
// ShardLoadSnapshot Tests
// =============================================================================

class ShardLoadSnapshotTest : public ::testing::Test {
protected:
    ShardLoadSnapshot snapshot;
};

TEST_F(ShardLoadSnapshotTest, DefaultConstructorCreatesEmptySnapshot) {
    EXPECT_EQ(snapshot.shard_id, 0u);
    EXPECT_EQ(snapshot.active_requests, 0u);
    EXPECT_EQ(snapshot.queued_requests, 0u);
    EXPECT_EQ(snapshot.total_requests, 0u);
}

TEST_F(ShardLoadSnapshotTest, LoadScoreIsZeroForEmptySnapshot) {
    EXPECT_DOUBLE_EQ(snapshot.load_score(), 0.0);
}

TEST_F(ShardLoadSnapshotTest, LoadScoreWeightsActiveRequestsHigher) {
    // Active requests have weight 2.0, queued have weight 1.0
    snapshot.active_requests = 10;
    snapshot.queued_requests = 0;
    double active_only_score = snapshot.load_score();
    EXPECT_DOUBLE_EQ(active_only_score, 20.0);  // 10 * 2.0

    snapshot.active_requests = 0;
    snapshot.queued_requests = 10;
    double queued_only_score = snapshot.load_score();
    EXPECT_DOUBLE_EQ(queued_only_score, 10.0);  // 10 * 1.0

    // Active requests contribute more to load score
    EXPECT_GT(active_only_score, queued_only_score);
}

TEST_F(ShardLoadSnapshotTest, LoadScoreCombinesActiveAndQueued) {
    snapshot.active_requests = 5;
    snapshot.queued_requests = 3;
    // Score = 5 * 2.0 + 3 * 1.0 = 13.0
    EXPECT_DOUBLE_EQ(snapshot.load_score(), 13.0);
}

TEST_F(ShardLoadSnapshotTest, TotalRequestsDoesNotAffectLoadScore) {
    snapshot.active_requests = 5;
    snapshot.total_requests = 1000000;  // Should not affect score
    EXPECT_DOUBLE_EQ(snapshot.load_score(), 10.0);  // 5 * 2.0
}

// =============================================================================
// ShardLoadBalancerConfig Tests
// =============================================================================

class ShardLoadBalancerConfigTest : public ::testing::Test {
protected:
    ShardLoadBalancerConfig config;
};

TEST_F(ShardLoadBalancerConfigTest, DefaultConfigIsValid) {
    EXPECT_TRUE(config.is_valid());
    EXPECT_TRUE(config.enabled);
    EXPECT_DOUBLE_EQ(config.min_load_difference, 0.2);
    EXPECT_EQ(config.local_processing_threshold, 10u);
    EXPECT_EQ(config.snapshot_refresh_interval_us, 1000u);
    EXPECT_FALSE(config.adaptive_mode);
}

TEST_F(ShardLoadBalancerConfigTest, MinLoadDifferenceValidRange) {
    config.min_load_difference = 0.0;
    EXPECT_TRUE(config.is_valid());

    config.min_load_difference = 0.5;
    EXPECT_TRUE(config.is_valid());

    config.min_load_difference = 1.0;
    EXPECT_TRUE(config.is_valid());
}

TEST_F(ShardLoadBalancerConfigTest, MinLoadDifferenceInvalidRange) {
    config.min_load_difference = -0.1;
    EXPECT_FALSE(config.is_valid());

    config.min_load_difference = 1.1;
    EXPECT_FALSE(config.is_valid());
}

// =============================================================================
// CrossShardRequestContext Tests
// =============================================================================

class CrossShardRequestContextTest : public ::testing::Test {
protected:
    CrossShardRequestContext ctx;
};

TEST_F(CrossShardRequestContextTest, DefaultConstructorCreatesEmptyContext) {
    EXPECT_TRUE(ctx.body_empty());
    EXPECT_EQ(ctx.body_size(), 0u);
    EXPECT_TRUE(ctx.request_id.empty());
    EXPECT_TRUE(ctx.client_ip.empty());
    EXPECT_FALSE(ctx.has_client_tokens);
    EXPECT_TRUE(ctx.client_tokens.empty());
}

TEST_F(CrossShardRequestContextTest, MoveConstructorWorks) {
    // Create temporary_buffer with test data
    std::string test_body = "test body";
    ctx.body = seastar::temporary_buffer<char>(test_body.data(), test_body.size());
    ctx.request_id = "req-123";
    ctx.client_ip = "192.168.1.1";

    CrossShardRequestContext moved(std::move(ctx));

    EXPECT_EQ(moved.body_view(), "test body");
    EXPECT_EQ(moved.request_id, "req-123");
    EXPECT_EQ(moved.client_ip, "192.168.1.1");
}

TEST_F(CrossShardRequestContextTest, MoveAssignmentWorks) {
    std::string test_body = "test body";
    ctx.body = seastar::temporary_buffer<char>(test_body.data(), test_body.size());
    ctx.request_id = "req-456";

    CrossShardRequestContext target;
    target = std::move(ctx);

    EXPECT_EQ(target.body_view(), "test body");
    EXPECT_EQ(target.request_id, "req-456");
}

TEST_F(CrossShardRequestContextTest, TokensCanBeSet) {
    ctx.client_tokens = {1, 2, 3, 4, 5};
    ctx.has_client_tokens = true;

    EXPECT_TRUE(ctx.has_client_tokens);
    EXPECT_EQ(ctx.client_tokens.size(), 5u);
    EXPECT_EQ(ctx.client_tokens[0], 1);
    EXPECT_EQ(ctx.client_tokens[4], 5);
}

TEST_F(CrossShardRequestContextTest, BodyViewReturnsCorrectStringView) {
    std::string test_data = "Hello, World!";
    ctx.body = seastar::temporary_buffer<char>(test_data.data(), test_data.size());

    std::string_view view = ctx.body_view();
    EXPECT_EQ(view, "Hello, World!");
    EXPECT_EQ(view.size(), 13u);
}

TEST_F(CrossShardRequestContextTest, FromBufferFactoryMethod) {
    std::string test_body = "request payload";
    auto buffer = seastar::temporary_buffer<char>(test_body.data(), test_body.size());

    auto ctx2 = CrossShardRequestContext::from_buffer(
        std::move(buffer),
        "req-789",
        "10.0.0.1",
        "POST",
        "/v1/chat/completions",
        "00-trace-span-01"
    );

    EXPECT_EQ(ctx2.body_view(), "request payload");
    EXPECT_EQ(ctx2.request_id, "req-789");
    EXPECT_EQ(ctx2.client_ip, "10.0.0.1");
    EXPECT_EQ(ctx2.method, "POST");
    EXPECT_EQ(ctx2.path, "/v1/chat/completions");
    EXPECT_EQ(ctx2.traceparent, "00-trace-span-01");
}

TEST_F(CrossShardRequestContextTest, ForceLocalAllocationPreservesAllFields) {
    // Setup context with all fields populated
    std::string test_body = "body data";
    ctx.body = seastar::temporary_buffer<char>(test_body.data(), test_body.size());
    ctx.request_id = "req-123";
    ctx.client_ip = "192.168.1.1";
    ctx.traceparent = "00-trace-span-01";
    ctx.method = "POST";
    ctx.path = "/api/v1/completions";
    ctx.client_tokens = {1, 2, 3, 4, 5};
    ctx.has_client_tokens = true;
    ctx.origin_shard = 7;

    // force_local_allocation() is an rvalue-ref qualified method
    // In real cross-shard scenarios, this creates fresh heap allocations
    // on the current shard. Here we verify data preservation.
    auto local = std::move(ctx).force_local_allocation();

    EXPECT_EQ(local.body_view(), "body data");
    EXPECT_EQ(local.request_id, "req-123");
    EXPECT_EQ(local.client_ip, "192.168.1.1");
    EXPECT_EQ(local.traceparent, "00-trace-span-01");
    EXPECT_EQ(local.method, "POST");
    EXPECT_EQ(local.path, "/api/v1/completions");
    EXPECT_EQ(local.client_tokens, std::vector<int32_t>({1, 2, 3, 4, 5}));
    EXPECT_TRUE(local.has_client_tokens);
    EXPECT_EQ(local.origin_shard, 7u);
}

// =============================================================================
// CrossShardResult Tests
// =============================================================================

class CrossShardResultTest : public ::testing::Test {};

TEST_F(CrossShardResultTest, DefaultConstructorCreatesSuccessResult) {
    CrossShardResult result;

    EXPECT_EQ(result.status_code, 200);
    EXPECT_TRUE(result.body.empty());
    EXPECT_FALSE(result.is_streaming);
    EXPECT_FALSE(result.has_error);
}

TEST_F(CrossShardResultTest, SuccessFactoryMethod) {
    auto result = CrossShardResult::success("response body");

    EXPECT_EQ(result.status_code, 200);
    EXPECT_EQ(result.body, "response body");
    EXPECT_FALSE(result.has_error);
}

TEST_F(CrossShardResultTest, ErrorFactoryMethod) {
    auto result = CrossShardResult::error(503, "Service unavailable");

    EXPECT_EQ(result.status_code, 503);
    EXPECT_TRUE(result.has_error);
    EXPECT_EQ(result.error_message, "Service unavailable");
}

TEST_F(CrossShardResultTest, StreamingFactoryMethod) {
    auto result = CrossShardResult::streaming();

    EXPECT_TRUE(result.is_streaming);
    EXPECT_EQ(result.status_code, 200);
}

// =============================================================================
// P2C Algorithm Logic Tests (without Seastar reactor)
// =============================================================================

class P2CAlgorithmTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create snapshots with different loads
        low_load.shard_id = 0;
        low_load.active_requests = 2;
        low_load.queued_requests = 0;

        medium_load.shard_id = 1;
        medium_load.active_requests = 10;
        medium_load.queued_requests = 5;

        high_load.shard_id = 2;
        high_load.active_requests = 50;
        high_load.queued_requests = 20;
    }

    ShardLoadSnapshot low_load;
    ShardLoadSnapshot medium_load;
    ShardLoadSnapshot high_load;
};

TEST_F(P2CAlgorithmTest, LoadScoresAreOrdered) {
    EXPECT_LT(low_load.load_score(), medium_load.load_score());
    EXPECT_LT(medium_load.load_score(), high_load.load_score());
}

TEST_F(P2CAlgorithmTest, P2CWouldSelectLowerLoad) {
    // Simulate P2C selection between low and high load
    double score_low = low_load.load_score();
    double score_high = high_load.load_score();

    // P2C selects the shard with lower score
    uint32_t selected = (score_low <= score_high) ? low_load.shard_id : high_load.shard_id;
    EXPECT_EQ(selected, low_load.shard_id);
}

TEST_F(P2CAlgorithmTest, MinLoadDifferenceThreshold) {
    // Test that marginal differences don't trigger cross-shard
    ShardLoadSnapshot local;
    local.active_requests = 10;

    ShardLoadSnapshot candidate;
    candidate.active_requests = 9;  // Only 10% less load

    double local_score = local.load_score();
    double candidate_score = candidate.load_score();
    double diff_ratio = (local_score - candidate_score) / local_score;

    // With min_load_difference = 0.2 (20%), this shouldn't trigger dispatch
    double min_load_difference = 0.2;
    bool should_dispatch = diff_ratio >= min_load_difference;
    EXPECT_FALSE(should_dispatch);

    // With a bigger difference
    candidate.active_requests = 5;  // 50% less load
    candidate_score = candidate.load_score();
    diff_ratio = (local_score - candidate_score) / local_score;
    should_dispatch = diff_ratio >= min_load_difference;
    EXPECT_TRUE(should_dispatch);
}

// =============================================================================
// Type Traits Tests
// =============================================================================

TEST(TypeTraitsTest, ShardLoadSnapshotIsCopyable) {
    EXPECT_TRUE(std::is_copy_constructible_v<ShardLoadSnapshot>);
    EXPECT_TRUE(std::is_copy_assignable_v<ShardLoadSnapshot>);
}

TEST(TypeTraitsTest, ShardLoadSnapshotIsMovable) {
    EXPECT_TRUE(std::is_move_constructible_v<ShardLoadSnapshot>);
    EXPECT_TRUE(std::is_move_assignable_v<ShardLoadSnapshot>);
}

TEST(TypeTraitsTest, CrossShardRequestContextIsNotCopyable) {
    EXPECT_FALSE(std::is_copy_constructible_v<CrossShardRequestContext>);
    EXPECT_FALSE(std::is_copy_assignable_v<CrossShardRequestContext>);
}

TEST(TypeTraitsTest, CrossShardRequestContextIsMovable) {
    EXPECT_TRUE(std::is_move_constructible_v<CrossShardRequestContext>);
    EXPECT_TRUE(std::is_move_assignable_v<CrossShardRequestContext>);
}

TEST(TypeTraitsTest, ShardLoadBalancerConfigIsValid) {
    EXPECT_TRUE(std::is_default_constructible_v<ShardLoadBalancerConfig>);
    EXPECT_TRUE(std::is_copy_constructible_v<ShardLoadBalancerConfig>);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
