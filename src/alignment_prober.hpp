// Ranvier Core - Backend Alignment Prober (BACKLOG section 30)
//
// Asks each backend with a live native KV-event stream whether it tokenizes
// like Ranvier: renders the fixed probe conversation through Ranvier's own
// routing path (RequestRewriter + the configured ChatTemplate + the shard's
// TokenizerService), POSTs the same messages to the backend's /tokenize, and
// classifies the two id sequences (alignment_probe.hpp).
//
// Driven from HealthService's shard-0 loop (no timer of its own, Rule #5): one
// run_cycle() per health cycle, awaited inside the loop, so the loop's gate
// covers every probe in flight. A backend is probed when its stream is fresh
// and has no verdict for its current complete epoch (the stream's publisher
// started at sequence 0 or its cache was cleared since the last verdict), so
// an engine restart or a model swap is re-probed. Failures back off.
//
// Warn-only: the verdict is logged and kept here. Quarantining a misaligned
// backend's stream is section 30 Step 3.

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include <seastar/core/future.hh>
#include <seastar/core/sharded.hh>
#include <seastar/net/socket_defs.hh>

#include "alignment_probe.hpp"
#include "chat_template.hpp"
#include "types.hpp"

namespace ranvier {

class TokenizerService;

class AlignmentProber {
public:
    struct Config {
        ChatTemplate chat_template;
        uint32_t block_alignment = 16;
        size_t prefix_token_length = 128;
        size_t indexed_depth = 2048;
        std::chrono::milliseconds timeout{2000};  // connect + request + response
        std::string tokenizer_path;               // for the misaligned warning only
    };

    struct Stats {
        uint64_t aligned = 0;
        uint64_t misaligned = 0;
        uint64_t unsupported = 0;
        uint64_t failed = 0;
    };

    AlignmentProber(Config cfg, seastar::sharded<TokenizerService>& tokenizer);

    // Shard 0 only. Probes every eligible backend in `backends` (bounded
    // concurrency) and prunes state for backends no longer listed.
    seastar::future<> run_cycle(std::vector<std::pair<BackendId, seastar::socket_address>> backends);

    // Latest verdict for a backend (nullopt: never probed, or only failures).
    std::optional<alignment_probe::Result> verdict(BackendId id) const;

    Stats stats() const { return _stats; }

    // Rule #4: one entry per tracked backend (same bound as HealthService).
    static constexpr size_t MAX_TRACKED_BACKENDS = 256;
    static constexpr size_t MAX_CONCURRENT_PROBES = 4;
    // Failed probes retry after 1, 2, 4, 8, 8, ... health cycles.
    static constexpr uint32_t MAX_BACKOFF_CYCLES = 8;

private:
    struct BackendState {
        bool has_verdict = false;           // a non-FAILED result is held
        uint64_t epoch = 0;                 // complete epoch the verdict belongs to
        alignment_probe::Result result;
        uint32_t consecutive_failures = 0;
        uint64_t next_attempt_cycle = 0;
    };

    struct RanvierSide {
        std::vector<int32_t> ids;
        size_t routing_prefix = 0;
    };

    struct Candidate {
        BackendId id = 0;
        seastar::socket_address addr;
        uint64_t epoch = 0;
    };

    // Ranvier's ids and routing prefix for the probe conversation; computed
    // once (the chat template and tokenizer only change with a restart).
    seastar::future<bool> ensure_ranvier_side();
    seastar::future<> probe_one(Candidate c);
    seastar::future<std::optional<alignment_probe::HttpResponse>>
        post_tokenize(seastar::socket_address addr);
    void record(const Candidate& c, alignment_probe::Result r, std::string_view error);

    Config _cfg;
    seastar::sharded<TokenizerService>& _tokenizer;
    std::string _chat_body;  // the probe conversation, as both sides receive it
    std::optional<RanvierSide> _ranvier;
    bool _ranvier_disabled = false;   // the template yields no text: never retried
    uint64_t _ranvier_failures = 0;   // tokenizer failures, for log throttling
    absl::flat_hash_map<BackendId, BackendState> _state;
    uint64_t _cycle = 0;
    uint64_t _state_overflow_drops = 0;
    Stats _stats;
};

}  // namespace ranvier
