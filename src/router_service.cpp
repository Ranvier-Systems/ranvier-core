#include "router_service.hpp"

#include <boost/range/irange.hpp>
#include <seastar/core/do_with.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/loop.hh>

namespace ranvier {

// The actual storage (One copy per CPU core)
thread_local RadixTree local_tree;

std::optional<BackendId> RouterService::lookup(const std::vector<int32_t>& tokens) {
    return local_tree.lookup(tokens);
}

seastar::future<> RouterService::learn_route_global(std::vector<int32_t> tokens, BackendId backend) {
    // Replicate the 'tokens' vector to all cores
    return seastar::do_with(std::move(tokens), [backend](std::vector<int32_t>& shared_tokens) {
        return seastar::parallel_for_each(boost::irange(0u, seastar::smp::count), [backend, &shared_tokens] (unsigned shard_id) {
            return seastar::smp::submit_to(shard_id, [backend, tokens = shared_tokens] {
                // This runs on the specific core
                local_tree.insert(tokens, backend);
                return seastar::make_ready_future<>();
            });
        });
    });
}

} // namespace ranvier
