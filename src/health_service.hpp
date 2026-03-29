#pragma once
#include <chrono>
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include "backend_registry.hpp"

namespace ranvier {

// Health check configuration
struct HealthServiceConfig {
    std::chrono::seconds check_interval{5};
    std::chrono::seconds check_timeout{3};
    uint32_t failure_threshold = 3;
    uint32_t recovery_threshold = 2;
};

class HealthService {
public:
    HealthService(BackendRegistry& registry, HealthServiceConfig config = {});

    // Start the background loop
    void start();

    // Stop cleanly
    seastar::future<> stop();

private:
    BackendRegistry& _registry;
    HealthServiceConfig _config;
    seastar::gate _gate; // Prevents shutdown while checking
    bool _running = false;

    // Track the background loop future for clean shutdown (Rule #5)
    seastar::future<> _loop_future = seastar::make_ready_future<>();

    // The main loop
    seastar::future<> run_loop();

    // Check a single host
    seastar::future<bool> check_backend(seastar::socket_address addr);
};

}  // namespace ranvier
