// libFuzzer harness for RadixTree::insert / RadixTree::lookup /
// RadixTree::evict_oldest / RadixTree::evict_lowest_trust.
//
// Targets audit findings H8 (split_long_prefix boundary read) and L9
// (recursive subspan logic), plus the per-origin LRU list invariant (T8):
// inserts carry a fuzzer-chosen RouteOrigin, eviction ops pop from the
// per-origin lists, and validate_lru_lists() runs after every operation
// to catch splice bugs from splits, growth, and origin-changing
// overwrites. The harness reads the fuzzer input as a sequence of
// operations; token sequences are constructed from the raw bytes so the
// fuzzer drives both length and content.
//
// See tests/fuzz/README.md for build and run instructions.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "node_slab.hpp"
#include "radix_tree.hpp"
#include "types.hpp"

namespace {

// One slab per process is fine — the fuzzer reuses the same RadixTree
// across iterations, so the slab must outlive every call.
ranvier::NodeSlab* slab_for_fuzz() {
    static std::unique_ptr<ranvier::NodeSlab> slab = [] {
        auto s = std::make_unique<ranvier::NodeSlab>();
        ranvier::set_node_slab(s.get());
        return s;
    }();
    return slab.get();
}

// Use block_alignment=1 to expose the most insert/lookup paths to the
// fuzzer; the production default would silently drop sub-aligned inserts.
ranvier::RadixTree& tree_for_fuzz() {
    static ranvier::RadixTree tree{1};
    return tree;
}

// Rule #4: the fuzz tree is bounded like production (the router evicts at
// capacity before every insert). This also keeps validate_lru_lists() —
// O(route_count) per op — cheap across a long fuzz run on the shared tree.
constexpr size_t kMaxFuzzRoutes = 4096;

// Decode the fuzzer input as a stream of operations:
//   byte 0: bits 0-1: opcode (0 = insert, 1 = lookup,
//                     2 = evict_oldest, 3 = evict_lowest_trust)
//           bits 2-7: RouteOrigin selector for insert (value % 3)
//   Ops 2/3 consume only the opcode byte. Ops 0/1 continue:
//   byte 1: token count N (0..255)
//   bytes 2..2+4N: N TokenIds (4 bytes each, little-endian — endian
//                  doesn't matter for fuzzing, only that the bytes flow
//                  through to the tree)
//   byte 2+4N: backend id (only used for insert)
//   ... next op
//
// Inputs that don't fit the schema are accepted; we just stop decoding.
void run_ops(const uint8_t* data, size_t size) {
    auto& tree = tree_for_fuzz();
    size_t i = 0;
    std::vector<ranvier::TokenId> tokens;

    while (i < size) {
        uint8_t opbyte = data[i++];
        uint8_t op = opbyte & 0x3;
        if (op == 2) {
            (void)tree.evict_oldest();
            tree.validate_lru_lists();
            continue;
        }
        if (op == 3) {
            (void)tree.evict_lowest_trust();
            tree.validate_lru_lists();
            continue;
        }
        if (i >= size) {
            break;
        }
        uint8_t n = data[i++];
        size_t need = static_cast<size_t>(n) * sizeof(ranvier::TokenId);
        if (i + need > size) {
            break;
        }
        tokens.clear();
        tokens.reserve(n);
        for (uint8_t k = 0; k < n; ++k) {
            ranvier::TokenId t = 0;
            for (int b = 0; b < 4; ++b) {
                t = static_cast<ranvier::TokenId>(
                    (static_cast<uint32_t>(t) << 8) | data[i + b]);
            }
            tokens.push_back(t);
            i += 4;
        }
        if (op == 0) {
            if (i >= size) {
                break;
            }
            ranvier::BackendId backend = data[i++];
            auto origin = static_cast<ranvier::RouteOrigin>((opbyte >> 2) % 3);
            while (tree.route_count() >= kMaxFuzzRoutes) {
                if (!tree.evict_lowest_trust()) {
                    break;
                }
            }
            tree.insert(std::span<const ranvier::TokenId>(tokens), backend, origin);
        } else {
            (void)tree.lookup(std::span<const ranvier::TokenId>(tokens));
        }
        tree.validate_lru_lists();
    }
}

}  // namespace

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) {
    slab_for_fuzz();
    (void)tree_for_fuzz();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    run_ops(data, size);
    return 0;
}
