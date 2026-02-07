// Ranvier Core - Circuit Breaker Unit Tests
//
// Tests for the circuit breaker pattern implementation with MAX_CIRCUITS bound (Hard Rule #4).
// Includes deterministic timing tests using TestClock for OPEN→HALF_OPEN transitions.
// Includes tests for the remove_circuit() functionality for cleanup when backends
// are deregistered.

#include "circuit_breaker.hpp"
#include "test_clock.hpp"
#include <gtest/gtest.h>
#include <chrono>

using namespace ranvier;

// =============================================================================
// CircuitBreaker Configuration Tests
// =============================================================================

class CircuitBreakerConfigTest : public ::testing::Test {
protected:
    CircuitBreaker::Config config;
};

TEST_F(CircuitBreakerConfigTest, DefaultConfigValues) {
    EXPECT_EQ(config.failure_threshold, 5u);
    EXPECT_EQ(config.success_threshold, 2u);
    EXPECT_EQ(config.recovery_timeout, std::chrono::seconds(30));
    EXPECT_TRUE(config.enabled);
}

TEST_F(CircuitBreakerConfigTest, ConfigCanBeCustomized) {
    config.failure_threshold = 10;
    config.success_threshold = 3;
    config.recovery_timeout = std::chrono::seconds(60);
    config.enabled = false;

    EXPECT_EQ(config.failure_threshold, 10u);
    EXPECT_EQ(config.success_threshold, 3u);
    EXPECT_EQ(config.recovery_timeout, std::chrono::seconds(60));
    EXPECT_FALSE(config.enabled);
}

// =============================================================================
// CircuitBreaker Basic Behavior Tests
// =============================================================================

class CircuitBreakerTest : public ::testing::Test {
protected:
    using TestCB = BasicCircuitBreaker<TestClock>;

    void SetUp() override {
        TestClock::reset();
        config.failure_threshold = 3;
        config.success_threshold = 2;
        config.recovery_timeout = std::chrono::seconds(1);
        config.enabled = true;
    }

    TestCB::Config config;
};

TEST_F(CircuitBreakerTest, DisabledBreakerAlwaysAllows) {
    config.enabled = false;
    TestCB cb(config);

    BackendId backend = 1;

    // Record many failures
    for (int i = 0; i < 100; ++i) {
        cb.record_failure(backend);
    }

    // Should still allow requests when disabled
    EXPECT_TRUE(cb.allow_request(backend));
    EXPECT_EQ(cb.get_state(backend), CircuitState::CLOSED);
}

TEST_F(CircuitBreakerTest, NewBackendStartsClosed) {
    TestCB cb(config);
    BackendId backend = 1;

    EXPECT_TRUE(cb.allow_request(backend));
    EXPECT_EQ(cb.get_state(backend), CircuitState::CLOSED);
    EXPECT_EQ(cb.get_failure_count(backend), 0u);
}

TEST_F(CircuitBreakerTest, CircuitOpensAfterFailureThreshold) {
    TestCB cb(config);
    BackendId backend = 1;

    // First request creates circuit
    EXPECT_TRUE(cb.allow_request(backend));

    // Record failures up to threshold
    for (uint32_t i = 0; i < config.failure_threshold; ++i) {
        cb.record_failure(backend);
    }

    // Circuit should now be open
    EXPECT_EQ(cb.get_state(backend), CircuitState::OPEN);
    EXPECT_FALSE(cb.allow_request(backend));
}

TEST_F(CircuitBreakerTest, SuccessResetsFailureCount) {
    TestCB cb(config);
    BackendId backend = 1;

    cb.allow_request(backend);

    // Record some failures (but not enough to open)
    cb.record_failure(backend);
    cb.record_failure(backend);
    EXPECT_EQ(cb.get_failure_count(backend), 2u);

    // Success resets failure count
    cb.record_success(backend);
    EXPECT_EQ(cb.get_failure_count(backend), 0u);
    EXPECT_EQ(cb.get_state(backend), CircuitState::CLOSED);
}

TEST_F(CircuitBreakerTest, CircuitTransitionsToHalfOpenAfterTimeout) {
    TestCB cb(config);
    BackendId backend = 1;

    cb.allow_request(backend);

    // Open the circuit
    for (uint32_t i = 0; i < config.failure_threshold; ++i) {
        cb.record_failure(backend);
    }
    EXPECT_EQ(cb.get_state(backend), CircuitState::OPEN);

    // Advance time past recovery timeout (deterministic, no sleep)
    TestClock::advance(config.recovery_timeout + std::chrono::milliseconds(1));

    // Next request should transition to half-open and be allowed
    EXPECT_TRUE(cb.allow_request(backend));
    EXPECT_EQ(cb.get_state(backend), CircuitState::HALF_OPEN);
}

TEST_F(CircuitBreakerTest, HalfOpenClosesAfterSuccessThreshold) {
    TestCB cb(config);
    BackendId backend = 1;

    cb.allow_request(backend);

    // Open the circuit
    for (uint32_t i = 0; i < config.failure_threshold; ++i) {
        cb.record_failure(backend);
    }

    // Advance past timeout and transition to half-open
    TestClock::advance(config.recovery_timeout + std::chrono::milliseconds(1));
    cb.allow_request(backend);
    EXPECT_EQ(cb.get_state(backend), CircuitState::HALF_OPEN);

    // Record successes up to threshold
    for (uint32_t i = 0; i < config.success_threshold; ++i) {
        cb.record_success(backend);
    }

    // Circuit should now be closed
    EXPECT_EQ(cb.get_state(backend), CircuitState::CLOSED);
}

TEST_F(CircuitBreakerTest, HalfOpenReopensOnFailure) {
    TestCB cb(config);
    BackendId backend = 1;

    cb.allow_request(backend);

    // Open the circuit
    for (uint32_t i = 0; i < config.failure_threshold; ++i) {
        cb.record_failure(backend);
    }

    // Advance past timeout and transition to half-open
    TestClock::advance(config.recovery_timeout + std::chrono::milliseconds(1));
    cb.allow_request(backend);
    EXPECT_EQ(cb.get_state(backend), CircuitState::HALF_OPEN);

    // Failure in half-open reopens circuit
    cb.record_failure(backend);
    EXPECT_EQ(cb.get_state(backend), CircuitState::OPEN);
}

// =============================================================================
// CircuitBreaker Reset Tests
// =============================================================================

TEST_F(CircuitBreakerTest, ResetClearsCircuit) {
    TestCB cb(config);
    BackendId backend = 1;

    cb.allow_request(backend);

    // Open the circuit
    for (uint32_t i = 0; i < config.failure_threshold; ++i) {
        cb.record_failure(backend);
    }
    EXPECT_EQ(cb.get_state(backend), CircuitState::OPEN);

    // Reset clears the circuit
    cb.reset(backend);
    EXPECT_EQ(cb.get_state(backend), CircuitState::CLOSED);  // New circuit, defaults to CLOSED
    EXPECT_EQ(cb.get_failure_count(backend), 0u);
}

TEST_F(CircuitBreakerTest, ResetAllClearsAllCircuits) {
    TestCB cb(config);

    // Create multiple circuits
    for (BackendId i = 1; i <= 5; ++i) {
        cb.allow_request(i);
        cb.record_failure(i);
    }

    cb.reset_all();

    // All circuits should be gone (defaults to CLOSED for non-existent)
    for (BackendId i = 1; i <= 5; ++i) {
        EXPECT_EQ(cb.get_state(i), CircuitState::CLOSED);
        EXPECT_EQ(cb.get_failure_count(i), 0u);
    }
}

// =============================================================================
// CircuitBreaker Remove Circuit Tests (Rule #4: Bounded Container Cleanup)
// =============================================================================

class CircuitBreakerRemoveTest : public ::testing::Test {
protected:
    using TestCB = BasicCircuitBreaker<TestClock>;

    void SetUp() override {
        TestClock::reset();
        config.failure_threshold = 3;
        config.success_threshold = 2;
        config.recovery_timeout = std::chrono::seconds(30);
        config.enabled = true;
    }

    TestCB::Config config;
};

TEST_F(CircuitBreakerRemoveTest, RemoveCircuitDeletesEntry) {
    TestCB cb(config);
    BackendId backend = 42;

    // Create circuit by allowing request
    EXPECT_TRUE(cb.allow_request(backend));
    cb.record_failure(backend);
    EXPECT_EQ(cb.get_failure_count(backend), 1u);

    // Remove the circuit
    cb.remove_circuit(backend);

    // Circuit should be gone (defaults to CLOSED for non-existent)
    EXPECT_EQ(cb.get_state(backend), CircuitState::CLOSED);
    EXPECT_EQ(cb.get_failure_count(backend), 0u);
}

TEST_F(CircuitBreakerRemoveTest, RemoveCircuitIncrementsCounter) {
    TestCB cb(config);
    BackendId backend = 42;

    EXPECT_EQ(cb.get_circuits_removed(), 0u);

    // Create and remove circuit
    cb.allow_request(backend);
    cb.remove_circuit(backend);

    EXPECT_EQ(cb.get_circuits_removed(), 1u);
}

TEST_F(CircuitBreakerRemoveTest, RemoveNonExistentCircuitDoesNotIncrementCounter) {
    TestCB cb(config);
    BackendId backend = 999;

    EXPECT_EQ(cb.get_circuits_removed(), 0u);

    // Remove non-existent circuit
    cb.remove_circuit(backend);

    // Counter should not increment for non-existent circuit
    EXPECT_EQ(cb.get_circuits_removed(), 0u);
}

TEST_F(CircuitBreakerRemoveTest, RemoveOpenCircuit) {
    TestCB cb(config);
    BackendId backend = 42;

    // Create and open circuit
    cb.allow_request(backend);
    for (uint32_t i = 0; i < config.failure_threshold; ++i) {
        cb.record_failure(backend);
    }
    EXPECT_EQ(cb.get_state(backend), CircuitState::OPEN);
    EXPECT_TRUE(cb.is_open(backend));

    // Remove the open circuit
    cb.remove_circuit(backend);

    // Circuit should be gone
    EXPECT_FALSE(cb.is_open(backend));
    EXPECT_EQ(cb.get_circuits_removed(), 1u);
}

TEST_F(CircuitBreakerRemoveTest, RemoveMultipleCircuits) {
    TestCB cb(config);

    // Create multiple circuits
    for (BackendId i = 1; i <= 5; ++i) {
        cb.allow_request(i);
    }

    // Remove all of them
    for (BackendId i = 1; i <= 5; ++i) {
        cb.remove_circuit(i);
    }

    EXPECT_EQ(cb.get_circuits_removed(), 5u);

    // All circuits should be gone
    for (BackendId i = 1; i <= 5; ++i) {
        EXPECT_EQ(cb.get_failure_count(i), 0u);
    }
}

TEST_F(CircuitBreakerRemoveTest, RemoveCircuitFreesSlotForNewCircuit) {
    // This test verifies that removing a circuit frees the slot,
    // allowing a new circuit to be tracked (important for Rule #4 cleanup)
    TestCB cb(config);

    BackendId old_backend = 1;
    BackendId new_backend = 2;

    // Create first circuit
    cb.allow_request(old_backend);
    cb.record_failure(old_backend);
    EXPECT_EQ(cb.get_failure_count(old_backend), 1u);

    // Remove it
    cb.remove_circuit(old_backend);

    // Create new circuit in freed slot
    cb.allow_request(new_backend);
    cb.record_failure(new_backend);
    EXPECT_EQ(cb.get_failure_count(new_backend), 1u);

    // Old circuit should still be gone
    EXPECT_EQ(cb.get_failure_count(old_backend), 0u);
}

// =============================================================================
// CircuitBreaker Monitoring Tests
// =============================================================================

TEST_F(CircuitBreakerRemoveTest, OpenCircuitCountAccurate) {
    TestCB cb(config);

    // Create some open circuits
    for (BackendId i = 1; i <= 3; ++i) {
        cb.allow_request(i);
        for (uint32_t j = 0; j < config.failure_threshold; ++j) {
            cb.record_failure(i);
        }
    }

    EXPECT_EQ(cb.open_circuit_count(), 3u);

    // Remove one open circuit
    cb.remove_circuit(1);
    EXPECT_EQ(cb.open_circuit_count(), 2u);
}

TEST_F(CircuitBreakerRemoveTest, CircuitsRemovedCounterPersistsAcrossOperations) {
    TestCB cb(config);

    // Create and remove circuits multiple times
    for (int round = 0; round < 3; ++round) {
        for (BackendId i = 1; i <= 5; ++i) {
            cb.allow_request(i);
        }
        for (BackendId i = 1; i <= 5; ++i) {
            cb.remove_circuit(i);
        }
    }

    // Counter should accumulate across all rounds
    EXPECT_EQ(cb.get_circuits_removed(), 15u);
}

// =============================================================================
// CircuitBreaker Hot-Reload Tests
// =============================================================================

TEST_F(CircuitBreakerRemoveTest, UpdateConfigPreservesRemovedCounter) {
    TestCB cb(config);

    // Remove some circuits
    cb.allow_request(1);
    cb.remove_circuit(1);
    cb.allow_request(2);
    cb.remove_circuit(2);
    EXPECT_EQ(cb.get_circuits_removed(), 2u);

    // Update config
    TestCB::Config new_config = config;
    new_config.failure_threshold = 10;
    cb.update_config(new_config);

    // Counter should be preserved
    EXPECT_EQ(cb.get_circuits_removed(), 2u);
    EXPECT_EQ(cb.config().failure_threshold, 10u);
}

// =============================================================================
// CircuitState String Conversion Tests
// =============================================================================

TEST(CircuitStateStringTest, ConvertsStatesCorrectly) {
    EXPECT_STREQ(circuit_state_to_string(CircuitState::CLOSED), "CLOSED");
    EXPECT_STREQ(circuit_state_to_string(CircuitState::OPEN), "OPEN");
    EXPECT_STREQ(circuit_state_to_string(CircuitState::HALF_OPEN), "HALF_OPEN");
}

// =============================================================================
// Deterministic Timing Tests (TestClock - no sleeps, instant execution)
// =============================================================================

class CircuitBreakerTimingTest : public ::testing::Test {
protected:
    using TestCB = BasicCircuitBreaker<TestClock>;

    void SetUp() override {
        TestClock::reset();
        config.failure_threshold = 3;
        config.success_threshold = 2;
        config.recovery_timeout = std::chrono::seconds(30);
        config.enabled = true;
    }

    TestCB::Config config;

    // Helper: open a circuit by recording enough failures
    void open_circuit(TestCB& cb, BackendId backend) {
        cb.allow_request(backend);
        for (uint32_t i = 0; i < config.failure_threshold; ++i) {
            cb.record_failure(backend);
        }
    }
};

TEST_F(CircuitBreakerTimingTest, OpenToHalfOpenAtExactTimeout) {
    TestCB cb(config);
    BackendId backend = 1;

    open_circuit(cb, backend);
    EXPECT_EQ(cb.get_state(backend), CircuitState::OPEN);

    // 1ms before timeout: still OPEN
    TestClock::advance(std::chrono::seconds(30) - std::chrono::milliseconds(1));
    EXPECT_FALSE(cb.allow_request(backend));
    EXPECT_EQ(cb.get_state(backend), CircuitState::OPEN);

    // Exactly at timeout: should transition to HALF_OPEN
    TestClock::advance(std::chrono::milliseconds(1));
    EXPECT_TRUE(cb.allow_request(backend));
    EXPECT_EQ(cb.get_state(backend), CircuitState::HALF_OPEN);
}

TEST_F(CircuitBreakerTimingTest, OpenStaysOpenBeforeTimeout) {
    TestCB cb(config);
    BackendId backend = 1;

    open_circuit(cb, backend);
    EXPECT_EQ(cb.get_state(backend), CircuitState::OPEN);

    // Advance multiple times within timeout window - should stay OPEN
    for (int i = 0; i < 29; ++i) {
        TestClock::advance(std::chrono::seconds(1));
        EXPECT_FALSE(cb.allow_request(backend));
        EXPECT_EQ(cb.get_state(backend), CircuitState::OPEN);
    }
}

TEST_F(CircuitBreakerTimingTest, HalfOpenToClosedFullCycle) {
    TestCB cb(config);
    BackendId backend = 1;

    // Phase 1: CLOSED -> OPEN
    open_circuit(cb, backend);
    EXPECT_EQ(cb.get_state(backend), CircuitState::OPEN);

    // Phase 2: Wait for timeout -> HALF_OPEN
    TestClock::advance(std::chrono::seconds(31));
    EXPECT_TRUE(cb.allow_request(backend));
    EXPECT_EQ(cb.get_state(backend), CircuitState::HALF_OPEN);

    // Phase 3: Successes -> CLOSED
    for (uint32_t i = 0; i < config.success_threshold; ++i) {
        cb.record_success(backend);
    }
    EXPECT_EQ(cb.get_state(backend), CircuitState::CLOSED);

    // Phase 4: Circuit works normally again
    EXPECT_TRUE(cb.allow_request(backend));
}

TEST_F(CircuitBreakerTimingTest, HalfOpenFailureResetsTimeout) {
    TestCB cb(config);
    BackendId backend = 1;

    open_circuit(cb, backend);

    // Advance past first timeout -> HALF_OPEN
    TestClock::advance(std::chrono::seconds(31));
    cb.allow_request(backend);
    EXPECT_EQ(cb.get_state(backend), CircuitState::HALF_OPEN);

    // Failure in HALF_OPEN -> back to OPEN (resets opened_at)
    cb.record_failure(backend);
    EXPECT_EQ(cb.get_state(backend), CircuitState::OPEN);

    // Must wait another full recovery_timeout from NOW
    TestClock::advance(std::chrono::seconds(29));
    EXPECT_FALSE(cb.allow_request(backend));  // Still OPEN (29s < 30s)

    TestClock::advance(std::chrono::seconds(2));  // Now 31s past re-open
    EXPECT_TRUE(cb.allow_request(backend));
    EXPECT_EQ(cb.get_state(backend), CircuitState::HALF_OPEN);
}

TEST_F(CircuitBreakerTimingTest, DifferentBackendsHaveIndependentTimers) {
    TestCB cb(config);
    BackendId backend_a = 1;
    BackendId backend_b = 2;

    // Open both circuits
    open_circuit(cb, backend_a);

    TestClock::advance(std::chrono::seconds(10));
    open_circuit(cb, backend_b);

    // After 20 more seconds: backend_a opened 30s ago, backend_b opened 20s ago
    TestClock::advance(std::chrono::seconds(20));

    // backend_a should transition to HALF_OPEN (30s elapsed)
    EXPECT_TRUE(cb.allow_request(backend_a));
    EXPECT_EQ(cb.get_state(backend_a), CircuitState::HALF_OPEN);

    // backend_b should still be OPEN (only 20s elapsed)
    EXPECT_FALSE(cb.allow_request(backend_b));
    EXPECT_EQ(cb.get_state(backend_b), CircuitState::OPEN);

    // Advance 10 more seconds: backend_b now at 30s
    TestClock::advance(std::chrono::seconds(10));
    EXPECT_TRUE(cb.allow_request(backend_b));
    EXPECT_EQ(cb.get_state(backend_b), CircuitState::HALF_OPEN);
}

TEST_F(CircuitBreakerTimingTest, ConfigurableRecoveryTimeout) {
    config.recovery_timeout = std::chrono::seconds(5);
    TestCB cb(config);
    BackendId backend = 1;

    open_circuit(cb, backend);

    // Should be OPEN at 4 seconds
    TestClock::advance(std::chrono::seconds(4));
    EXPECT_FALSE(cb.allow_request(backend));
    EXPECT_EQ(cb.get_state(backend), CircuitState::OPEN);

    // Should transition at 5 seconds
    TestClock::advance(std::chrono::seconds(1));
    EXPECT_TRUE(cb.allow_request(backend));
    EXPECT_EQ(cb.get_state(backend), CircuitState::HALF_OPEN);
}

TEST_F(CircuitBreakerTimingTest, MultipleOpenCloseReopenCycles) {
    config.recovery_timeout = std::chrono::seconds(10);
    TestCB cb(config);
    BackendId backend = 1;

    for (int cycle = 0; cycle < 3; ++cycle) {
        // Open the circuit
        open_circuit(cb, backend);
        EXPECT_EQ(cb.get_state(backend), CircuitState::OPEN);

        // Wait for timeout
        TestClock::advance(std::chrono::seconds(11));
        cb.allow_request(backend);
        EXPECT_EQ(cb.get_state(backend), CircuitState::HALF_OPEN);

        // Fail probe -> reopen
        cb.record_failure(backend);
        EXPECT_EQ(cb.get_state(backend), CircuitState::OPEN);
    }

    // Finally succeed
    TestClock::advance(std::chrono::seconds(11));
    cb.allow_request(backend);
    EXPECT_EQ(cb.get_state(backend), CircuitState::HALF_OPEN);

    for (uint32_t i = 0; i < config.success_threshold; ++i) {
        cb.record_success(backend);
    }
    EXPECT_EQ(cb.get_state(backend), CircuitState::CLOSED);
}
