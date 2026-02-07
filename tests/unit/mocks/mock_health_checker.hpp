// Ranvier Core - Google Mock for HealthChecker
//
// Abstract HealthChecker interface and GMock mock for dependency isolation.
//
// Production code uses HealthService (src/health_service.hpp), which is a
// concrete class tightly coupled to Seastar networking (seastar::connect,
// seastar::socket_address). This interface extracts the health-checking
// contract so that consumers can be tested without real network connections.
//
// Future refactoring: HealthService could implement this interface to enable
// full dependency injection in production code.
//
// Usage:
//   #include "mocks/mock_health_checker.hpp"
//   MockHealthChecker checker;
//   EXPECT_CALL(checker, check_backend("10.0.0.1", 8080))
//       .WillOnce(Return(true));

#pragma once

#include "types.hpp"
#include <gmock/gmock.h>
#include <cstdint>
#include <string>

namespace ranvier {

// Abstract interface for backend health checking.
// Decouples health-check consumers from Seastar networking.
class HealthChecker {
public:
    virtual ~HealthChecker() = default;

    // Check if a backend at the given host:port is alive.
    // Returns true if the backend responded within the timeout.
    virtual bool check_backend(const std::string& host, uint16_t port) = 0;

    // Check if a backend identified by BackendId is healthy.
    // Returns true if the backend is reachable and responding.
    virtual bool is_healthy(BackendId id) = 0;
};

class MockHealthChecker : public HealthChecker {
public:
    MOCK_METHOD(bool, check_backend,
                (const std::string& host, uint16_t port),
                (override));
    MOCK_METHOD(bool, is_healthy, (BackendId id), (override));
};

}  // namespace ranvier
