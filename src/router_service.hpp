#pragma once

#include "radix_tree.hpp"

#include <optional>
#include <vector>

#include <seastar/core/future.hh>

namespace ranvier {

class RouterService {
public:
    // Lookup on the CURRENT core (Fast)
    std::optional<BackendId> lookup(const std::vector<int32_t>& tokens);

    // Broadcast update to ALL cores (Slow/Async)
    seastar::future<> learn_route_global(std::vector<int32_t> tokens, BackendId backend);
};

} // namespace ranvier
