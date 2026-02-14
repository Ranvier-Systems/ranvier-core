#pragma once

#include <cassert>
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
    size_t max_backends = 1000;                    // Max unique backend addresses in pool map (Rule #4)
};

// A bundle representing an active connection with timestamp tracking
//
// Clock injection: Template parameter defaults to std::chrono::steady_clock
// for zero-overhead production use. Tests use TestClock for deterministic timing.
//
// Suppress deprecation warnings for Seastar stream default ctor and move-assignment.
// Seastar deprecated these operations but our pool requires default-constructible
// and move-assignable bundles for std::deque storage and element erasure.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
template<typename Clock = std::chrono::steady_clock>
struct BasicConnectionBundle {
    seastar::connected_socket fd;
    seastar::input_stream<char> in;
    seastar::output_stream<char> out;
    seastar::socket_address addr;
    bool is_valid = true;
    typename Clock::time_point created_at;
    typename Clock::time_point last_used;

    BasicConnectionBundle()
        : created_at(Clock::now())
        , last_used(Clock::now()) {}

    BasicConnectionBundle(seastar::connected_socket&& socket,
                     seastar::input_stream<char>&& input,
                     seastar::output_stream<char>&& output,
                     seastar::socket_address address)
        : fd(std::move(socket))
        , in(std::move(input))
        , out(std::move(output))
        , addr(address)
        , is_valid(true)
        , created_at(Clock::now())
        , last_used(Clock::now()) {}

    // Close the connection
    seastar::future<> close() {
        if (!is_valid) return seastar::make_ready_future<>();
        is_valid = false;
        return out.close().then([this] {
            return in.close();
        }).handle_exception([addr = this->addr](auto ep) {
            // Rule #9: every catch must log at warn level
            log_pool.warn("Failed to close connection to {}: {}", addr, ep);
        });
    }

    // Check if connection has been idle too long
    bool is_expired(std::chrono::seconds timeout) const {
        auto now = Clock::now();
        return (now - last_used) > timeout;
    }

    // Check if connection has exceeded its maximum age (TTL)
    // Returns true if connection should be closed regardless of recent activity
    bool is_too_old(std::chrono::seconds max_age) const {
        auto now = Clock::now();
        return (now - created_at) > max_age;
    }

    // Update last used timestamp
    void touch() {
        last_used = Clock::now();
    }

    // Check if connection received FIN from backend (half-open detection)
    // This is a synchronous check that inspects the input stream's EOF status
    bool is_half_open() const {
        // A connection is considered half-open if eof() returns true
        // This means the backend has closed its side of the connection
        return in.eof();
    }
};

#pragma GCC diagnostic pop

// Backward-compatible alias: production code uses ConnectionBundle unchanged
using ConnectionBundle = BasicConnectionBundle<>;

// Close policy for production: async close keeps bundle alive on heap until
// Seastar stream close completes, preventing batch-mode assertion failures.
struct AsyncClosePolicy {
    template<typename Bundle>
    static void close(Bundle&& bundle) {
        auto ptr = std::make_unique<Bundle>(std::move(bundle));
        auto* raw_ptr = ptr.get();
        (void)raw_ptr->close().finally([p = std::move(ptr)] {});
    }
};

// Close policy for unit tests: synchronous drop without Seastar future operations.
// Safe for default-constructed bundles whose null streams can be destroyed directly.
// Avoids reactor dependency (.then()/.finally() call need_preempt() which segfaults
// without a running Seastar reactor).
struct SyncClosePolicy {
    template<typename Bundle>
    static void close(Bundle&& bundle) {
        bundle.is_valid = false;
    }
};

// Connection pool with LRU + TTL connection management
//
// Clock injection: Template parameter defaults to std::chrono::steady_clock
// for zero-overhead production use. Tests use TestClock for deterministic timing.
//
// ClosePolicy injection: Controls how evicted bundles are closed.
// Production uses AsyncClosePolicy (heap + .finally() to keep bundle alive).
// Unit tests use SyncClosePolicy to avoid requiring a Seastar reactor.
template<typename Clock = std::chrono::steady_clock, typename ClosePolicy = AsyncClosePolicy>
class BasicConnectionPool {
public:
    using Bundle = BasicConnectionBundle<Clock>;

    explicit BasicConnectionPool(ConnectionPoolConfig config = {})
        : _config(config)
        , _reaper_timer([this] { reap_dead_connections(); })
    {
        // Start the dead connection reaper timer
        if (_config.reaper_interval.count() > 0) {
            arm_reaper_timer();
        }
    }

    ~BasicConnectionPool() {
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
    seastar::future<Bundle> get(seastar::socket_address addr,
                                           const std::string& request_id = "") {
        // Use find() instead of operator[] to avoid auto-inserting empty map
        // entries for unknown addresses (Rule #4: bounded containers)
        auto pool_it = _pools.find(addr);

        if (pool_it != _pools.end()) {
            auto& pool = pool_it->second;

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
                _connections_reused++;
                bundle.touch();
                debug_validate_idle_count();
                return seastar::make_ready_future<Bundle>(std::move(bundle));
            }

            // All connections were stale - remove empty map entry (Rule #4)
            _pools.erase(pool_it);
        }

        // No pooled connection available, create new one
        if (!request_id.empty()) {
            log_pool.debug("[{}] Creating new connection to {}", request_id, addr);
        }
        debug_validate_idle_count();
        return create_connection(addr, request_id);
    }

    // GET FRESH: Always create a new connection, bypassing pool cache.
    // Use when a pooled connection was detected as stale after use.
    // request_id: Optional request ID for tracing (empty string if not tracing)
    seastar::future<Bundle> get_fresh(seastar::socket_address addr,
                                      const std::string& request_id = "") {
        if (!request_id.empty()) {
            log_pool.info("[{}] Creating fresh connection to {} (bypassing pool)",
                          request_id, addr);
        }
        return create_connection(addr, request_id);
    }

    // PUT: Return connection to pool
    // request_id: Optional request ID for tracing (empty string if not tracing)
    void put(Bundle&& bundle, const std::string& request_id = "") {
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

        // Rule #4: Check MAX_BACKENDS before inserting new map entry
        auto pool_it = _pools.find(bundle.addr);
        if (pool_it == _pools.end()) {
            if (_pools.size() >= _config.max_backends) {
                // Too many unique backends - reject pooling this connection
                log_pool.warn("Backend limit reached ({} backends), not pooling connection to {}",
                              _pools.size(), bundle.addr);
                _backends_rejected++;
                close_bundle_async(std::move(bundle));
                return;
            }
            pool_it = _pools.emplace(bundle.addr, std::deque<Bundle>{}).first;
        }
        auto& pool = pool_it->second;

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
        debug_validate_idle_count();
    }

    // Get pool statistics
    struct Stats {
        size_t total_idle_connections;
        size_t num_backends;
        size_t max_per_host;
        size_t max_total;
        size_t max_backends;
        size_t dead_connections_reaped;      // Total dead connections detected and closed
        size_t connections_reaped_max_age;   // Connections closed due to exceeding max age
        size_t backends_rejected;            // Connections not pooled due to backend limit
        size_t connections_created;          // Total new connections opened
        size_t connections_reused;           // Total connections served from pool
    };

    Stats stats() const {
        return Stats{
            _total_idle_connections,
            _pools.size(),
            _config.max_connections_per_host,
            _config.max_total_connections,
            _config.max_backends,
            _dead_connections_reaped,
            _connections_reaped_max_age,
            _backends_rejected,
            _connections_created,
            _connections_reused
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
        debug_validate_idle_count();
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

        debug_validate_idle_count();
        return closed;
    }

private:
    // Create a new TCP connection with nodelay and keepalive configured.
    // Shared by get() (pool miss) and get_fresh() (bypassing pool).
    seastar::future<Bundle> create_connection(seastar::socket_address addr,
                                              const std::string& request_id) {
        _connections_created++;
        return seastar::connect(addr).then([this, addr, request_id](seastar::connected_socket fd) {
            fd.set_nodelay(true);

            if (_config.tcp_keepalive) {
                try {
                    seastar::net::tcp_keepalive_params tcp_params;
                    tcp_params.idle = _config.keepalive_idle;
                    tcp_params.interval = _config.keepalive_interval;
                    tcp_params.count = _config.keepalive_count;

                    seastar::net::keepalive_params params = tcp_params;

                    fd.set_keepalive(true);
                    fd.set_keepalive_parameters(params);

                    if (!request_id.empty()) {
                        log_pool.trace("[{}] TCP keepalive enabled for connection to {}", request_id, addr);
                    }
                } catch (const std::exception& e) {
                    log_pool.warn("Failed to set TCP keepalive for {}: {}", addr, e.what());
                }
            }

            auto in = fd.input();
            auto out = fd.output();
            return Bundle{std::move(fd), std::move(in), std::move(out), addr};
        });
    }

    ConnectionPoolConfig _config;
    std::unordered_map<seastar::socket_address, std::deque<Bundle>> _pools;
    size_t _total_idle_connections = 0;
    size_t _dead_connections_reaped = 0;
    size_t _connections_reaped_max_age = 0;
    size_t _backends_rejected = 0;
    size_t _connections_created = 0;
    size_t _connections_reused = 0;
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
        auto oldest_it = _pools.end();
        typename Clock::time_point oldest_time = Clock::now();

        for (auto it = _pools.begin(); it != _pools.end(); ++it) {
            auto& pool = it->second;
            if (!pool.empty() && pool.front().last_used < oldest_time) {
                oldest_time = pool.front().last_used;
                oldest_it = it;
            }
        }

        if (oldest_it != _pools.end()) {
            auto& pool = oldest_it->second;
            if (!pool.empty()) {
                auto oldest = std::move(pool.front());
                pool.pop_front();
                _total_idle_connections--;
                close_bundle_async(std::move(oldest));
            }
        }
    }

    // Debug-only: validate _total_idle_connections matches actual pool contents.
    // Catches counter drift from missed increment/decrement paths.
    void debug_validate_idle_count() const {
#ifndef NDEBUG
        size_t actual = 0;
        for (const auto& [addr, pool] : _pools) {
            actual += pool.size();
        }
        assert(actual == _total_idle_connections &&
               "connection_pool: _total_idle_connections out of sync with actual pool sizes");
#endif
    }

    // Close a connection bundle via the configured close policy.
    // Production (AsyncClosePolicy): keeps bundle alive on heap during async close
    // to prevent Seastar output_stream batch-mode assertion failures.
    // Tests (SyncClosePolicy): synchronous drop for null streams without reactor.
    static void close_bundle_async(Bundle&& bundle) {
        ClosePolicy::close(std::move(bundle));
    }
};

// Backward-compatible alias: production code uses ConnectionPool unchanged
using ConnectionPool = BasicConnectionPool<>;

} // namespace ranvier
