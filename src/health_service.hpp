#pragma once
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include "router_service.hpp"

namespace ranvier {

class HealthService {
public:
    HealthService(RouterService& router);

    // Start the background loop
    void start();

    // Stop cleanly
    seastar::future<> stop();

private:
    RouterService& _router;
    seastar::gate _gate; // Prevents shutdown while checking
    bool _running = false;

    // The main loop
    seastar::future<> run_loop();

    // Check a single host
    seastar::future<bool> check_backend(seastar::socket_address addr);
};

}
