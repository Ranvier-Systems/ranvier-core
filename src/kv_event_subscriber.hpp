// Ranvier Core - Native KV-Event Subscriber (vLLM ZMQ stream → routing state)
//
// Subscribes to each opted-in backend's vLLM KV-event publisher (ZMQ PUB,
// msgpack payloads) and feeds translated operations into RouterService's
// per-shard residency state. Compiled only WITH_KV_EVENTS=ON; everything
// ZMQ lives behind this service on ONE dedicated OS thread.
//
// Threading model (Rule #12 — the tokenizer thread pool is the template):
//
//   reactor (any shard)          worker OS thread              shard 0 reactor
//   add/remove_subscription ──► MPSC command queue
//                                zmq_poll over N SUB sockets
//                                decode (kv_event_decoder)
//                                translate (kv_event_ledger)
//                                ops batch ── alien::run_on ──► on_shipment()
//                                                               gate + realloc
//                                                               apply_native_kv_ops
//
// Sequence handling: each published frame carries an 8-byte big-endian
// sequence number. A gap means PUB/SUB dropped frames (slow joiner, HWM,
// publisher restart): the ledger state for that backend is unknowable, so we
// wipe it and ship RESET — routing falls back to the probabilistic residency
// signal until the stream re-coheres through subsequent BlockStored events.
// (vLLM's replay socket can close gaps without a reset; deliberately not in
// this phase.)
//
// Rule #1:  nothing here runs on the hot path; reactor-side calls are
//           lock-free MPSC enqueues.
// Rule #4:  command queue, per-backend ledger, ops batches all capped.
// Rule #13: stop() signals, joins the worker, then closes the apply gate.
// Rule #15: shipped ops are reallocated reactor-side before use.
// Rule #18: shipment applies are gate-guarded with their own error handling.

#pragma once

#include "config.hpp"
#include "kv_event_ledger.hpp"
#include "mpsc_ring_buffer.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include <seastar/core/alien.hh>
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>

namespace ranvier {

class KvEventSubscriberService {
public:
    explicit KvEventSubscriberService(const KvEventsConfig& cfg, uint32_t block_alignment);
    ~KvEventSubscriberService();

    KvEventSubscriberService(const KvEventSubscriberService&) = delete;
    KvEventSubscriberService& operator=(const KvEventSubscriberService&) = delete;

    // Spawn the worker thread. Call once from shard 0 after RouterService is
    // up (shipments target shard 0's RouterService entry points).
    void start(seastar::alien::instance& alien);

    // Shutdown: signal the worker, join it (bounded by one poll interval),
    // then close the apply gate so in-flight shipments drain before the
    // owning Application tears down RouterService.
    seastar::future<> stop();

    // Reactor-side, lock-free (MPSC enqueue). Returns false when the command
    // queue is full (Rule #4 backpressure) — callers log and may retry on
    // their next discovery cycle.
    bool add_subscription(BackendId id, std::string endpoint);
    bool remove_subscription(BackendId id);

    struct StatsSnapshot {
        uint64_t batches_decoded = 0;
        uint64_t decode_errors = 0;
        uint64_t sequence_gaps = 0;
        uint64_t ops_shipped = 0;
        uint64_t shipments = 0;
        uint64_t shipments_throttled = 0;  // worker waited on the in-flight cap
        uint64_t commands_dropped = 0;
    };
    StatsSnapshot stats() const;

private:
    struct Command {
        enum class Kind : uint8_t { ADD, REMOVE, STOP } kind = Kind::STOP;
        BackendId backend = 0;
        std::string endpoint;
    };

    void worker_loop(seastar::alien::instance& alien);
    void ship(seastar::alien::instance& alien, std::vector<NativeKvOp>&& ops);
    // Runs on shard 0's reactor: gate-guard, reallocate, apply.
    void on_shipment(std::vector<NativeKvOp> ops) noexcept;

    KvEventsConfig _cfg;
    KvBlockLedger::Limits _ledger_limits;

    MpscRingBuffer<Command> _commands;
    std::atomic<bool> _shutdown{false};
    std::atomic<uint32_t> _inflight{0};
    std::thread _worker;
    bool _started = false;

    // Closed in stop() AFTER the worker joins; every shipment apply runs
    // under it so teardown waits for in-flight RouterService broadcasts.
    seastar::gate _apply_gate;

    // Worker-written, reactor-read (relaxed: monotonic counters for metrics).
    std::atomic<uint64_t> _batches_decoded{0};
    std::atomic<uint64_t> _decode_errors{0};
    std::atomic<uint64_t> _sequence_gaps{0};
    std::atomic<uint64_t> _ops_shipped{0};
    std::atomic<uint64_t> _shipments{0};
    std::atomic<uint64_t> _shipments_throttled{0};
    std::atomic<uint64_t> _commands_dropped{0};
};

}  // namespace ranvier
