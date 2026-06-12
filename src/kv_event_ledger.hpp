// Ranvier Core - KV Block Ledger (pure hash bridge)
//
// Translates vLLM's block-granular KV events into Ranvier routing-state
// operations. vLLM block hashes are engine-internal and never numerically
// comparable to Ranvier's prefix hashes; this ledger bridges them WITHOUT
// aligning hash algorithms:
//
//   hash_prefix() is a sequential FNV-1a over token bytes (block-aligned
//   truncation), so it extends incrementally. The ledger stores, per tracked
//   vLLM block, the FNV accumulator and cumulative token count at that
//   block's boundary. A BlockStored event continues its parent's accumulator
//   over the event's token_ids, yielding Ranvier's exact prefix_hash at every
//   block boundary in O(tokens) — 16 bytes of state per block, no token
//   storage.
//
//   Boundary identity: at a block boundary, cumulative count = k*block_size.
//   When block_size % block_alignment == 0, hash_prefix's internal alignment
//   keeps the full count, so the raw accumulator IS the prefix_hash that
//   route learning / the prefix_hash_index use at that depth. The ledger
//   refuses backends whose block_size breaks this invariant (misaligned
//   accumulators would poison the index with hashes nothing ever looks up).
//
// Ownership/threading: lives on the KV-event subscriber's dedicated OS
// thread, single-threaded access, no Seastar types — PURE and reactor-free
// unit-testable. Output is a flat vector of NativeKvOp that the subscriber
// ships to shard 0.
//
// Rule #4: per-backend block maps are double-bounded — max_tracked_blocks
// (hard entry cap, drop + counter on overflow) and max_indexed_token_depth
// (chains are only tracked while their cumulative depth can still matter to
// routing; deeper blocks are ignored, which also prunes their descendants
// since children of untracked parents are orphans).

#pragma once

#include "kv_event_decoder.hpp"
#include "parse_utils.hpp"
#include "types.hpp"

#include <absl/container/flat_hash_map.h>

#include <cstdint>
#include <vector>

namespace ranvier {

// One translated routing-state operation, in Ranvier's hash domain.
struct NativeKvOp {
    enum class Kind : uint8_t {
        UPSERT,   // backend now holds the prefix ending at prefix_hash
        REMOVE,   // backend no longer holds that prefix
        CLEAR,    // backend reported AllBlocksCleared (cache empty)
        RESET,    // stream fault (sequence gap / decode failure): ledger wiped,
                  // verified-residency trust for this backend must drop
        ALIVE,    // subscription heartbeat: stream healthy, refresh freshness
    };
    Kind kind = Kind::ALIVE;
    BackendId backend = 0;
    uint64_t prefix_hash = 0;   // UPSERT/REMOVE only
    uint64_t ts_ms = 0;
};

class KvBlockLedger {
public:
    struct Limits {
        uint32_t block_alignment = 16;          // RoutingConfig::block_alignment
        uint32_t max_indexed_token_depth = 2048;
        uint32_t max_tracked_blocks = 65536;    // per backend (Rule #4)
    };

    struct Stats {
        uint64_t blocks_tracked_total = 0;
        uint64_t blocks_dropped_capacity = 0;
        uint64_t blocks_dropped_depth = 0;
        uint64_t orphan_chains = 0;        // parent hash unknown (capped/dropped/raced)
        uint64_t malformed_events = 0;     // token count not divisible by block count
        uint64_t misaligned_backends = 0;  // block_size % alignment != 0 (rejected)
    };

    explicit KvBlockLedger(Limits limits) : _limits(limits) {}

    // Translate one decoded batch for one backend, appending ops in wire
    // order. Returns false only for the misaligned-block_size rejection —
    // the caller should disable native mode for that backend.
    bool translate(BackendId backend, const KvEventBatch& batch,
                   std::vector<NativeKvOp>& out) {
        // AllBlocksCleared semantics: the cache is empty NOW. Wire order
        // within the batch doesn't matter beyond that — drop ledger state
        // first, emit CLEAR first, then apply any stores from the same batch
        // (a clear followed by re-stores is how a restarting engine looks).
        if (batch.all_cleared) {
            _blocks[backend].clear();
            out.push_back({NativeKvOp::Kind::CLEAR, backend, 0, batch.ts_ms});
        }

        // Interleave stored/removed back into wire order using the order
        // indices the decoder kept. Both vectors are wire-ordered, so a
        // two-finger merge reconstructs the sequence.
        size_t si = 0;
        size_t ri = 0;
        while (si < batch.stored.size() || ri < batch.removed.size()) {
            bool take_stored =
                ri >= batch.removed.size() ||
                (si < batch.stored.size() &&
                 batch.order_stored[si] < batch.order_removed[ri]);
            if (take_stored) {
                if (!on_stored(backend, batch.stored[si], batch.ts_ms, out)) {
                    return false;
                }
                ++si;
            } else {
                on_removed(backend, batch.removed[ri], batch.ts_ms, out);
                ++ri;
            }
        }
        return true;
    }

    // Stream fault: forget everything about the backend and tell the shards
    // to stop trusting verified residency for it until the stream re-coheres.
    void reset_backend(BackendId backend, uint64_t ts_ms, std::vector<NativeKvOp>& out) {
        _blocks.erase(backend);
        out.push_back({NativeKvOp::Kind::RESET, backend, 0, ts_ms});
    }

    void drop_backend(BackendId backend) { _blocks.erase(backend); }

    size_t tracked_blocks(BackendId backend) const {
        auto it = _blocks.find(backend);
        return it == _blocks.end() ? 0 : it->second.size();
    }

    const Stats& stats() const { return _stats; }

private:
    struct BlockState {
        uint64_t fnv_state = FNV_OFFSET_BASIS;
        uint32_t token_count = 0;
    };
    using BlockMap = absl::flat_hash_map<uint64_t, BlockState>;

    static uint64_t fnv_extend(uint64_t state, const int32_t* tokens, size_t count) {
        const uint8_t* data = reinterpret_cast<const uint8_t*>(tokens);
        size_t byte_len = count * sizeof(int32_t);
        for (size_t i = 0; i < byte_len; ++i) {
            state ^= data[i];
            state *= FNV_PRIME;
        }
        return state;
    }

    bool on_stored(BackendId backend, const KvBlockStored& ev, uint64_t ts_ms,
                   std::vector<NativeKvOp>& out) {
        if (ev.block_size == 0 || ev.block_hashes.empty()) {
            _stats.malformed_events++;
            return true;
        }
        // Boundary-identity invariant (see header comment). This is per
        // backend in principle, but block_size is per event on the wire.
        if (ev.block_size % _limits.block_alignment != 0) {
            _stats.misaligned_backends++;
            return false;
        }
        // token_ids covers the stored blocks contiguously. vLLM only hashes
        // FULL blocks, so the division must be exact; tolerate nothing else.
        if (ev.token_ids.size() != ev.block_hashes.size() * ev.block_size) {
            _stats.malformed_events++;
            return true;
        }

        BlockState state;  // root chain: fresh accumulator
        if (ev.parent_block_hash.has_value()) {
            auto& backend_blocks = _blocks[backend];
            auto parent = backend_blocks.find(*ev.parent_block_hash);
            if (parent == backend_blocks.end()) {
                // Unknown parent: beyond our depth cap, evicted from a full
                // map, or stored before we subscribed. The whole chain is
                // untrackable — count once and move on.
                _stats.orphan_chains++;
                return true;
            }
            state = parent->second;
        }

        auto& backend_blocks = _blocks[backend];
        for (size_t b = 0; b < ev.block_hashes.size(); ++b) {
            // Depth cap BEFORE extending: blocks past the routable depth are
            // not tracked, and their children become orphans (pruned free).
            if (state.token_count >= _limits.max_indexed_token_depth) {
                _stats.blocks_dropped_depth++;
                return true;
            }
            state.fnv_state = fnv_extend(
                state.fnv_state, ev.token_ids.data() + b * ev.block_size, ev.block_size);
            state.token_count += ev.block_size;

            if (backend_blocks.size() >= _limits.max_tracked_blocks &&
                !backend_blocks.contains(ev.block_hashes[b])) {
                _stats.blocks_dropped_capacity++;
                return true;
            }
            backend_blocks[ev.block_hashes[b]] = state;
            _stats.blocks_tracked_total++;

            // At a block boundary with block_size % alignment == 0 the raw
            // accumulator equals hash_prefix(tokens, token_count, alignment).
            out.push_back({NativeKvOp::Kind::UPSERT, backend, state.fnv_state, ts_ms});
        }
        return true;
    }

    void on_removed(BackendId backend, const KvBlockRemoved& ev, uint64_t ts_ms,
                    std::vector<NativeKvOp>& out) {
        auto it = _blocks.find(backend);
        if (it == _blocks.end()) return;
        for (uint64_t vllm_hash : ev.block_hashes) {
            auto block = it->second.find(vllm_hash);
            if (block == it->second.end()) continue;  // untracked depth — nothing indexed
            out.push_back({NativeKvOp::Kind::REMOVE, backend,
                           block->second.fnv_state, ts_ms});
            it->second.erase(block);
        }
    }

    Limits _limits;
    Stats _stats;
    absl::flat_hash_map<BackendId, BlockMap> _blocks;
};

}  // namespace ranvier
