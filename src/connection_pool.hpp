#pragma once
#include <seastar/core/reactor.hh>
#include <seastar/net/api.hh>
#include <seastar/core/iostream.hh>
#include <unordered_map>
#include <vector>
#include <iostream>

namespace ranvier {

// A bundle representing an active connection
struct ConnectionBundle {
    seastar::connected_socket fd;
    seastar::input_stream<char> in;
    seastar::output_stream<char> out;
    seastar::socket_address addr;
    bool is_valid = true; // Mark false if an error occurs

    // Helper to close everything if we destroy a bad connection
    seastar::future<> close() {
        if (!is_valid) return seastar::make_ready_future<>();
        return out.close().then([this] {
            return in.close();
        }).handle_exception([](auto ep) {}); // Ignore errors on close
    }
};

class ConnectionPool {
public:
    // GET: Reuse existing or create new
    seastar::future<ConnectionBundle> get(seastar::socket_address addr) {
        auto& pool = _pools[addr];

        std::cout << "[Pool] 🆕 Opening NEW connection to " << addr << "\n";
        if (!pool.empty()) {
            std::cout << "[Pool] ♻️  Reusing warm connection to " << addr << "\n";
            auto bundle = std::move(pool.back());
            pool.pop_back();
            return seastar::make_ready_future<ConnectionBundle>(std::move(bundle));
        }

        if (!pool.empty()) {
            // LIFO (Last-In-First-Out) is better for cache locality and checking dead connections
            auto bundle = std::move(pool.back());
            pool.pop_back();
            // Optional: Check if socket is closed by peer? (Advanced)
            return seastar::make_ready_future<ConnectionBundle>(std::move(bundle));
        }

        // Create new connection
        return seastar::connect(addr).then([addr](seastar::connected_socket fd) {
            // Disable Nagle's algorithm for lower latency
            fd.set_nodelay(true);
            auto in = fd.input();
            auto out = fd.output();
            return ConnectionBundle{std::move(fd), std::move(in), std::move(out), addr};
        });
    }

    // RETURN: Put back into pool
    void put(ConnectionBundle&& bundle) {
        if (!bundle.is_valid) {
            // If marked invalid (e.g. read error), fire-and-forget close
            (void)bundle.close();
            return;
        }
        _pools[bundle.addr].push_back(std::move(bundle));
    }

private:
    // Map IP:Port -> List of Idle Connections
    std::unordered_map<seastar::socket_address, std::vector<ConnectionBundle>> _pools;
};

} // namespace ranvier
