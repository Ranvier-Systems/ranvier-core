// Ranvier Core - Circuit Breaker
//
// Implements the circuit breaker pattern for graceful degradation.
// Tracks backend failures and automatically stops routing to failing backends.
//
// Clock injection: Template parameter defaults to std::chrono::steady_clock
// for zero-overhead production use. Tests use TestClock for deterministic timing.
//
// States:
// - CLOSED: Normal operation, requests flow through
// - OPEN: Backend has failed too many times, requests are blocked
// - HALF_OPEN: Testing if backend has recovered, allowing one probe request

#pragma once

#include "types.hpp"

#include <chrono>
#include <unordered_map>

namespace ranvier {

// Circuit breaker state
enum class CircuitState {
    CLOSED,     // Normal operation
    OPEN,       // Blocking requests
    HALF_OPEN   // Testing recovery
};

// Per-backend circuit state
// Clock parameter allows injecting a test clock for deterministic timing
template<typename Clock = std::chrono::steady_clock>
struct BasicBackendCircuit {
    CircuitState state = CircuitState::CLOSED;
    uint32_t failure_count = 0;
    uint32_t success_count = 0;
    typename Clock::time_point last_failure_time;
    typename Clock::time_point opened_at;

    BasicBackendCircuit() = default;
};

// Backward-compatible alias: production code uses BackendCircuit unchanged
using BackendCircuit = BasicBackendCircuit<>;

// Thread-local circuit breaker (no locks needed in Seastar's shared-nothing model)
// Clock parameter allows injecting a test clock for deterministic timing.
// Production default (steady_clock) is resolved at compile time with zero overhead.
template<typename Clock = std::chrono::steady_clock>
class BasicCircuitBreaker {
    // Maximum number of unique circuits to track (Rule #4: bounded containers)
    // Prevents OOM from malicious/buggy backends flooding unique IDs
    static constexpr size_t MAX_CIRCUITS = 10000;

public:
    // Configuration for circuit breaker behavior
    struct Config {
        uint32_t failure_threshold = 5;           // Failures before opening circuit
        uint32_t success_threshold = 2;           // Successes in half-open to close
        std::chrono::seconds recovery_timeout{30}; // Time before trying half-open
        bool enabled = true;                       // Enable/disable circuit breaker
    };

    explicit BasicCircuitBreaker(Config config)
        : _config(std::move(config)) {}

    // Check if a request to this backend should be allowed
    // Returns true if allowed, false if circuit is open
    // Rule #4: Bounded container - fails open (allows request) if limit reached
    bool allow_request(BackendId backend_id) {
        if (!_config.enabled) {
            return true;
        }

        auto it = _circuits.find(backend_id);
        if (it == _circuits.end()) {
            // New circuit needed - check bounds (Rule #4)
            if (_circuits.size() >= MAX_CIRCUITS) {
                ++_circuits_overflow;
                return true;  // Fail-open: allow request when limit reached
            }
            // Create new circuit in CLOSED state
            _circuits.emplace(backend_id, BasicBackendCircuit<Clock>{});
            return true;  // New circuit starts CLOSED, allow request
        }

        auto& circuit = it->second;
        auto now = Clock::now();

        switch (circuit.state) {
            case CircuitState::CLOSED:
                return true;

            case CircuitState::OPEN:
                // Check if recovery timeout has passed
                if (now - circuit.opened_at >= _config.recovery_timeout) {
                    // Transition to half-open
                    circuit.state = CircuitState::HALF_OPEN;
                    circuit.success_count = 0;
                    return true;  // Allow probe request
                }
                return false;  // Still blocking

            case CircuitState::HALF_OPEN:
                // In half-open, we allow requests to test recovery
                return true;
        }

        return true;  // Default allow
    }

    // Record a successful request
    // Rule #4: Bounded container - skips if circuit doesn't exist and limit reached
    void record_success(BackendId backend_id) {
        if (!_config.enabled) {
            return;
        }

        auto it = _circuits.find(backend_id);
        if (it == _circuits.end()) {
            // Circuit doesn't exist - likely already at limit or never tracked
            // No need to create a new circuit just for success recording
            return;
        }

        auto& circuit = it->second;

        switch (circuit.state) {
            case CircuitState::CLOSED:
                // Reset failure count on success
                circuit.failure_count = 0;
                break;

            case CircuitState::HALF_OPEN:
                circuit.success_count++;
                if (circuit.success_count >= _config.success_threshold) {
                    // Backend recovered, close circuit
                    circuit.state = CircuitState::CLOSED;
                    circuit.failure_count = 0;
                    circuit.success_count = 0;
                }
                break;

            case CircuitState::OPEN:
                // Shouldn't happen, but reset if we somehow got a success
                circuit.state = CircuitState::CLOSED;
                circuit.failure_count = 0;
                break;
        }
    }

    // Record a failed request
    // Rule #4: Bounded container - skips if circuit doesn't exist and limit reached
    void record_failure(BackendId backend_id) {
        if (!_config.enabled) {
            return;
        }

        auto it = _circuits.find(backend_id);
        if (it == _circuits.end()) {
            // Circuit doesn't exist - check bounds before creating (Rule #4)
            if (_circuits.size() >= MAX_CIRCUITS) {
                ++_circuits_overflow;
                return;  // Skip: cannot track new circuit when limit reached
            }
            // Create new circuit and record the failure
            it = _circuits.emplace(backend_id, BasicBackendCircuit<Clock>{}).first;
        }

        auto& circuit = it->second;
        auto now = Clock::now();

        circuit.failure_count++;
        circuit.last_failure_time = now;

        switch (circuit.state) {
            case CircuitState::CLOSED:
                if (circuit.failure_count >= _config.failure_threshold) {
                    // Open the circuit
                    circuit.state = CircuitState::OPEN;
                    circuit.opened_at = now;
                }
                break;

            case CircuitState::HALF_OPEN:
                // Probe failed, back to open
                circuit.state = CircuitState::OPEN;
                circuit.opened_at = now;
                circuit.success_count = 0;
                break;

            case CircuitState::OPEN:
                // Already open, just update timing
                break;
        }
    }

    // Get circuit state for a backend (for monitoring/logging)
    CircuitState get_state(BackendId backend_id) const {
        auto it = _circuits.find(backend_id);
        if (it == _circuits.end()) {
            return CircuitState::CLOSED;
        }
        return it->second.state;
    }

    // Get failure count for a backend
    uint32_t get_failure_count(BackendId backend_id) const {
        auto it = _circuits.find(backend_id);
        if (it == _circuits.end()) {
            return 0;
        }
        return it->second.failure_count;
    }

    // Check if backend circuit is open
    bool is_open(BackendId backend_id) const {
        auto it = _circuits.find(backend_id);
        if (it == _circuits.end()) {
            return false;
        }
        return it->second.state == CircuitState::OPEN;
    }

    // Reset circuit for a backend (e.g., after manual intervention)
    void reset(BackendId backend_id) {
        _circuits.erase(backend_id);
    }

    // Remove circuit entry when backend is deregistered (Rule #4: cleanup for bounded container)
    // This prevents dead backend circuits from consuming slots until MAX_CIRCUITS is reached.
    // Must be called from the shard that owns this CircuitBreaker instance.
    void remove_circuit(BackendId backend_id) {
        auto it = _circuits.find(backend_id);
        if (it != _circuits.end()) {
            _circuits.erase(it);
            ++_circuits_removed;
        }
    }

    // Reset all circuits
    void reset_all() {
        _circuits.clear();
    }

    // Get number of open circuits (for monitoring)
    size_t open_circuit_count() const {
        size_t count = 0;
        for (const auto& [id, circuit] : _circuits) {
            if (circuit.state == CircuitState::OPEN) {
                count++;
            }
        }
        return count;
    }

    bool is_enabled() const { return _config.enabled; }

    const Config& config() const { return _config; }

    // Get overflow count for circuit limit (for monitoring)
    uint64_t get_circuits_overflow() const { return _circuits_overflow; }

    // Get count of circuits removed when backends were deregistered (for monitoring)
    uint64_t get_circuits_removed() const { return _circuits_removed; }

    // Hot-reload: Update configuration at runtime
    void update_config(const Config& config) {
        _config = config;
        // Note: Existing circuit states are preserved
    }

private:
    Config _config;
    std::unordered_map<BackendId, BasicBackendCircuit<Clock>> _circuits;
    uint64_t _circuits_overflow = 0;  // Times circuit limit was hit (Rule #4)
    uint64_t _circuits_removed = 0;   // Circuits cleaned up when backends deregistered (Rule #4)
};

// Backward-compatible alias: production code uses CircuitBreaker unchanged
using CircuitBreaker = BasicCircuitBreaker<>;

// Convert circuit state to string for logging
inline const char* circuit_state_to_string(CircuitState state) {
    switch (state) {
        case CircuitState::CLOSED: return "CLOSED";
        case CircuitState::OPEN: return "OPEN";
        case CircuitState::HALF_OPEN: return "HALF_OPEN";
        default: return "UNKNOWN";
    }
}

}  // namespace ranvier
