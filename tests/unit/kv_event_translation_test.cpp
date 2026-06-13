// Unit tests for the native KV-event decoder and the hash-bridge ledger.
//
// Both components are pure (no Seastar, no ZMQ): payloads are hand-crafted
// msgpack bytes matching vLLM's msgspec encoding (array_like structs, leading
// tag string per event), and the ledger is exercised directly.
//
// The load-bearing case is BridgingInvariant: chained block accumulation must
// reproduce hash_prefix() of the concatenated token stream exactly — that
// identity is what lets BlockStored/BlockRemoved events address the same
// prefix_hash_index entries that route learning creates.

#include "kv_event_decoder.hpp"
#include "kv_event_ledger.hpp"
#include "parse_utils.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <numeric>
#include <string_view>
#include <vector>

using namespace ranvier;

namespace {

// Minimal msgpack writer for test payloads.
struct Packer {
    std::vector<uint8_t> buf;

    void raw(uint8_t b) { buf.push_back(b); }
    void be(uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i) buf.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
    }
    void arr(size_t n) {
        if (n <= 15) raw(static_cast<uint8_t>(0x90 | n));
        else { raw(0xdc); be(n, 2); }
    }
    void map(size_t n) { raw(static_cast<uint8_t>(0x80 | n)); }
    void str(std::string_view s) {
        raw(static_cast<uint8_t>(0xa0 | s.size()));
        buf.insert(buf.end(), s.begin(), s.end());
    }
    void u64(uint64_t v) { raw(0xcf); be(v, 8); }
    void i64(int64_t v) { raw(0xd3); be(static_cast<uint64_t>(v), 8); }
    void fixint(uint8_t v) { raw(v); }  // 0..127
    void f64(double d) {
        raw(0xcb);
        uint64_t bits;
        std::memcpy(&bits, &d, sizeof(bits));
        be(bits, 8);
    }
    void nil() { raw(0xc0); }

    void int_arr_u64(const std::vector<uint64_t>& vals) {
        arr(vals.size());
        for (auto v : vals) u64(v);
    }
    void token_arr(const std::vector<int32_t>& toks) {
        arr(toks.size());
        for (auto t : toks) i64(t);
    }
};

std::vector<int32_t> make_tokens(size_t n, int32_t start = 1000) {
    std::vector<int32_t> t(n);
    std::iota(t.begin(), t.end(), start);
    return t;
}

// Batch with a single BlockStored event.
std::vector<uint8_t> stored_batch(double ts, const std::vector<uint64_t>& hashes,
                                  std::optional<uint64_t> parent,
                                  const std::vector<int32_t>& tokens,
                                  uint32_t block_size) {
    Packer p;
    p.arr(2);          // [ts, events]
    p.f64(ts);
    p.arr(1);          // one event
    p.arr(6);          // [tag, hashes, parent, tokens, block_size, lora_id]
    p.str("BlockStored");
    p.int_arr_u64(hashes);
    if (parent) p.u64(*parent); else p.nil();
    p.token_arr(tokens);
    p.fixint(static_cast<uint8_t>(block_size));
    p.nil();           // lora_id
    return p.buf;
}

std::vector<uint8_t> removed_batch(double ts, const std::vector<uint64_t>& hashes) {
    Packer p;
    p.arr(2);
    p.f64(ts);
    p.arr(1);
    p.arr(2);
    p.str("BlockRemoved");
    p.int_arr_u64(hashes);
    return p.buf;
}

KvBlockLedger::Limits limits(uint32_t alignment = 16, uint32_t depth = 2048,
                             uint32_t max_blocks = 65536,
                             uint32_t materialize = 0) {
    return {alignment, depth, max_blocks, materialize};
}

}  // namespace

// =============================================================================
// Decoder
// =============================================================================

TEST(KvDecoder, BlockStoredBasic) {
    auto tokens = make_tokens(32);
    auto buf = stored_batch(1711000000.5, {0xAA, 0xBB}, std::nullopt, tokens, 16);

    auto batch = decode_kv_event_batch(buf.data(), buf.size());
    ASSERT_TRUE(batch.has_value());
    EXPECT_EQ(batch->ts_ms, 1711000000500ULL);
    ASSERT_EQ(batch->stored.size(), 1u);
    EXPECT_EQ(batch->removed.size(), 0u);
    EXPECT_FALSE(batch->all_cleared);
    EXPECT_EQ(batch->unknown_events, 0u);

    const auto& ev = batch->stored[0];
    EXPECT_EQ(ev.block_hashes, (std::vector<uint64_t>{0xAA, 0xBB}));
    EXPECT_FALSE(ev.parent_block_hash.has_value());
    EXPECT_EQ(ev.token_ids, tokens);
    EXPECT_EQ(ev.block_size, 16u);
}

TEST(KvDecoder, BlockRemovedAndCleared) {
    Packer p;
    p.arr(2);
    p.f64(2.0);
    p.arr(2);
    p.arr(2);
    p.str("BlockRemoved");
    p.int_arr_u64({0xCC});
    p.arr(1);
    p.str("AllBlocksCleared");

    auto batch = decode_kv_event_batch(p.buf.data(), p.buf.size());
    ASSERT_TRUE(batch.has_value());
    ASSERT_EQ(batch->removed.size(), 1u);
    EXPECT_EQ(batch->removed[0].block_hashes, (std::vector<uint64_t>{0xCC}));
    EXPECT_TRUE(batch->all_cleared);
}

TEST(KvDecoder, TolerantToExtraFieldsAndBatchElements) {
    // Newer-vLLM shape: extra event field (e.g. medium), extra batch element
    // (e.g. data_parallel_rank). Both must be skipped, not fatal.
    auto tokens = make_tokens(16);
    Packer p;
    p.arr(3);              // [ts, events, extra]
    p.f64(3.0);
    p.arr(1);
    p.arr(7);              // BlockStored + one extra field
    p.str("BlockStored");
    p.int_arr_u64({0x1});
    p.nil();
    p.token_arr(tokens);
    p.fixint(16);
    p.nil();
    p.str("gpu");          // extra field
    p.fixint(7);           // batch extra element

    auto batch = decode_kv_event_batch(p.buf.data(), p.buf.size());
    ASSERT_TRUE(batch.has_value());
    ASSERT_EQ(batch->stored.size(), 1u);
    EXPECT_EQ(batch->stored[0].block_size, 16u);
}

TEST(KvDecoder, OmittedTrailingOptionalTolerated) {
    // omit_defaults: lora_id absent → 5-element BlockStored array is legal.
    auto tokens = make_tokens(16);
    Packer p;
    p.arr(2);
    p.f64(4.0);
    p.arr(1);
    p.arr(5);
    p.str("BlockStored");
    p.int_arr_u64({0x2});
    p.nil();
    p.token_arr(tokens);
    p.fixint(16);

    auto batch = decode_kv_event_batch(p.buf.data(), p.buf.size());
    ASSERT_TRUE(batch.has_value());
    ASSERT_EQ(batch->stored.size(), 1u);
}

TEST(KvDecoder, UnknownTagSkippedNotFatal) {
    Packer p;
    p.arr(2);
    p.f64(5.0);
    p.arr(2);
    p.arr(3);              // unknown event with payload to skip
    p.str("BlockMigrated");
    p.int_arr_u64({0x9});
    p.map(1);
    p.str("k");
    p.fixint(1);
    p.arr(2);              // then a normal removal
    p.str("BlockRemoved");
    p.int_arr_u64({0x3});

    auto batch = decode_kv_event_batch(p.buf.data(), p.buf.size());
    ASSERT_TRUE(batch.has_value());
    EXPECT_EQ(batch->unknown_events, 1u);
    ASSERT_EQ(batch->removed.size(), 1u);
}

TEST(KvDecoder, NegativeBlockHashBitCast) {
    // Python's builtin hash can be negative; the wire carries int64. Identity
    // comparison is all the ledger needs, so the bit-cast value must round-trip.
    auto buf = removed_batch(6.0, {});
    Packer p;
    p.arr(2);
    p.f64(6.0);
    p.arr(1);
    p.arr(2);
    p.str("BlockRemoved");
    p.arr(1);
    p.i64(-2);

    auto batch = decode_kv_event_batch(p.buf.data(), p.buf.size());
    ASSERT_TRUE(batch.has_value());
    ASSERT_EQ(batch->removed.size(), 1u);
    EXPECT_EQ(batch->removed[0].block_hashes[0], static_cast<uint64_t>(int64_t{-2}));
    (void)buf;
}

TEST(KvDecoder, RejectsTruncatedAndGarbage) {
    auto tokens = make_tokens(16);
    auto good = stored_batch(7.0, {0x5}, std::nullopt, tokens, 16);
    for (size_t cut : {size_t{1}, good.size() / 2, good.size() - 1}) {
        EXPECT_FALSE(decode_kv_event_batch(good.data(), cut).has_value()) << "cut=" << cut;
    }
    std::vector<uint8_t> garbage = {0xc1, 0x00, 0x01};  // 0xc1 is reserved
    EXPECT_FALSE(decode_kv_event_batch(garbage.data(), garbage.size()).has_value());
    EXPECT_FALSE(decode_kv_event_batch(nullptr, 10).has_value());
    EXPECT_FALSE(decode_kv_event_batch(good.data(), 0).has_value());
}

// =============================================================================
// Ledger: the hash bridge
// =============================================================================

TEST(KvLedger, BridgingInvariant) {
    // THE invariant: chained accumulation == hash_prefix() of the concatenated
    // token stream at every block boundary. 48 tokens, block_size 16,
    // alignment 16; blocks A,B arrive together (parent nil), C chains off B.
    auto tokens = make_tokens(48);
    KvBlockLedger ledger(limits());
    std::vector<NativeKvOp> ops;

    auto payload1 = stored_batch(1.0, {0xA, 0xB}, std::nullopt,
                                 {tokens.begin(), tokens.begin() + 32}, 16);
    auto b1 = decode_kv_event_batch(payload1.data(), payload1.size());
    ASSERT_TRUE(b1.has_value());
    ASSERT_TRUE(ledger.translate(7, *b1, ops));

    auto payload2 = stored_batch(2.0, {0xC}, uint64_t{0xB},
                                 {tokens.begin() + 32, tokens.end()}, 16);
    auto b2 = decode_kv_event_batch(payload2.data(), payload2.size());
    ASSERT_TRUE(b2.has_value());
    ASSERT_TRUE(ledger.translate(7, *b2, ops));

    ASSERT_EQ(ops.size(), 3u);
    for (const auto& op : ops) {
        EXPECT_EQ(op.kind, NativeKvOp::Kind::UPSERT);
        EXPECT_EQ(op.backend, 7);
    }
    EXPECT_EQ(ops[0].prefix_hash, hash_prefix(tokens.data(), 16, 16));
    EXPECT_EQ(ops[1].prefix_hash, hash_prefix(tokens.data(), 32, 16));
    EXPECT_EQ(ops[2].prefix_hash, hash_prefix(tokens.data(), 48, 16));
    EXPECT_EQ(ops[0].ts_ms, 1000u);
    EXPECT_EQ(ops[2].ts_ms, 2000u);
}

TEST(KvLedger, RemovalEmitsTheSameBridgedHash) {
    auto tokens = make_tokens(32);
    KvBlockLedger ledger(limits());
    std::vector<NativeKvOp> ops;

    auto p1 = stored_batch(1.0, {0xA, 0xB}, std::nullopt, tokens, 16);
    auto b1 = decode_kv_event_batch(p1.data(), p1.size());
    ASSERT_TRUE(ledger.translate(7, *b1, ops));
    ops.clear();

    auto p2 = removed_batch(2.0, {0xB});
    auto b2 = decode_kv_event_batch(p2.data(), p2.size());
    ASSERT_TRUE(ledger.translate(7, *b2, ops));

    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0].kind, NativeKvOp::Kind::REMOVE);
    EXPECT_EQ(ops[0].prefix_hash, hash_prefix(tokens.data(), 32, 16));
    EXPECT_EQ(ledger.tracked_blocks(7), 1u);  // A remains
}

TEST(KvLedger, OrphanParentProducesNoOps) {
    auto tokens = make_tokens(16);
    KvBlockLedger ledger(limits());
    std::vector<NativeKvOp> ops;

    auto p = stored_batch(1.0, {0xC}, uint64_t{0xDEAD}, tokens, 16);
    auto b = decode_kv_event_batch(p.data(), p.size());
    ASSERT_TRUE(ledger.translate(7, *b, ops));
    EXPECT_TRUE(ops.empty());
    EXPECT_EQ(ledger.stats().orphan_chains, 1u);
}

TEST(KvLedger, DepthCapStopsTrackingChain) {
    auto tokens = make_tokens(48);
    KvBlockLedger ledger(limits(16, /*depth=*/32));
    std::vector<NativeKvOp> ops;

    auto p = stored_batch(1.0, {0xA, 0xB, 0xC}, std::nullopt, tokens, 16);
    auto b = decode_kv_event_batch(p.data(), p.size());
    ASSERT_TRUE(ledger.translate(7, *b, ops));

    // Blocks at depths 16 and 32 indexed; the 48-deep block is dropped.
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ledger.stats().blocks_dropped_depth, 1u);
    EXPECT_EQ(ledger.tracked_blocks(7), 2u);
}

TEST(KvLedger, CapacityCapDropsWithCounter) {
    auto tokens = make_tokens(32);
    KvBlockLedger ledger(limits(16, 2048, /*max_blocks=*/1));
    std::vector<NativeKvOp> ops;

    auto p = stored_batch(1.0, {0xA, 0xB}, std::nullopt, tokens, 16);
    auto b = decode_kv_event_batch(p.data(), p.size());
    ASSERT_TRUE(ledger.translate(7, *b, ops));
    EXPECT_EQ(ops.size(), 1u);
    EXPECT_EQ(ledger.stats().blocks_dropped_capacity, 1u);
}

TEST(KvLedger, MisalignedBlockSizeRejected) {
    auto tokens = make_tokens(10);
    KvBlockLedger ledger(limits(/*alignment=*/16));
    std::vector<NativeKvOp> ops;

    auto p = stored_batch(1.0, {0xA}, std::nullopt, tokens, 10);
    auto b = decode_kv_event_batch(p.data(), p.size());
    ASSERT_TRUE(b.has_value());
    EXPECT_FALSE(ledger.translate(7, *b, ops));
    EXPECT_EQ(ledger.stats().misaligned_backends, 1u);
}

TEST(KvLedger, MalformedTokenCountSkipped) {
    auto tokens = make_tokens(17);  // not hashes(1) * block_size(16)
    KvBlockLedger ledger(limits());
    std::vector<NativeKvOp> ops;

    auto p = stored_batch(1.0, {0xA}, std::nullopt, tokens, 16);
    auto b = decode_kv_event_batch(p.data(), p.size());
    ASSERT_TRUE(ledger.translate(7, *b, ops));
    EXPECT_TRUE(ops.empty());
    EXPECT_EQ(ledger.stats().malformed_events, 1u);
}

TEST(KvLedger, AllClearedWipesAndOrphansFollowers) {
    auto tokens = make_tokens(16);
    KvBlockLedger ledger(limits());
    std::vector<NativeKvOp> ops;

    auto p1 = stored_batch(1.0, {0xA}, std::nullopt, tokens, 16);
    auto b1 = decode_kv_event_batch(p1.data(), p1.size());
    ASSERT_TRUE(ledger.translate(7, *b1, ops));
    ops.clear();

    Packer p;
    p.arr(2);
    p.f64(2.0);
    p.arr(1);
    p.arr(1);
    p.str("AllBlocksCleared");
    auto b2 = decode_kv_event_batch(p.buf.data(), p.buf.size());
    ASSERT_TRUE(ledger.translate(7, *b2, ops));
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0].kind, NativeKvOp::Kind::CLEAR);
    EXPECT_EQ(ledger.tracked_blocks(7), 0u);

    // A child of the pre-clear block is now an orphan.
    ops.clear();
    auto p3 = stored_batch(3.0, {0xB}, uint64_t{0xA}, tokens, 16);
    auto b3 = decode_kv_event_batch(p3.data(), p3.size());
    ASSERT_TRUE(ledger.translate(7, *b3, ops));
    EXPECT_TRUE(ops.empty());
    EXPECT_EQ(ledger.stats().orphan_chains, 1u);
}

TEST(KvLedger, ResetEmitsResetAndWipes) {
    auto tokens = make_tokens(16);
    KvBlockLedger ledger(limits());
    std::vector<NativeKvOp> ops;

    auto p1 = stored_batch(1.0, {0xA}, std::nullopt, tokens, 16);
    auto b1 = decode_kv_event_batch(p1.data(), p1.size());
    ASSERT_TRUE(ledger.translate(7, *b1, ops));
    ops.clear();

    ledger.reset_backend(7, 5000, ops);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0].kind, NativeKvOp::Kind::RESET);
    EXPECT_EQ(ops[0].ts_ms, 5000u);
    EXPECT_EQ(ledger.tracked_blocks(7), 0u);
}

TEST(KvLedger, WireOrderPreservedAcrossEventTypes) {
    // Batch: BlockRemoved(A) BEFORE BlockStored(A) — a remove/re-store cycle.
    // Translation must keep that order so the re-store's UPSERT lands last.
    auto tokens = make_tokens(16);
    KvBlockLedger ledger(limits());
    std::vector<NativeKvOp> ops;

    auto p0 = stored_batch(1.0, {0xA}, std::nullopt, tokens, 16);
    auto b0 = decode_kv_event_batch(p0.data(), p0.size());
    ASSERT_TRUE(ledger.translate(7, *b0, ops));
    ops.clear();

    Packer p;
    p.arr(2);
    p.f64(2.0);
    p.arr(2);
    p.arr(2);                       // event 0: remove A
    p.str("BlockRemoved");
    p.int_arr_u64({0xA});
    p.arr(6);                       // event 1: re-store A
    p.str("BlockStored");
    p.int_arr_u64({0xA});
    p.nil();
    p.token_arr(tokens);
    p.fixint(16);
    p.nil();

    auto b = decode_kv_event_batch(p.buf.data(), p.buf.size());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(ledger.translate(7, *b, ops));
    ASSERT_EQ(ops.size(), 2u);
    EXPECT_EQ(ops[0].kind, NativeKvOp::Kind::REMOVE);
    EXPECT_EQ(ops[1].kind, NativeKvOp::Kind::UPSERT);
    EXPECT_EQ(ops[0].prefix_hash, ops[1].prefix_hash);
}

// =============================================================================
// Ledger: route materialization
// =============================================================================

TEST(KvLedgerMaterialize, EmitsCumulativeTokensAndNewBoundaries) {
    auto all = make_tokens(48);
    KvBlockLedger ledger(limits(16, 2048, 65536, /*materialize=*/128));
    std::vector<NativeKvOp> ops;

    // Event 1: blocks A,B (tokens 0..31). New boundaries: 16 and 32.
    auto p1 = stored_batch(1.0, {0xA, 0xB}, std::nullopt,
                           {all.begin(), all.begin() + 32}, 16);
    auto b1 = decode_kv_event_batch(p1.data(), p1.size());
    ASSERT_TRUE(ledger.translate(7, *b1, ops));

    ASSERT_EQ(ops.size(), 3u);  // 2 UPSERTs + 1 MATERIALIZE
    const auto& m1 = ops.back();
    EXPECT_EQ(m1.kind, NativeKvOp::Kind::MATERIALIZE);
    EXPECT_EQ(m1.backend, 7);
    EXPECT_EQ(m1.first_new_token_count, 16u);
    EXPECT_EQ(m1.boundary_step, 16u);
    EXPECT_EQ(m1.tokens, std::vector<int32_t>(all.begin(), all.begin() + 32));
    EXPECT_EQ(m1.weight(), 1u + 2u);  // two boundary insertions

    // Event 2: block C chains off B (tokens 32..47). Cumulative tokens must
    // be rebuilt through the parent walk; only boundary 48 is new.
    ops.clear();
    auto p2 = stored_batch(2.0, {0xC}, uint64_t{0xB},
                           {all.begin() + 32, all.end()}, 16);
    auto b2 = decode_kv_event_batch(p2.data(), p2.size());
    ASSERT_TRUE(ledger.translate(7, *b2, ops));
    ASSERT_EQ(ops.size(), 2u);  // 1 UPSERT + 1 MATERIALIZE
    const auto& m2 = ops.back();
    EXPECT_EQ(m2.kind, NativeKvOp::Kind::MATERIALIZE);
    EXPECT_EQ(m2.first_new_token_count, 48u);
    EXPECT_EQ(m2.tokens, all);
    EXPECT_EQ(ledger.stats().materialize_ops, 2u);
}

TEST(KvLedgerMaterialize, CapBoundsTokensAndSkipsBeyond) {
    auto all = make_tokens(64);
    KvBlockLedger ledger(limits(16, 2048, 65536, /*materialize=*/32));
    std::vector<NativeKvOp> ops;

    // Blocks at depths 16,32,48,64: materialization covers only up to 32.
    auto p = stored_batch(1.0, {0xA, 0xB, 0xC, 0xD}, std::nullopt, all, 16);
    auto b = decode_kv_event_batch(p.data(), p.size());
    ASSERT_TRUE(ledger.translate(7, *b, ops));

    size_t mat = 0;
    for (const auto& op : ops) {
        if (op.kind == NativeKvOp::Kind::MATERIALIZE) {
            mat++;
            EXPECT_EQ(op.tokens.size(), 32u);
            EXPECT_EQ(op.first_new_token_count, 16u);
        }
    }
    EXPECT_EQ(mat, 1u);

    // A follow-up event entirely beyond the cap emits no MATERIALIZE.
    ops.clear();
    auto deeper = make_tokens(16, 5000);
    auto p2 = stored_batch(2.0, {0xE}, uint64_t{0xD}, deeper, 16);
    auto b2 = decode_kv_event_batch(p2.data(), p2.size());
    ASSERT_TRUE(ledger.translate(7, *b2, ops));
    for (const auto& op : ops) {
        EXPECT_NE(op.kind, NativeKvOp::Kind::MATERIALIZE);
    }
}

TEST(KvLedgerMaterialize, DisabledEmitsNothing) {
    auto all = make_tokens(32);
    KvBlockLedger ledger(limits());  // materialize = 0
    std::vector<NativeKvOp> ops;
    auto p = stored_batch(1.0, {0xA, 0xB}, std::nullopt, all, 16);
    auto b = decode_kv_event_batch(p.data(), p.size());
    ASSERT_TRUE(ledger.translate(7, *b, ops));
    for (const auto& op : ops) {
        EXPECT_NE(op.kind, NativeKvOp::Kind::MATERIALIZE);
    }
    EXPECT_EQ(ledger.stats().materialize_ops, 0u);
}

// =============================================================================
// Replay frame helpers
// =============================================================================

TEST(ReplayFrames, RequestEncodingRoundTrips) {
    uint8_t buf[8];
    encode_replay_request(0x0102030405060708ULL, buf);
    int64_t seq = 0;
    ASSERT_TRUE(decode_replay_seq(buf, sizeof(buf), seq));
    EXPECT_EQ(static_cast<uint64_t>(seq), 0x0102030405060708ULL);
}

TEST(ReplayFrames, SentinelDetection) {
    uint8_t buf[8];
    encode_replay_request(static_cast<uint64_t>(int64_t{-1}), buf);
    int64_t seq = 0;
    ASSERT_TRUE(decode_replay_seq(buf, sizeof(buf), seq));
    EXPECT_EQ(seq, -1);
    EXPECT_TRUE(is_replay_sentinel(seq, 0));
    EXPECT_FALSE(is_replay_sentinel(seq, 10));   // payload present: not the sentinel
    EXPECT_FALSE(is_replay_sentinel(42, 0));     // positive seq: not the sentinel
}

TEST(ReplayFrames, RejectsWrongLength) {
    uint8_t buf[8] = {0};
    int64_t seq = 0;
    EXPECT_FALSE(decode_replay_seq(buf, 7, seq));
    EXPECT_FALSE(decode_replay_seq(nullptr, 8, seq));
}
