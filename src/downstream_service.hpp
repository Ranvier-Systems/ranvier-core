// Ranvier Core - Downstream Service Lifecycle Seam
//
// Lifecycle hook for a downstream/embedder-owned service that needs to run for
// the lifetime of the process but has no home in core's service graph — an
// embedder-owned endpoint, a periodic job, a sidecar client. Core exposes only
// pre-reactor main() and the telemetry / usage-ledger sink factories as entry
// points; none of those own a start/stop lifetime. This completes the
// embeddability series by giving embedders one.
//
// =============================================================================
// CONTRACT (binding on all implementations)
// =============================================================================
//
//   - The factory is invoked ONCE, on shard 0, after every core service is up.
//     The returned service's start() is co_awaited before the app reports
//     RUNNING; a failed start() aborts startup exactly like a core service.
//
//   - stop() runs at the BEGINNING of teardown, before any core service stops —
//     the reverse of startup order (started last, stopped first). The service
//     may therefore assume routing / gossip / HTTP are still live during stop().
//
//   - start()/stop() MUST NOT throw a C++ exception before returning a future
//     (Hard Rule #22): wrap any synchronous failure in a failed future, or use
//     a coroutine.
//
//   - No factory installed => the whole hook is inert and startup/teardown are
//     bit-for-bit unchanged.

#pragma once

#include <functional>
#include <memory>
#include <utility>

#include <seastar/core/future.hh>

namespace ranvier {

// Abstract downstream service. See contract above.
class DownstreamService {
public:
    virtual ~DownstreamService() = default;

    // Brought up on shard 0 after core services are running.
    virtual seastar::future<> start() = 0;

    // Torn down first, before core services stop (reverse-order discipline).
    virtual seastar::future<> stop() = 0;
};

// Process-wide seam for the single downstream service the Application brings up
// on shard 0. OSS leaves it unset and nothing runs; a downstream build installs
// a factory via set_downstream_service_factory() during its own start-up, BEFORE
// Application::startup() reaches the hook. An empty factory resets to "none".
//
// Not racy runtime state (cf. Hard Rule #11, mirroring the telemetry_sink
// factory seam): the slot is written once and read once, both on shard 0 during
// the single-threaded start-up sequence, and is never touched afterwards — not
// on the hot path, not from other shards. The function-local static supplies the
// standard thread-safe one-time init of the slot itself.
using DownstreamServiceFactory = std::function<std::unique_ptr<DownstreamService>()>;

// Shared storage for the factory seam; prefer the set/get accessors below.
inline DownstreamServiceFactory& downstream_service_factory_slot() {
    static DownstreamServiceFactory slot;  // empty until a build installs one
    return slot;
}

inline void set_downstream_service_factory(DownstreamServiceFactory factory) {
    downstream_service_factory_slot() = std::move(factory);
}

// The installed factory, or an empty std::function when unset. Unlike the sink
// factories there is no default service: an empty result means "run nothing",
// which the Application treats as the unchanged-behaviour path.
inline DownstreamServiceFactory get_downstream_service_factory() {
    return downstream_service_factory_slot();
}

}  // namespace ranvier
