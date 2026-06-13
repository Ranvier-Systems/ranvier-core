// Ranvier Core - Usage-Ledger Sink Interface
//
// Pluggable export target for per-API-key usage events. One hot-path entry
// point (record) plus a shutdown drain (flush); the default is NoopUsageLedgerSink.
//
// This is the per-request sibling of telemetry_sink.hpp. The telemetry sink
// exports a periodic content-free *aggregate* from shard 0; this sink receives
// one event *per request* on the shard that handled it, so external metering /
// attribution / billing backends can consume per-key usage without core
// hard-depending on any concrete implementation.
//
// =============================================================================
// CONTRACT (binding on all implementations)
// =============================================================================
//
//   - record() MUST NOT block the reactor. It is called on the request
//     completion path of whichever shard handled the request; a stall here
//     stalls that shard. Real work (network export, serialisation, disk) MUST
//     be offloaded — e.g. enqueue to a bounded MPSC ring drained by a
//     dedicated OS worker, mirroring async_persistence.
//
//   - record() MUST NOT throw. It runs in the request coroutine's cleanup
//     phase; an escaping exception would corrupt completion handling. Swallow
//     and account for internal errors (drop-counter + rate-limited log).
//
//   - record() MUST be cheap and lock-free on the caller's shard. The instance
//     is per-shard (the factory is invoked once per shard, see below), so a
//     sink needing a single process-wide exporter should share that backend
//     behind its per-shard front-ends (e.g. one process-wide ring all shard
//     sinks push to) rather than taking a cross-shard lock here.
//
//   - record() MUST tolerate UsageEvents at any format_version. Read the fields
//     you know, ignore the rest. Same forward-compat discipline as
//     UsageEvent / WindowReport / CacheStatePacket.
//
//   - flush() is called once per shard during teardown, before the reactor
//     stops, to drain any internal buffer. It MAY block on its own worker
//     join / network flush but MUST resolve the returned future. The default
//     has nothing to drain.

#pragma once

#include "usage_ledger_schema.hpp"

#include <functional>
#include <memory>
#include <utility>

#include <seastar/core/future.hh>

namespace ranvier {

// Abstract sink. See contract above.
class UsageLedgerSink {
public:
    virtual ~UsageLedgerSink() = default;

    // Hot-path, per-request entry point. Synchronous and non-blocking: enqueue
    // and return. Must not throw. Taken by value so a buffering sink can move
    // the event's strings into its ring without a copy (mirrors
    // AsyncPersistenceManager::queue_log_request).
    virtual void record(UsageEvent event) = 0;

    // Drain at shutdown. Called once per shard during teardown, before the
    // reactor stops. Default: nothing to drain.
    virtual seastar::future<> flush() { return seastar::make_ready_future<>(); }
};

// Discards every event. The default when usage_ledger.enabled=true but no real
// sink has been wired (and no sink is created at all when the config switch is
// off).
class NoopUsageLedgerSink final : public UsageLedgerSink {
public:
    void record(UsageEvent /*event*/) override {}
};

inline std::unique_ptr<UsageLedgerSink> make_default_usage_ledger_sink() {
    return std::make_unique<NoopUsageLedgerSink>();
}

// Process-wide seam for choosing which sink each shard installs at start-up.
// OSS leaves it unset and falls back to make_default_usage_ledger_sink()
// (NoopUsageLedgerSink); a downstream build installs a real exporter via
// set_usage_ledger_sink_factory() during its own start-up, BEFORE
// Application::init_usage_ledger() invokes the factory on each shard. An empty
// factory resets to the default.
//
// The factory is invoked once PER SHARD (unlike the telemetry sink, which is
// shard-0-only) because usage events occur on every shard; each invocation
// yields that shard's own instance. Implementations needing a single shared
// backend should hide it behind the per-shard front-ends (see contract).
//
// Not racy runtime state (cf. Hard Rule #11): the slot is written once on shard
// 0 during single-threaded start-up and only read afterwards (once per shard,
// during the start-up broadcast); it is never touched on the hot path. The
// function-local static supplies the standard thread-safe one-time init of the
// slot itself.
using UsageLedgerSinkFactory = std::function<std::unique_ptr<UsageLedgerSink>()>;

// Shared storage for the factory seam; prefer the set/get accessors below.
inline UsageLedgerSinkFactory& usage_ledger_sink_factory_slot() {
    static UsageLedgerSinkFactory slot;  // empty until a build installs one
    return slot;
}

inline void set_usage_ledger_sink_factory(UsageLedgerSinkFactory factory) {
    usage_ledger_sink_factory_slot() = std::move(factory);
}

// The installed factory, or make_default_usage_ledger_sink (Noop) when unset.
inline UsageLedgerSinkFactory get_usage_ledger_sink_factory() {
    const UsageLedgerSinkFactory& slot = usage_ledger_sink_factory_slot();
    return slot ? slot : UsageLedgerSinkFactory{&make_default_usage_ledger_sink};
}

}  // namespace ranvier
