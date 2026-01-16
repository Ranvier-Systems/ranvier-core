// Ranvier Core - Circuit Breaker Unit Tests
//
// Tests for the circuit breaker pattern implementation with MAX_CIRCUITS bound (Hard Rule #4).
// Includes tests for the new remove_circuit() functionality for cleanup when backends
// are deregistered.

#include "circuit_breaker.hpp"
#include <gtest/gtest.h>
#include <thread>
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
    void SetUp() override {
        config.failure_threshold = 3;
        config.success_threshold = 2;
        config.recovery_timeout = std::chrono::seconds(1);
        config.enabled = true;
    }

    CircuitBreaker::Config config;
};

TEST_F(CircuitBreakerTest, DisabledBreakerAlwaysAllows) {
    config.enabled = false;
    CircuitBreaker cb(config);

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
    CircuitBreaker cb(config);
    BackendId backend = 1;

    EXPECT_TRUE(cb.allow_request(backend));
    EXPECT_EQ(cb.get_state(backend), CircuitState::CLOSED);
    EXPECT_EQ(cb.get_failure_count(backend), 0u);
}

TEST_F(CircuitBreakerTest, CircuitOpensAfterFailureThreshold) {
    CircuitBreaker cb(config);
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
    CircuitBreaker cb(config);
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
    CircuitBreaker cb(config);
    BackendId backend = 1;

    cb.allow_request(backend);

    // Open the circuit
    for (uint32_t i = 0; i < config.failure_threshold; ++i) {
        cb.record_failure(backend);
    }
    EXPECT_EQ(cb.get_state(backend), CircuitState::OPEN);

    // Wait for recovery timeout
    std::this_thread::sleep_for(config.recovery_timeout + std::chrono::milliseconds(100));

    // Next request should transition to half-open and be allowed
    EXPECT_TRUE(cb.allow_request(backend));
    EXPECT_EQ(cb.get_state(backend), CircuitState::HALF_OPEN);
}

TEST_F(CircuitBreakerTest, HalfOpenClosesAfterSuccessThreshold) {
    CircuitBreaker cb(config);
    BackendId backend = 1;

    cb.allow_request(backend);

    // Open the circuit
    for (uint32_t i = 0; i < config.failure_threshold; ++i) {
        cb.record_failure(backend);
    }

    // Wait and transition to half-open
    std::this_thread::sleep_for(config.recovery_timeout + std::chrono::milliseconds(100));
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
    CircuitBreaker cb(config);
    BackendId backend = 1;

    cb.allow_request(backend);

    // Open the circuit
    for (uint32_t i = 0; i < config.failure_threshold; ++i) {
        cb.record_failure(backend);
    }

    // Wait and transition to half-open
    std::this_thread::sleep_for(config.recovery_timeout + std::chrono::milliseconds(100));
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
    CircuitBreaker cb(config);
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
    CircuitBreaker cb(config);

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
    void SetUp() override {
        config.failure_threshold = 3;
        config.success_threshold = 2;
        config.recovery_timeout = std::chrono::seconds(30);
        config.enabled = true;
    }

    CircuitBreaker::Config config;
};

TEST_F(CircuitBreakerRemoveTest, RemoveCircuitDeletesEntry) {
    CircuitBreaker cb(config);
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
    CircuitBreaker cb(config);
    BackendId backend = 42;

    EXPECT_EQ(cb.get_circuits_removed(), 0u);

    // Create and remove circuit
    cb.allow_request(backend);
    cb.remove_circuit(backend);

    EXPECT_EQ(cb.get_circuits_removed(), 1u);
}

TEST_F(CircuitBreakerRemoveTest, RemoveNonExistentCircuitDoesNotIncrementCounter) {
    CircuitBreaker cb(config);
    BackendId backend = 999;

    EXPECT_EQ(cb.get_circuits_removed(), 0u);

    // Remove non-existent circuit
    cb.remove_circuit(backend);

    // Counter should not increment for non-existent circuit
    EXPECT_EQ(cb.get_circuits_removed(), 0u);
}

TEST_F(CircuitBreakerRemoveTest, RemoveOpenCircuit) {
    CircuitBreaker cb(config);
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
    CircuitBreaker cb(config);

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
    CircuitBreaker cb(config);

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
    CircuitBreaker cb(config);

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
    CircuitBreaker cb(config);

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
    CircuitBreaker cb(config);

    // Remove some circuits
    cb.allow_request(1);
    cb.remove_circuit(1);
    cb.allow_request(2);
    cb.remove_circuit(2);
    EXPECT_EQ(cb.get_circuits_removed(), 2u);

    // Update config
    CircuitBreaker::Config new_config = config;
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
