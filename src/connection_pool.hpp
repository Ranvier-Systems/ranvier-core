#pragma once

#include <deque>
#include <unordered_map>
#include <chrono>
#include <iostream>
#include <string>
#include <system_error>

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
};

// A bundle representing an active connection with timestamp tracking
struct ConnectionBundle {
    seastar::connected_socket fd;
    seastar::input_stream<char> in;
    seastar::output_stream<char> out;
    seastar::socket_address addr;
    bool is_valid = true;
    std::chrono::steady_clock::time_point last_used;

    ConnectionBundle() : last_used(std::chrono::steady_clock::now()) {}

    ConnectionBundle(seastar::connected_socket&& socket,
                     seastar::input_stream<char>&& input,
                     seastar::output_stream<char>&& output,
                     seastar::socket_address address)
        : fd(std::move(socket))
        , in(std::move(input))
        , out(std::move(output))
        , addr(address)
        , is_valid(true)
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
        _reaper_timer.cancel();
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
                (void)bundle.close(); // Fire-and-forget close
                continue;
            }

            // Zombie/half-open detection: check if backend sent FIN
            if (bundle.is_half_open()) {
                if (!request_id.empty()) {
                    log_pool.debug("[{}] Closing half-open connection to {} (backend closed)",
                                   request_id, addr);
                }
                _dead_connections_reaped++;
                (void)bundle.close(); // Fire-and-forget close
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
            (void)bundle.close();
            return;
        }

        // Check for half-open before pooling - don't return dead connections
        if (bundle.is_half_open()) {
            if (!request_id.empty()) {
                log_pool.debug("[{}] Not pooling half-open connection to {}",
                               request_id, bundle.addr);
            }
            _dead_connections_reaped++;
            (void)bundle.close();
            return;
        }

        bundle.touch();
        auto& pool = _pools[bundle.addr];

        // Check per-host limit
        if (pool.size() >= _config.max_connections_per_host) {
            // Pool full - evict oldest (front of deque)
            auto& oldest = pool.front();
            (void)oldest.close();
            pool.pop_front();
            _total_idle_connections--;
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
        size_t dead_connections_reaped;  // Total dead connections detected and closed
    };

    Stats stats() const {
        return Stats{
            _total_idle_connections,
            _pools.size(),
            _config.max_connections_per_host,
            _config.max_total_connections,
            _dead_connections_reaped
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
            (void)bundle.close();
            closed++;
        }
        _total_idle_connections -= pool.size();
        _pools.erase(it);

        if (closed > 0) {
            log_pool.info("Cleared {} pooled connections for removed backend {}", closed, addr);
        }
        return closed;
    }

    // Cleanup expired and half-open connections (call periodically)
    size_t cleanup_expired() {
        size_t closed = 0;
        for (auto& [addr, pool] : _pools) {
            auto it = pool.begin();
            while (it != pool.end()) {
                bool should_close = false;

                // Check for expired (idle too long)
                if (it->is_expired(_config.idle_timeout)) {
                    log_pool.trace("Reaping expired connection to {}", addr);
                    should_close = true;
                }
                // Check for half-open (backend closed)
                else if (it->is_half_open()) {
                    log_pool.trace("Reaping half-open connection to {}", addr);
                    should_close = true;
                }

                if (should_close) {
                    (void)it->close();
                    it = pool.erase(it);
                    _total_idle_connections--;
                    _dead_connections_reaped++;
                    closed++;
                } else {
                    ++it;
                }
            }
        }
        return closed;
    }

private:
    ConnectionPoolConfig _config;
    std::unordered_map<seastar::socket_address, std::deque<ConnectionBundle>> _pools;
    size_t _total_idle_connections = 0;
    size_t _dead_connections_reaped = 0;
    seastar::timer<> _reaper_timer;

    // Arm the reaper timer for next execution
    void arm_reaper_timer() {
        _reaper_timer.arm(_config.reaper_interval);
    }

    // Dead connection reaper - runs periodically to clean up stale connections
    // This catches half-open sockets that received FIN from backend and expired connections
    void reap_dead_connections() {
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
                (void)pool.front().close();
                pool.pop_front();
                _total_idle_connections--;
            }
        }
    }
};

} // namespace ranvier
