// Ranvier Core - Native KV-Event Subscriber implementation
//
// See kv_event_subscriber.hpp for the threading model and rule mapping.
// All ZMQ usage is confined to this translation unit and the worker thread;
// the reactor never touches a ZMQ socket.

#include "kv_event_subscriber.hpp"

#include "kv_event_decoder.hpp"
#include "logging.hpp"
#include "router_service.hpp"

#include <zmq.h>

#include <absl/container/flat_hash_map.h>

#include <chrono>
#include <cstring>
#include <functional>
#include <vector>

#include <seastar/util/log.hh>

namespace ranvier {

static seastar::logger log_kv("kv_events");

namespace {

struct Subscription {
    void* socket = nullptr;
    std::string endpoint;
    std::string replay_endpoint;  // empty = gaps reset instead of replaying
    uint64_t next_seq = 0;
    bool synced = false;  // false until the first frame fixes the sequence base
};

// Frames of one published message. vLLM publishes [topic, seq(8B BE),
// payload]; tolerate variations by keying on shape: the last frame is the
// payload, the nearest preceding exactly-8-byte frame is the sequence.
struct Message {
    std::vector<std::vector<uint8_t>> frames;
};

bool recv_message(void* socket, Message& out, uint64_t& dropped_oversize) {
    out.frames.clear();
    constexpr size_t kMaxFrames = 8;
    int more = 1;
    while (more) {
        zmq_msg_t m;
        zmq_msg_init(&m);
        int rc = zmq_msg_recv(&m, socket, ZMQ_DONTWAIT);
        if (rc < 0) {
            // Nothing readable (first frame) or a torn multipart (mid-message
            // EAGAIN can't happen within one message; treat any failure as
            // "no complete message").
            zmq_msg_close(&m);
            return false;
        }
        size_t len = zmq_msg_size(&m);
        if (out.frames.size() < kMaxFrames && len <= kKvMaxPayloadBytes) {
            const uint8_t* d = static_cast<const uint8_t*>(zmq_msg_data(&m));
            out.frames.emplace_back(d, d + len);
        } else {
            dropped_oversize++;
        }
        more = 0;
        size_t more_size = sizeof(more);
        zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &more_size);
        zmq_msg_close(&m);
    }
    return !out.frames.empty();
}

bool split_seq_payload(const Message& msg, uint64_t& seq,
                       const std::vector<uint8_t>** payload) {
    if (msg.frames.empty()) return false;
    *payload = &msg.frames.back();
    for (size_t i = msg.frames.size(); i-- > 1;) {
        const auto& f = msg.frames[i - 1];
        if (f.size() == 8) {
            seq = 0;
            for (uint8_t b : f) seq = (seq << 8) | b;
            return true;
        }
    }
    return false;
}

// Recover missed batches from vLLM's replay ROUTER socket. Sends the start
// sequence as 8 bytes big-endian; the publisher streams (seq, payload)
// messages from its bounded buffer, ending with a sentinel (seq = -1, empty
// payload). A DEALER socket is used deliberately: REQ's strict send/recv
// alternation cannot consume a multi-message stream.
//
// Gap repair (upto_exclusive != UINT64_MAX): succeeds only when the stream
// covers [from_seq, upto_exclusive) contiguously starting exactly at
// from_seq — a hole or short buffer fails, and the caller falls back to the
// reset path. Connect-backfill (upto_exclusive == UINT64_MAX): the first
// received sequence sets the base (the buffer's tail is trimmed by design),
// contiguity is required after it, and reaching the sentinel is success —
// even with zero batches (fresh publisher).
//
// Runs entirely on the worker thread; blocking inside the poll deadline is
// legal here (Rule #12: off-reactor).
// flush_ops ships accumulated ops mid-replay: a full backfill can cover the
// publisher's whole buffer (~10k batches), and holding every translated op
// in one vector until the sentinel would be an unbounded spike (Rule #4).
bool attempt_replay(void* ctx, Subscription& sub, BackendId backend,
                    uint64_t from_seq, uint64_t upto_exclusive,
                    uint32_t timeout_ms, KvBlockLedger& ledger,
                    std::vector<NativeKvOp>& ops,
                    const std::function<void(std::vector<NativeKvOp>&&)>& flush_ops,
                    uint32_t flush_threshold,
                    std::atomic<uint64_t>& replay_batches,
                    std::atomic<uint64_t>& decode_errors) {
    // Bound on accepted replay messages: comfortably above vLLM's default
    // replay buffer (10k batches); a publisher streaming beyond this is
    // misbehaving and the replay fails closed to the reset path.
    constexpr uint32_t kMaxReplayBatches = 65536;
    uint32_t accepted = 0;
    void* sock = zmq_socket(ctx, ZMQ_DEALER);
    if (sock == nullptr) return false;
    int linger = 0;
    zmq_setsockopt(sock, ZMQ_LINGER, &linger, sizeof(linger));
    if (zmq_connect(sock, sub.replay_endpoint.c_str()) != 0) {
        log_kv.warn("Replay connect('{}') failed for backend {}: {}",
                    sub.replay_endpoint, backend, zmq_strerror(zmq_errno()));
        zmq_close(sock);
        return false;
    }
    uint8_t request[8];
    encode_replay_request(from_seq, request);
    if (zmq_send(sock, request, sizeof(request), 0) != static_cast<int>(sizeof(request))) {
        zmq_close(sock);
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    uint64_t next_expected = from_seq;
    uint64_t dropped = 0;
    bool first = true;
    bool sentinel_seen = false;
    bool ok = true;

    while (ok && !sentinel_seen) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) { ok = false; break; }
        zmq_pollitem_t item{sock, 0, ZMQ_POLLIN, 0};
        if (zmq_poll(&item, 1, static_cast<long>(remaining)) <= 0) { ok = false; break; }

        Message msg;
        while (recv_message(sock, msg, dropped)) {
            // Re-check bounds INSIDE the drain: a fast publisher can keep
            // this socket readable indefinitely, and the outer poll deadline
            // alone would never fire.
            if (std::chrono::steady_clock::now() >= deadline ||
                ++accepted > kMaxReplayBatches) {
                ok = false;
                break;
            }
            uint64_t raw_seq = 0;
            const std::vector<uint8_t>* payload = nullptr;
            if (!split_seq_payload(msg, raw_seq, &payload)) {
                continue;  // unrecognized frame shape: skip, deadline bounds us
            }
            const int64_t seq = static_cast<int64_t>(raw_seq);
            if (seq < 0) {
                // Sentinel (canonically seq=-1 + empty payload); any negative
                // sequence ends the stream.
                sentinel_seen = true;
                break;
            }
            const uint64_t s = raw_seq;
            if (upto_exclusive != UINT64_MAX && s >= upto_exclusive) {
                continue;  // past the gap: drain to the sentinel
            }
            if (first) {
                if (upto_exclusive != UINT64_MAX && s != from_seq) {
                    ok = false;  // buffer no longer covers the gap start
                    break;
                }
                next_expected = s;
                first = false;
            }
            if (s != next_expected) {
                ok = false;  // hole inside the replay stream
                break;
            }
            auto batch = decode_kv_event_batch(payload->data(), payload->size());
            if (!batch.has_value()) {
                decode_errors.fetch_add(1, std::memory_order_relaxed);
                ok = false;
                break;
            }
            if (!ledger.translate(backend, *batch, ops)) {
                ok = false;
                break;
            }
            replay_batches.fetch_add(1, std::memory_order_relaxed);
            next_expected = s + 1;
            if (s + 1 > sub.next_seq) {
                sub.next_seq = s + 1;
            }
            sub.synced = true;

            if (ops.size() >= flush_threshold) {
                flush_ops(std::move(ops));
                ops = {};
            }
        }
    }
    zmq_close(sock);
    if (!ok || !sentinel_seen) return false;
    if (upto_exclusive != UINT64_MAX && next_expected < upto_exclusive) {
        return false;  // sentinel arrived before the gap was fully covered
    }
    return true;
}

}  // namespace

KvEventSubscriberService::KvEventSubscriberService(const KvEventsConfig& cfg,
                                                   uint32_t block_alignment)
    : _cfg(cfg)
    , _ledger_limits{block_alignment, cfg.max_indexed_token_depth,
                     cfg.max_tracked_blocks_per_backend,
                     cfg.materialize_routes ? cfg.max_materialize_tokens : 0}
    , _commands(64) {}

KvEventSubscriberService::~KvEventSubscriberService() {
    // stop() must have run (Rule #13 posture). Defensive: a still-running
    // worker would dangle `this`; detachless join here is the lesser evil.
    if (_worker.joinable()) {
        _shutdown.store(true, std::memory_order_release);
        _worker.join();
    }
}

void KvEventSubscriberService::start(seastar::alien::instance& alien) {
    if (_started) return;
    _started = true;
    _worker = std::thread([this, &alien] { worker_loop(alien); });
    log_kv.info("KV-event subscriber started (topic='{}', poll={}ms, depth_cap={} tokens)",
                _cfg.topic, _cfg.poll_interval_ms, _cfg.max_indexed_token_depth);
}

seastar::future<> KvEventSubscriberService::stop() {
    if (_started) {
        _shutdown.store(true, std::memory_order_release);
        Command stop_cmd;
        stop_cmd.kind = Command::Kind::STOP;
        (void)_commands.try_push(std::move(stop_cmd));  // shutdown flag is authoritative
        if (_worker.joinable()) {
            // Bounded by one poll interval + socket teardown; shutdown-time
            // join follows the tokenizer thread-pool precedent.
            _worker.join();
        }
        _started = false;
    }
    // Worker is gone: no new shipments. Drain in-flight applies.
    return _apply_gate.close();
}

bool KvEventSubscriberService::add_subscription(BackendId id, std::string endpoint,
                                                std::string replay_endpoint) {
    Command cmd;
    cmd.kind = Command::Kind::ADD;
    cmd.backend = id;
    cmd.endpoint = std::move(endpoint);
    cmd.replay_endpoint = std::move(replay_endpoint);
    if (!_commands.try_push(std::move(cmd))) {
        _commands_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool KvEventSubscriberService::remove_subscription(BackendId id) {
    Command cmd;
    cmd.kind = Command::Kind::REMOVE;
    cmd.backend = id;
    if (!_commands.try_push(std::move(cmd))) {
        _commands_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

KvEventSubscriberService::StatsSnapshot KvEventSubscriberService::stats() const {
    return {_batches_decoded.load(std::memory_order_relaxed),
            _decode_errors.load(std::memory_order_relaxed),
            _sequence_gaps.load(std::memory_order_relaxed),
            _ops_shipped.load(std::memory_order_relaxed),
            _shipments.load(std::memory_order_relaxed),
            _shipments_throttled.load(std::memory_order_relaxed),
            _commands_dropped.load(std::memory_order_relaxed),
            _replay_requests.load(std::memory_order_relaxed),
            _replay_batches.load(std::memory_order_relaxed),
            _replay_failures.load(std::memory_order_relaxed)};
}

void KvEventSubscriberService::ship(seastar::alien::instance& alien,
                                    std::vector<NativeKvOp>&& ops) {
    // One translate() call can emit far more ops than the shipment cap (a
    // single BlockStored may carry thousands of hashes), so chunk here: the
    // cap is the Rule #17 bound on each synchronous per-shard application.
    // Chunks are cut by summed op WEIGHT (a MATERIALIZE counts one unit per
    // route insertion it will perform), so token-heavy ops can't smuggle an
    // oversized application past the cap.
    for (size_t off = 0; off < ops.size();) {
        size_t end = off;
        uint64_t weight = 0;
        while (end < ops.size()) {
            const uint64_t w = ops[end].weight();
            if (end > off && weight + w > _cfg.max_ops_per_shipment) break;
            weight += w;
            ++end;
        }
        const size_t n = end - off;
        std::vector<NativeKvOp> chunk(
            std::make_move_iterator(ops.begin() + static_cast<ptrdiff_t>(off)),
            std::make_move_iterator(ops.begin() + static_cast<ptrdiff_t>(end)));

        // Bounded in-flight shipments: the worker waits (it's an OS thread —
        // sleeping is legal here) rather than flooding shard 0's task queue.
        while (_inflight.load(std::memory_order_acquire) >= _cfg.max_inflight_shipments) {
            if (_shutdown.load(std::memory_order_acquire)) return;
            _shipments_throttled.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        _inflight.fetch_add(1, std::memory_order_acq_rel);
        _ops_shipped.fetch_add(n, std::memory_order_relaxed);
        _shipments.fetch_add(1, std::memory_order_relaxed);
        off = end;

        try {
            seastar::alien::run_on(alien, 0,
                [this, chunk = std::move(chunk)]() mutable noexcept {
                    on_shipment(std::move(chunk));
                });
        } catch (const std::exception& e) {
            log_kv.warn("KV-event shipment post failed: {}", e.what());
            _inflight.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
}

void KvEventSubscriberService::on_shipment(std::vector<NativeKvOp> ops) noexcept {
    // Shard 0 reactor. `this` is valid: stop() joins the worker (no further
    // posts) and then closes _apply_gate, which waits for these chains.
    if (_apply_gate.is_closed()) {
        _inflight.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    // Rule #15: the vector's buffer was allocated on the worker thread —
    // reallocate reactor-side before handing it to the broadcast.
    std::vector<NativeKvOp> local(ops.begin(), ops.end());
    (void)seastar::with_gate(_apply_gate, [local = std::move(local)]() mutable {
        return RouterService::apply_native_kv_ops(std::move(local));
    }).handle_exception([](std::exception_ptr ep) {
        log_kv.warn("Native KV op application failed: {}", ep);
    }).finally([this] {
        _inflight.fetch_sub(1, std::memory_order_acq_rel);
    });
}

void KvEventSubscriberService::worker_loop(seastar::alien::instance& alien) {
    void* ctx = zmq_ctx_new();
    if (ctx == nullptr) {
        log_kv.error("zmq_ctx_new failed; KV-event subscriber inert");
        return;
    }

    KvBlockLedger ledger(_ledger_limits);
    absl::flat_hash_map<BackendId, Subscription> subs;
    static constexpr size_t kMaxSubscriptions = 256;  // Hard Rule #4

    uint64_t dropped_oversize = 0;
    auto last_heartbeat = std::chrono::steady_clock::now();

    auto now_ms = [] {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    };

    while (!_shutdown.load(std::memory_order_acquire)) {
        std::vector<NativeKvOp> ops;

        // Drain control commands (reallocate endpoint strings worker-side).
        Command cmd;
        while (_commands.try_pop(cmd)) {
            if (cmd.kind == Command::Kind::STOP) {
                _shutdown.store(true, std::memory_order_release);
                break;
            }
            if (cmd.kind == Command::Kind::REMOVE) {
                auto it = subs.find(cmd.backend);
                if (it != subs.end()) {
                    zmq_close(it->second.socket);
                    subs.erase(it);
                    ledger.drop_backend(cmd.backend);
                    log_kv.info("KV-event subscription removed for backend {}", cmd.backend);
                }
                continue;
            }
            // ADD (idempotent: re-add replaces, e.g. endpoint change)
            if (subs.size() >= kMaxSubscriptions && !subs.contains(cmd.backend)) {
                log_kv.warn("KV-event subscription cap ({}) reached; backend {} not subscribed",
                            kMaxSubscriptions, cmd.backend);
                continue;
            }
            std::string endpoint(cmd.endpoint.data(), cmd.endpoint.size());
            void* sock = zmq_socket(ctx, ZMQ_SUB);
            if (sock == nullptr) {
                log_kv.warn("zmq_socket failed for backend {}", cmd.backend);
                continue;
            }
            int linger = 0;
            zmq_setsockopt(sock, ZMQ_LINGER, &linger, sizeof(linger));
            zmq_setsockopt(sock, ZMQ_SUBSCRIBE, _cfg.topic.data(), _cfg.topic.size());
            if (zmq_connect(sock, endpoint.c_str()) != 0) {
                log_kv.warn("zmq_connect('{}') failed for backend {}: {}",
                            endpoint, cmd.backend, zmq_strerror(zmq_errno()));
                zmq_close(sock);
                continue;
            }
            auto existing = subs.find(cmd.backend);
            if (existing != subs.end()) {
                zmq_close(existing->second.socket);
                ledger.drop_backend(cmd.backend);
            }
            std::string replay_endpoint(cmd.replay_endpoint.data(),
                                        cmd.replay_endpoint.size());
            subs[cmd.backend] = Subscription{sock, std::move(endpoint),
                                             std::move(replay_endpoint), 0, false};
            auto& added = subs[cmd.backend];
            log_kv.info("KV-event subscription added: backend {} ← {} (replay: {})",
                        cmd.backend, added.endpoint,
                        added.replay_endpoint.empty() ? "none" : added.replay_endpoint);

            // Connect-backfill: pull the publisher's buffered history so a
            // restarted Ranvier recovers warm backends' recent block state
            // (bounded by the publisher's replay buffer — this is the
            // cold-start story, PUB/SUB itself has no snapshot).
            if (!added.replay_endpoint.empty() && _cfg.replay_on_connect) {
                _replay_requests.fetch_add(1, std::memory_order_relaxed);
                auto flush = [this, &alien](std::vector<NativeKvOp>&& v) {
                    ship(alien, std::move(v));
                };
                if (!attempt_replay(ctx, added, cmd.backend, 0, UINT64_MAX,
                                    _cfg.replay_timeout_ms, ledger, ops,
                                    flush, _cfg.max_ops_per_shipment,
                                    _replay_batches, _decode_errors)) {
                    _replay_failures.fetch_add(1, std::memory_order_relaxed);
                    log_kv.warn("Connect-backfill failed for backend {}; starting "
                                "from live stream only", cmd.backend);
                }
            }
        }
        if (_shutdown.load(std::memory_order_acquire)) break;

        // Poll all sockets. Rebuilt each cycle: subscription churn is rare
        // and N ≤ 256, so the rebuild cost is noise against the poll wait.
        std::vector<zmq_pollitem_t> items;
        std::vector<BackendId> item_backends;
        items.reserve(subs.size());
        item_backends.reserve(subs.size());
        for (auto& [id, sub] : subs) {
            items.push_back(zmq_pollitem_t{sub.socket, 0, ZMQ_POLLIN, 0});
            item_backends.push_back(id);
        }

        if (items.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(_cfg.poll_interval_ms));
        } else {
            zmq_poll(items.data(), static_cast<int>(items.size()),
                     static_cast<long>(_cfg.poll_interval_ms));
        }

        for (size_t i = 0; i < items.size(); ++i) {
            if (!(items[i].revents & ZMQ_POLLIN)) continue;
            BackendId backend = item_backends[i];
            auto it_sub = subs.find(backend);
            if (it_sub == subs.end()) continue;  // erased earlier this cycle
            auto& sub = it_sub->second;

            try {

            // Drain everything readable on this socket this cycle, shipping
            // in op-capped chunks so each reactor application stays bounded.
            Message msg;
            while (recv_message(sub.socket, msg, dropped_oversize)) {
                uint64_t seq = 0;
                const std::vector<uint8_t>* payload = nullptr;
                if (!split_seq_payload(msg, seq, &payload)) {
                    _decode_errors.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                if (sub.synced && seq != sub.next_seq) {
                    _sequence_gaps.fetch_add(1, std::memory_order_relaxed);
                    bool repaired = false;
                    // Replay can only fill a FORWARD gap; a rewind (publisher
                    // restart resetting its sequence) always resets.
                    if (!sub.replay_endpoint.empty() && seq > sub.next_seq) {
                        _replay_requests.fetch_add(1, std::memory_order_relaxed);
                        auto flush = [this, &alien](std::vector<NativeKvOp>&& v) {
                            ship(alien, std::move(v));
                        };
                        repaired = attempt_replay(ctx, sub, backend, sub.next_seq,
                                                  seq, _cfg.replay_timeout_ms,
                                                  ledger, ops, flush,
                                                  _cfg.max_ops_per_shipment,
                                                  _replay_batches, _decode_errors);
                        if (!repaired) {
                            _replay_failures.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    if (repaired) {
                        log_kv.info("KV-event gap for backend {} repaired via replay "
                                    "(seq {}..{})", backend, sub.next_seq, seq);
                    } else {
                        log_kv.warn("KV-event sequence gap for backend {} (expected {}, "
                                    "got {}); resetting native state",
                                    backend, sub.next_seq, seq);
                        ledger.reset_backend(backend, now_ms(), ops);
                    }
                }
                sub.next_seq = seq + 1;
                sub.synced = true;

                auto batch = decode_kv_event_batch(payload->data(), payload->size());
                if (!batch.has_value()) {
                    _decode_errors.fetch_add(1, std::memory_order_relaxed);
                    log_kv.warn("KV-event decode failure for backend {} ({} bytes); "
                                "resetting native state", backend, payload->size());
                    ledger.reset_backend(backend, now_ms(), ops);
                    continue;
                }
                _batches_decoded.fetch_add(1, std::memory_order_relaxed);

                if (!ledger.translate(backend, *batch, ops)) {
                    // block_size incompatible with block_alignment: hashes can
                    // never match — verified residency is impossible here.
                    log_kv.error("Backend {}: vLLM block_size incompatible with "
                                 "routing block_alignment; disabling native KV mode "
                                 "for this backend", backend);
                    ledger.reset_backend(backend, now_ms(), ops);
                    zmq_close(sub.socket);
                    subs.erase(backend);
                    break;
                }

                if (ops.size() >= _cfg.max_ops_per_shipment) {
                    ship(alien, std::move(ops));
                    ops = {};
                }
            }
            } catch (const std::exception& e) {
                // Rule #9: a worker-thread exception (allocation failure,
                // ledger pathology) must not escape worker_loop and
                // terminate the process. Treat as a stream fault: reset
                // this backend and keep serving the others.
                log_kv.warn("KV-event processing failed for backend {}: {}; "
                            "resetting native state", backend, e.what());
                ledger.reset_backend(backend, now_ms(), ops);
            }
        }

        // Heartbeats keep verified-residency freshness alive on idle streams.
        auto now = std::chrono::steady_clock::now();
        if (now - last_heartbeat >=
            std::chrono::milliseconds(_cfg.heartbeat_interval_ms)) {
            last_heartbeat = now;
            uint64_t ts = now_ms();
            for (auto& [id, sub] : subs) {
                if (sub.synced) {
                    ops.push_back({NativeKvOp::Kind::ALIVE, id, 0, ts});
                }
            }
        }

        ship(alien, std::move(ops));
    }

    for (auto& [id, sub] : subs) {
        zmq_close(sub.socket);
    }
    zmq_ctx_term(ctx);
    if (dropped_oversize > 0) {
        log_kv.warn("KV-event worker dropped {} oversized/excess frames", dropped_oversize);
    }
    log_kv.info("KV-event subscriber worker exited");
}

}  // namespace ranvier
