// libFuzzer harness for RadixTree::insert / RadixTree::lookup.
//
// Targets audit findings H8 (split_long_prefix boundary read) and L9
// (recursive subspan logic). The harness reads the fuzzer input as a
// sequence of operations, each operation feeding a token vector to either
// insert() or lookup(). Token sequences are constructed from the raw
// bytes so the fuzzer drives both length and content.
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

// Decode the fuzzer input as a stream of operations:
//   byte 0: opcode (0 = insert, 1 = lookup)
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

    while (i + 2 <= size) {
        uint8_t op = data[i++] & 0x1;
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
            tree.insert(std::span<const ranvier::TokenId>(tokens), backend);
        } else {
            (void)tree.lookup(std::span<const ranvier::TokenId>(tokens));
        }
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
