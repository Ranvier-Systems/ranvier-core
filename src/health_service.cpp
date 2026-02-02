#include "health_service.hpp"
#include "logging.hpp"

#include <seastar/core/sleep.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/net/api.hh>

using namespace seastar;

namespace ranvier {

HealthService::HealthService(RouterService& router, HealthServiceConfig config)
    : _router(router), _config(config) {}

void HealthService::start() {
    _running = true;
    // Launch the loop in the background (fire and forget, but tracked by gate)
    (void)run_loop();
}

future<> HealthService::stop() {
    _running = false;
    return _gate.close(); // Wait for current checks to finish
}

future<> HealthService::run_loop() {
    // Only run on Core 0 to avoid DDOSing backends
    if (this_shard_id() != 0) co_return;

    try {
        while (_running) {
            // 1. Enter Gate (So we don't crash on shutdown)
            auto holder = _gate.hold();

            // 2. Get list of backends
            auto ids = _router.get_all_backend_ids();

            for (auto id : ids) {
                // We must use the Router to resolve the IP (RouterService is thread-safe for reads)
                auto addr_opt = _router.get_backend_address(id);
                if (!addr_opt.has_value()) continue; // Already marked dead or missing?

                // 3. Check Health (TCP Connect)
                bool is_alive = co_await check_backend(addr_opt.value());

                // 4. Update State (Broadcasts to all cores)
                // Note: In a real system, you'd only broadcast on CHANGE.
                // For simplicity/logging, we update blindly or let Router dedup.
                co_await _router.set_backend_status_global(id, is_alive);
            }

            // 5. Sleep for configured interval
            co_await seastar::sleep(_config.check_interval);
        }
    } catch (const seastar::gate_closed_exception&) {
        // Expected during shutdown - gate closed while loop was running
        log_health.debug("Health check loop exiting: gate closed during shutdown");
    } catch (const std::exception& e) {
        // Rule #9: Log unexpected exceptions at warn level
        log_health.warn("Health check loop exiting unexpectedly: {}", e.what());
    } catch (...) {
        // Rule #9: Log unknown exceptions at warn level
        log_health.warn("Health check loop exiting: unknown exception");
    }
}

future<bool> HealthService::check_backend(socket_address addr) {
    // Use configured timeout for health check connections
    auto deadline = lowres_clock::now() + _config.check_timeout;

    return with_timeout(deadline, seastar::connect(addr))
        .then([](seastar::connected_socket fd) {
            // Success - connection will be closed when fd goes out of scope
            return true;
        })
        .handle_exception([](auto ep) {
            // Timeout, connection refused, network error, etc.
            return false;
        });
}

} // namespace ranvier
