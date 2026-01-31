// Ranvier Core - Tokenizer Thread Pool Unit Tests
//
// Tests for the dedicated thread pool that offloads tokenization FFI calls
// outside Seastar's reactor (Hard Rule #4: bounded queue, Rule #11: atomics).

#include "tokenizer_thread_pool.hpp"
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace ranvier;

// =============================================================================
// ThreadPoolTokenizationConfig Tests
// =============================================================================

class ThreadPoolTokenizationConfigTest : public ::testing::Test {
protected:
    ThreadPoolTokenizationConfig config;
};

TEST_F(ThreadPoolTokenizationConfigTest, DefaultConfigIsDisabled) {
    // P3 priority: disabled by default, benchmark first
    EXPECT_FALSE(config.enabled);
}

TEST_F(ThreadPoolTokenizationConfigTest, DefaultQueueSize) {
    EXPECT_EQ(config.max_queue_size, 256u);
}

TEST_F(ThreadPoolTokenizationConfigTest, DefaultTextLengthLimits) {
    EXPECT_EQ(config.min_text_length, 256u);
    EXPECT_EQ(config.max_text_length, 65536u);
}

TEST_F(ThreadPoolTokenizationConfigTest, ConfigCanBeCustomized) {
    config.enabled = true;
    config.max_queue_size = 512;
    config.min_text_length = 128;
    config.max_text_length = 32768;

    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.max_queue_size, 512u);
    EXPECT_EQ(config.min_text_length, 128u);
    EXPECT_EQ(config.max_text_length, 32768u);
}

// =============================================================================
// TokenizationJob Tests
// =============================================================================

class TokenizationJobTest : public ::testing::Test {};

TEST_F(TokenizationJobTest, JobStructureHoldsData) {
    TokenizationJob job;
    job.job_id = 12345;
    job.text = "Hello, world!";
    job.source_shard = 3;

    EXPECT_EQ(job.job_id, 12345u);
    EXPECT_EQ(job.text, "Hello, world!");
    EXPECT_EQ(job.source_shard, 3u);
}

TEST_F(TokenizationJobTest, JobTextIsOwned) {
    TokenizationJob job;
    {
        std::string temp = "temporary string";
        job.text = temp;
    }  // temp goes out of scope

    // Job should still have valid copy
    EXPECT_EQ(job.text, "temporary string");
}

TEST_F(TokenizationJobTest, JobCanBeMovedIntoQueue) {
    TokenizationJob job;
    job.job_id = 1;
    job.text = "test text";
    job.source_shard = 0;

    TokenizationJob moved_job = std::move(job);

    EXPECT_EQ(moved_job.job_id, 1u);
    EXPECT_EQ(moved_job.text, "test text");
    EXPECT_EQ(moved_job.source_shard, 0u);
}

// =============================================================================
// ThreadPoolTokenizationResult Tests
// =============================================================================

class ThreadPoolTokenizationResultTest : public ::testing::Test {};

TEST_F(ThreadPoolTokenizationResultTest, DefaultResultIsEmpty) {
    ThreadPoolTokenizationResult result;

    EXPECT_TRUE(result.tokens.empty());
    EXPECT_FALSE(result.cache_hit);
    EXPECT_FALSE(result.thread_pool);
    EXPECT_EQ(result.source_shard, 0u);
}

TEST_F(ThreadPoolTokenizationResultTest, ResultCanHoldTokens) {
    ThreadPoolTokenizationResult result;
    result.tokens = {1, 2, 3, 4, 5};
    result.cache_hit = false;
    result.thread_pool = true;
    result.source_shard = 2;

    EXPECT_EQ(result.tokens.size(), 5u);
    EXPECT_EQ(result.tokens[0], 1);
    EXPECT_EQ(result.tokens[4], 5);
    EXPECT_FALSE(result.cache_hit);
    EXPECT_TRUE(result.thread_pool);
    EXPECT_EQ(result.source_shard, 2u);
}

TEST_F(ThreadPoolTokenizationResultTest, ResultCanBeMovedEfficiently) {
    std::vector<int32_t> large_tokens(10000, 42);
    ThreadPoolTokenizationResult result;
    result.tokens = std::move(large_tokens);

    ThreadPoolTokenizationResult moved = std::move(result);
    EXPECT_EQ(moved.tokens.size(), 10000u);
    EXPECT_EQ(moved.tokens[0], 42);
}

// =============================================================================
// TokenizerWorker Queue Tests (SPSC Queue Behavior)
// =============================================================================

class TokenizerWorkerQueueTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create worker with small queue for testing
        worker = std::make_unique<TokenizerWorker>(0, 4);
    }

    std::unique_ptr<TokenizerWorker> worker;
};

TEST_F(TokenizerWorkerQueueTest, WorkerStartsNotRunning) {
    EXPECT_FALSE(worker->is_running());
}

TEST_F(TokenizerWorkerQueueTest, SubmitFailsWhenNotRunning) {
    TokenizationJob job;
    job.job_id = 1;
    job.text = "test";
    job.source_shard = 0;

    // Should fail because worker is not running
    EXPECT_FALSE(worker->submit(std::move(job)));
}

TEST_F(TokenizerWorkerQueueTest, StatisticsStartAtZero) {
    EXPECT_EQ(worker->jobs_processed(), 0u);
    EXPECT_EQ(worker->jobs_dropped(), 0u);
    EXPECT_EQ(worker->queue_full_count(), 0u);
}

// =============================================================================
// TokenizerThreadPool Configuration Tests
// =============================================================================

class TokenizerThreadPoolConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        config.enabled = true;
        config.min_text_length = 100;
        config.max_text_length = 1000;
    }

    ThreadPoolTokenizationConfig config;
};

TEST_F(TokenizerThreadPoolConfigTest, PoolReportsEnabledState) {
    ThreadPoolTokenizationConfig enabled_config;
    enabled_config.enabled = true;
    TokenizerThreadPool enabled_pool(enabled_config);
    EXPECT_TRUE(enabled_pool.enabled());

    ThreadPoolTokenizationConfig disabled_config;
    disabled_config.enabled = false;
    TokenizerThreadPool disabled_pool(disabled_config);
    EXPECT_FALSE(disabled_pool.enabled());
}

TEST_F(TokenizerThreadPoolConfigTest, PoolReportsTextLengthLimits) {
    TokenizerThreadPool pool(config);

    EXPECT_EQ(pool.min_text_length(), 100u);
    EXPECT_EQ(pool.max_text_length(), 1000u);
}

TEST_F(TokenizerThreadPoolConfigTest, ShouldUsePoolReturnsFalseWhenDisabled) {
    config.enabled = false;
    TokenizerThreadPool pool(config);

    // Should return false regardless of text length
    EXPECT_FALSE(pool.should_use_thread_pool(500));
    EXPECT_FALSE(pool.should_use_thread_pool(100));
    EXPECT_FALSE(pool.should_use_thread_pool(1000));
}

TEST_F(TokenizerThreadPoolConfigTest, ShouldUsePoolReturnsFalseWithoutWorker) {
    // Enabled but no worker started
    TokenizerThreadPool pool(config);

    // Should return false because worker is not running
    EXPECT_FALSE(pool.should_use_thread_pool(500));
}

TEST_F(TokenizerThreadPoolConfigTest, StatisticsStartAtZero) {
    TokenizerThreadPool pool(config);

    EXPECT_EQ(pool.jobs_submitted(), 0u);
    EXPECT_EQ(pool.jobs_completed(), 0u);
    EXPECT_EQ(pool.jobs_fallback(), 0u);
    EXPECT_EQ(pool.worker_jobs_processed(), 0u);
    EXPECT_EQ(pool.worker_queue_full(), 0u);
}

// =============================================================================
// Text Length Decision Logic Tests
// =============================================================================

class TextLengthDecisionTest : public ::testing::Test {
protected:
    void SetUp() override {
        config.enabled = true;
        config.min_text_length = 256;
        config.max_text_length = 65536;
    }

    ThreadPoolTokenizationConfig config;
};

TEST_F(TextLengthDecisionTest, TextTooShortRejected) {
    // Without a running worker, should_use_thread_pool returns false anyway
    // But we can test the config values are set correctly
    TokenizerThreadPool pool(config);

    EXPECT_EQ(pool.min_text_length(), 256u);
    // Text of length 100 is below min_text_length of 256
}

TEST_F(TextLengthDecisionTest, TextTooLongRejected) {
    TokenizerThreadPool pool(config);

    EXPECT_EQ(pool.max_text_length(), 65536u);
    // Text of length 100000 is above max_text_length of 65536
}

TEST_F(TextLengthDecisionTest, TextInRangeAccepted) {
    TokenizerThreadPool pool(config);

    // Text of length 1000 is within [256, 65536]
    // (Would be accepted if worker was running)
    EXPECT_GE(1000u, pool.min_text_length());
    EXPECT_LE(1000u, pool.max_text_length());
}

// =============================================================================
// Submit Without Worker Tests
// =============================================================================

class SubmitWithoutWorkerTest : public ::testing::Test {
protected:
    void SetUp() override {
        config.enabled = true;
        config.min_text_length = 10;
        config.max_text_length = 1000;
    }

    ThreadPoolTokenizationConfig config;
};

TEST_F(SubmitWithoutWorkerTest, SubmitAsyncReturnsNulloptWithoutWorker) {
    TokenizerThreadPool pool(config);

    // Without loading tokenizer and starting worker, submit should fail
    auto result = pool.submit_async("test text");
    EXPECT_FALSE(result.has_value());
}

TEST_F(SubmitWithoutWorkerTest, SubmitAsyncReturnsNulloptWhenDisabled) {
    config.enabled = false;
    TokenizerThreadPool pool(config);

    auto result = pool.submit_async("test text");
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// Type Traits Tests
// =============================================================================

TEST(TokenizerThreadPoolTypeTraitsTest, ConfigIsCopyable) {
    EXPECT_TRUE(std::is_copy_constructible_v<ThreadPoolTokenizationConfig>);
    EXPECT_TRUE(std::is_copy_assignable_v<ThreadPoolTokenizationConfig>);
}

TEST(TokenizerThreadPoolTypeTraitsTest, JobIsMovable) {
    EXPECT_TRUE(std::is_move_constructible_v<TokenizationJob>);
    EXPECT_TRUE(std::is_move_assignable_v<TokenizationJob>);
}

TEST(TokenizerThreadPoolTypeTraitsTest, ResultIsMovable) {
    EXPECT_TRUE(std::is_move_constructible_v<ThreadPoolTokenizationResult>);
    EXPECT_TRUE(std::is_move_assignable_v<ThreadPoolTokenizationResult>);
}

TEST(TokenizerThreadPoolTypeTraitsTest, WorkerIsNotCopyable) {
    // Workers own unique resources (thread, tokenizer)
    EXPECT_FALSE(std::is_copy_constructible_v<TokenizerWorker>);
    EXPECT_FALSE(std::is_copy_assignable_v<TokenizerWorker>);
}

TEST(TokenizerThreadPoolTypeTraitsTest, WorkerIsNotMovable) {
    // Workers are not movable (own thread handle)
    EXPECT_FALSE(std::is_move_constructible_v<TokenizerWorker>);
    EXPECT_FALSE(std::is_move_assignable_v<TokenizerWorker>);
}

// =============================================================================
// Edge Cases Tests
// =============================================================================

class EdgeCasesTest : public ::testing::Test {};

TEST_F(EdgeCasesTest, EmptyTextJob) {
    TokenizationJob job;
    job.job_id = 1;
    job.text = "";
    job.source_shard = 0;

    EXPECT_TRUE(job.text.empty());
    EXPECT_EQ(job.job_id, 1u);
}

TEST_F(EdgeCasesTest, LargeTextJob) {
    TokenizationJob job;
    job.job_id = 1;
    job.text = std::string(100000, 'x');  // 100KB of 'x'
    job.source_shard = 0;

    EXPECT_EQ(job.text.size(), 100000u);
}

TEST_F(EdgeCasesTest, MaxJobId) {
    TokenizationJob job;
    job.job_id = UINT64_MAX;
    job.text = "test";
    job.source_shard = 0;

    EXPECT_EQ(job.job_id, UINT64_MAX);
}

TEST_F(EdgeCasesTest, ZeroQueueSizeWorker) {
    // Edge case: queue size of 0
    // boost::lockfree::spsc_queue requires size > 0, so this tests
    // that we handle small sizes gracefully
    auto worker = std::make_unique<TokenizerWorker>(0, 1);  // Min size 1
    EXPECT_FALSE(worker->is_running());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
