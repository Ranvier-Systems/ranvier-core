#pragma once

#include <deque>
#include <memory>
#include <unordered_map>
#include <chrono>
#include <string>
#include <system_error>

#include <seastar/core/gate.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/timer.hh>
#include <seastar/net/api.hh>

#include "logging.hpp"

namespace ranvier {

// Configuration for connection pool behavior
struct ConnectionPoolConfig {
    size_t max_connections_per_host = 10;       // Max idle connections per backend
    std::chrono::seconds idle_timeout{60};      // Close connections idle longer than this
    size_t max_total_connections = 100;         // Max total idle connections across all backends
    std::chrono::seconds reaper_interval{15};   // How often to run the dead connection reaper
    bool tcp_keepalive = true;                  // Enable TCP keep-alives on connections
    std::chrono::seconds keepalive_idle{30};    // Time before first keepalive probe
    std::chrono::seconds keepalive_interval{10};// Interval between keepalive probes
    unsigned keepalive_count = 3;               // Number of failed probes before closing
    std::chrono::seconds max_connection_age{300}; // Max lifetime of a connection (5 min default)
};

// A bundle representing an active connection with timestamp tracking
struct ConnectionBundle {
    seastar::connected_socket fd;
    seastar::input_stream<char> in;
    seastar::output_stream<char> out;
    seastar::socket_address addr;
    bool is_valid = true;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_used;

    ConnectionBundle()
        : created_at(std::chrono::steady_clock::now())
        , last_used(std::chrono::steady_clock::now()) {}

    ConnectionBundle(seastar::connected_socket&& socket,
                     seastar::input_stream<char>&& input,
                     seastar::output_stream<char>&& output,
                     seastar::socket_address address)
        : fd(std::move(socket))
        , in(std::move(input))
        , out(std::move(output))
        , addr(address)
        , is_valid(true)
        , created_at(std::chrono::steady_clock::now())
        , last_used(std::chrono::steady_clock::now()) {}

    // Close the connection
    seastar::future<> close() {
        if (!is_valid) return seastar::make_ready_future<>();
        is_valid = false;
        return out.close().then([this] {
            return in.close();
        }).handle_exception([](auto) {}); // Ignore errors on close
    }

    // Check if connection has been idle too long
    bool is_expired(std::chrono::seconds timeout) const {
        auto now = std::chrono::steady_clock::now();
        return (now - last_used) > timeout;
    }

    // Check if connection has exceeded its maximum age (TTL)
    // Returns true if connection should be closed regardless of recent activity
    bool is_too_old(std::chrono::seconds max_age) const {
        auto now = std::chrono::steady_clock::now();
        return (now - created_at) > max_age;
    }

    // Update last used timestamp
    void touch() {
        last_used = std::chrono::steady_clock::now();
    }

    // Check if connection received FIN from backend (half-open detection)
    // This is a synchronous check that inspects the input stream's EOF status
    bool is_half_open() const {
        // A connection is considered half-open if eof() returns true
        // This means the backend has closed its side of the connection
        return in.eof();
    }
};

class ConnectionPool {
public:
    explicit ConnectionPool(ConnectionPoolConfig config = {})
        : _config(config)
        , _reaper_timer([this] { reap_dead_connections(); })
    {
        // Start the dead connection reaper timer
        if (_config.reaper_interval.count() > 0) {
            arm_reaper_timer();
        }
    }

    ~ConnectionPool() {
        // Note: stop() should be called before destruction to properly wait
        // for in-flight timer callbacks. Destructor cancels as a safety net.
        _reaper_timer.cancel();
    }

    // Stop the connection pool gracefully.
    // MUST be called before destruction to ensure timer callback safety.
    // Follows Rule #5: Timer callbacks need gate guards.
    //
    // Order is critical:
    //   1. Close _timer_gate (waits for in-flight callbacks to complete)
    //   2. Cancel timer (prevents future callbacks from being scheduled)
    //
    // This guarantees no timer callback can access `this` after stop() returns.
    seastar::future<> stop() {
        return _timer_gate.close().then([this] {
            _reaper_timer.cancel();
        });
    }

    // GET: Reuse existing or create new connection
    // request_id: Optional request ID for tracing (empty string if not tracing)
    // Performs liveness checks on pooled connections before returning them
    seastar::future<ConnectionBundle> get(seastar::socket_address addr,
                                           const std::string& request_id = "") {
        auto& pool = _pools[addr];

        // Try to find a valid, non-expired, live connection (LIFO for cache locality)
        while (!pool.empty()) {
            auto bundle = std::move(pool.back());
            pool.pop_back();
            _total_idle_connections--;

            // Skip expired connections (idle too long)
            if (bundle.is_expired(_config.idle_timeout)) {
                if (!request_id.empty()) {
                    log_pool.debug("[{}] Closing expired pooled connection to {}",
                                   request_id, addr);
                }
                _dead_connections_reaped++;
                // Move to heap to keep alive during async close
                close_bundle_async(std::move(bundle));
                continue;
            }

            // Skip connections that exceeded max age (TTL)
            if (bundle.is_too_old(_config.max_connection_age)) {
                if (!request_id.empty()) {
                    log_pool.debug("[{}] Closing max-age pooled connection to {} (created too long ago)",
                                   request_id, addr);
                }
                _dead_connections_reaped++;
                _connections_reaped_max_age++;
                // Move to heap to keep alive during async close
                close_bundle_async(std::move(bundle));
                continue;
            }

            // Zombie/half-open detection: check if backend sent FIN
            if (bundle.is_half_open()) {
                if (!request_id.empty()) {
                    log_pool.debug("[{}] Closing half-open connection to {} (backend closed)",
                                   request_id, addr);
                }
                _dead_connections_reaped++;
                // Move to heap to keep alive during async close
                close_bundle_async(std::move(bundle));
                continue;
            }

            // Connection passed liveness checks
            if (!request_id.empty()) {
                log_pool.debug("[{}] Reusing pooled connection to {}", request_id, addr);
            }
            bundle.touch();
            return seastar::make_ready_future<ConnectionBundle>(std::move(bundle));
        }

        // No pooled connection available, create new one
        if (!request_id.empty()) {
            log_pool.debug("[{}] Creating new connection to {}", request_id, addr);
        }
        return seastar::connect(addr).then([this, addr, request_id](seastar::connected_socket fd) {
            // Disable Nagle's algorithm for lower latency
            fd.set_nodelay(true);

            // Enable TCP keep-alives to detect stale network paths
            if (_config.tcp_keepalive) {
                try {
                    // Initialize the specific TCP parameters struct
                    seastar::net::tcp_keepalive_params tcp_params;
                    tcp_params.idle = _config.keepalive_idle;
                    tcp_params.interval = _config.keepalive_interval;
                    tcp_params.count = _config.keepalive_count;

                    // Assign the struct to the variant
                    seastar::net::keepalive_params params = tcp_params;

                    fd.set_keepalive(true);
                    fd.set_keepalive_parameters(params);

                    if (!request_id.empty()) {
                        log_pool.trace("[{}] TCP keepalive enabled for connection to {}", request_id, addr);
                    }
                } catch (const std::exception& e) {
                    // Log but don't fail - keepalive is a nice-to-have
                    log_pool.warn("Failed to set TCP keepalive for {}: {}", addr, e.what());
                }
            }

            auto in = fd.input();
            auto out = fd.output();
            return ConnectionBundle{std::move(fd), std::move(in), std::move(out), addr};
        });
    }

    // PUT: Return connection to pool
    // request_id: Optional request ID for tracing (empty string if not tracing)
    void put(ConnectionBundle&& bundle, const std::string& request_id = "") {
        if (!bundle.is_valid) {
            if (!request_id.empty()) {
                log_pool.debug("[{}] Closing invalid connection to {}",
                               request_id, bundle.addr);
            }
            close_bundle_async(std::move(bundle));
            return;
        }

        // Check for half-open before pooling - don't return dead connections
        if (bundle.is_half_open()) {
            if (!request_id.empty()) {
                log_pool.debug("[{}] Not pooling half-open connection to {}",
                               request_id, bundle.addr);
            }
            _dead_connections_reaped++;
            close_bundle_async(std::move(bundle));
            return;
        }

        bundle.touch();
        auto& pool = _pools[bundle.addr];

        // Check per-host limit
        if (pool.size() >= _config.max_connections_per_host) {
            // Pool full - evict oldest (front of deque)
            auto oldest = std::move(pool.front());
            pool.pop_front();
            _total_idle_connections--;
            // Close asynchronously, keeping bundle alive until close completes
            close_bundle_async(std::move(oldest));
        }

        // Check total limit
        if (_total_idle_connections >= _config.max_total_connections) {
            // Find and evict the globally oldest connection
            evict_oldest_global();
        }

        if (!request_id.empty()) {
            log_pool.debug("[{}] Returning connection to pool for {}",
                           request_id, bundle.addr);
        }
        pool.push_back(std::move(bundle));
        _total_idle_connections++;
    }

    // Get pool statistics
    struct Stats {
        size_t total_idle_connections;
        size_t num_backends;
        size_t max_per_host;
        size_t max_total;
        size_t dead_connections_reaped;      // Total dead connections detected and closed
        size_t connections_reaped_max_age;   // Connections closed due to exceeding max age
    };

    Stats stats() const {
        return Stats{
            _total_idle_connections,
            _pools.size(),
            _config.max_connections_per_host,
            _config.max_total_connections,
            _dead_connections_reaped,
            _connections_reaped_max_age
        };
    }

    // Clear all idle connections for a specific backend address
    // Call this when a backend is being removed to release resources
    size_t clear_pool(seastar::socket_address addr) {
        auto it = _pools.find(addr);
        if (it == _pools.end()) {
            return 0;
        }

        size_t closed = 0;
        auto& pool = it->second;
        for (auto& bundle : pool) {
            // Move each bundle to close asynchronously
            close_bundle_async(std::move(bundle));
            closed++;
        }
        _total_idle_connections -= pool.size();
        _pools.erase(it);

        if (closed > 0) {
            log_pool.info("Cleared {} pooled connections for removed backend {}", closed, addr);
        }
        return closed;
    }

    // Cleanup expired, max-age, and half-open connections (call periodically)
    // Rule #4: Erase empty map entries to prevent unbounded map growth
    size_t cleanup_expired() {
        size_t closed = 0;
        // Collect empty pools to erase after iteration (can't erase during range-for)
        std::vector<seastar::socket_address> empty_pools;

        for (auto& [addr, pool] : _pools) {
            auto it = pool.begin();
            while (it != pool.end()) {
                bool should_close = false;
                bool is_max_age = false;

                // Check for expired (idle too long)
                if (it->is_expired(_config.idle_timeout)) {
                    log_pool.trace("Reaping expired connection to {}", addr);
                    should_close = true;
                }
                // Check for max age exceeded (TTL)
                else if (it->is_too_old(_config.max_connection_age)) {
                    log_pool.trace("Reaping max-age connection to {} (created too long ago)", addr);
                    should_close = true;
                    is_max_age = true;
                }
                // Check for half-open (backend closed)
                else if (it->is_half_open()) {
                    log_pool.trace("Reaping half-open connection to {}", addr);
                    should_close = true;
                }

                if (should_close) {
                    // Move bundle out and close asynchronously
                    close_bundle_async(std::move(*it));
                    it = pool.erase(it);
                    _total_idle_connections--;
                    _dead_connections_reaped++;
                    if (is_max_age) {
                        _connections_reaped_max_age++;
                    }
                    closed++;
                } else {
                    ++it;
                }
            }

            // Track empty pools for removal (Rule #4: cleanup map entries)
            if (pool.empty()) {
                empty_pools.push_back(addr);
            }
        }

        // Erase empty map entries to prevent unbounded growth (Rule #4)
        for (const auto& addr : empty_pools) {
            _pools.erase(addr);
            log_pool.trace("Removed empty pool entry for {}", addr);
        }

        return closed;
    }

private:
    ConnectionPoolConfig _config;
    std::unordered_map<seastar::socket_address, std::deque<ConnectionBundle>> _pools;
    size_t _total_idle_connections = 0;
    size_t _dead_connections_reaped = 0;
    size_t _connections_reaped_max_age = 0;
    seastar::timer<> _reaper_timer;

    // RAII gate for timer callback lifetime safety (Rule #5).
    // Timer callbacks capture `this`, creating a potential use-after-free if the
    // callback executes after destruction begins. The _timer_gate ensures:
    //   - Timer callbacks acquire a gate::holder at entry (fails if gate is closed)
    //   - stop() closes the gate FIRST (waits for in-flight callbacks to complete)
    //   - Only then are timers cancelled and resources freed
    seastar::gate _timer_gate;

    // Arm the reaper timer for next execution
    void arm_reaper_timer() {
        _reaper_timer.arm(_config.reaper_interval);
    }

    // Dead connection reaper - runs periodically to clean up stale connections
    // This catches half-open sockets that received FIN from backend and expired connections
    //
    // Timer Safety (Rule #5): Acquires gate holder to prevent execution during shutdown.
    // If the gate is closed (stop() in progress), the callback safely exits without
    // accessing any member state - preventing use-after-free on `this`.
    void reap_dead_connections() {
        // RAII Timer Safety: Acquire gate holder to prevent execution during shutdown.
        // If the gate is closed (stop() in progress), this throws gate_closed_exception
        // and the callback safely exits without accessing any member state.
        seastar::gate::holder timer_holder;
        try {
            timer_holder = _timer_gate.hold();
        } catch (const seastar::gate_closed_exception&) {
            // Gate closed - stop() is in progress, exit safely
            return;
        }

        size_t reaped = cleanup_expired();
        if (reaped > 0) {
            log_pool.debug("Dead connection reaper: closed {} stale connections", reaped);
        }
        // Re-arm for next cycle
        arm_reaper_timer();
    }

    // Evict the oldest connection across all pools
    void evict_oldest_global() {
        seastar::socket_address* oldest_addr = nullptr;
        std::chrono::steady_clock::time_point oldest_time = std::chrono::steady_clock::now();

        for (auto& [addr, pool] : _pools) {
            if (!pool.empty() && pool.front().last_used < oldest_time) {
                oldest_time = pool.front().last_used;
                oldest_addr = const_cast<seastar::socket_address*>(&addr);
            }
        }

        if (oldest_addr) {
            auto& pool = _pools[*oldest_addr];
            if (!pool.empty()) {
                auto oldest = std::move(pool.front());
                pool.pop_front();
                _total_idle_connections--;
                close_bundle_async(std::move(oldest));
            }
        }
    }

    // Close a connection bundle asynchronously, keeping it alive until close completes.
    // This prevents the Seastar output_stream assertion failure that occurs when
    // a stream is destroyed while still in batch mode.
    static void close_bundle_async(ConnectionBundle&& bundle) {
        // Move bundle to heap so it outlives this scope
        auto ptr = std::make_unique<ConnectionBundle>(std::move(bundle));
        auto* raw_ptr = ptr.get();
        // Start close and destroy bundle after completion
        (void)raw_ptr->close().finally([p = std::move(ptr)] {
            // unique_ptr releases the bundle here after close completes
        });
    }
};

} // namespace ranvier
