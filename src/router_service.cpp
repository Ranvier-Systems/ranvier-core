#include "router_service.hpp"

#include <random>
#include <unordered_map>

#include <boost/range/irange.hpp>
#include <seastar/core/do_with.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/loop.hh>

namespace ranvier {

// Thread-local storage (One copy per CPU core)
thread_local RadixTree local_tree;
thread_local std::unordered_map<BackendId, seastar::socket_address> local_backends;

// Thread-local RNG
// We seed it once per core so every core has a unique random sequence.
thread_local std::mt19937 rng(std::random_device{}());

// ---------------------------------------------------------
// DATA PLANE
// ---------------------------------------------------------

std::optional<BackendId> RouterService::lookup(const std::vector<int32_t>& tokens) {
    return local_tree.lookup(tokens);
}

std::optional<seastar::socket_address> RouterService::get_backend_address(BackendId id) {
    auto it = local_backends.find(id);
    if (it != local_backends.end()) {
        return it->second;
    }
    return std::nullopt;
}

// ---------------------------------------------------------
// CONTROL PLANE (Broadcasting)
// ---------------------------------------------------------

seastar::future<> RouterService::learn_route_global(std::vector<int32_t> tokens, BackendId backend) {
    return seastar::do_with(std::move(tokens), [backend](std::vector<int32_t>& shared_tokens) {
        return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count), [backend, &shared_tokens] (unsigned shard_id) {
            return seastar::smp::submit_to(shard_id, [backend, tokens = shared_tokens] {
                local_tree.insert(tokens, backend);
                return seastar::make_ready_future<>();
            });
        });
    });
}

seastar::future<> RouterService::register_backend_global(BackendId id, seastar::socket_address addr) {
    // Replicate the IP:Port mapping to all cores
    return seastar::do_with(addr, [id](seastar::socket_address& shared_addr) {
        return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count), [id, &shared_addr] (unsigned shard_id) {
            return seastar::smp::submit_to(shard_id, [id, addr = shared_addr] {
                local_backends[id] = addr;
                return seastar::make_ready_future<>();
            });
        });
    });
}

std::optional<BackendId> RouterService::get_random_backend() {
    if (local_backends.empty()) {
        return std::nullopt;
    }

    // Pick a random index
    std::uniform_int_distribution<size_t> dist(0, local_backends.size() - 1);
    size_t random_index = dist(rng);

    // Advance iterator (Standard map approach)
    // Note: In production, you'd keep a separate vector<BackendId> for O(1) random access
    auto it = local_backends.begin();
    std::advance(it, random_index);

    return it->first;
}

} // namespace ranvier
