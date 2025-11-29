#include "router_service.hpp"

#include <seastar/core/smp.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/do_with.hh>
#include <boost/range/irange.hpp>
#include <unordered_map>
#include <random>
#include <chrono>

namespace ranvier {

thread_local RadixTree local_tree;

// Primary Storage (Map)
thread_local std::unordered_map<BackendId, seastar::socket_address> local_backends;
// Secondary Index (Vector) for O(1) Randomness
thread_local std::vector<BackendId> local_backend_ids;

// Improved Seeder: Mix RandomDevice with Time to ensure Docker doesn't give same seed
thread_local std::mt19937 rng([]() {
    std::random_device rd;
    auto time_seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return std::mt19937(rd() ^ time_seed);
}());

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

std::optional<BackendId> RouterService::get_random_backend() {
    if (local_backend_ids.empty()) return std::nullopt;

    // O(1) Random Selection from Vector
    // This removes any "Map Bucket" bias
    std::uniform_int_distribution<size_t> dist(0, local_backend_ids.size() - 1);
    size_t random_index = dist(rng);

    return local_backend_ids[random_index];
}

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
    return seastar::do_with(addr, [id](seastar::socket_address& shared_addr) {
        return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count), [id, &shared_addr] (unsigned shard_id) {
            return seastar::smp::submit_to(shard_id, [id, addr = shared_addr] {
                // Update Map
                local_backends[id] = addr;

                // Update Vector (Check for duplicates first)
                bool exists = false;
                for (auto existing : local_backend_ids) {
                    if (existing == id) { exists = true; break; }
                }
                if (!exists) {
                    local_backend_ids.push_back(id);
                }

                return seastar::make_ready_future<>();
            });
        });
    });
}

} // namespace ranvier
