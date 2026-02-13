// Ranvier Core - Cross-Shard Request Unit Tests
//
// Tests for validation functions, size limits, force_local_allocation() copy
// behavior, and result factory methods. Uses stub headers for Seastar
// (tests/stubs/) to keep this a pure-C++ test.

#include "cross_shard_request.hpp"
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace ranvier;

// =============================================================================
// CrossShardRequestLimits Tests
// =============================================================================

class CrossShardRequestLimitsTest : public ::testing::Test {};

TEST_F(CrossShardRequestLimitsTest, BodySizeLimit) {
    EXPECT_EQ(CrossShardRequestLimits::max_body_size, 128u * 1024 * 1024);
}

TEST_F(CrossShardRequestLimitsTest, ClientTokensLimit) {
    EXPECT_EQ(CrossShardRequestLimits::max_client_tokens, 128u * 1024);
}

TEST_F(CrossShardRequestLimitsTest, StringFieldLimit) {
    EXPECT_EQ(CrossShardRequestLimits::max_string_field_length, 4096u);
}

TEST_F(CrossShardRequestLimitsTest, PathLengthLimit) {
    EXPECT_EQ(CrossShardRequestLimits::max_path_length, 8192u);
}

// =============================================================================
// CrossShardRequestContext Validation Tests
// =============================================================================

class CrossShardRequestValidationTest : public ::testing::Test {
protected:
    // Helper to create a context with a body of given size
    CrossShardRequestContext make_ctx_with_body(size_t body_size) {
        CrossShardRequestContext ctx;
        std::string data(body_size, 'x');
        ctx.body = seastar::temporary_buffer<char>(data.data(), data.size());
        return ctx;
    }
};

TEST_F(CrossShardRequestValidationTest, EmptyContextIsValid) {
    CrossShardRequestContext ctx;
    EXPECT_TRUE(ctx.is_valid());
}

TEST_F(CrossShardRequestValidationTest, BodyAtLimitIsValid) {
    auto ctx = make_ctx_with_body(CrossShardRequestLimits::max_body_size);
    EXPECT_TRUE(ctx.is_valid());
}

TEST_F(CrossShardRequestValidationTest, BodyOverLimitIsInvalid) {
    auto ctx = make_ctx_with_body(CrossShardRequestLimits::max_body_size + 1);
    EXPECT_FALSE(ctx.is_valid());
}

TEST_F(CrossShardRequestValidationTest, RequestIdAtLimitIsValid) {
    CrossShardRequestContext ctx;
    ctx.request_id = std::string(CrossShardRequestLimits::max_string_field_length, 'r');
    EXPECT_TRUE(ctx.is_valid());
}

TEST_F(CrossShardRequestValidationTest, RequestIdOverLimitIsInvalid) {
    CrossShardRequestContext ctx;
    ctx.request_id = std::string(CrossShardRequestLimits::max_string_field_length + 1, 'r');
    EXPECT_FALSE(ctx.is_valid());
}

TEST_F(CrossShardRequestValidationTest, ClientIpOverLimitIsInvalid) {
    CrossShardRequestContext ctx;
    ctx.client_ip = std::string(CrossShardRequestLimits::max_string_field_length + 1, 'i');
    EXPECT_FALSE(ctx.is_valid());
}

TEST_F(CrossShardRequestValidationTest, TraceparentOverLimitIsInvalid) {
    CrossShardRequestContext ctx;
    ctx.traceparent = std::string(CrossShardRequestLimits::max_string_field_length + 1, 't');
    EXPECT_FALSE(ctx.is_valid());
}

TEST_F(CrossShardRequestValidationTest, PathAtLimitIsValid) {
    CrossShardRequestContext ctx;
    ctx.path = std::string(CrossShardRequestLimits::max_path_length, '/');
    EXPECT_TRUE(ctx.is_valid());
}

TEST_F(CrossShardRequestValidationTest, PathOverLimitIsInvalid) {
    CrossShardRequestContext ctx;
    ctx.path = std::string(CrossShardRequestLimits::max_path_length + 1, '/');
    EXPECT_FALSE(ctx.is_valid());
}

TEST_F(CrossShardRequestValidationTest, TokenCountAtLimitIsValid) {
    CrossShardRequestContext ctx;
    ctx.client_tokens.resize(CrossShardRequestLimits::max_client_tokens);
    EXPECT_TRUE(ctx.is_valid());
}

TEST_F(CrossShardRequestValidationTest, TokenCountOverLimitIsInvalid) {
    CrossShardRequestContext ctx;
    ctx.client_tokens.resize(CrossShardRequestLimits::max_client_tokens + 1);
    EXPECT_FALSE(ctx.is_valid());
}

TEST_F(CrossShardRequestValidationTest, MultipleFieldsAtLimitIsValid) {
    CrossShardRequestContext ctx;
    ctx.request_id = std::string(CrossShardRequestLimits::max_string_field_length, 'a');
    ctx.client_ip = std::string(CrossShardRequestLimits::max_string_field_length, 'b');
    ctx.traceparent = std::string(CrossShardRequestLimits::max_string_field_length, 'c');
    ctx.path = std::string(CrossShardRequestLimits::max_path_length, '/');
    ctx.client_tokens.resize(CrossShardRequestLimits::max_client_tokens);
    EXPECT_TRUE(ctx.is_valid());
}

// =============================================================================
// Body Accessor Tests
// =============================================================================

class CrossShardRequestBodyTest : public ::testing::Test {};

TEST_F(CrossShardRequestBodyTest, EmptyBodyDefaults) {
    CrossShardRequestContext ctx;
    EXPECT_TRUE(ctx.body_empty());
    EXPECT_EQ(ctx.body_size(), 0u);
    EXPECT_TRUE(ctx.body_view().empty());
}

TEST_F(CrossShardRequestBodyTest, BodyViewReturnsCorrectContent) {
    CrossShardRequestContext ctx;
    const char* data = "Hello, World!";
    ctx.body = seastar::temporary_buffer<char>(data, 13);
    EXPECT_EQ(ctx.body_size(), 13u);
    EXPECT_FALSE(ctx.body_empty());
    EXPECT_EQ(ctx.body_view(), "Hello, World!");
}

TEST_F(CrossShardRequestBodyTest, BodyViewPreservesBinaryData) {
    CrossShardRequestContext ctx;
    const char data[] = "ab\0cd";
    ctx.body = seastar::temporary_buffer<char>(data, 5);
    EXPECT_EQ(ctx.body_size(), 5u);
    auto view = ctx.body_view();
    EXPECT_EQ(view.size(), 5u);
    EXPECT_EQ(view[2], '\0');
}

// =============================================================================
// Estimated Memory Usage Tests
// =============================================================================

class CrossShardRequestMemoryTest : public ::testing::Test {};

TEST_F(CrossShardRequestMemoryTest, EmptyContextHasMinimalUsage) {
    CrossShardRequestContext ctx;
    // Empty context: body is 0, strings have small or zero capacity
    EXPECT_GE(ctx.estimated_memory_usage(), 0u);
}

TEST_F(CrossShardRequestMemoryTest, BodyContributesToMemoryUsage) {
    CrossShardRequestContext ctx;
    ctx.body = seastar::temporary_buffer<char>("data", 4);
    size_t with_body = ctx.estimated_memory_usage();

    CrossShardRequestContext empty;
    size_t without_body = empty.estimated_memory_usage();

    EXPECT_GT(with_body, without_body);
}

TEST_F(CrossShardRequestMemoryTest, TokensContributeToMemoryUsage) {
    CrossShardRequestContext ctx;
    ctx.client_tokens.resize(1000);
    size_t with_tokens = ctx.estimated_memory_usage();

    CrossShardRequestContext empty;
    size_t without_tokens = empty.estimated_memory_usage();

    EXPECT_GT(with_tokens, without_tokens);
}

// =============================================================================
// force_local_allocation Tests
// =============================================================================

class ForceLocalAllocationTest : public ::testing::Test {};

TEST_F(ForceLocalAllocationTest, CopiesStringFields) {
    CrossShardRequestContext ctx;
    ctx.request_id = "req-123";
    ctx.client_ip = "10.0.0.1";
    ctx.traceparent = "00-traceid-spanid-01";
    ctx.method = "POST";
    ctx.path = "/v1/chat/completions";

    auto local = std::move(ctx).force_local_allocation();

    EXPECT_EQ(local.request_id, "req-123");
    EXPECT_EQ(local.client_ip, "10.0.0.1");
    EXPECT_EQ(local.traceparent, "00-traceid-spanid-01");
    EXPECT_EQ(local.method, "POST");
    EXPECT_EQ(local.path, "/v1/chat/completions");
}

TEST_F(ForceLocalAllocationTest, CopiesTokenVector) {
    CrossShardRequestContext ctx;
    ctx.client_tokens = {100, 200, 300, 400};
    ctx.has_client_tokens = true;

    auto local = std::move(ctx).force_local_allocation();

    ASSERT_EQ(local.client_tokens.size(), 4u);
    EXPECT_EQ(local.client_tokens[0], 100);
    EXPECT_EQ(local.client_tokens[3], 400);
    EXPECT_TRUE(local.has_client_tokens);
}

TEST_F(ForceLocalAllocationTest, MovesBody) {
    CrossShardRequestContext ctx;
    ctx.body = seastar::temporary_buffer<char>("test body", 9);

    auto local = std::move(ctx).force_local_allocation();

    EXPECT_EQ(local.body_size(), 9u);
    EXPECT_EQ(local.body_view(), "test body");
}

TEST_F(ForceLocalAllocationTest, PreservesOriginShard) {
    CrossShardRequestContext ctx;
    ctx.origin_shard = 42;

    auto local = std::move(ctx).force_local_allocation();

    EXPECT_EQ(local.origin_shard, 42u);
}

TEST_F(ForceLocalAllocationTest, ProducesFreshStringAllocations) {
    CrossShardRequestContext ctx;
    ctx.request_id = "original-id";
    const char* original_ptr = ctx.request_id.data();

    auto local = std::move(ctx).force_local_allocation();

    // The local copy should have a different heap allocation
    // (the whole point of force_local_allocation)
    EXPECT_NE(local.request_id.data(), original_ptr);
    EXPECT_EQ(local.request_id, "original-id");
}

TEST_F(ForceLocalAllocationTest, EmptyContextRoundTrips) {
    CrossShardRequestContext ctx;
    auto local = std::move(ctx).force_local_allocation();

    EXPECT_TRUE(local.request_id.empty());
    EXPECT_TRUE(local.client_ip.empty());
    EXPECT_TRUE(local.body_empty());
    EXPECT_TRUE(local.client_tokens.empty());
    EXPECT_FALSE(local.has_client_tokens);
}

// =============================================================================
// CrossShardRequestCreateResult Factory Tests
// =============================================================================

class CrossShardRequestCreateResultTest : public ::testing::Test {};

TEST_F(CrossShardRequestCreateResultTest, OkResultHasContext) {
    CrossShardRequestContext ctx;
    ctx.request_id = "test";
    auto result = CrossShardRequestCreateResult::ok(std::move(ctx));

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.error.empty());
    ASSERT_TRUE(result.context.has_value());
    EXPECT_EQ(result.context->request_id, "test");
}

TEST_F(CrossShardRequestCreateResultTest, FailResultHasError) {
    auto result = CrossShardRequestCreateResult::fail("body too large");

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.context.has_value());
    EXPECT_EQ(result.error, "body too large");
}

// =============================================================================
// CrossShardResult Factory Tests
// =============================================================================

class CrossShardResultTest : public ::testing::Test {};

TEST_F(CrossShardResultTest, SuccessDefaults) {
    auto r = CrossShardResult::success("response body");
    EXPECT_EQ(r.status_code, 200);
    EXPECT_EQ(r.body, "response body");
    EXPECT_FALSE(r.has_error);
    EXPECT_FALSE(r.is_streaming);
}

TEST_F(CrossShardResultTest, ErrorResult) {
    auto r = CrossShardResult::error(503, "backend unavailable");
    EXPECT_EQ(r.status_code, 503);
    EXPECT_TRUE(r.has_error);
    EXPECT_EQ(r.error_message, "backend unavailable");
}

TEST_F(CrossShardResultTest, StreamingResult) {
    auto r = CrossShardResult::streaming();
    EXPECT_TRUE(r.is_streaming);
    EXPECT_EQ(r.status_code, 200);
    EXPECT_FALSE(r.has_error);
}

// =============================================================================
// Move Semantics Tests
// =============================================================================

class CrossShardRequestMoveTest : public ::testing::Test {};

TEST_F(CrossShardRequestMoveTest, MoveConstructor) {
    CrossShardRequestContext ctx;
    ctx.request_id = "moveme";
    ctx.body = seastar::temporary_buffer<char>("body", 4);

    CrossShardRequestContext moved(std::move(ctx));
    EXPECT_EQ(moved.request_id, "moveme");
    EXPECT_EQ(moved.body_size(), 4u);
}

TEST_F(CrossShardRequestMoveTest, MoveAssignment) {
    CrossShardRequestContext ctx;
    ctx.request_id = "moveme";

    CrossShardRequestContext target;
    target = std::move(ctx);
    EXPECT_EQ(target.request_id, "moveme");
}

TEST_F(CrossShardRequestMoveTest, IsNonCopyable) {
    EXPECT_FALSE(std::is_copy_constructible_v<CrossShardRequestContext>);
    EXPECT_FALSE(std::is_copy_assignable_v<CrossShardRequestContext>);
}

TEST_F(CrossShardRequestMoveTest, IsMoveConstructible) {
    EXPECT_TRUE(std::is_move_constructible_v<CrossShardRequestContext>);
    EXPECT_TRUE(std::is_move_assignable_v<CrossShardRequestContext>);
}
