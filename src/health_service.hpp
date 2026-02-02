#pragma once
#include <chrono>
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include "router_service.hpp"

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
    HealthService(RouterService& router, HealthServiceConfig config = {});

    // Start the background loop
    void start();

    // Stop cleanly
    seastar::future<> stop();

private:
    RouterService& _router;
    HealthServiceConfig _config;
    seastar::gate _gate; // Prevents shutdown while checking
    bool _running = false;

    // The main loop
    seastar::future<> run_loop();

    // Check a single host
    seastar::future<bool> check_backend(seastar::socket_address addr);
};

}  // namespace ranvier
