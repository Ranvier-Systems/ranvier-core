// libFuzzer harness for decode_kv_event_batch (vLLM KV-event msgpack decoder).
//
// This is the untrusted-input surface with the widest attack exposure and the
// least prior coverage: the payload is a msgpack blob arriving over ZMQ from a
// vLLM publisher (kv_event_subscriber.cpp), decoded on the subscriber's worker
// thread. decode_kv_event_batch() promises "never throws, nullopt on any
// structural error" — this harness drives arbitrary bytes at it under
// ASan/UBSan to hold that promise honest.
//
// Targets adversarial-audit (Pass A) findings:
//   A1 — float64 timestamp cast to uint64 is UB for inf / out-of-range values
//        (decode_kv_event_batch, the ts_ms assignment). UBSan trips on the
//        first inf-valued 0xcb timestamp the fuzzer reaches.
//
// The decoder is pure (types.hpp only), so — like radix_tree_fuzz — this
// harness needs no Seastar and no ZMQ. It also exercises the replay-frame
// helpers, which parse the same untrusted stream's sequence framing.
//
// See tests/fuzz/README.md for build and run instructions.

#include <cstddef>
#include <cstdint>

#include "kv_event_decoder.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Primary target: the batch decoder. Contract is total (never throws,
    // nullopt on malformed) — any abort/UB/OOM here is a finding.
    auto batch = ranvier::decode_kv_event_batch(data, size);

    // Touch the decoded structure so the optimizer can't elide the decode and
    // so field reads (block_size, ts_ms, token/hash vectors) are exercised.
    if (batch.has_value()) {
        volatile uint64_t sink = batch->ts_ms;
        sink ^= batch->stored.size();
        sink ^= batch->removed.size();
        sink ^= batch->unknown_events;
        sink ^= (batch->all_cleared ? 1u : 0u);
        for (const auto& s : batch->stored) {
            sink ^= s.block_size;
            sink ^= s.block_hashes.size();
            sink ^= s.token_ids.size();
            sink ^= (s.parent_block_hash.has_value() ? *s.parent_block_hash : 0u);
        }
        for (const auto& rm : batch->removed) {
            sink ^= rm.block_hashes.size();
        }
        (void)sink;
    }

    // Secondary target: the replay-sequence framing helpers, which parse the
    // same untrusted ZMQ stream's per-message sequence header. decode_replay_seq
    // requires exactly 8 bytes; feed whatever length the fuzzer produced.
    int64_t seq = 0;
    if (ranvier::decode_replay_seq(data, size, seq)) {
        volatile bool sentinel = ranvier::is_replay_sentinel(seq, size);
        (void)sentinel;
    }

    return 0;
}
