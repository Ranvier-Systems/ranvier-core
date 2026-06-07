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

#include <functional>
#include <memory>
#include <utility>

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

// Process-wide seam for choosing which sink the shard-0 emitter installs at
// start-up. OSS leaves it unset and falls back to make_default_telemetry_sink()
// (NoopSink); a downstream build installs a real exporter via
// set_telemetry_sink_factory() during its own start-up, BEFORE
// Application::init_telemetry_service() arms the emitter. An empty factory
// resets to the default.
//
// Not racy runtime state (cf. Hard Rule #11): the slot is written once and read
// once, both on shard 0 during the single-threaded start-up sequence, and is
// never touched afterwards — not on the hot path, not from other shards. The
// function-local static supplies the standard thread-safe one-time init of the
// slot itself.
using TelemetrySinkFactory = std::function<std::unique_ptr<TelemetrySink>()>;

// Shared storage for the factory seam; prefer the set/get accessors below.
inline TelemetrySinkFactory& telemetry_sink_factory_slot() {
    static TelemetrySinkFactory slot;  // empty until a build installs one
    return slot;
}

inline void set_telemetry_sink_factory(TelemetrySinkFactory factory) {
    telemetry_sink_factory_slot() = std::move(factory);
}

// The installed factory, or make_default_telemetry_sink (NoopSink) when unset.
inline TelemetrySinkFactory get_telemetry_sink_factory() {
    const TelemetrySinkFactory& slot = telemetry_sink_factory_slot();
    return slot ? slot : TelemetrySinkFactory{&make_default_telemetry_sink};
}

}  // namespace ranvier
