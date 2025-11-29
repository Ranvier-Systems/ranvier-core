#pragma once

#include "radix_tree.hpp"

#include <optional>
#include <vector>

#include <seastar/core/future.hh>
#include <seastar/net/socket_defs.hh>

namespace ranvier {

class RouterService {
public:
    // 1. DATA PLANE (Fast Lookups)
    // Find which Backend ID owns this prefix
    std::optional<BackendId> lookup(const std::vector<int32_t>& tokens);

    // Resolve ID -> IP:Port
    std::optional<seastar::socket_address> get_backend_address(BackendId id);

    // 2. CONTROL PLANE (Async Broadcasts)
    // Teach the tree a new prefix (Prefix -> ID)
    seastar::future<> learn_route_global(std::vector<int32_t> tokens, BackendId backend);

    // Teach the system a new server (ID -> IP:Port)
    seastar::future<> register_backend_global(BackendId id, seastar::socket_address addr);

    std::optional<BackendId> get_random_backend();
};

} // namespace ranvier
