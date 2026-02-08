// Ranvier Core - Proxy Retry Policy
//
// Pure retry/backoff/fallback decision logic extracted from HttpController.
// No Seastar dependencies — all decisions are pure functions or template-injected.
//
// Clock injection: Template parameter defaults to std::chrono::steady_clock
// for zero-overhead production use. Tests use TestClock for deterministic timing.
//
// Extracted components:
// - ConnectionErrorType: classify connection errors from std::system_error
// - ProxyRetryPolicy: retry count, exponential backoff, fallback decisions
// - select_fallback_backend: pure fallback selection over a candidate list
// - BackpressurePolicy: concurrency and queue-depth rejection decisions

#pragma once

#include "types.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <system_error>
#include <vector>

// EPIPE / ECONNRESET constants
#include <cerrno>

namespace ranvier {

// ---------------------------------------------------------------------------
// Connection Error Classification
// ---------------------------------------------------------------------------

// Classifies connection-level errors for retry decisions.
// These error types indicate the backend closed the connection.
enum class ConnectionErrorType {
    NONE,
    BROKEN_PIPE,      // EPIPE - write to closed socket
    CONNECTION_RESET   // ECONNRESET - connection reset by peer
};

// Classify an exception_ptr as a connection error type.
// Returns NONE for non-connection errors (including non-system_error exceptions).
// This is a classifier, not an error handler — callers log as appropriate.
inline ConnectionErrorType classify_connection_error(std::exception_ptr ep) {
    if (!ep) return ConnectionErrorType::NONE;

    try {
        std::rethrow_exception(ep);
    } catch (const std::system_error& e) {
        if (e.code() == std::errc::broken_pipe ||
            e.code().value() == EPIPE) {
            return ConnectionErrorType::BROKEN_PIPE;
        }
        if (e.code() == std::errc::connection_reset ||
            e.code().value() == ECONNRESET) {
            return ConnectionErrorType::CONNECTION_RESET;
        }
    } catch (...) {
        // Not a system_error — intentionally returns NONE.
        // Non-connection exceptions are handled by callers.
    }
    return ConnectionErrorType::NONE;
}

// Human-readable string for a ConnectionErrorType value.
inline const char* connection_error_to_string(ConnectionErrorType type) {
    switch (type) {
        case ConnectionErrorType::BROKEN_PIPE: return "broken pipe (EPIPE)";
        case ConnectionErrorType::CONNECTION_RESET: return "connection reset (ECONNRESET)";
        default: return "unknown";
    }
}

// Returns true if the error type represents a retryable connection failure.
inline bool is_retryable_connection_error(ConnectionErrorType type) {
    return type == ConnectionErrorType::BROKEN_PIPE ||
           type == ConnectionErrorType::CONNECTION_RESET;
}

// ---------------------------------------------------------------------------
// Proxy Retry Policy
// ---------------------------------------------------------------------------

// Action the caller should take after a connection failure.
enum class RetryAction {
    RETRY_SAME_BACKEND,    // Sleep for backoff, then retry same backend
    TRY_FALLBACK,          // Switch to a different backend immediately (no sleep)
    GIVE_UP_DEADLINE,      // Request deadline exceeded — stop trying
    GIVE_UP_MAX_RETRIES    // All retries and fallbacks exhausted
};

// Configuration for retry/backoff behavior.
// Mirrors RetrySettings from http_controller.hpp but is self-contained.
struct RetryPolicyConfig {
    uint32_t max_retries = 3;
    std::chrono::milliseconds initial_backoff{100};
    std::chrono::milliseconds max_backoff{5000};
    double backoff_multiplier = 2.0;
    uint32_t max_fallback_attempts = 3;
    bool fallback_enabled = true;
};

// Mutable state tracked across retry attempts.
// Created via ProxyRetryPolicy::make_initial_state().
template<typename Clock = std::chrono::steady_clock>
struct BasicRetryState {
    uint32_t retry_attempt = 0;
    uint32_t fallback_attempts = 0;
    std::chrono::milliseconds current_backoff{100};
    typename Clock::time_point deadline;
    bool connection_failed = false;
};

using RetryState = BasicRetryState<>;

// Pure decision engine for proxy retry/backoff/fallback logic.
// Clock parameter allows injecting TestClock for deterministic tests.
// Production default (steady_clock) is resolved at compile time with zero overhead.
template<typename Clock = std::chrono::steady_clock>
class BasicProxyRetryPolicy {
public:
    explicit BasicProxyRetryPolicy(RetryPolicyConfig config)
        : _config(std::move(config)) {}

    // Create initial retry state for a new request.
    BasicRetryState<Clock> make_initial_state(typename Clock::time_point deadline) const {
        BasicRetryState<Clock> state;
        state.current_backoff = _config.initial_backoff;
        state.deadline = deadline;
        return state;
    }

    // Check if the request deadline has been exceeded.
    bool is_deadline_exceeded(const BasicRetryState<Clock>& state) const {
        return Clock::now() >= state.deadline;
    }

    // Main decision function: given a connection failure, decide what to do next.
    //
    // Parameters:
    //   state - mutable retry state (updated in place)
    //   fallback_candidate_available - true if get_fallback_backend() would return a value
    //
    // Returns the action the caller should take.
    //
    // Logic mirrors establish_backend_connection() from http_controller.cpp:
    //   1. Deadline check → GIVE_UP_DEADLINE
    //   2. Fallback attempt (if enabled, under limit, candidate available) → TRY_FALLBACK
    //      - Fallback does NOT consume a retry attempt
    //   3. Retry same backend with backoff (if retries remain) → RETRY_SAME_BACKEND
    //   4. All options exhausted → GIVE_UP_MAX_RETRIES
    RetryAction decide_on_failure(BasicRetryState<Clock>& state,
                                  bool fallback_candidate_available) const {
        // 1. Deadline exceeded
        if (is_deadline_exceeded(state)) {
            state.connection_failed = true;
            return RetryAction::GIVE_UP_DEADLINE;
        }

        // 2. Try fallback to different backend
        if (_config.fallback_enabled &&
            fallback_candidate_available &&
            state.fallback_attempts < _config.max_fallback_attempts) {
            state.fallback_attempts++;
            return RetryAction::TRY_FALLBACK;
        }

        // 3. Retry same backend (with backoff)
        if (state.retry_attempt < _config.max_retries) {
            state.retry_attempt++;
            return RetryAction::RETRY_SAME_BACKEND;
        }

        // 4. Exhausted
        state.connection_failed = true;
        return RetryAction::GIVE_UP_MAX_RETRIES;
    }

    // Get the current backoff duration and advance to the next interval.
    // Call this when decide_on_failure returns RETRY_SAME_BACKEND.
    std::chrono::milliseconds consume_backoff(BasicRetryState<Clock>& state) const {
        auto current = state.current_backoff;
        auto next = std::chrono::milliseconds(
            static_cast<int64_t>(state.current_backoff.count() * _config.backoff_multiplier));
        state.current_backoff = std::min(next, _config.max_backoff);
        return current;
    }

    // Check if the retry loop should continue (used as loop condition).
    // Returns false when retries are exhausted or deadline has passed.
    bool should_continue(const BasicRetryState<Clock>& state) const {
        return !state.connection_failed &&
               state.retry_attempt <= _config.max_retries &&
               !is_deadline_exceeded(state);
    }

    const RetryPolicyConfig& config() const { return _config; }

private:
    RetryPolicyConfig _config;
};

// Backward-compatible alias for production code
using ProxyRetryPolicy = BasicProxyRetryPolicy<>;

// ---------------------------------------------------------------------------
// Fallback Backend Selection
// ---------------------------------------------------------------------------

// Select a fallback backend from a list of candidates.
// Pure function — no side effects, no Seastar dependency.
//
// Parameters:
//   all_backends - complete list of known backend IDs
//   failed_id    - the backend that just failed (excluded from selection)
//   allow        - predicate returning true if a backend is eligible (e.g., circuit breaker check)
//
// Returns the first eligible backend, or nullopt if none available.
template<typename AllowPredicate>
std::optional<BackendId> select_fallback_backend(
    const std::vector<BackendId>& all_backends,
    BackendId failed_id,
    AllowPredicate allow) {
    for (auto id : all_backends) {
        if (id != failed_id && allow(id)) {
            return id;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Backpressure Policy
// ---------------------------------------------------------------------------

// Backpressure decision logic — pure functions, no Seastar dependency.
// These model the concurrency-limiting and queue-depth checks from handle_proxy().

struct BackpressureConfig {
    size_t max_concurrent_requests = 1000;       // 0 = unlimited
    bool enable_persistence_backpressure = true;
    double persistence_queue_threshold = 0.8;    // Start throttling at 80%
    uint32_t retry_after_seconds = 1;
};

// Result of a backpressure check.
enum class BackpressureDecision {
    ACCEPT,                     // Request may proceed
    REJECT_CONCURRENCY,         // At concurrency limit
    REJECT_PERSISTENCE_QUEUE    // Persistence queue too deep
};

// Check whether to accept or reject based on concurrency limit.
// current_active: number of currently in-flight requests on this shard.
// Returns ACCEPT if under limit (or limit is 0/unlimited), REJECT_CONCURRENCY otherwise.
inline BackpressureDecision check_concurrency(
    size_t current_active,
    size_t max_concurrent) {
    if (max_concurrent == 0) {
        return BackpressureDecision::ACCEPT;  // Unlimited
    }
    if (current_active >= max_concurrent) {
        return BackpressureDecision::REJECT_CONCURRENCY;
    }
    return BackpressureDecision::ACCEPT;
}

// Check whether to reject based on persistence queue depth.
// queue_depth: current number of pending persistence writes.
// max_queue_depth: maximum configured queue depth.
// threshold: ratio (0.0–1.0) at which to start rejecting.
// enabled: whether persistence backpressure is enabled at all.
inline BackpressureDecision check_persistence_backpressure(
    size_t queue_depth,
    size_t max_queue_depth,
    double threshold,
    bool enabled) {
    if (!enabled || max_queue_depth == 0) {
        return BackpressureDecision::ACCEPT;
    }
    double ratio = static_cast<double>(queue_depth) / static_cast<double>(max_queue_depth);
    if (ratio >= threshold) {
        return BackpressureDecision::REJECT_PERSISTENCE_QUEUE;
    }
    return BackpressureDecision::ACCEPT;
}

}  // namespace ranvier
