// Ranvier Core - Telemetry Sink Interface
//
// Pluggable export target for per-window aggregate routing/cache reports.
// One virtual entry point; the default is NoopSink.
//
// =============================================================================
// CONTRACT (binding on all implementations)
// =============================================================================
//
//   - consume() MUST NOT block the reactor. It is called from the periodic
//     emitter on shard 0; a stall here freezes that shard. Real work
//     (network export, serialisation) must be offloaded — e.g. MPSC ring →
//     dedicated OS worker, mirroring async_persistence.
//
//   - consume() MUST tolerate WindowReports at any format_version. Read
//     the fields you know, ignore the rest. Same forward-compat discipline
//     as CacheStatePacket::deserialize (see gossip_protocol.hpp).
//
//   - The emitter NEVER awaits consume() on the request path. If the prior
//     future has not resolved by the next window tick, the new report is
//     dropped and a counter incremented — no queue, no backpressure into
//     routing decisions. Sinks needing durability across outages must
//     bound their own internal buffer.

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

// Discards every report. Default when telemetry_sink.enabled=true but no
// real sink has been wired (and the emitter is not armed at all when the
// config switch is off).
class NoopSink final : public TelemetrySink {
public:
    seastar::future<> consume(const WindowReport& /*report*/) override {
        return seastar::make_ready_future<>();
    }
};

inline std::unique_ptr<TelemetrySink> make_default_telemetry_sink() {
    return std::make_unique<NoopSink>();
}

}  // namespace ranvier
