// Ranvier Core - vLLM KV-Event Decoder (pure, bounded msgpack subset)
//
// Decodes vLLM's native KV-cache event stream: a `KVEventBatch` published
// over ZMQ as a msgpack payload (msgspec-encoded, array_like structs with a
// leading tag string on each event). Schema reference: vLLM
// `vllm/distributed/kv_events.py` and the documented subscriber example.
//
//   KVEventBatch  = [ ts: float64, events: [Event, ...], ...extras ]
//   BlockStored   = [ "BlockStored", block_hashes: [int...],
//                     parent_block_hash: int|nil, token_ids: [int...],
//                     block_size: int, lora_id: int|nil, ...extras ]
//   BlockRemoved  = [ "BlockRemoved", block_hashes: [int...], ...extras ]
//   AllBlocksCleared = [ "AllBlocksCleared" ]
//
// Forward-compatibility posture: UNKNOWN event tags are skipped (counted, not
// fatal), EXTRA trailing array elements are skipped, and absent trailing
// optionals default — msgspec's `omit_defaults` makes short arrays legal.
// The operator's vLLM version is validated against this decoder during
// integration testing; schema drift surfaces as decode_errors, never UB.
//
// This header is PURE: no Seastar, no ZMQ — directly unit-testable. It runs
// on the KV-event subscriber's dedicated OS thread, never on the reactor.
//
// Rule #4: every container and recursion is explicitly bounded (kMax* caps).
// Rule #10: all integer reads are explicit bounds-checked byte assembly.

#pragma once

#include "types.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

namespace ranvier {

// Hard caps on decoded structures (Rule #4). Generous against real batches
// (vLLM batches per scheduler step); a hostile or corrupt payload hits a cap
// and the whole batch is rejected — the gap/reset path then resynchronizes.
inline constexpr size_t kKvMaxPayloadBytes   = 4 * 1024 * 1024;
inline constexpr size_t kKvMaxEventsPerBatch = 8192;
inline constexpr size_t kKvMaxHashesPerEvent = 16384;
inline constexpr size_t kKvMaxTokensPerEvent = 256 * 1024;
inline constexpr size_t kKvMaxSkipDepth      = 16;

struct KvBlockStored {
    std::vector<uint64_t> block_hashes;
    std::optional<uint64_t> parent_block_hash;
    std::vector<int32_t> token_ids;
    uint32_t block_size = 0;
};

struct KvBlockRemoved {
    std::vector<uint64_t> block_hashes;
};

// One decoded batch. Events keep wire order — ordering is semantically
// load-bearing (a parent's BlockStored precedes its children's).
struct KvEventBatch {
    uint64_t ts_ms = 0;            // batch timestamp, milliseconds
    std::vector<KvBlockStored> stored;
    std::vector<KvBlockRemoved> removed;
    // Wire order across the two vectors: order_stored[i] gives the position
    // of stored[i] in the original event sequence (likewise removed).
    std::vector<uint32_t> order_stored;
    std::vector<uint32_t> order_removed;
    bool all_cleared = false;       // an AllBlocksCleared appeared in the batch
    uint32_t unknown_events = 0;    // skipped (forward compat)
};

namespace kv_msgpack {

// Cursor over the payload. All reads bounds-check against `end`.
struct Reader {
    const uint8_t* p;
    const uint8_t* end;

    bool ok(size_t n) const { return static_cast<size_t>(end - p) >= n; }
    size_t remaining() const { return static_cast<size_t>(end - p); }

    bool u8(uint8_t& out) {
        if (!ok(1)) return false;
        out = *p++;
        return true;
    }

    bool big_endian(size_t n, uint64_t& out) {
        if (!ok(n)) return false;
        out = 0;
        for (size_t i = 0; i < n; ++i) out = (out << 8) | *p++;
        return true;
    }
};

// Decoded scalar kinds the schema needs. Anything else is "other" and only
// legal where skip_value() is used.
enum class Kind : uint8_t { INT, F64, STR, ARR, MAP, NIL, BOOL, BIN, OTHER };

struct Header {
    Kind kind = Kind::OTHER;
    uint64_t uval = 0;     // INT: raw value bits; ARR/MAP/STR/BIN: length
    bool negative = false; // INT only: value is int64-negative (bits in uval)
    double fval = 0.0;     // F64 only
};

// Reads one msgpack value header (and the payload for scalars). For
// STR/BIN the cursor is left AT the bytes; for ARR/MAP at the first element.
inline bool read_header(Reader& r, Header& h) {
    uint8_t b;
    if (!r.u8(b)) return false;

    if (b <= 0x7f) { h.kind = Kind::INT; h.uval = b; return true; }
    if (b >= 0xe0) { h.kind = Kind::INT; h.uval = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(b))); h.negative = true; return true; }
    if (b >= 0xa0 && b <= 0xbf) { h.kind = Kind::STR; h.uval = b & 0x1f; return true; }
    if (b >= 0x90 && b <= 0x9f) { h.kind = Kind::ARR; h.uval = b & 0x0f; return true; }
    if (b >= 0x80 && b <= 0x8f) { h.kind = Kind::MAP; h.uval = b & 0x0f; return true; }

    uint64_t v = 0;
    switch (b) {
    case 0xc0: h.kind = Kind::NIL; return true;
    case 0xc2: h.kind = Kind::BOOL; h.uval = 0; return true;
    case 0xc3: h.kind = Kind::BOOL; h.uval = 1; return true;
    case 0xcc: if (!r.big_endian(1, v)) return false; h.kind = Kind::INT; h.uval = v; return true;
    case 0xcd: if (!r.big_endian(2, v)) return false; h.kind = Kind::INT; h.uval = v; return true;
    case 0xce: if (!r.big_endian(4, v)) return false; h.kind = Kind::INT; h.uval = v; return true;
    case 0xcf: if (!r.big_endian(8, v)) return false; h.kind = Kind::INT; h.uval = v; return true;
    case 0xd0: if (!r.big_endian(1, v)) return false; h.kind = Kind::INT; h.uval = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(v))); h.negative = static_cast<int8_t>(v) < 0; return true;
    case 0xd1: if (!r.big_endian(2, v)) return false; h.kind = Kind::INT; h.uval = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(v))); h.negative = static_cast<int16_t>(v) < 0; return true;
    case 0xd2: if (!r.big_endian(4, v)) return false; h.kind = Kind::INT; h.uval = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); h.negative = static_cast<int32_t>(v) < 0; return true;
    case 0xd3: if (!r.big_endian(8, v)) return false; h.kind = Kind::INT; h.uval = v; h.negative = static_cast<int64_t>(v) < 0; return true;
    case 0xca: { // float32 — widen
        if (!r.big_endian(4, v)) return false;
        uint32_t bits = static_cast<uint32_t>(v);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        h.kind = Kind::F64; h.fval = static_cast<double>(f);
        return true;
    }
    case 0xcb: {
        if (!r.big_endian(8, v)) return false;
        double d;
        std::memcpy(&d, &v, sizeof(d));
        h.kind = Kind::F64; h.fval = d;
        return true;
    }
    case 0xd9: if (!r.big_endian(1, v)) return false; h.kind = Kind::STR; h.uval = v; return true;
    case 0xda: if (!r.big_endian(2, v)) return false; h.kind = Kind::STR; h.uval = v; return true;
    case 0xdb: if (!r.big_endian(4, v)) return false; h.kind = Kind::STR; h.uval = v; return true;
    case 0xc4: if (!r.big_endian(1, v)) return false; h.kind = Kind::BIN; h.uval = v; return true;
    case 0xc5: if (!r.big_endian(2, v)) return false; h.kind = Kind::BIN; h.uval = v; return true;
    case 0xc6: if (!r.big_endian(4, v)) return false; h.kind = Kind::BIN; h.uval = v; return true;
    case 0xdc: if (!r.big_endian(2, v)) return false; h.kind = Kind::ARR; h.uval = v; return true;
    case 0xdd: if (!r.big_endian(4, v)) return false; h.kind = Kind::ARR; h.uval = v; return true;
    case 0xde: if (!r.big_endian(2, v)) return false; h.kind = Kind::MAP; h.uval = v; return true;
    case 0xdf: if (!r.big_endian(4, v)) return false; h.kind = Kind::MAP; h.uval = v; return true;
    default:
        // ext types and reserved bytes — not part of the schema subset.
        return false;
    }
}

// Skip one value of any supported kind. Depth-capped recursion (Rule #4 /
// Rule #17 posture: bounded work even on adversarial nesting).
inline bool skip_value(Reader& r, size_t depth = 0) {
    if (depth > kKvMaxSkipDepth) return false;
    Header h;
    if (!read_header(r, h)) return false;
    switch (h.kind) {
    case Kind::STR:
    case Kind::BIN:
        if (!r.ok(h.uval)) return false;
        r.p += h.uval;
        return true;
    case Kind::ARR:
        if (h.uval > kKvMaxEventsPerBatch + kKvMaxHashesPerEvent) return false;
        for (uint64_t i = 0; i < h.uval; ++i) {
            if (!skip_value(r, depth + 1)) return false;
        }
        return true;
    case Kind::MAP:
        if (h.uval > kKvMaxEventsPerBatch) return false;
        for (uint64_t i = 0; i < h.uval; ++i) {
            if (!skip_value(r, depth + 1)) return false;
            if (!skip_value(r, depth + 1)) return false;
        }
        return true;
    default:
        return true;  // scalars consumed by read_header
    }
}

inline bool read_str(Reader& r, std::string_view& out, size_t max_len = 64) {
    Header h;
    if (!read_header(r, h) || h.kind != Kind::STR || h.uval > max_len || !r.ok(h.uval)) {
        return false;
    }
    out = std::string_view(reinterpret_cast<const char*>(r.p), h.uval);
    r.p += h.uval;
    return true;
}

// Integer array → uint64 values (vLLM block hashes are 64-bit ints; negative
// Python hashes arrive as int64 — stored bit-cast, which is fine because the
// ledger only ever compares them for identity).
inline bool read_u64_array(Reader& r, std::vector<uint64_t>& out, size_t max_n) {
    Header h;
    if (!read_header(r, h) || h.kind != Kind::ARR || h.uval > max_n) return false;
    // Reserve at most what the remaining payload can encode: every element is
    // ≥1 wire byte, so a tiny payload claiming a huge array (h.uval up to max_n)
    // can't actually deliver more than r.remaining() elements — clamp the
    // reserve to that bound rather than eagerly allocating for the claim (Rule #4).
    const size_t fits = r.remaining();
    out.reserve(out.size() + (h.uval < fits ? h.uval : fits));
    for (uint64_t i = 0; i < h.uval; ++i) {
        Header e;
        if (!read_header(r, e) || e.kind != Kind::INT) return false;
        out.push_back(e.uval);
    }
    return true;
}

inline bool read_token_array(Reader& r, std::vector<int32_t>& out, size_t max_n) {
    Header h;
    if (!read_header(r, h) || h.kind != Kind::ARR || h.uval > max_n) return false;
    // See read_u64_array: clamp the reserve to the bytes actually remaining so a
    // small payload's inflated array length can't force a large transient alloc.
    const size_t fits = r.remaining();
    out.reserve(out.size() + (h.uval < fits ? h.uval : fits));
    for (uint64_t i = 0; i < h.uval; ++i) {
        Header e;
        if (!read_header(r, e) || e.kind != Kind::INT) return false;
        out.push_back(static_cast<int32_t>(static_cast<int64_t>(e.uval)));
    }
    return true;
}

}  // namespace kv_msgpack

// Decode one KVEventBatch payload. Returns nullopt on any structural error
// or cap violation — callers treat that as a stream fault (counted, then the
// per-backend gap/reset path takes over). Never throws.
inline std::optional<KvEventBatch> decode_kv_event_batch(const uint8_t* data, size_t len) {
    using namespace kv_msgpack;
    if (data == nullptr || len == 0 || len > kKvMaxPayloadBytes) return std::nullopt;

    Reader r{data, data + len};
    Header batch;
    if (!read_header(r, batch) || batch.kind != Kind::ARR || batch.uval < 2) return std::nullopt;

    KvEventBatch out;

    Header ts;
    if (!read_header(r, ts)) return std::nullopt;
    if (ts.kind == Kind::F64) {
        // ts.fval is memcpy'd raw from a hostile-controllable 0xcb wire double.
        // A float→unsigned cast of +inf or any value ≥ 2^64 is UB ([conv.fpint]);
        // clamp on the largest double strictly below 2^64 (2^64 itself is not
        // representable, so it is the correct in-range ceiling). NaN/+inf and
        // non-positive timestamps collapse to 0.
        if (std::isfinite(ts.fval) && ts.fval > 0.0) {
            const double ms = ts.fval * 1000.0;
            out.ts_ms = ms >= 18446744073709549568.0 ? UINT64_MAX
                                                     : static_cast<uint64_t>(ms);
        } else {
            out.ts_ms = 0;
        }
    } else if (ts.kind == Kind::INT && !ts.negative) {
        // ts.uval * 1000 wraps for a hostile ts.uval > 2^64/1000 (defined, but a
        // wrong ordering key); cap it the same way the float path clamps.
        out.ts_ms = ts.uval > UINT64_MAX / 1000 ? UINT64_MAX : ts.uval * 1000;
    } else {
        return std::nullopt;
    }

    Header events;
    if (!read_header(r, events) || events.kind != Kind::ARR ||
        events.uval > kKvMaxEventsPerBatch) {
        return std::nullopt;
    }

    for (uint64_t i = 0; i < events.uval; ++i) {
        Header ev;
        if (!read_header(r, ev) || ev.kind != Kind::ARR || ev.uval < 1) return std::nullopt;
        uint64_t remaining = ev.uval - 1;

        std::string_view tag;
        if (!read_str(r, tag)) return std::nullopt;

        auto skip_rest = [&](uint64_t n) {
            for (uint64_t k = 0; k < n; ++k) {
                if (!skip_value(r)) return false;
            }
            return true;
        };

        if (tag == "BlockStored") {
            // Fields: block_hashes, parent_block_hash, token_ids, block_size,
            // [lora_id], [extras...] — trailing optionals may be omitted.
            if (remaining < 4) return std::nullopt;
            KvBlockStored ev_out;
            if (!read_u64_array(r, ev_out.block_hashes, kKvMaxHashesPerEvent)) return std::nullopt;

            Header parent;
            if (!read_header(r, parent)) return std::nullopt;
            if (parent.kind == Kind::INT) {
                ev_out.parent_block_hash = parent.uval;
            } else if (parent.kind != Kind::NIL) {
                return std::nullopt;
            }

            if (!read_token_array(r, ev_out.token_ids, kKvMaxTokensPerEvent)) return std::nullopt;

            Header bs;
            if (!read_header(r, bs) || bs.kind != Kind::INT || bs.negative ||
                bs.uval == 0 || bs.uval > kKvMaxTokensPerEvent) {
                return std::nullopt;
            }
            ev_out.block_size = static_cast<uint32_t>(bs.uval);

            if (!skip_rest(remaining - 4)) return std::nullopt;
            out.order_stored.push_back(static_cast<uint32_t>(i));
            out.stored.push_back(std::move(ev_out));
        } else if (tag == "BlockRemoved") {
            if (remaining < 1) return std::nullopt;
            KvBlockRemoved ev_out;
            if (!read_u64_array(r, ev_out.block_hashes, kKvMaxHashesPerEvent)) return std::nullopt;
            if (!skip_rest(remaining - 1)) return std::nullopt;
            out.order_removed.push_back(static_cast<uint32_t>(i));
            out.removed.push_back(std::move(ev_out));
        } else if (tag == "AllBlocksCleared") {
            if (!skip_rest(remaining)) return std::nullopt;
            out.all_cleared = true;
        } else {
            // Unknown event type: forward-compat skip, never fatal.
            if (!skip_rest(remaining)) return std::nullopt;
            out.unknown_events++;
        }
    }

    // Trailing batch fields beyond [ts, events] (e.g. data_parallel_rank in
    // newer vLLM versions): tolerated and ignored.
    for (uint64_t i = 2; i < batch.uval; ++i) {
        if (!skip_value(r)) return std::nullopt;
    }

    return out;
}

// =============================================================================
// Replay-frame helpers (pure)
// =============================================================================
// vLLM's publisher pairs the PUB stream with an optional ROUTER socket: a
// consumer sends the missed start sequence as an 8-byte big-endian integer
// and receives buffered batches as (seq, payload) messages, terminated by a
// sentinel (seq = -1, empty payload). Byte-level helpers live here so the
// protocol is unit-testable without ZMQ.

inline void encode_replay_request(uint64_t start_seq, uint8_t out[8]) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>((start_seq >> (8 * (7 - i))) & 0xff);
    }
}

// Decodes an 8-byte big-endian SIGNED sequence (the sentinel is -1).
inline bool decode_replay_seq(const uint8_t* data, size_t len, int64_t& seq) {
    if (data == nullptr || len != 8) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < 8; ++i) v = (v << 8) | data[i];
    seq = static_cast<int64_t>(v);
    return true;
}

inline bool is_replay_sentinel(int64_t seq, size_t payload_len) {
    return seq < 0 && payload_len == 0;
}

}  // namespace ranvier
