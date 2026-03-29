#pragma once

#include "types.hpp"

#include <optional>
#include <vector>

#include <seastar/core/future.hh>
#include <seastar/net/socket_defs.hh>

namespace ranvier {

// Abstract interface for backend lifecycle operations.
// Extracted from RouterService to decouple HealthService,
// LocalDiscoveryService, and future vLLM metrics ingestion.
class BackendRegistry {
public:
    virtual ~BackendRegistry() = default;

    // --- Read operations (used by HealthService) ---

    virtual std::vector<BackendId> get_all_backend_ids() const = 0;

    virtual std::optional<seastar::socket_address>
        get_backend_address(BackendId id) const = 0;

    // --- Write operations (used by HealthService + discovery services) ---

    virtual seastar::future<>
        set_backend_status_global(BackendId id, bool is_alive) = 0;

    virtual seastar::future<>
        register_backend_global(BackendId id, seastar::socket_address addr,
                                uint32_t weight = 100, uint32_t priority = 0) = 0;

    virtual seastar::future<>
        unregister_backend_global(BackendId id) = 0;
};

}  // namespace ranvier
