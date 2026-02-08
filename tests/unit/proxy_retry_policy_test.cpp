// Ranvier Core - Proxy Retry Policy Unit Tests
//
// Tests for the extracted retry/backoff/fallback decision logic, connection
// error classification, fallback backend selection, and backpressure decisions.
// Pure C++ — no Seastar dependency.

#include "proxy_retry_policy.hpp"
#include "test_clock.hpp"

#include <gtest/gtest.h>
#include <stdexcept>
#include <system_error>
#include <vector>

using namespace ranvier;

// =============================================================================
// Connection Error Classification Tests
// =============================================================================

class ConnectionErrorClassificationTest : public ::testing::Test {};

TEST_F(ConnectionErrorClassificationTest, NullExceptionReturnsNone) {
    EXPECT_EQ(classify_connection_error(nullptr), ConnectionErrorType::NONE);
}

TEST_F(ConnectionErrorClassificationTest, BrokenPipeFromErrc) {
    try {
        throw std::system_error(std::make_error_code(std::errc::broken_pipe));
    } catch (...) {
        EXPECT_EQ(classify_connection_error(std::current_exception()),
                  ConnectionErrorType::BROKEN_PIPE);
    }
}

TEST_F(ConnectionErrorClassificationTest, ConnectionResetFromErrc) {
    try {
        throw std::system_error(std::make_error_code(std::errc::connection_reset));
    } catch (...) {
        EXPECT_EQ(classify_connection_error(std::current_exception()),
                  ConnectionErrorType::CONNECTION_RESET);
    }
}

TEST_F(ConnectionErrorClassificationTest, BrokenPipeFromErrno) {
    try {
        throw std::system_error(EPIPE, std::generic_category());
    } catch (...) {
        EXPECT_EQ(classify_connection_error(std::current_exception()),
                  ConnectionErrorType::BROKEN_PIPE);
    }
}

TEST_F(ConnectionErrorClassificationTest, ConnectionResetFromErrno) {
    try {
        throw std::system_error(ECONNRESET, std::generic_category());
    } catch (...) {
        EXPECT_EQ(classify_connection_error(std::current_exception()),
                  ConnectionErrorType::CONNECTION_RESET);
    }
}

TEST_F(ConnectionErrorClassificationTest, OtherSystemErrorReturnsNone) {
    try {
        throw std::system_error(std::make_error_code(std::errc::permission_denied));
    } catch (...) {
        EXPECT_EQ(classify_connection_error(std::current_exception()),
                  ConnectionErrorType::NONE);
    }
}

TEST_F(ConnectionErrorClassificationTest, NonSystemErrorReturnsNone) {
    try {
        throw std::runtime_error("something else");
    } catch (...) {
        EXPECT_EQ(classify_connection_error(std::current_exception()),
                  ConnectionErrorType::NONE);
    }
}

TEST_F(ConnectionErrorClassificationTest, StringExceptionReturnsNone) {
    try {
        throw std::string("unexpected");
    } catch (...) {
        EXPECT_EQ(classify_connection_error(std::current_exception()),
                  ConnectionErrorType::NONE);
    }
}

TEST_F(ConnectionErrorClassificationTest, ErrorToStringBrokenPipe) {
    EXPECT_STREQ(connection_error_to_string(ConnectionErrorType::BROKEN_PIPE),
                 "broken pipe (EPIPE)");
}

TEST_F(ConnectionErrorClassificationTest, ErrorToStringConnectionReset) {
    EXPECT_STREQ(connection_error_to_string(ConnectionErrorType::CONNECTION_RESET),
                 "connection reset (ECONNRESET)");
}

TEST_F(ConnectionErrorClassificationTest, ErrorToStringNone) {
    EXPECT_STREQ(connection_error_to_string(ConnectionErrorType::NONE), "unknown");
}

TEST_F(ConnectionErrorClassificationTest, IsRetryableBrokenPipe) {
    EXPECT_TRUE(is_retryable_connection_error(ConnectionErrorType::BROKEN_PIPE));
}

TEST_F(ConnectionErrorClassificationTest, IsRetryableConnectionReset) {
    EXPECT_TRUE(is_retryable_connection_error(ConnectionErrorType::CONNECTION_RESET));
}

TEST_F(ConnectionErrorClassificationTest, IsRetryableNone) {
    EXPECT_FALSE(is_retryable_connection_error(ConnectionErrorType::NONE));
}

// =============================================================================
// Proxy Retry Policy — Backoff Calculation Tests
// =============================================================================

class RetryPolicyBackoffTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestClock::reset();
    }

    RetryPolicyConfig make_config(
        uint32_t max_retries = 3,
        std::chrono::milliseconds initial_backoff = std::chrono::milliseconds{100},
        std::chrono::milliseconds max_backoff = std::chrono::milliseconds{5000},
        double multiplier = 2.0) {
        return RetryPolicyConfig{max_retries, initial_backoff, max_backoff, multiplier, 3, true};
    }
};

TEST_F(RetryPolicyBackoffTest, InitialBackoffIsConfigured) {
    BasicProxyRetryPolicy<TestClock> policy(make_config());
    auto deadline = TestClock::now() + std::chrono::seconds(60);
    auto state = policy.make_initial_state(deadline);

    auto backoff = policy.consume_backoff(state);
    EXPECT_EQ(backoff, std::chrono::milliseconds(100));
}

TEST_F(RetryPolicyBackoffTest, BackoffDoublesExponentially) {
    BasicProxyRetryPolicy<TestClock> policy(make_config());
    auto deadline = TestClock::now() + std::chrono::seconds(60);
    auto state = policy.make_initial_state(deadline);

    EXPECT_EQ(policy.consume_backoff(state), std::chrono::milliseconds(100));
    EXPECT_EQ(policy.consume_backoff(state), std::chrono::milliseconds(200));
    EXPECT_EQ(policy.consume_backoff(state), std::chrono::milliseconds(400));
    EXPECT_EQ(policy.consume_backoff(state), std::chrono::milliseconds(800));
}

TEST_F(RetryPolicyBackoffTest, BackoffCappedAtMax) {
    auto config = make_config(10, std::chrono::milliseconds{1000},
                              std::chrono::milliseconds{3000}, 2.0);
    BasicProxyRetryPolicy<TestClock> policy(config);
    auto deadline = TestClock::now() + std::chrono::seconds(60);
    auto state = policy.make_initial_state(deadline);

    EXPECT_EQ(policy.consume_backoff(state), std::chrono::milliseconds(1000));  // 1000
    EXPECT_EQ(policy.consume_backoff(state), std::chrono::milliseconds(2000));  // 2000
    EXPECT_EQ(policy.consume_backoff(state), std::chrono::milliseconds(3000));  // capped at 3000
    EXPECT_EQ(policy.consume_backoff(state), std::chrono::milliseconds(3000));  // stays at cap
}

TEST_F(RetryPolicyBackoffTest, CustomMultiplier) {
    auto config = make_config(5, std::chrono::milliseconds{100},
                              std::chrono::milliseconds{10000}, 3.0);
    BasicProxyRetryPolicy<TestClock> policy(config);
    auto deadline = TestClock::now() + std::chrono::seconds(60);
    auto state = policy.make_initial_state(deadline);

    EXPECT_EQ(policy.consume_backoff(state), std::chrono::milliseconds(100));
    EXPECT_EQ(policy.consume_backoff(state), std::chrono::milliseconds(300));
    EXPECT_EQ(policy.consume_backoff(state), std::chrono::milliseconds(900));
    EXPECT_EQ(policy.consume_backoff(state), std::chrono::milliseconds(2700));
}

TEST_F(RetryPolicyBackoffTest, MultiplierOfOneMeansConstantBackoff) {
    auto config = make_config(5, std::chrono::milliseconds{500},
                              std::chrono::milliseconds{5000}, 1.0);
    BasicProxyRetryPolicy<TestClock> policy(config);
    auto deadline = TestClock::now() + std::chrono::seconds(60);
    auto state = policy.make_initial_state(deadline);

    EXPECT_EQ(policy.consume_backoff(state), std::chrono::milliseconds(500));
    EXPECT_EQ(policy.consume_backoff(state), std::chrono::milliseconds(500));
    EXPECT_EQ(policy.consume_backoff(state), std::chrono::milliseconds(500));
}

// =============================================================================
// Proxy Retry Policy — Retry Count Tests
// =============================================================================

class RetryPolicyRetryCountTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestClock::reset();
    }
};

TEST_F(RetryPolicyRetryCountTest, ZeroRetriesMeansNoRetry) {
    RetryPolicyConfig config;
    config.max_retries = 0;
    config.fallback_enabled = false;
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(60);
    auto state = policy.make_initial_state(deadline);

    // First failure with no retries and no fallback → give up
    auto action = policy.decide_on_failure(state, false);
    EXPECT_EQ(action, RetryAction::GIVE_UP_MAX_RETRIES);
    EXPECT_TRUE(state.connection_failed);
}

TEST_F(RetryPolicyRetryCountTest, OneRetryGivesOneMoreAttempt) {
    RetryPolicyConfig config;
    config.max_retries = 1;
    config.fallback_enabled = false;
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(60);
    auto state = policy.make_initial_state(deadline);

    // First failure → retry
    auto action1 = policy.decide_on_failure(state, false);
    EXPECT_EQ(action1, RetryAction::RETRY_SAME_BACKEND);
    EXPECT_EQ(state.retry_attempt, 1u);
    EXPECT_FALSE(state.connection_failed);

    // Second failure → give up
    auto action2 = policy.decide_on_failure(state, false);
    EXPECT_EQ(action2, RetryAction::GIVE_UP_MAX_RETRIES);
    EXPECT_TRUE(state.connection_failed);
}

TEST_F(RetryPolicyRetryCountTest, ThreeRetriesGivesThreeMoreAttempts) {
    RetryPolicyConfig config;
    config.max_retries = 3;
    config.fallback_enabled = false;
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(60);
    auto state = policy.make_initial_state(deadline);

    EXPECT_EQ(policy.decide_on_failure(state, false), RetryAction::RETRY_SAME_BACKEND);
    EXPECT_EQ(policy.decide_on_failure(state, false), RetryAction::RETRY_SAME_BACKEND);
    EXPECT_EQ(policy.decide_on_failure(state, false), RetryAction::RETRY_SAME_BACKEND);
    EXPECT_EQ(state.retry_attempt, 3u);

    // Fourth failure → give up
    EXPECT_EQ(policy.decide_on_failure(state, false), RetryAction::GIVE_UP_MAX_RETRIES);
    EXPECT_TRUE(state.connection_failed);
}

TEST_F(RetryPolicyRetryCountTest, ShouldContinueReflectsState) {
    RetryPolicyConfig config;
    config.max_retries = 1;
    config.fallback_enabled = false;
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(60);
    auto state = policy.make_initial_state(deadline);

    EXPECT_TRUE(policy.should_continue(state));

    policy.decide_on_failure(state, false);  // retry
    EXPECT_TRUE(policy.should_continue(state));

    policy.decide_on_failure(state, false);  // give up
    EXPECT_FALSE(policy.should_continue(state));
}

// =============================================================================
// Proxy Retry Policy — Deadline Tests
// =============================================================================

class RetryPolicyDeadlineTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestClock::reset();
    }
};

TEST_F(RetryPolicyDeadlineTest, DeadlineNotExceededInitially) {
    RetryPolicyConfig config;
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(10);
    auto state = policy.make_initial_state(deadline);

    EXPECT_FALSE(policy.is_deadline_exceeded(state));
}

TEST_F(RetryPolicyDeadlineTest, DeadlineExceededAfterTimeAdvance) {
    RetryPolicyConfig config;
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(10);
    auto state = policy.make_initial_state(deadline);

    TestClock::advance(std::chrono::seconds(10));
    EXPECT_TRUE(policy.is_deadline_exceeded(state));
}

TEST_F(RetryPolicyDeadlineTest, DeadlineExceededOverridesRetry) {
    RetryPolicyConfig config;
    config.max_retries = 10;  // Plenty of retries
    config.fallback_enabled = false;
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(5);
    auto state = policy.make_initial_state(deadline);

    // First failure — within deadline → retry
    auto action1 = policy.decide_on_failure(state, false);
    EXPECT_EQ(action1, RetryAction::RETRY_SAME_BACKEND);

    // Advance past deadline
    TestClock::advance(std::chrono::seconds(6));

    // Next failure — past deadline → give up
    auto action2 = policy.decide_on_failure(state, false);
    EXPECT_EQ(action2, RetryAction::GIVE_UP_DEADLINE);
    EXPECT_TRUE(state.connection_failed);
}

TEST_F(RetryPolicyDeadlineTest, DeadlineExceededOverridesFallback) {
    RetryPolicyConfig config;
    config.max_retries = 10;
    config.fallback_enabled = true;
    config.max_fallback_attempts = 5;
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(5);
    auto state = policy.make_initial_state(deadline);

    TestClock::advance(std::chrono::seconds(6));

    // Even with fallback candidate available, deadline takes priority
    auto action = policy.decide_on_failure(state, true);
    EXPECT_EQ(action, RetryAction::GIVE_UP_DEADLINE);
}

TEST_F(RetryPolicyDeadlineTest, ShouldContinueFalseAfterDeadline) {
    RetryPolicyConfig config;
    config.max_retries = 10;
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(5);
    auto state = policy.make_initial_state(deadline);

    EXPECT_TRUE(policy.should_continue(state));

    TestClock::advance(std::chrono::seconds(6));
    EXPECT_FALSE(policy.should_continue(state));
}

TEST_F(RetryPolicyDeadlineTest, ExactDeadlineBoundary) {
    RetryPolicyConfig config;
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(5);
    auto state = policy.make_initial_state(deadline);

    TestClock::advance(std::chrono::seconds(5));
    // At exactly the deadline, it's considered exceeded (>=)
    EXPECT_TRUE(policy.is_deadline_exceeded(state));
}

// =============================================================================
// Proxy Retry Policy — Fallback Decision Tests
// =============================================================================

class RetryPolicyFallbackTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestClock::reset();
    }
};

TEST_F(RetryPolicyFallbackTest, FallbackPreferredOverRetry) {
    RetryPolicyConfig config;
    config.max_retries = 3;
    config.fallback_enabled = true;
    config.max_fallback_attempts = 3;
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(60);
    auto state = policy.make_initial_state(deadline);

    // With fallback available, it should be preferred over retry
    auto action = policy.decide_on_failure(state, true);
    EXPECT_EQ(action, RetryAction::TRY_FALLBACK);
    EXPECT_EQ(state.fallback_attempts, 1u);
    EXPECT_EQ(state.retry_attempt, 0u);  // Fallback doesn't consume retries
}

TEST_F(RetryPolicyFallbackTest, FallbackDoesNotConsumeRetry) {
    RetryPolicyConfig config;
    config.max_retries = 1;
    config.fallback_enabled = true;
    config.max_fallback_attempts = 2;
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(60);
    auto state = policy.make_initial_state(deadline);

    // Two fallbacks
    EXPECT_EQ(policy.decide_on_failure(state, true), RetryAction::TRY_FALLBACK);
    EXPECT_EQ(policy.decide_on_failure(state, true), RetryAction::TRY_FALLBACK);
    EXPECT_EQ(state.fallback_attempts, 2u);
    EXPECT_EQ(state.retry_attempt, 0u);

    // Fallback limit hit, now retries
    EXPECT_EQ(policy.decide_on_failure(state, true), RetryAction::RETRY_SAME_BACKEND);
    EXPECT_EQ(state.retry_attempt, 1u);

    // Retries also exhausted
    EXPECT_EQ(policy.decide_on_failure(state, true), RetryAction::GIVE_UP_MAX_RETRIES);
}

TEST_F(RetryPolicyFallbackTest, FallbackDisabledFallsBackToRetry) {
    RetryPolicyConfig config;
    config.max_retries = 2;
    config.fallback_enabled = false;
    config.max_fallback_attempts = 5;  // Doesn't matter — disabled
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(60);
    auto state = policy.make_initial_state(deadline);

    // Even with candidate available, fallback disabled → retry
    auto action = policy.decide_on_failure(state, true);
    EXPECT_EQ(action, RetryAction::RETRY_SAME_BACKEND);
}

TEST_F(RetryPolicyFallbackTest, NoCandidateFallsBackToRetry) {
    RetryPolicyConfig config;
    config.max_retries = 2;
    config.fallback_enabled = true;
    config.max_fallback_attempts = 5;
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(60);
    auto state = policy.make_initial_state(deadline);

    // No candidate available → retry
    auto action = policy.decide_on_failure(state, false);
    EXPECT_EQ(action, RetryAction::RETRY_SAME_BACKEND);
}

TEST_F(RetryPolicyFallbackTest, MaxFallbackAttemptsRespected) {
    RetryPolicyConfig config;
    config.max_retries = 0;
    config.fallback_enabled = true;
    config.max_fallback_attempts = 2;
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(60);
    auto state = policy.make_initial_state(deadline);

    // 2 fallbacks, then give up (no retries)
    EXPECT_EQ(policy.decide_on_failure(state, true), RetryAction::TRY_FALLBACK);
    EXPECT_EQ(policy.decide_on_failure(state, true), RetryAction::TRY_FALLBACK);
    EXPECT_EQ(policy.decide_on_failure(state, true), RetryAction::GIVE_UP_MAX_RETRIES);
}

TEST_F(RetryPolicyFallbackTest, ZeroFallbackAttemptsSkipsFallback) {
    RetryPolicyConfig config;
    config.max_retries = 1;
    config.fallback_enabled = true;
    config.max_fallback_attempts = 0;
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(60);
    auto state = policy.make_initial_state(deadline);

    // Fallback enabled but max_fallback_attempts=0 → skip directly to retry
    auto action = policy.decide_on_failure(state, true);
    EXPECT_EQ(action, RetryAction::RETRY_SAME_BACKEND);
}

// =============================================================================
// Proxy Retry Policy — Combined Scenario Tests
// =============================================================================

class RetryPolicyCombinedTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestClock::reset();
    }
};

TEST_F(RetryPolicyCombinedTest, RealisticScenarioFallbackThenRetryThenGiveUp) {
    // Config: 2 retries, 2 fallbacks
    RetryPolicyConfig config;
    config.max_retries = 2;
    config.fallback_enabled = true;
    config.max_fallback_attempts = 2;
    config.initial_backoff = std::chrono::milliseconds{100};
    config.backoff_multiplier = 2.0;
    config.max_backoff = std::chrono::milliseconds{1000};
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(60);
    auto state = policy.make_initial_state(deadline);

    // Failure 1: fallback to backend B (candidate available)
    EXPECT_EQ(policy.decide_on_failure(state, true), RetryAction::TRY_FALLBACK);
    EXPECT_EQ(state.fallback_attempts, 1u);

    // Failure 2: fallback to backend C (candidate available)
    EXPECT_EQ(policy.decide_on_failure(state, true), RetryAction::TRY_FALLBACK);
    EXPECT_EQ(state.fallback_attempts, 2u);

    // Failure 3: fallback exhausted, retry with backoff
    auto action3 = policy.decide_on_failure(state, true);
    EXPECT_EQ(action3, RetryAction::RETRY_SAME_BACKEND);
    auto backoff1 = policy.consume_backoff(state);
    EXPECT_EQ(backoff1, std::chrono::milliseconds(100));

    // Failure 4: retry with doubled backoff
    EXPECT_EQ(policy.decide_on_failure(state, true), RetryAction::RETRY_SAME_BACKEND);
    auto backoff2 = policy.consume_backoff(state);
    EXPECT_EQ(backoff2, std::chrono::milliseconds(200));

    // Failure 5: all exhausted
    EXPECT_EQ(policy.decide_on_failure(state, true), RetryAction::GIVE_UP_MAX_RETRIES);
    EXPECT_TRUE(state.connection_failed);
}

TEST_F(RetryPolicyCombinedTest, DeadlineDuringFallbackSequence) {
    RetryPolicyConfig config;
    config.max_retries = 5;
    config.fallback_enabled = true;
    config.max_fallback_attempts = 5;
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(3);
    auto state = policy.make_initial_state(deadline);

    // First fallback succeeds in being tried
    EXPECT_EQ(policy.decide_on_failure(state, true), RetryAction::TRY_FALLBACK);

    // Time passes past deadline
    TestClock::advance(std::chrono::seconds(4));

    // Next failure → deadline exceeded, even though fallbacks and retries remain
    EXPECT_EQ(policy.decide_on_failure(state, true), RetryAction::GIVE_UP_DEADLINE);
}

TEST_F(RetryPolicyCombinedTest, IntermittentFallbackAvailability) {
    RetryPolicyConfig config;
    config.max_retries = 3;
    config.fallback_enabled = true;
    config.max_fallback_attempts = 5;
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(60);
    auto state = policy.make_initial_state(deadline);

    // No candidate → retry
    EXPECT_EQ(policy.decide_on_failure(state, false), RetryAction::RETRY_SAME_BACKEND);
    EXPECT_EQ(state.retry_attempt, 1u);

    // Candidate appears → fallback
    EXPECT_EQ(policy.decide_on_failure(state, true), RetryAction::TRY_FALLBACK);
    EXPECT_EQ(state.fallback_attempts, 1u);
    EXPECT_EQ(state.retry_attempt, 1u);  // retry count unchanged

    // No candidate again → retry
    EXPECT_EQ(policy.decide_on_failure(state, false), RetryAction::RETRY_SAME_BACKEND);
    EXPECT_EQ(state.retry_attempt, 2u);
}

// =============================================================================
// Fallback Backend Selection Tests
// =============================================================================

class FallbackSelectionTest : public ::testing::Test {};

TEST_F(FallbackSelectionTest, SelectsFirstAvailable) {
    std::vector<BackendId> backends = {1, 2, 3, 4};
    BackendId failed = 1;
    auto allow_all = [](BackendId) { return true; };

    auto result = select_fallback_backend(backends, failed, allow_all);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 2);  // First that isn't 1
}

TEST_F(FallbackSelectionTest, SkipsFailedBackend) {
    std::vector<BackendId> backends = {1, 2, 3};
    BackendId failed = 2;
    auto allow_all = [](BackendId) { return true; };

    auto result = select_fallback_backend(backends, failed, allow_all);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 1);  // First available that isn't 2
}

TEST_F(FallbackSelectionTest, SkipsDisallowedBackends) {
    std::vector<BackendId> backends = {1, 2, 3, 4};
    BackendId failed = 1;
    // Only backend 4 is allowed by circuit breaker
    auto allow_only_4 = [](BackendId id) { return id == 4; };

    auto result = select_fallback_backend(backends, failed, allow_only_4);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 4);
}

TEST_F(FallbackSelectionTest, NoFallbackWhenAllDisallowed) {
    std::vector<BackendId> backends = {1, 2, 3};
    BackendId failed = 1;
    auto allow_none = [](BackendId) { return false; };

    auto result = select_fallback_backend(backends, failed, allow_none);
    EXPECT_FALSE(result.has_value());
}

TEST_F(FallbackSelectionTest, NoFallbackWhenOnlyFailed) {
    std::vector<BackendId> backends = {1};
    BackendId failed = 1;
    auto allow_all = [](BackendId) { return true; };

    auto result = select_fallback_backend(backends, failed, allow_all);
    EXPECT_FALSE(result.has_value());
}

TEST_F(FallbackSelectionTest, NoFallbackWhenEmpty) {
    std::vector<BackendId> backends;
    BackendId failed = 1;
    auto allow_all = [](BackendId) { return true; };

    auto result = select_fallback_backend(backends, failed, allow_all);
    EXPECT_FALSE(result.has_value());
}

TEST_F(FallbackSelectionTest, AllBackendsAreFailedOrDisallowed) {
    std::vector<BackendId> backends = {1, 2, 3};
    BackendId failed = 1;
    // Backend 2 and 3 are circuit-broken
    auto allow = [](BackendId id) { return id == 1; };

    auto result = select_fallback_backend(backends, failed, allow);
    EXPECT_FALSE(result.has_value());  // 1 is failed, 2+3 are disallowed
}

TEST_F(FallbackSelectionTest, LargeBackendList) {
    std::vector<BackendId> backends;
    for (int i = 0; i < 100; i++) {
        backends.push_back(i);
    }
    BackendId failed = 50;
    // Only backend 99 is allowed
    auto allow_99 = [](BackendId id) { return id == 99; };

    auto result = select_fallback_backend(backends, failed, allow_99);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 99);
}

// =============================================================================
// Backpressure Policy — Concurrency Tests
// =============================================================================

class BackpressureConcurrencyTest : public ::testing::Test {};

TEST_F(BackpressureConcurrencyTest, AcceptUnderLimit) {
    EXPECT_EQ(check_concurrency(500, 1000), BackpressureDecision::ACCEPT);
}

TEST_F(BackpressureConcurrencyTest, AcceptAtZero) {
    EXPECT_EQ(check_concurrency(0, 1000), BackpressureDecision::ACCEPT);
}

TEST_F(BackpressureConcurrencyTest, RejectAtLimit) {
    EXPECT_EQ(check_concurrency(1000, 1000), BackpressureDecision::REJECT_CONCURRENCY);
}

TEST_F(BackpressureConcurrencyTest, RejectOverLimit) {
    EXPECT_EQ(check_concurrency(1500, 1000), BackpressureDecision::REJECT_CONCURRENCY);
}

TEST_F(BackpressureConcurrencyTest, UnlimitedWhenMaxIsZero) {
    EXPECT_EQ(check_concurrency(999999, 0), BackpressureDecision::ACCEPT);
}

TEST_F(BackpressureConcurrencyTest, AcceptOneUnderLimit) {
    EXPECT_EQ(check_concurrency(999, 1000), BackpressureDecision::ACCEPT);
}

TEST_F(BackpressureConcurrencyTest, MaxOfOneRejectsSecond) {
    EXPECT_EQ(check_concurrency(0, 1), BackpressureDecision::ACCEPT);
    EXPECT_EQ(check_concurrency(1, 1), BackpressureDecision::REJECT_CONCURRENCY);
}

// =============================================================================
// Backpressure Policy — Persistence Queue Tests
// =============================================================================

class BackpressurePersistenceTest : public ::testing::Test {};

TEST_F(BackpressurePersistenceTest, AcceptUnderThreshold) {
    // 70% full, threshold at 80%
    EXPECT_EQ(check_persistence_backpressure(70, 100, 0.8, true),
              BackpressureDecision::ACCEPT);
}

TEST_F(BackpressurePersistenceTest, RejectAtThreshold) {
    // 80% full, threshold at 80%
    EXPECT_EQ(check_persistence_backpressure(80, 100, 0.8, true),
              BackpressureDecision::REJECT_PERSISTENCE_QUEUE);
}

TEST_F(BackpressurePersistenceTest, RejectOverThreshold) {
    // 95% full, threshold at 80%
    EXPECT_EQ(check_persistence_backpressure(95, 100, 0.8, true),
              BackpressureDecision::REJECT_PERSISTENCE_QUEUE);
}

TEST_F(BackpressurePersistenceTest, AcceptWhenDisabled) {
    // 100% full but backpressure disabled
    EXPECT_EQ(check_persistence_backpressure(100, 100, 0.8, false),
              BackpressureDecision::ACCEPT);
}

TEST_F(BackpressurePersistenceTest, AcceptWhenMaxQueueIsZero) {
    // max_queue_depth of 0 means persistence not configured
    EXPECT_EQ(check_persistence_backpressure(100, 0, 0.8, true),
              BackpressureDecision::ACCEPT);
}

TEST_F(BackpressurePersistenceTest, EmptyQueueAccepted) {
    EXPECT_EQ(check_persistence_backpressure(0, 100, 0.8, true),
              BackpressureDecision::ACCEPT);
}

TEST_F(BackpressurePersistenceTest, ThresholdAtOneRejectsFullOnly) {
    // threshold = 1.0 means reject only at 100%
    EXPECT_EQ(check_persistence_backpressure(99, 100, 1.0, true),
              BackpressureDecision::ACCEPT);
    EXPECT_EQ(check_persistence_backpressure(100, 100, 1.0, true),
              BackpressureDecision::REJECT_PERSISTENCE_QUEUE);
}

TEST_F(BackpressurePersistenceTest, ThresholdAtZeroRejectsAlways) {
    // threshold = 0.0 means always reject: ratio 0/100 = 0.0 >= 0.0 is true
    EXPECT_EQ(check_persistence_backpressure(0, 100, 0.0, true),
              BackpressureDecision::REJECT_PERSISTENCE_QUEUE);
    EXPECT_EQ(check_persistence_backpressure(1, 100, 0.0, true),
              BackpressureDecision::REJECT_PERSISTENCE_QUEUE);
}

TEST_F(BackpressurePersistenceTest, LargeQueueValues) {
    // 800K of 1M, threshold at 0.9 → under threshold (80%)
    EXPECT_EQ(check_persistence_backpressure(800000, 1000000, 0.9, true),
              BackpressureDecision::ACCEPT);
    // 900K of 1M → at threshold
    EXPECT_EQ(check_persistence_backpressure(900000, 1000000, 0.9, true),
              BackpressureDecision::REJECT_PERSISTENCE_QUEUE);
}

// =============================================================================
// RetryState Initial Conditions Tests
// =============================================================================

class RetryStateTest : public ::testing::Test {
protected:
    void SetUp() override {
        TestClock::reset();
    }
};

TEST_F(RetryStateTest, InitialStateHasZeroCounts) {
    RetryPolicyConfig config;
    config.initial_backoff = std::chrono::milliseconds{250};
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(30);
    auto state = policy.make_initial_state(deadline);

    EXPECT_EQ(state.retry_attempt, 0u);
    EXPECT_EQ(state.fallback_attempts, 0u);
    EXPECT_EQ(state.current_backoff, std::chrono::milliseconds(250));
    EXPECT_FALSE(state.connection_failed);
}

TEST_F(RetryStateTest, DeadlineIsPreserved) {
    RetryPolicyConfig config;
    BasicProxyRetryPolicy<TestClock> policy(config);

    auto deadline = TestClock::now() + std::chrono::seconds(42);
    auto state = policy.make_initial_state(deadline);

    // Advance time but don't reach deadline
    TestClock::advance(std::chrono::seconds(41));
    EXPECT_FALSE(policy.is_deadline_exceeded(state));

    TestClock::advance(std::chrono::seconds(1));
    EXPECT_TRUE(policy.is_deadline_exceeded(state));
}

// =============================================================================
// Config Access Tests
// =============================================================================

class RetryPolicyConfigTest : public ::testing::Test {};

TEST_F(RetryPolicyConfigTest, ConfigIsAccessible) {
    RetryPolicyConfig config;
    config.max_retries = 7;
    config.initial_backoff = std::chrono::milliseconds{500};
    config.max_backoff = std::chrono::milliseconds{10000};
    config.backoff_multiplier = 1.5;
    config.max_fallback_attempts = 4;
    config.fallback_enabled = false;

    BasicProxyRetryPolicy<TestClock> policy(config);
    const auto& c = policy.config();

    EXPECT_EQ(c.max_retries, 7u);
    EXPECT_EQ(c.initial_backoff, std::chrono::milliseconds(500));
    EXPECT_EQ(c.max_backoff, std::chrono::milliseconds(10000));
    EXPECT_DOUBLE_EQ(c.backoff_multiplier, 1.5);
    EXPECT_EQ(c.max_fallback_attempts, 4u);
    EXPECT_FALSE(c.fallback_enabled);
}

// =============================================================================
// Stale Connection Retry Decision Tests
// =============================================================================

class StaleConnectionRetryTest : public ::testing::Test {};

TEST_F(StaleConnectionRetryTest, RetryTriggeredOnEmptyResponse) {
    // All conditions met: no errors, 0 bytes, budget remaining
    auto decision = should_retry_stale_connection(
        /*timed_out=*/false, /*connection_error=*/false,
        /*client_disconnected=*/false, /*connection_failed=*/false,
        /*bytes_written=*/0, /*attempt=*/0, /*max_retries=*/1);
    EXPECT_TRUE(decision.should_retry);
    EXPECT_STREQ(decision.reason, "empty response on pooled connection");
}

TEST_F(StaleConnectionRetryTest, NoRetryWhenDisabled) {
    auto decision = should_retry_stale_connection(
        false, false, false, false, 0, 0, /*max_retries=*/0);
    EXPECT_FALSE(decision.should_retry);
    EXPECT_STREQ(decision.reason, "stale retry disabled");
}

TEST_F(StaleConnectionRetryTest, NoRetryWhenTimedOut) {
    auto decision = should_retry_stale_connection(
        /*timed_out=*/true, false, false, false, 0, 0, 1);
    EXPECT_FALSE(decision.should_retry);
    EXPECT_STREQ(decision.reason, "request timed out");
}

TEST_F(StaleConnectionRetryTest, NoRetryWhenConnectionError) {
    auto decision = should_retry_stale_connection(
        false, /*connection_error=*/true, false, false, 0, 0, 1);
    EXPECT_FALSE(decision.should_retry);
    EXPECT_STREQ(decision.reason, "connection error occurred");
}

TEST_F(StaleConnectionRetryTest, NoRetryWhenClientDisconnected) {
    auto decision = should_retry_stale_connection(
        false, false, /*client_disconnected=*/true, false, 0, 0, 1);
    EXPECT_FALSE(decision.should_retry);
    EXPECT_STREQ(decision.reason, "client disconnected");
}

TEST_F(StaleConnectionRetryTest, NoRetryWhenConnectionFailed) {
    auto decision = should_retry_stale_connection(
        false, false, false, /*connection_failed=*/true, 0, 0, 1);
    EXPECT_FALSE(decision.should_retry);
    EXPECT_STREQ(decision.reason, "connection failed");
}

TEST_F(StaleConnectionRetryTest, NoRetryWhenDataWritten) {
    auto decision = should_retry_stale_connection(
        false, false, false, false, /*bytes_written=*/42, 0, 1);
    EXPECT_FALSE(decision.should_retry);
    EXPECT_STREQ(decision.reason, "data already written to client");
}

TEST_F(StaleConnectionRetryTest, NoRetryWhenBudgetExhausted) {
    auto decision = should_retry_stale_connection(
        false, false, false, false, 0, /*attempt=*/1, /*max_retries=*/1);
    EXPECT_FALSE(decision.should_retry);
    EXPECT_STREQ(decision.reason, "stale retry limit reached");
}

TEST_F(StaleConnectionRetryTest, RetryWithHigherBudget) {
    // With max_retries=3, attempt 0 should still retry
    auto decision = should_retry_stale_connection(
        false, false, false, false, 0, /*attempt=*/2, /*max_retries=*/3);
    EXPECT_TRUE(decision.should_retry);
}

TEST_F(StaleConnectionRetryTest, NoRetryAtExactBudgetLimit) {
    // attempt == max_retries should NOT retry
    auto decision = should_retry_stale_connection(
        false, false, false, false, 0, /*attempt=*/3, /*max_retries=*/3);
    EXPECT_FALSE(decision.should_retry);
    EXPECT_STREQ(decision.reason, "stale retry limit reached");
}
