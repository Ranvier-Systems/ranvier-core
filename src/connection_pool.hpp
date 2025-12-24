#pragma once

#include <deque>
#include <unordered_map>
#include <chrono>
#include <iostream>
#include <string>

#include <seastar/core/iostream.hh>
#include <seastar/core/reactor.hh>
#include <seastar/net/api.hh>

#include "logging.hpp"

namespace ranvier {

// Configuration for connection pool behavior
struct ConnectionPoolConfig {
    size_t max_connections_per_host = 10;      // Max idle connections per backend
    std::chrono::seconds idle_timeout{60};      // Close connections idle longer than this
    size_t max_total_connections = 100;         // Max total idle connections across all backends
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
};

class ConnectionPool {
public:
    explicit ConnectionPool(ConnectionPoolConfig config = {})
        : _config(config) {}

    // GET: Reuse existing or create new connection
    // request_id: Optional request ID for tracing (empty string if not tracing)
    seastar::future<ConnectionBundle> get(seastar::socket_address addr,
                                           const std::string& request_id = "") {
        auto& pool = _pools[addr];

        // Try to find a valid, non-expired connection (LIFO for cache locality)
        while (!pool.empty()) {
            auto bundle = std::move(pool.back());
            pool.pop_back();
            _total_idle_connections--;

            // Skip expired connections
            if (bundle.is_expired(_config.idle_timeout)) {
                if (!request_id.empty()) {
                    log_pool.debug("[{}] Closing expired pooled connection to {}",
                                   request_id, addr);
                }
                (void)bundle.close(); // Fire-and-forget close
                continue;
            }

            // Found a good connection
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
        return seastar::connect(addr).then([addr](seastar::connected_socket fd) {
            fd.set_nodelay(true); // Disable Nagle's algorithm for lower latency
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
    };

    Stats stats() const {
        return Stats{
            _total_idle_connections,
            _pools.size(),
            _config.max_connections_per_host,
            _config.max_total_connections
        };
    }

    // Cleanup expired connections (call periodically)
    size_t cleanup_expired() {
        size_t closed = 0;
        for (auto& [addr, pool] : _pools) {
            auto it = pool.begin();
            while (it != pool.end()) {
                if (it->is_expired(_config.idle_timeout)) {
                    (void)it->close();
                    it = pool.erase(it);
                    _total_idle_connections--;
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
