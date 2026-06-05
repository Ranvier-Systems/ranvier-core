// Ranvier Core - Telemetry Sink Interface
//
// Pluggable export target for per-window aggregate routing/cache reports. One
// virtual entry point; concrete sinks live in their own translation units and
// are wired in at startup by the operator (default is NoopSink, which is what
// the stock build ships).
//
// =============================================================================
// CONTRACT (binding on all implementations)
// =============================================================================
//
//   - consume() MUST NOT block the reactor. The emitter calls it from the
//     periodic-export timer on shard 0, not from the request hot path, but
//     any reactor stall here still freezes that shard. If real work is
//     needed (network export, serialisation), the implementation must
//     offload (e.g. MPSC ring → dedicated OS worker, mirroring
//     async_persistence) and return promptly.
//
//   - consume() MUST tolerate WindowReports at any format_version. Read the
//     fields you know, ignore the rest. This is the same forward-compat
//     discipline used by CacheStatePacket::deserialize (see
//     gossip_protocol.hpp §"FORWARD COMPATIBILITY") and is the contract
//     that lets the catalog accumulate comparable data across releases.
//
//   - The emitter NEVER awaits consume() on the request path. If the
//     returned future has not resolved by the next window tick, the new
//     report is DROPPED and a counter incremented — there is no queue and
//     no backpressure propagation to routing decisions. Implementations
//     that need durability across sink-side outages must provide their own
//     internal buffering with a bounded discard policy.
//
//   - This PR ships only the interface and NoopSink. Remote transports,
//     managed backends, and any aggregation service are out of scope —
//     plug them in behind this same interface in a follow-up.

#pragma once

#include "telemetry_schema.hpp"

#include <memory>

#include <seastar/core/future.hh>

namespace ranvier {

// Abstract sink. See contract above.
class TelemetrySink {
public:
    virtual ~TelemetrySink() = default;

    // Periodic export entry point. Returns a future that resolves when the
    // sink has accepted (not necessarily exported) the report. Must not throw
    // C++ exceptions before returning a future — wrap any synchronous failure
    // in a failed future per Hard Rule #22.
    virtual seastar::future<> consume(const WindowReport& report) = 0;
};

// Default sink: discards every report. The stock build wires this in so that
// telemetry being "off" is genuinely zero cost on the export path (the
// emitter still doesn't run at all when the config switch is disabled; this
// sink is the safe default for when the switch is on but no real sink is
// configured).
class NoopSink final : public TelemetrySink {
public:
    seastar::future<> consume(const WindowReport& /*report*/) override {
        return seastar::make_ready_future<>();
    }
};

// Factory for the default sink. Returns a NoopSink. Operators wire real
// sinks in directly via Application::set_telemetry_sink() (see application.cpp).
inline std::unique_ptr<TelemetrySink> make_default_telemetry_sink() {
    return std::make_unique<NoopSink>();
}

}  // namespace ranvier
