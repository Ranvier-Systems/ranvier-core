#pragma once

#include <vector>
#include <cstdint>
#include <optional>
#include <array>
#include <memory>
#include <span>
#include <algorithm>
#include <chrono>
#include <functional>
#include <cassert>

#include <absl/container/inlined_vector.h>

#include "byte_order.hpp"
#include "types.hpp"
#include "node_slab.hpp"

// ============================================================================
// RadixTree: Adaptive Radix Tree (ART) for Prefix-Based Routing
// ============================================================================
//
// This file is the #1 load-bearing file in the codebase (per Strategic Assessment).
// It provides the core data structure for prefix cache lookups on every request.
//
// HARD RULES APPLICABLE TO THIS FILE:
// ───────────────────────────────────────────────────────────────────────────────
//
// Rule #1 (No Blocking on Hot Path):
//   - lookup(), lookup_instrumented(), find_child() are HOT PATH functions
//   - They run on every incoming request and MUST be lock-free
//   - O(key_length) traversal with no heap allocations on cache hit
//   - All node access is via raw pointers (no atomic refcounting)
//
// Rule #4 (Bounded Memory via Slab Allocator):
//   - All nodes allocated via NodeSlab (shard-local free-list allocator)
//   - insert() triggers LRU eviction when route_count >= max_routes
//   - evict_oldest() / evict_oldest_remote() implement LRU policy
//   - compact() reclaims memory from tombstoned nodes periodically
//   - Node transitions (Node4→Node16→Node48→Node256) use slab, not heap
//
// Rule #9 (Per-Shard Data Structures):
//   - RadixTree instances are shard-local (via g_shard_state->tree)
//   - NodeSlab is thread_local (accessed via get_node_slab())
//   - No cross-shard pointers - all nodes belong to the owning shard
//   - std::unique_ptr ownership prevents accidental cross-shard sharing
//
// Rule #14 (Cross-Shard Dispatch Safety):
//   - Data inserted into the tree MUST be locally allocated on this shard
//   - RouterService handles foreign_ptr unwrapping before calling insert()
//   - Never pass heap-owning types (vector, string) directly across shards
//
// See .dev-context/claude-context.md for full Hard Rules documentation.
// ============================================================================

/*
Adaptive Radix Tree (ART) Implementation

Design Philosophy:
- Path Compression: Every node stores a prefix vector to skip checking individual nodes
- Adaptive Nodes: Node4 → Node16 → Node48 → Node256 based on child count
- Zero-Copy Lookups: Uses std::span<int32_t> for the lookup key
- Unique Ownership: Uses std::unique_ptr for node ownership (Seastar shared-nothing model)

Node Types:
- Node4:   1-4 children,   linear search through keys vector
- Node16:  5-16 children,  linear search (SIMD-friendly in production)
- Node48:  17-48 children, 256-byte index array → 48 child pointers
- Node256: 49-256 children, direct 256-element array

Memory Model:
- RadixTree instances are thread-local (owned uniquely by RouterService on each shard)
- No cross-thread sharing, so std::unique_ptr is used instead of std::shared_ptr
- This eliminates atomic reference counting overhead (lock xadd instructions)
*/

namespace ranvier {

// =============================================================================
// Types and Enums
// =============================================================================

enum class RouteOrigin : uint8_t {
    LOCAL = 0,   // Learned from direct request on this node (highest trust)
    PUSH   = 1,  // Backend-pushed "loaded" cache event
    REMOTE = 2   // Learned from cluster gossip (can be evicted more aggressively)
    // Numeric ordering is load-bearing: lower value = higher trust / lower
    // eviction priority. Eviction precedence is REMOTE > PUSH > LOCAL.
};

enum class NodeType : uint8_t {
    Node4,
    Node16,
    Node48,
    Node256
};

// Hard Rule #4: Every growing container needs an explicit MAX_SIZE.
// Prefixes longer than this are split into chains of nodes.
//
// Units are BYTES.  Since the tree operates on big-endian TokenId bytes
// internally (4 bytes per logical token), 1024 bytes corresponds to 256
// logical tokens worth of path compression — matching the pre-refactor
// logical capacity.  split_long_prefix() chains longer prefixes into a
// series of nodes so no single node's prefix vector exceeds this bound.
static constexpr size_t MAX_PREFIX_LENGTH = 1024;

// =============================================================================
// TokenId <-> Byte Encoding (Big-Endian)
// =============================================================================
//
// The tree operates internally on byte-level keys rather than TokenId-level
// keys.  This is what ART actually requires for wide (non-byte) alphabets:
// Node256 becomes a true direct-mapped 256-slot array with no collision
// handling, and the "Node256 full" edge case is eliminated by construction.
//
// Big-endian is the chosen encoding because:
//   1. LLM vocabulary IDs cluster in the low end of the TokenId range
//      (typically < 128k, so the upper 1-2 bytes are usually 0x00).
//   2. Big-endian places those common-zero bytes at the top of the tree,
//      where path compression can collapse them into a single shared root
//      prefix shared by every route.
//   3. Lexicographic byte order matches numerical TokenId order, which
//      simplifies reasoning about prefix matching.
//
// The encoding assumes TokenId is exactly 4 bytes.  The static_assert below
// will fail the build if that ever changes, forcing a conscious revisit.
//
static_assert(sizeof(TokenId) == 4,
              "Byte encoding assumes TokenId is exactly 4 bytes (int32_t). "
              "If TokenId changes width, update encode/decode_token_be and "
              "review the radix tree's byte-alignment invariants.");

// Encode a single TokenId as 4 big-endian bytes, appending to `out`.
// `Out` must support `push_back(uint8_t)` (e.g., std::vector<uint8_t>,
// absl::InlinedVector<uint8_t, N>).
//
// Delegates to be_write_u32 in byte_order.hpp.  The cast through uint32_t
// gives well-defined bit behavior for negative TokenIds (two's complement
// preserved), even though tokenizers don't produce negative IDs in practice.
template <typename Out>
inline void encode_token_be(TokenId token, Out& out) {
    be_write_u32(out, static_cast<uint32_t>(token));
}

// Encode a sequence of TokenIds as big-endian bytes, appending to `out`.
// Caller should reserve `out.capacity() >= tokens.size() * sizeof(TokenId)`
// beforehand to avoid reallocation on the hot path.
template <typename Out>
inline void encode_tokens_be(std::span<const TokenId> tokens, Out& out) {
    for (TokenId t : tokens) {
        encode_token_be(t, out);
    }
}

// Decode 4 big-endian bytes starting at `bytes[offset]` into a TokenId.
// Precondition: offset + sizeof(TokenId) <= bytes.size().
//
// Unlike be_read_u32 (raw pointer, no bounds check), this span-based wrapper
// asserts the offset is in range — important because it's called from the
// tree traversal loop where an out-of-range read would corrupt results
// silently rather than crash cleanly.
inline TokenId decode_token_be(std::span<const uint8_t> bytes, size_t offset) {
    assert(offset + sizeof(TokenId) <= bytes.size() &&
           "decode_token_be: offset out of range");
    return static_cast<TokenId>(be_read_u32(bytes.data() + offset));
}

// Decode a TokenId-aligned byte sequence into TokenIds, appending to `out`.
// Precondition: bytes.size() % sizeof(TokenId) == 0.
// This alignment invariant is guaranteed for any byte sequence that
// represents a complete root-to-leaf path in the radix tree, because all
// inserts enter the tree through the TokenId-span public API.
template <typename Out>
inline void decode_tokens_be(std::span<const uint8_t> bytes, Out& out) {
    assert(bytes.size() % sizeof(TokenId) == 0 &&
           "decode_tokens_be: byte length must be TokenId-aligned");
    for (size_t i = 0; i + sizeof(TokenId) <= bytes.size(); i += sizeof(TokenId)) {
        out.push_back(decode_token_be(bytes, i));
    }
}

// =============================================================================
// Node Structures
// =============================================================================

struct Node {
    NodeType type;
    // Byte-level path compression prefix.  Inline storage for 32 bytes
    // (= 8 TokenIds worth) eliminates heap allocation for short prefixes,
    // the common case after path compression splits.  Longer prefixes
    // spill to the heap transparently via absl::InlinedVector.
    absl::InlinedVector<uint8_t, 32> prefix;
    std::optional<BackendId> leaf_value;
    RouteOrigin origin = RouteOrigin::LOCAL;
    std::chrono::steady_clock::time_point last_accessed;

    // Intrusive LRU list pointers (only meaningful for leaf nodes).
    // Maintained by RadixTree — do not modify directly.
    Node* lru_prev = nullptr;  // Toward head (more recent)
    Node* lru_next = nullptr;  // Toward tail (older)

    explicit Node(NodeType t) : type(t), last_accessed(std::chrono::steady_clock::now()) {}
    virtual ~Node() = default;
};

struct Node4 : public Node {
    std::vector<uint8_t> keys;
    std::vector<NodePtr> children;

    Node4() : Node(NodeType::Node4) {
        keys.reserve(4);
        children.reserve(4);
    }
};

struct Node16 : public Node {
    std::vector<uint8_t> keys;
    std::vector<NodePtr> children;

    Node16() : Node(NodeType::Node16) {
        keys.reserve(16);
        children.reserve(16);
    }
};

struct Node48 : public Node {
    static constexpr uint8_t EMPTY_MARKER = 255;

    std::array<uint8_t, 256> index;
    std::vector<NodePtr> children;
    std::vector<uint8_t> keys;

    Node48() : Node(NodeType::Node48) {
        index.fill(EMPTY_MARKER);
        children.reserve(48);
        keys.reserve(48);
    }
};

struct Node256 : public Node {
    // Byte-keyed direct-mapped array: the slot index IS the key.  No
    // separate `keys` member is needed because the byte alphabet is
    // exactly 256 — there are no collisions by construction, unlike the
    // previous TokenId-keyed implementation that used lossy hashing.
    // An empty slot is simply a null child pointer.
    std::array<NodePtr, 256> children;

    Node256() : Node(NodeType::Node256) {}
};

// =============================================================================
// Route Entry (for iteration)
// =============================================================================

struct RouteEntry {
    std::vector<TokenId> tokens;
    BackendId backend;
    RouteOrigin origin;
    std::chrono::steady_clock::time_point last_accessed;
};

// Statistics about tree structure for performance monitoring
struct TreeStats {
    size_t total_nodes = 0;           // Total number of nodes in tree
    size_t node4_count = 0;           // Number of Node4 nodes
    size_t node16_count = 0;          // Number of Node16 nodes
    size_t node48_count = 0;          // Number of Node48 nodes
    size_t node256_count = 0;         // Number of Node256 nodes
    size_t total_prefix_length = 0;   // Sum of all prefix lengths
    double average_prefix_length = 0; // Average prefix length (path compression effectiveness)
};

// Statistics from tree compaction operation
struct CompactionStats {
    size_t nodes_removed = 0;         // Empty nodes deleted and returned to slab
    size_t nodes_shrunk = 0;          // Nodes downsized (e.g., Node256 → Node48)
    size_t bytes_reclaimed = 0;       // Estimated bytes returned to free list
};

// Result from lookup with instrumentation data
struct LookupResult {
    std::optional<BackendId> backend;
    // Bytes matched via path compression (bigger = better path compression).
    // The public `ranvier_radix_tree_average_prefix_skip_length` metric is
    // a *token* count — RouterService divides this by sizeof(TokenId) at
    // the recording site to preserve the external token-semantic gauge.
    size_t prefix_bytes_skipped = 0;
    size_t nodes_traversed = 0;       // Number of nodes visited during lookup
};

// =============================================================================
// RadixTree Class
// =============================================================================

class RadixTree {
public:
    explicit RadixTree(uint32_t block_alignment = 16)
        : block_alignment_(block_alignment)
        , root_(make_initial_root()) {}

private:
    // Helper to create root node with proper null-check
    static NodePtr make_initial_root() {
        NodeSlab* s = get_node_slab();
        if (!s) [[unlikely]] {
            throw std::runtime_error("NodeSlab not initialized before RadixTree construction");
        }
        return s->make_node<Node4>();
    }

public:

    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------

    // =========================================================================
    // insert() - Route Insertion with Slab Allocation
    // =========================================================================
    //
    // HARD RULE #4 (Bounded Memory via Slab Allocator):
    // All node allocations go through NodeSlab (shard-local free-list):
    //   - make_node<NodeT>() allocates from pre-allocated pools
    //   - Node transitions (Node4→Node16→Node48→Node256) use slab
    //   - No direct heap allocation (std::make_unique bypassed)
    //
    // HARD RULE #14 (Cross-Shard Dispatch Safety):
    // The tokens span MUST reference locally-allocated memory:
    //   - RouterService unwraps foreign_ptr and creates local copy
    //   - Never pass heap-owning types directly from other shards
    //   - The span itself is safe (just a view, no ownership)
    //
    // LRU eviction is handled by the caller (RouterService) before insert:
    //   - Caller checks: if (route_count() >= max_routes) evict_oldest()
    //   - This keeps eviction policy in the service layer
    //
    void insert(std::span<const TokenId> tokens, BackendId backend,
                RouteOrigin origin = RouteOrigin::LOCAL) {
        size_t aligned_len = (tokens.size() / block_alignment_) * block_alignment_;
        if (aligned_len == 0) return;

        // Encode aligned TokenIds to a byte buffer for the byte-level
        // tree.  The InlinedVector's 1024-byte inline storage (= 256
        // tokens) keeps the scratch buffer allocation-free for every
        // realistic LLM prompt size; only pathologically long prompts
        // (>256 tokens) spill to the heap.  The output length is
        // always a multiple of sizeof(TokenId), which enforces the
        // leaf-alignment invariant (leaves only exist at TokenId-
        // aligned byte depths).
        absl::InlinedVector<uint8_t, 1024> key_bytes;
        key_bytes.reserve(aligned_len * sizeof(TokenId));
        encode_tokens_be(tokens.subspan(0, aligned_len), key_bytes);

        auto [new_root, is_new_route] = insert_recursive(
            std::move(root_),
            std::span<const uint8_t>(key_bytes.data(), key_bytes.size()),
            backend, origin);
        root_ = std::move(new_root);
        if (is_new_route) {
            route_count_++;
        }
    }

    std::optional<BackendId> lookup(std::span<const TokenId> tokens) {
        // 1024-byte inline buffer = 256 tokens; covers every realistic
        // LLM prompt without heap allocation on the hot path.  See the
        // corresponding note in insert() above for the rationale.
        absl::InlinedVector<uint8_t, 1024> key_bytes;
        key_bytes.reserve(tokens.size() * sizeof(TokenId));
        encode_tokens_be(tokens, key_bytes);
        return lookup_recursive(root_.get(),
            std::span<const uint8_t>(key_bytes.data(), key_bytes.size()));
    }

    // =========================================================================
    // lookup_instrumented() - Prefix Cache Lookup (HOT PATH)
    // =========================================================================
    //
    // HARD RULE #1 (No Blocking on Hot Path):
    // This function runs on every incoming request. It MUST remain lock-free:
    //   - O(key_length) traversal via iterative loop (no recursion overhead)
    //   - No heap allocations on cache hit for prompts up to 256 tokens
    //     (the 1024-byte inline scratch buffer holds the byte-encoded
    //     key; only pathologically long prompts spill to the heap)
    //   - No mutex, no atomics, no cross-shard access
    //   - LRU timestamp update is the only write (single steady_clock::now())
    //
    // Returns LookupResult with:
    //   - backend: The matched BackendId (if found)
    //   - prefix_bytes_skipped: Bytes matched via path compression
    //       (RouterService divides by sizeof(TokenId) before recording
    //        into the external `average_prefix_skip_length` gauge to
    //        preserve the token-semantic metric that dashboards expect)
    //   - nodes_traversed: Number of nodes visited (for metrics)
    //
    LookupResult lookup_instrumented(std::span<const TokenId> tokens) {
        absl::InlinedVector<uint8_t, 1024> key_bytes;
        key_bytes.reserve(tokens.size() * sizeof(TokenId));
        encode_tokens_be(tokens, key_bytes);
        return lookup_with_stats(root_.get(),
            std::span<const uint8_t>(key_bytes.data(), key_bytes.size()));
    }

    size_t route_count() const { return route_count_; }

    // Calculate tree statistics for monitoring path compression effectiveness
    TreeStats get_tree_stats() const {
        TreeStats stats;
        calculate_tree_stats(root_.get(), stats);
        if (stats.total_nodes > 0) {
            stats.average_prefix_length =
                static_cast<double>(stats.total_prefix_length) /
                static_cast<double>(stats.total_nodes);
        }
        return stats;
    }

    template<typename Callback>
    void for_each_leaf(Callback&& callback) const {
        // Accumulate byte-level path during traversal; decode back to
        // TokenIds only at leaf emission so the callback still receives
        // a logical RouteEntry.  The byte accumulator is guaranteed to
        // be TokenId-aligned at every leaf because inserts only enter
        // the tree through the TokenId-span public API.
        std::vector<uint8_t> path;
        for_each_leaf_recursive(root_.get(), path, std::forward<Callback>(callback));
    }

    // =============================================================================
    // Tree Dump/Serialization for Admin API
    // =============================================================================
    //
    // The DumpNode struct below contains a `vector<pair<uint8_t, DumpNode>>`,
    // a self-referential type. C++17 explicitly allows incomplete types in
    // std::vector, but Clang + libstdc++ instantiates vector's destructor
    // more eagerly than GCC + libstdc++ does: the first call site for
    // `dump_node()` forces synthesis of DumpNode's implicit destructor
    // before the struct's closing brace, which transitively asks
    // libstdc++'s `is_destructible` trait to inspect an incomplete
    // DumpNode (via `pair<uint8_t, DumpNode>`'s implicit-default
    // constructibility check). To defer that chain past the closing
    // brace we user-declare `~DumpNode()` here and define it out-of-line
    // as `= default` after RadixTree's body — at that point DumpNode is
    // complete, the trait succeeds, and the vector's destructor
    // instantiates cleanly on both clang+libstdc++ and gcc+libstdc++.
    // The user-declared destructor does not disqualify aggregate
    // initialisation in C++20, so existing aggregate-init call sites
    // (e.g. `DumpNode{"empty", {}, std::nullopt, "LOCAL", 0, {}}`)
    // continue to compile unchanged. The `#ifndef RANVIER_FUZZING`
    // guard remains because the fuzz harnesses don't exercise dump().
#ifndef RANVIER_FUZZING

    // Represents a node in the serialized tree structure.
    //
    // NOTE: Prefix and child edge keys are exposed as raw bytes, not as
    // logical TokenIds.  This is an honest representation of the tree's
    // internal byte-level structure — a node's "prefix" may span part of
    // a TokenId if it sits between two token boundaries after a split.
    // Consumers that want token-level output can decode 4 consecutive
    // root-to-leaf bytes at a time using decode_token_be().
    struct DumpNode {
        std::string type;                    // "Node4", "Node16", "Node48", "Node256"
        std::vector<uint8_t> prefix;         // Path compression prefix (raw bytes)
        std::optional<BackendId> backend;    // Leaf value (if any)
        std::string origin;                  // "LOCAL" or "REMOTE"
        int64_t last_accessed_ms;            // Milliseconds since epoch
        std::vector<std::pair<uint8_t, DumpNode>> children;  // Edge key is a raw byte

        // User-declared destructor; defined `= default` out-of-line below
        // RadixTree's closing brace. See comment block above for why.
        ~DumpNode();
    };

    // Dump the entire tree structure for inspection
    DumpNode dump() const {
        return dump_node(root_.get());
    }

    // Dump subtree starting from a specific prefix (returns nullopt if prefix not found).
    // Public API takes a TokenId prefix; internally we encode it to bytes
    // and walk the byte-level tree.
    std::optional<DumpNode> dump_with_prefix(std::span<const TokenId> prefix_filter) const {
        // Encode TokenId prefix filter to byte form for internal walk.
        // Uses the same 1024-byte inline capacity as the other public
        // entry points for consistency; dump_with_prefix is an admin
        // cold path so the stack cost is immaterial either way.
        absl::InlinedVector<uint8_t, 1024> byte_filter;
        byte_filter.reserve(prefix_filter.size() * sizeof(TokenId));
        encode_tokens_be(prefix_filter, byte_filter);

        // Navigate to the node matching the byte prefix
        Node* node = root_.get();
        size_t remaining_prefix = 0;

        while (node != nullptr && remaining_prefix < byte_filter.size()) {
            // Check how much of the node's prefix matches
            size_t match_len = 0;
            while (match_len < node->prefix.size() &&
                   remaining_prefix + match_len < byte_filter.size() &&
                   node->prefix[match_len] == byte_filter[remaining_prefix + match_len]) {
                match_len++;
            }

            // If we didn't match the entire node prefix, prefix not found
            if (match_len < node->prefix.size()) {
                // Partial match - return this subtree if we matched something
                if (remaining_prefix > 0 || match_len > 0) {
                    return dump_node(node);
                }
                return std::nullopt;
            }

            remaining_prefix += match_len;

            // If we've consumed the entire filter, return this subtree
            if (remaining_prefix >= byte_filter.size()) {
                return dump_node(node);
            }

            // Navigate to child
            node = find_child(node, byte_filter[remaining_prefix]);
            remaining_prefix++;
        }

        if (node) {
            return dump_node(node);
        }
        return std::nullopt;
    }

private:
    DumpNode dump_node(Node* node) const {
        if (!node) {
            return DumpNode{"empty", {}, std::nullopt, "LOCAL", 0, {}};
        }

        DumpNode result;
        switch (node->type) {
            case NodeType::Node4: result.type = "Node4"; break;
            case NodeType::Node16: result.type = "Node16"; break;
            case NodeType::Node48: result.type = "Node48"; break;
            case NodeType::Node256: result.type = "Node256"; break;
        }
        result.prefix.assign(node->prefix.begin(), node->prefix.end());
        result.backend = node->leaf_value;
        switch (node->origin) {
            case RouteOrigin::LOCAL:  result.origin = "LOCAL"; break;
            case RouteOrigin::PUSH:   result.origin = "PUSH"; break;
            case RouteOrigin::REMOTE: result.origin = "REMOTE"; break;
        }
        result.last_accessed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            node->last_accessed.time_since_epoch()).count();

        // Collect children (edge keys are bytes in the byte-level tree).
        visit_children(node, [this, &result](uint8_t key, Node* child) {
            result.children.emplace_back(key, dump_node(child));
        });

        return result;
    }

#endif  // RANVIER_FUZZING

public:

    size_t remove_expired(std::chrono::steady_clock::time_point cutoff) {
        size_t removed = 0;
        remove_expired_recursive(root_.get(), cutoff, removed);
        route_count_ = (route_count_ > removed) ? route_count_ - removed : 0;
        return removed;
    }

    // Compression-aware TTL: each backend may have a different cutoff time.
    // cutoff_fn(BackendId) returns the expiry cutoff for that backend.
    // Routes whose last_accessed < cutoff_fn(backend) are removed.
    template<typename CutoffFn>
    size_t remove_expired(CutoffFn&& cutoff_fn) {
        size_t removed = 0;
        remove_expired_per_backend_recursive(root_.get(), cutoff_fn, removed);
        route_count_ = (route_count_ > removed) ? route_count_ - removed : 0;
        return removed;
    }

    // =========================================================================
    // evict_oldest() / evict_oldest_remote() - LRU Eviction Policy
    // =========================================================================
    //
    // HARD RULE #4 (Bounded Memory):
    // These methods enforce the LRU eviction policy to prevent unbounded growth:
    //   - evict_oldest(): Evicts the least-recently-accessed route (any origin)
    //   - evict_oldest_remote(): Prefers evicting REMOTE routes first
    //
    // Eviction strategy:
    //   1. Find leaf with oldest last_accessed timestamp
    //   2. Clear leaf_value (tombstone the route)
    //   3. Decrement route_count_
    //   4. compact() later reclaims empty nodes and returns memory to slab
    //
    // Called by RouterService before insert() when at capacity:
    //   while (tree->route_count() >= max_routes) tree->evict_oldest();
    //
    bool evict_oldest() {
        if (!lru_tail_) return false;
        Node* oldest = lru_tail_;
        lru_remove(oldest);
        oldest->leaf_value = std::nullopt;
        if (route_count_ > 0) route_count_--;
        return true;
    }

    bool evict_oldest_remote() {
        // Scan from tail (oldest) looking for a REMOTE route.
        for (Node* n = lru_tail_; n != nullptr; n = n->lru_prev) {
            if (n->origin == RouteOrigin::REMOTE) {
                lru_remove(n);
                n->leaf_value = std::nullopt;
                if (route_count_ > 0) route_count_--;
                return true;
            }
        }
        return evict_oldest();
    }

    size_t remove_routes_by_backend(BackendId backend, RouteOrigin origin) {
        size_t removed = 0;
        remove_by_backend_recursive(root_.get(), backend, origin, removed);
        route_count_ = (route_count_ > removed) ? route_count_ - removed : 0;
        return removed;
    }

    // =========================================================================
    // Tree Compaction - Reclaims memory from tombstoned nodes
    // =========================================================================
    //
    // HARD RULE #4 (Bounded Memory):
    // compact() is the second phase of memory management (after LRU eviction):
    //   - Phase 1: evict_oldest() tombstones routes (clears leaf_value)
    //   - Phase 2: compact() reclaims empty nodes and shrinks oversized nodes
    //
    // Compaction returns memory to NodeSlab free lists:
    //   - Empty nodes (no leaf, no children) → deleted via SlabNodeDeleter
    //   - Oversized nodes (e.g., Node256 with 4 children) → shrunk to smaller type
    //   - Shrinking creates new node via slab, old node returned to slab
    //
    // Called periodically by RouterService TTL timer (every 60s).
    //

    // Compact the tree by removing empty nodes and optionally shrinking oversized nodes.
    // Call periodically (e.g., every 60s) to reclaim memory after route deletions.
    // Returns statistics about the compaction operation.
    CompactionStats compact() {
        CompactionStats stats;
        // Compact children of root (root itself is never deleted)
        compact_children(root_.get(), stats);
        // Optionally shrink the root if it's oversized
        if (should_shrink(root_.get())) {
            root_ = shrink_node(std::move(root_), stats);
        }
        return stats;
    }

    // Estimate total memory used by the radix tree (nodes only, via slab stats).
    // O(1) — reads slab allocator counters, no tree traversal.
    // Used by ranvier_radix_tree_bytes gauge for capacity planning.
    size_t estimate_memory_bytes() const {
        NodeSlab* s = get_node_slab();
        if (!s) return 0;
        auto stats = s->get_stats();
        return stats.allocated_nodes[0] * NODE4_SIZE
             + stats.allocated_nodes[1] * NODE16_SIZE
             + stats.allocated_nodes[2] * NODE48_SIZE
             + stats.allocated_nodes[3] * NODE256_SIZE;
    }

private:
    NodePtr root_;
    uint32_t block_alignment_;
    size_t route_count_ = 0;

    // Intrusive LRU doubly-linked list of leaf nodes.
    // lru_head_ = most recently accessed, lru_tail_ = oldest (eviction target).
    Node* lru_head_ = nullptr;
    Node* lru_tail_ = nullptr;

    // -------------------------------------------------------------------------
    // Slab Allocator Access
    // -------------------------------------------------------------------------
    //
    // HARD RULE #9 (Per-Shard Data Structures):
    // NodeSlab is thread_local - each shard has its own allocator:
    //   - get_node_slab() returns the shard-local slab instance
    //   - Initialized by ShardLocalState::init() in RouterService
    //   - No cross-shard sharing of slab or nodes
    //
    // HARD RULE #4 (Bounded Memory):
    // Slab provides bounded, predictable memory allocation:
    //   - Pre-allocated pools per node type (Node4/16/48/256)
    //   - Free-list recycling (no fragmentation)
    //   - SlabNodeDeleter returns nodes to free list on destruction
    //

    // Get the thread-local slab allocator
    NodeSlab* slab() const {
        NodeSlab* s = get_node_slab();
        assert(s != nullptr && "NodeSlab not initialized for this shard");
        return s;
    }

    // Factory method to create nodes via the slab allocator
    template<typename NodeT>
    NodePtr make_node() const {
        return slab()->make_node<NodeT>();
    }

    // -------------------------------------------------------------------------
    // Intrusive LRU List Operations
    // -------------------------------------------------------------------------
    //
    // O(1) LRU maintenance for leaf nodes.  Only nodes with leaf_value are
    // linked.  The list is ordered by access time: head = most recent,
    // tail = oldest (eviction candidate).
    //

    // Unlink a node from the LRU list (safe to call on unlinked nodes).
    void lru_remove(Node* node) {
        if (!node) return;
        if (node->lru_prev) {
            node->lru_prev->lru_next = node->lru_next;
        } else if (lru_head_ == node) {
            lru_head_ = node->lru_next;
        }
        if (node->lru_next) {
            node->lru_next->lru_prev = node->lru_prev;
        } else if (lru_tail_ == node) {
            lru_tail_ = node->lru_prev;
        }
        node->lru_prev = nullptr;
        node->lru_next = nullptr;
    }

    // Insert a node at the head of the LRU list (most recently accessed).
    void lru_push_front(Node* node) {
        node->lru_prev = nullptr;
        node->lru_next = lru_head_;
        if (lru_head_) {
            lru_head_->lru_prev = node;
        }
        lru_head_ = node;
        if (!lru_tail_) {
            lru_tail_ = node;
        }
    }

    // Move an existing node to the head (touch on access).
    void lru_touch(Node* node) {
        if (!node || node == lru_head_) return;
        lru_remove(node);
        lru_push_front(node);
    }

    // -------------------------------------------------------------------------
    // Small-Node Template Helpers
    // -------------------------------------------------------------------------
    //
    // Node4, Node16, and Node48 all use parallel keys/children vectors with
    // identical access patterns.  These template helpers eliminate duplication
    // across the per-type switch branches.  Node256 (direct-index array) is
    // always handled separately.
    //
    // Keys are raw bytes after the multi-byte ART refactor — one slot per
    // distinct byte value.  No collision handling is needed because the
    // byte alphabet is exactly 256.

    // Linear-scan find on any node with .keys / .children vectors.
    template<typename SmallNodeT>
    static Node* find_child_keyed(SmallNodeT* n, uint8_t key) {
        for (size_t i = 0; i < n->keys.size(); i++) {
            if (n->keys[i] == key) return n->children[i].get();
        }
        return nullptr;
    }

    // Move child out of a keyed node (leaves stale entry for set_child to update).
    template<typename SmallNodeT>
    static NodePtr extract_child_keyed(SmallNodeT* n, uint8_t key) {
        for (size_t i = 0; i < n->keys.size(); i++) {
            if (n->keys[i] == key) return std::move(n->children[i]);
        }
        return nullptr;
    }

    // Replace existing child in a keyed node.
    template<typename SmallNodeT>
    static void set_child_keyed(SmallNodeT* n, uint8_t key, NodePtr child) {
        for (size_t i = 0; i < n->keys.size(); i++) {
            if (n->keys[i] == key) {
                n->children[i] = std::move(child);
                return;
            }
        }
    }

    // Visit all children of a keyed node.
    template<typename SmallNodeT, typename Callback>
    static void visit_children_keyed(SmallNodeT* n, Callback&& callback) {
        for (size_t i = 0; i < n->keys.size(); i++) {
            callback(n->keys[i], n->children[i].get());
        }
    }

    // Erase child by key from a keyed node (used by compaction).
    template<typename SmallNodeT>
    static NodeType remove_child_keyed(SmallNodeT* n, uint8_t key) {
        for (size_t i = 0; i < n->keys.size(); i++) {
            if (n->keys[i] == key) {
                NodeType removed_type = n->children[i]->type;
                n->keys.erase(n->keys.begin() + static_cast<ptrdiff_t>(i));
                n->children.erase(n->children.begin() + static_cast<ptrdiff_t>(i));
                return removed_type;
            }
        }
        return NodeType::Node4; // Fallback (shouldn't happen)
    }

    // -------------------------------------------------------------------------
    // Node Visitor Helpers
    // -------------------------------------------------------------------------

    // Visit all children of a node with a callback: void(uint8_t key, Node* child)
    template<typename Callback>
    static void visit_children(Node* node, Callback&& callback) {
        switch (node->type) {
            case NodeType::Node4:
                visit_children_keyed(static_cast<Node4*>(node), callback);
                break;
            case NodeType::Node16:
                visit_children_keyed(static_cast<Node16*>(node), callback);
                break;
            case NodeType::Node48:
                visit_children_keyed(static_cast<Node48*>(node), callback);
                break;
            case NodeType::Node256: {
                auto* n = static_cast<Node256*>(node);
                for (int i = 0; i < 256; i++) {
                    if (n->children[i]) {
                        callback(static_cast<uint8_t>(i), n->children[i].get());
                    }
                }
                break;
            }
        }
    }

    // Get the number of children in a node
    static size_t child_count(Node* node) {
        switch (node->type) {
            case NodeType::Node4:
                return static_cast<Node4*>(node)->keys.size();
            case NodeType::Node16:
                return static_cast<Node16*>(node)->keys.size();
            case NodeType::Node48:
                return static_cast<Node48*>(node)->keys.size();
            case NodeType::Node256: {
                size_t count = 0;
                auto* n = static_cast<Node256*>(node);
                for (int i = 0; i < 256; i++) {
                    if (n->children[i]) count++;
                }
                return count;
            }
        }
        return 0;
    }

    // -------------------------------------------------------------------------
    // Child Access Operations (HOT PATH)
    // -------------------------------------------------------------------------
    //
    // HARD RULE #1 (No Blocking on Hot Path):
    // find_child() is called on every tree traversal step. It MUST be lock-free:
    //   - Node4/Node16: O(n) linear scan (n <= 16, cache-friendly)
    //   - Node48: O(1) index lookup (byte → slot index, no key verification
    //     needed because the byte alphabet is exactly 256 and each byte maps
    //     to a unique slot)
    //   - Node256: O(1) direct array access — the byte IS the index
    //   - No heap allocations, no atomics, no cross-shard access
    //

    // Find child by byte key - hot path, optimized for common cases
    Node* find_child(Node* node, uint8_t key) const {
        switch (node->type) {
            case NodeType::Node4:
                return find_child_keyed(static_cast<Node4*>(node), key);
            case NodeType::Node16:
                return find_child_keyed(static_cast<Node16*>(node), key);
            case NodeType::Node48: {
                auto* n = static_cast<Node48*>(node);
                uint8_t idx = n->index[key];
                if (idx != Node48::EMPTY_MARKER) [[likely]] {
                    return n->children[idx].get();
                }
                return nullptr;
            }
            case NodeType::Node256: {
                // Direct-mapped array lookup.  The slot index IS the key —
                // no hash, no collision fallback, no key verification.
                auto* n = static_cast<Node256*>(node);
                return n->children[key].get();
            }
        }
        return nullptr;
    }

    NodePtr extract_child(Node* node, uint8_t key) {
        switch (node->type) {
            case NodeType::Node4:
                return extract_child_keyed(static_cast<Node4*>(node), key);
            case NodeType::Node16:
                return extract_child_keyed(static_cast<Node16*>(node), key);
            case NodeType::Node48: {
                auto* n = static_cast<Node48*>(node);
                uint8_t idx = n->index[key];
                if (idx != Node48::EMPTY_MARKER) {
                    return std::move(n->children[idx]);
                }
                return nullptr;
            }
            case NodeType::Node256: {
                auto* n = static_cast<Node256*>(node);
                return std::move(n->children[key]);
            }
        }
        return nullptr;
    }

    void set_child(Node* node, uint8_t key, NodePtr child) {
        switch (node->type) {
            case NodeType::Node4:
                set_child_keyed(static_cast<Node4*>(node), key, std::move(child));
                return;
            case NodeType::Node16:
                set_child_keyed(static_cast<Node16*>(node), key, std::move(child));
                return;
            case NodeType::Node48: {
                auto* n = static_cast<Node48*>(node);
                uint8_t idx = n->index[key];
                if (idx != Node48::EMPTY_MARKER) {
                    n->children[idx] = std::move(child);
                }
                return;
            }
            case NodeType::Node256: {
                auto* n = static_cast<Node256*>(node);
                n->children[key] = std::move(child);
                return;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Node Growth Operations (Node4 → Node16 → Node48 → Node256)
    // -------------------------------------------------------------------------
    //
    // HARD RULE #4 (Bounded Memory via Slab Allocator):
    // Node transitions allocate larger nodes via make_node<NodeT>():
    //   - grow_to_node16(): Node4 full (4 children) → Node16
    //   - grow_to_node48(): Node16 full (16 children) → Node48
    //   - grow_to_node256(): Node48 full (48 children) → Node256
    //
    // All allocations use NodeSlab (shard-local free-list):
    //   - Pre-allocated pools per node type
    //   - O(1) allocation from free list (no malloc on hot path)
    //   - Old node returned to slab when unique_ptr destructs
    //
    // HARD RULE #9 (Per-Shard Data Structures):
    // Nodes are shard-local - never shared across shards:
    //   - std::unique_ptr ensures single ownership
    //   - SlabNodeDeleter returns nodes to THIS shard's free list
    //   - No atomic refcounting (unlike std::shared_ptr)
    //

    // Transfer metadata and LRU list position from src to dest.
    // src's leaf_value and LRU pointers are cleared after transfer.
    void transfer_node_metadata(Node* dest, Node* src) {
        dest->prefix = std::move(src->prefix);
        dest->leaf_value = src->leaf_value;
        dest->origin = src->origin;
        dest->last_accessed = src->last_accessed;

        // Splice dest into src's position in the LRU list
        if (src->leaf_value.has_value()) {
            dest->lru_prev = src->lru_prev;
            dest->lru_next = src->lru_next;
            if (src->lru_prev) src->lru_prev->lru_next = dest;
            if (src->lru_next) src->lru_next->lru_prev = dest;
            if (lru_head_ == src) lru_head_ = dest;
            if (lru_tail_ == src) lru_tail_ = dest;
            src->lru_prev = nullptr;
            src->lru_next = nullptr;
        }
    }

    NodePtr add_child(NodePtr parent, uint8_t key, NodePtr child) {
        switch (parent->type) {
            case NodeType::Node4: {
                auto* n = static_cast<Node4*>(parent.get());
                if (n->keys.size() < 4) {
                    n->keys.push_back(key);
                    n->children.push_back(std::move(child));
                    return parent;
                }
                return grow_to_node16(std::move(parent), key, std::move(child));
            }
            case NodeType::Node16: {
                auto* n = static_cast<Node16*>(parent.get());
                if (n->keys.size() < 16) {
                    n->keys.push_back(key);
                    n->children.push_back(std::move(child));
                    return parent;
                }
                return grow_to_node48(std::move(parent), key, std::move(child));
            }
            case NodeType::Node48: {
                auto* n = static_cast<Node48*>(parent.get());
                if (n->children.size() < 48) {
                    uint8_t pos = static_cast<uint8_t>(n->children.size());
                    n->index[key] = pos;
                    n->children.push_back(std::move(child));
                    n->keys.push_back(key);
                    return parent;
                }
                return grow_to_node256(std::move(parent), key, std::move(child));
            }
            case NodeType::Node256: {
                // Direct-mapped assignment.  The byte alphabet is exactly 256
                // so there is no "full" state reachable from add_child — at
                // most one entry per byte value, and if it's already present
                // we simply overwrite (update semantics).  The old lossy-hash
                // "Node256 is full" silent-drop case is gone by construction.
                auto* n = static_cast<Node256*>(parent.get());
                n->children[key] = std::move(child);
                return parent;
            }
        }
        return parent;
    }

    NodePtr grow_to_node16(NodePtr parent, uint8_t key, NodePtr child) {
        auto* n4 = static_cast<Node4*>(parent.get());
        auto n16_ptr = make_node<Node16>();
        auto* n16 = static_cast<Node16*>(n16_ptr.get());

        transfer_node_metadata(n16, n4);
        n16->keys = std::move(n4->keys);
        n16->children = std::move(n4->children);
        n16->keys.push_back(key);
        n16->children.push_back(std::move(child));

        return n16_ptr;
    }

    NodePtr grow_to_node48(NodePtr parent, uint8_t key, NodePtr child) {
        auto* n16 = static_cast<Node16*>(parent.get());
        auto n48_ptr = make_node<Node48>();
        auto* n48 = static_cast<Node48*>(n48_ptr.get());

        transfer_node_metadata(n48, n16);
        for (size_t i = 0; i < n16->keys.size(); i++) {
            // Each byte key has a unique slot in the index — no collisions.
            n48->index[n16->keys[i]] = static_cast<uint8_t>(i);
            n48->children.push_back(std::move(n16->children[i]));
            n48->keys.push_back(n16->keys[i]);
        }

        uint8_t pos = static_cast<uint8_t>(n48->children.size());
        n48->index[key] = pos;
        n48->children.push_back(std::move(child));
        n48->keys.push_back(key);

        return n48_ptr;
    }

    NodePtr grow_to_node256(NodePtr parent, uint8_t key, NodePtr child) {
        auto* n48 = static_cast<Node48*>(parent.get());
        auto n256_ptr = make_node<Node256>();
        auto* n256 = static_cast<Node256*>(n256_ptr.get());

        transfer_node_metadata(n256, n48);

        // Move existing children into their direct-mapped slots.  Each
        // byte key maps to exactly one slot; no collision handling is
        // needed, and if Node48 had every possible entry (128 distinct
        // keys wouldn't actually reach 48-item capacity, but still) the
        // promotion is straightforward.
        for (size_t i = 0; i < n48->keys.size(); i++) {
            uint8_t idx = n48->keys[i];
            n256->children[idx] = std::move(n48->children[i]);
        }

        // Add the new child directly into its slot.
        n256->children[key] = std::move(child);

        return n256_ptr;
    }

    // -------------------------------------------------------------------------
    // Node Creation Helper
    // -------------------------------------------------------------------------

    NodePtr create_node_for_capacity(size_t capacity) const {
        if (capacity <= 4) return make_node<Node4>();
        if (capacity <= 16) return make_node<Node16>();
        if (capacity <= 48) return make_node<Node48>();
        return make_node<Node256>();
    }

    // Split a node whose prefix exceeds MAX_PREFIX_LENGTH into a chain.
    // The node keeps the first MAX_PREFIX_LENGTH bytes; excess is pushed
    // into a new child node linked via the first excess byte as edge key.
    void split_long_prefix(Node* node) {
        while (node->prefix.size() > MAX_PREFIX_LENGTH) {
            uint8_t edge_key = node->prefix[MAX_PREFIX_LENGTH];
            auto new_child = make_node<Node4>();

            // Move the excess suffix (after edge key) to child prefix
            if (MAX_PREFIX_LENGTH + 1 < node->prefix.size()) {
                new_child->prefix.assign(
                    node->prefix.begin() + MAX_PREFIX_LENGTH + 1,
                    node->prefix.end()
                );
            }

            // Transfer leaf data to child
            new_child->leaf_value = node->leaf_value;
            new_child->origin = node->origin;
            new_child->last_accessed = node->last_accessed;

            // Transfer LRU position to child
            if (node->leaf_value.has_value()) {
                new_child->lru_prev = node->lru_prev;
                new_child->lru_next = node->lru_next;
                if (node->lru_prev) node->lru_prev->lru_next = new_child.get();
                if (node->lru_next) node->lru_next->lru_prev = new_child.get();
                if (lru_head_ == node) lru_head_ = new_child.get();
                if (lru_tail_ == node) lru_tail_ = new_child.get();
                node->lru_prev = nullptr;
                node->lru_next = nullptr;
            }
            node->leaf_value = std::nullopt;

            // Move existing children to new_child
            move_children_to_new_node(node, new_child.get());

            // Truncate parent prefix and add child
            node->prefix.resize(MAX_PREFIX_LENGTH);
            add_single_child_after_split(node, edge_key, std::move(new_child));
        }
    }

    // -------------------------------------------------------------------------
    // Split Operation
    // -------------------------------------------------------------------------

    void split_node(Node* node, size_t split_point) {
        uint8_t edge_key = node->prefix[split_point];
        size_t num_children = child_count(node);

        // Create appropriately-sized child node
        auto new_child = create_node_for_capacity(num_children);

        // Move prefix suffix to child
        if (split_point + 1 < node->prefix.size()) {
            new_child->prefix.assign(
                node->prefix.begin() + split_point + 1,
                node->prefix.end()
            );
        }

        // Transfer leaf data to child (including LRU position)
        new_child->leaf_value = node->leaf_value;
        new_child->origin = node->origin;
        new_child->last_accessed = node->last_accessed;
        if (node->leaf_value.has_value()) {
            // Replace node's position in the LRU list with new_child
            new_child->lru_prev = node->lru_prev;
            new_child->lru_next = node->lru_next;
            if (node->lru_prev) node->lru_prev->lru_next = new_child.get();
            if (node->lru_next) node->lru_next->lru_prev = new_child.get();
            if (lru_head_ == node) lru_head_ = new_child.get();
            if (lru_tail_ == node) lru_tail_ = new_child.get();
            node->lru_prev = nullptr;
            node->lru_next = nullptr;
        }
        node->leaf_value = std::nullopt;

        // Move children to new node
        move_children_to_new_node(node, new_child.get());

        // Truncate parent prefix and add single child
        node->prefix.resize(split_point);
        add_single_child_after_split(node, edge_key, std::move(new_child));

        // Rule #4: bound prefix length (defense-in-depth for split results)
        if (node->prefix.size() > MAX_PREFIX_LENGTH) {
            split_long_prefix(node);
        }
    }

    void move_children_to_new_node(Node* src, Node* dest) {
        switch (src->type) {
            case NodeType::Node4:
                move_from_small_node(static_cast<Node4*>(src), dest);
                break;
            case NodeType::Node16:
                move_from_small_node(static_cast<Node16*>(src), dest);
                break;
            case NodeType::Node48:
                move_from_node48(static_cast<Node48*>(src), dest);
                break;
            case NodeType::Node256:
                move_from_node256(static_cast<Node256*>(src), dest);
                break;
        }
    }

    template<typename SmallNode>
    void move_from_small_node(SmallNode* src, Node* dest) {
        switch (dest->type) {
            case NodeType::Node4: {
                auto* d = static_cast<Node4*>(dest);
                d->keys = std::move(src->keys);
                d->children = std::move(src->children);
                break;
            }
            case NodeType::Node16: {
                auto* d = static_cast<Node16*>(dest);
                d->keys = std::move(src->keys);
                d->children = std::move(src->children);
                break;
            }
            default:
                break;  // Should not happen for small source nodes
        }
        src->keys.clear();
        src->children.clear();
    }

    void move_from_node48(Node48* src, Node* dest) {
        switch (dest->type) {
            case NodeType::Node4: {
                auto* d = static_cast<Node4*>(dest);
                d->keys = std::move(src->keys);
                d->children = std::move(src->children);
                break;
            }
            case NodeType::Node16: {
                auto* d = static_cast<Node16*>(dest);
                d->keys = std::move(src->keys);
                d->children = std::move(src->children);
                break;
            }
            case NodeType::Node48: {
                auto* d = static_cast<Node48*>(dest);
                d->keys = std::move(src->keys);
                d->children = std::move(src->children);
                d->index = src->index;
                break;
            }
            default:
                break;
        }
        src->index.fill(Node48::EMPTY_MARKER);
        src->keys.clear();
        src->children.clear();
    }

    void move_from_node256(Node256* src, Node* dest) {
        switch (dest->type) {
            case NodeType::Node4: {
                auto* d = static_cast<Node4*>(dest);
                for (int i = 0; i < 256; i++) {
                    if (src->children[i]) {
                        d->keys.push_back(static_cast<uint8_t>(i));
                        d->children.push_back(std::move(src->children[i]));
                    }
                }
                break;
            }
            case NodeType::Node16: {
                auto* d = static_cast<Node16*>(dest);
                for (int i = 0; i < 256; i++) {
                    if (src->children[i]) {
                        d->keys.push_back(static_cast<uint8_t>(i));
                        d->children.push_back(std::move(src->children[i]));
                    }
                }
                break;
            }
            case NodeType::Node48: {
                auto* d = static_cast<Node48*>(dest);
                for (int i = 0; i < 256; i++) {
                    if (src->children[i]) {
                        uint8_t pos = static_cast<uint8_t>(d->children.size());
                        d->index[i] = pos;
                        d->keys.push_back(static_cast<uint8_t>(i));
                        d->children.push_back(std::move(src->children[i]));
                    }
                }
                break;
            }
            case NodeType::Node256: {
                auto* d = static_cast<Node256*>(dest);
                for (int i = 0; i < 256; i++) {
                    d->children[i] = std::move(src->children[i]);
                }
                break;
            }
        }
    }

    // Append a key+child to a keyed node (used after split when capacity is guaranteed).
    template<typename SmallNodeT>
    static void append_child_keyed(SmallNodeT* n, uint8_t key, NodePtr child) {
        n->keys.push_back(key);
        n->children.push_back(std::move(child));
    }

    void add_single_child_after_split(Node* node, uint8_t key, NodePtr child) {
        switch (node->type) {
            case NodeType::Node4:
                append_child_keyed(static_cast<Node4*>(node), key, std::move(child));
                break;
            case NodeType::Node16:
                append_child_keyed(static_cast<Node16*>(node), key, std::move(child));
                break;
            case NodeType::Node48: {
                auto* n = static_cast<Node48*>(node);
                // The node is empty post-split (caller just moved all
                // children out), so position 0 is correct.
                n->index[key] = 0;
                n->children.push_back(std::move(child));
                n->keys.push_back(key);
                break;
            }
            case NodeType::Node256: {
                auto* n = static_cast<Node256*>(node);
                n->children[key] = std::move(child);
                break;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Tree Statistics Implementation
    // -------------------------------------------------------------------------

    void calculate_tree_stats(Node* node, TreeStats& stats) const {
        if (!node) return;

        stats.total_nodes++;
        stats.total_prefix_length += node->prefix.size();

        switch (node->type) {
            case NodeType::Node4:
                stats.node4_count++;
                break;
            case NodeType::Node16:
                stats.node16_count++;
                break;
            case NodeType::Node48:
                stats.node48_count++;
                break;
            case NodeType::Node256:
                stats.node256_count++;
                break;
        }

        visit_children(node, [this, &stats](uint8_t, Node* child) {
            calculate_tree_stats(child, stats);
        });
    }

    // -------------------------------------------------------------------------
    // Instrumented Lookup Implementation (HOT PATH)
    // -------------------------------------------------------------------------
    //
    // HARD RULE #1 (No Blocking on Hot Path):
    // lookup_with_stats() implements the core traversal algorithm:
    //   - Iterative (not recursive) to avoid stack overhead
    //   - O(key_length) complexity, bounded by input size
    //   - Only writes: LRU timestamp update (single steady_clock::now())
    //   - No heap allocations during traversal
    //

    // Lookup with performance instrumentation.  Takes a byte-encoded key
    // (produced by the public entry point via encode_tokens_be); all
    // prefix/edge comparisons operate byte-by-byte.
    // Returns LookupResult with prefix_bytes_skipped and nodes_traversed.
    LookupResult lookup_with_stats(Node* node, std::span<const uint8_t> key_bytes) {
        LookupResult result;
        std::optional<BackendId> best_match = std::nullopt;
        Node* best_match_node = nullptr;

        while (node != nullptr) {
            result.nodes_traversed++;

            // Check prefix match
            const auto& prefix = node->prefix;
            size_t prefix_len = prefix.size();
            size_t bytes_len = key_bytes.size();
            size_t match_len = 0;

            // Fast prefix comparison
            while (match_len < prefix_len && match_len < bytes_len) {
                if (prefix[match_len] != key_bytes[match_len]) {
                    // Prefix mismatch - return best match so far
                    if (best_match_node) {
                        best_match_node->last_accessed = std::chrono::steady_clock::now();
                        lru_touch(best_match_node);
                    }
                    result.backend = best_match;
                    return result;
                }
                match_len++;
            }

            // Track bytes skipped via path compression.  RouterService
            // divides this by sizeof(TokenId) before feeding the external
            // `average_prefix_skip_length` gauge to keep its semantics.
            result.prefix_bytes_skipped += match_len;

            // Input shorter than prefix - return best match
            if (match_len < prefix_len) {
                if (best_match_node) {
                    best_match_node->last_accessed = std::chrono::steady_clock::now();
                    lru_touch(best_match_node);
                }
                result.backend = best_match;
                return result;
            }

            // Update best match if this node has a value
            if (node->leaf_value.has_value()) {
                best_match = node->leaf_value;
                best_match_node = node;
            }

            // Advance past matched prefix
            key_bytes = key_bytes.subspan(match_len);

            // If no more bytes, we're done
            if (key_bytes.empty()) [[unlikely]] {
                break;
            }

            // Find child for next edge byte
            node = find_child(node, key_bytes[0]);
            key_bytes = key_bytes.subspan(1);
        }

        // Touch LRU for matched node
        if (best_match_node) {
            best_match_node->last_accessed = std::chrono::steady_clock::now();
            lru_touch(best_match_node);
        }
        result.backend = best_match;
        return result;
    }

    // -------------------------------------------------------------------------
    // Insert Implementation
    // -------------------------------------------------------------------------

    // Returns {node, is_new_route} where is_new_route is true only when a
    // previously non-existent leaf was created (not when an existing leaf was
    // updated). This allows insert() to keep route_count_ accurate.
    //
    // Operates on byte-encoded keys.  The caller (public insert()) is
    // responsible for encoding TokenIds to bytes and for ensuring the
    // input length is a multiple of sizeof(TokenId), which maintains
    // the leaf-alignment invariant (leaves only exist at TokenId-aligned
    // byte depths from the root).
    std::pair<NodePtr, bool> insert_recursive(NodePtr node,
                             std::span<const uint8_t> key_bytes,
                             BackendId backend,
                             RouteOrigin origin) {
        // Calculate prefix match length
        size_t match_len = 0;
        while (match_len < node->prefix.size() && match_len < key_bytes.size()) {
            if (node->prefix[match_len] != key_bytes[match_len]) break;
            match_len++;
        }

        // Split if prefix mismatch
        if (match_len < node->prefix.size()) {
            split_node(node.get(), match_len);
        }

        auto remaining = key_bytes.subspan(match_len);

        // Exact match - update leaf
        if (remaining.empty()) {
            bool is_new = !node->leaf_value.has_value();
            node->leaf_value = backend;
            node->origin = origin;
            node->last_accessed = std::chrono::steady_clock::now();
            if (is_new) {
                lru_push_front(node.get());
            } else {
                lru_touch(node.get());
            }
            return {std::move(node), is_new};
        }

        // Traverse or create child
        uint8_t next_key = remaining[0];
        if (find_child(node.get(), next_key)) {
            auto child_ptr = extract_child(node.get(), next_key);
            auto [new_child, is_new] = insert_recursive(std::move(child_ptr), remaining.subspan(1), backend, origin);
            set_child(node.get(), next_key, std::move(new_child));
            return {std::move(node), is_new};
        } else {
            auto new_child = make_node<Node4>();
            if (remaining.size() > 1) {
                new_child->prefix.assign(remaining.begin() + 1, remaining.end());
            }
            new_child->leaf_value = backend;
            new_child->origin = origin;
            lru_push_front(new_child.get());
            // Rule #4: bound prefix length to prevent pathological memory usage
            if (new_child->prefix.size() > MAX_PREFIX_LENGTH) {
                split_long_prefix(new_child.get());
            }
            node = add_child(std::move(node), next_key, std::move(new_child));
            return {std::move(node), true};
        }
    }

    // -------------------------------------------------------------------------
    // Lookup Implementation (HOT PATH)
    // -------------------------------------------------------------------------
    //
    // HARD RULE #1 (No Blocking on Hot Path):
    // lookup_recursive() is the non-instrumented fast path:
    //   - Same guarantees as lookup_with_stats() but without metrics
    //   - Iterative despite the name (optimized from original recursive impl)
    //   - Used when metrics overhead is not needed
    //

    // Iterative lookup for better performance (no recursion overhead).
    // Takes byte-encoded keys; the public lookup() wrapper handles encoding.
    std::optional<BackendId> lookup_recursive(Node* node, std::span<const uint8_t> key_bytes) {
        std::optional<BackendId> best_match = std::nullopt;
        Node* best_match_node = nullptr;

        while (node != nullptr) {
            // Check prefix match
            const auto& prefix = node->prefix;
            size_t prefix_len = prefix.size();
            size_t bytes_len = key_bytes.size();
            size_t match_len = 0;

            // Fast prefix comparison
            while (match_len < prefix_len && match_len < bytes_len) {
                if (prefix[match_len] != key_bytes[match_len]) {
                    // Prefix mismatch - return best match so far
                    if (best_match_node) {
                        best_match_node->last_accessed = std::chrono::steady_clock::now();
                        lru_touch(best_match_node);
                    }
                    return best_match;
                }
                match_len++;
            }

            // Input shorter than prefix - return best match
            if (match_len < prefix_len) {
                if (best_match_node) {
                    best_match_node->last_accessed = std::chrono::steady_clock::now();
                    lru_touch(best_match_node);
                }
                return best_match;
            }

            // Update best match if this node has a value
            if (node->leaf_value.has_value()) {
                best_match = node->leaf_value;
                best_match_node = node;
            }

            // Advance past matched prefix
            key_bytes = key_bytes.subspan(match_len);

            // If no more bytes, we're done
            if (key_bytes.empty()) [[unlikely]] {
                break;
            }

            // Find child for next edge byte
            node = find_child(node, key_bytes[0]);
            key_bytes = key_bytes.subspan(1);
        }

        // Touch LRU for matched node
        if (best_match_node) {
            best_match_node->last_accessed = std::chrono::steady_clock::now();
            lru_touch(best_match_node);
        }
        return best_match;
    }

    // -------------------------------------------------------------------------
    // Traversal Implementation
    // -------------------------------------------------------------------------

    template<typename Callback>
    void for_each_leaf_recursive(Node* node, std::vector<uint8_t>& path, Callback&& callback) const {
        if (!node) return;

        size_t prefix_start = path.size();
        path.insert(path.end(), node->prefix.begin(), node->prefix.end());

        if (node->leaf_value.has_value()) {
            // Leaves must sit at TokenId-aligned byte depths because all
            // inserts go through the TokenId-span public API.  Decode the
            // accumulated byte path back to logical TokenIds for the
            // callback — RouteEntry's tokens field is token-level.
            assert(path.size() % sizeof(TokenId) == 0 &&
                   "for_each_leaf: leaf is not at a TokenId-aligned byte depth");
            std::vector<TokenId> token_path;
            token_path.reserve(path.size() / sizeof(TokenId));
            decode_tokens_be(
                std::span<const uint8_t>(path.data(), path.size()),
                token_path);
            RouteEntry entry{std::move(token_path), node->leaf_value.value(),
                             node->origin, node->last_accessed};
            callback(entry);
        }

        visit_children(node, [&](uint8_t key, Node* child) {
            path.push_back(key);
            for_each_leaf_recursive(child, path, callback);
            path.pop_back();
        });

        path.resize(prefix_start);
    }

    // -------------------------------------------------------------------------
    // Eviction Implementation
    // -------------------------------------------------------------------------

    void remove_expired_recursive(Node* node, std::chrono::steady_clock::time_point cutoff, size_t& removed) {
        if (!node) return;

        if (node->leaf_value.has_value() && node->last_accessed < cutoff) {
            lru_remove(node);
            node->leaf_value = std::nullopt;
            removed++;
        }

        visit_children(node, [&](uint8_t, Node* child) {
            remove_expired_recursive(child, cutoff, removed);
        });
    }

    // Per-backend cutoff variant: each route's expiry depends on its backend.
    template<typename CutoffFn>
    void remove_expired_per_backend_recursive(Node* node, CutoffFn& cutoff_fn, size_t& removed) {
        if (!node) return;

        if (node->leaf_value.has_value()) {
            auto cutoff = cutoff_fn(node->leaf_value.value());
            if (node->last_accessed < cutoff) {
                lru_remove(node);
                node->leaf_value = std::nullopt;
                removed++;
            }
        }

        visit_children(node, [&](uint8_t, Node* child) {
            remove_expired_per_backend_recursive(child, cutoff_fn, removed);
        });
    }

    void remove_by_backend_recursive(Node* node, BackendId backend, RouteOrigin origin, size_t& removed) {
        if (!node) return;

        if (node->leaf_value.has_value() &&
            node->leaf_value.value() == backend &&
            node->origin == origin) {
            lru_remove(node);
            node->leaf_value = std::nullopt;
            removed++;
        }

        visit_children(node, [&](uint8_t, Node* child) {
            remove_by_backend_recursive(child, backend, origin, removed);
        });
    }

    // -------------------------------------------------------------------------
    // Compaction Implementation
    // -------------------------------------------------------------------------

    // Estimated byte sizes for each node type (for metrics)
    static constexpr size_t NODE4_SIZE = 192;
    static constexpr size_t NODE16_SIZE = 384;
    static constexpr size_t NODE48_SIZE = 640;
    static constexpr size_t NODE256_SIZE = 2240;

    static size_t node_byte_size(NodeType type) {
        switch (type) {
            case NodeType::Node4: return NODE4_SIZE;
            case NodeType::Node16: return NODE16_SIZE;
            case NodeType::Node48: return NODE48_SIZE;
            case NodeType::Node256: return NODE256_SIZE;
        }
        return 0;
    }

    // Check if a node is empty (no leaf value, no children)
    static bool is_node_empty(Node* node) {
        return !node->leaf_value.has_value() && child_count(node) == 0;
    }

    // Check if a node should be shrunk to a smaller type
    static bool should_shrink(Node* node) {
        size_t count = child_count(node);
        switch (node->type) {
            case NodeType::Node256: return count <= 48;
            case NodeType::Node48: return count <= 16;
            case NodeType::Node16: return count <= 4;
            case NodeType::Node4: return false; // Already smallest
        }
        return false;
    }

    // Compact helper for keyed node types (Node4/Node16/Node48).
    template<typename SmallNodeT>
    void compact_children_keyed(SmallNodeT* n, CompactionStats& stats,
                                std::vector<uint8_t>& keys_to_remove) {
        for (size_t i = 0; i < n->keys.size(); i++) {
            Node* child = n->children[i].get();
            compact_children(child, stats);
            if (should_shrink(child)) {
                n->children[i] = shrink_node(std::move(n->children[i]), stats);
            }
            if (is_node_empty(n->children[i].get())) {
                keys_to_remove.push_back(n->keys[i]);
            }
        }
    }

    // Recursively compact children of a node, removing empty ones
    void compact_children(Node* node, CompactionStats& stats) {
        if (!node) return;

        // Collect keys of children to remove (can't modify during iteration)
        std::vector<uint8_t> keys_to_remove;
        keys_to_remove.reserve(8); // Bounded allocation (Rule #4)

        switch (node->type) {
            case NodeType::Node4:
                compact_children_keyed(static_cast<Node4*>(node), stats, keys_to_remove);
                break;
            case NodeType::Node16:
                compact_children_keyed(static_cast<Node16*>(node), stats, keys_to_remove);
                break;
            case NodeType::Node48:
                compact_children_keyed(static_cast<Node48*>(node), stats, keys_to_remove);
                break;
            case NodeType::Node256: {
                auto* n = static_cast<Node256*>(node);
                for (int i = 0; i < 256; i++) {
                    if (n->children[i]) {
                        Node* child = n->children[i].get();
                        compact_children(child, stats);
                        if (should_shrink(child)) {
                            n->children[i] = shrink_node(std::move(n->children[i]), stats);
                        }
                        if (is_node_empty(n->children[i].get())) {
                            keys_to_remove.push_back(static_cast<uint8_t>(i));
                        }
                    }
                }
                break;
            }
        }

        // Remove empty children (triggers SlabNodeDeleter, returns memory to free list)
        for (uint8_t key : keys_to_remove) {
            NodeType removed_type = remove_child(node, key);
            stats.nodes_removed++;
            stats.bytes_reclaimed += node_byte_size(removed_type);
        }
    }

    // Remove a child by byte key, returns the type of the removed node
    NodeType remove_child(Node* parent, uint8_t key) {
        switch (parent->type) {
            case NodeType::Node4:
                return remove_child_keyed(static_cast<Node4*>(parent), key);
            case NodeType::Node16:
                return remove_child_keyed(static_cast<Node16*>(parent), key);
            case NodeType::Node48: {
                auto* n = static_cast<Node48*>(parent);
                uint8_t indexed_pos = n->index[key];

                // Direct index lookup — each byte key maps to a unique
                // slot, no collision handling needed.
                if (indexed_pos != Node48::EMPTY_MARKER) {
                    NodeType removed_type = n->children[indexed_pos]->type;
                    n->children.erase(n->children.begin() + indexed_pos);
                    n->keys.erase(n->keys.begin() + indexed_pos);
                    n->index[key] = Node48::EMPTY_MARKER;
                    // Reindex other entries whose slot position shifted down.
                    for (size_t i = 0; i < 256; i++) {
                        if (n->index[i] != Node48::EMPTY_MARKER && n->index[i] > indexed_pos) {
                            n->index[i]--;
                        }
                    }
                    return removed_type;
                }
                break;
            }
            case NodeType::Node256: {
                auto* n = static_cast<Node256*>(parent);
                if (n->children[key]) {
                    NodeType removed_type = n->children[key]->type;
                    n->children[key].reset();
                    return removed_type;
                }
                break;
            }
        }
        return NodeType::Node4; // Fallback (shouldn't happen)
    }

    // Shrink a node to the appropriate smaller type
    NodePtr shrink_node(NodePtr node, CompactionStats& stats) {
        size_t count = child_count(node.get());

        switch (node->type) {
            case NodeType::Node256:
                if (count <= 4) {
                    stats.nodes_shrunk++;
                    stats.bytes_reclaimed += NODE256_SIZE - NODE4_SIZE;
                    return shrink_to_node4(std::move(node));
                } else if (count <= 16) {
                    stats.nodes_shrunk++;
                    stats.bytes_reclaimed += NODE256_SIZE - NODE16_SIZE;
                    return shrink_to_node16(std::move(node));
                } else if (count <= 48) {
                    stats.nodes_shrunk++;
                    stats.bytes_reclaimed += NODE256_SIZE - NODE48_SIZE;
                    return shrink_to_node48(std::move(node));
                }
                break;
            case NodeType::Node48:
                if (count <= 4) {
                    stats.nodes_shrunk++;
                    stats.bytes_reclaimed += NODE48_SIZE - NODE4_SIZE;
                    return shrink_to_node4(std::move(node));
                } else if (count <= 16) {
                    stats.nodes_shrunk++;
                    stats.bytes_reclaimed += NODE48_SIZE - NODE16_SIZE;
                    return shrink_to_node16(std::move(node));
                }
                break;
            case NodeType::Node16:
                if (count <= 4) {
                    stats.nodes_shrunk++;
                    stats.bytes_reclaimed += NODE16_SIZE - NODE4_SIZE;
                    return shrink_to_node4(std::move(node));
                }
                break;
            case NodeType::Node4:
                break; // Already smallest
        }
        return node;
    }

    // -------------------------------------------------------------------------
    // Shrink Operations (reverse of grow_to_*)
    // -------------------------------------------------------------------------

    NodePtr shrink_to_node4(NodePtr node) {
        auto n4_ptr = make_node<Node4>();
        auto* n4 = static_cast<Node4*>(n4_ptr.get());
        transfer_node_metadata(n4, node.get());

        // Move up to 4 children
        size_t count = 0;
        switch (node->type) {
            case NodeType::Node16: {
                auto* src = static_cast<Node16*>(node.get());
                for (size_t i = 0; i < src->keys.size() && count < 4; i++, count++) {
                    n4->keys.push_back(src->keys[i]);
                    n4->children.push_back(std::move(src->children[i]));
                }
                break;
            }
            case NodeType::Node48: {
                auto* src = static_cast<Node48*>(node.get());
                for (size_t i = 0; i < src->keys.size() && count < 4; i++, count++) {
                    n4->keys.push_back(src->keys[i]);
                    n4->children.push_back(std::move(src->children[i]));
                }
                break;
            }
            case NodeType::Node256: {
                auto* src = static_cast<Node256*>(node.get());
                for (int i = 0; i < 256 && count < 4; i++) {
                    if (src->children[i]) {
                        n4->keys.push_back(static_cast<uint8_t>(i));
                        n4->children.push_back(std::move(src->children[i]));
                        count++;
                    }
                }
                break;
            }
            default:
                break;
        }
        return n4_ptr;
    }

    NodePtr shrink_to_node16(NodePtr node) {
        auto n16_ptr = make_node<Node16>();
        auto* n16 = static_cast<Node16*>(n16_ptr.get());
        transfer_node_metadata(n16, node.get());

        // Move up to 16 children
        size_t count = 0;
        switch (node->type) {
            case NodeType::Node48: {
                auto* src = static_cast<Node48*>(node.get());
                for (size_t i = 0; i < src->keys.size() && count < 16; i++, count++) {
                    n16->keys.push_back(src->keys[i]);
                    n16->children.push_back(std::move(src->children[i]));
                }
                break;
            }
            case NodeType::Node256: {
                auto* src = static_cast<Node256*>(node.get());
                for (int i = 0; i < 256 && count < 16; i++) {
                    if (src->children[i]) {
                        n16->keys.push_back(static_cast<uint8_t>(i));
                        n16->children.push_back(std::move(src->children[i]));
                        count++;
                    }
                }
                break;
            }
            default:
                break;
        }
        return n16_ptr;
    }

    NodePtr shrink_to_node48(NodePtr node) {
        auto n48_ptr = make_node<Node48>();
        auto* n48 = static_cast<Node48*>(n48_ptr.get());
        transfer_node_metadata(n48, node.get());

        // Only Node256 can shrink to Node48
        if (node->type == NodeType::Node256) {
            auto* src = static_cast<Node256*>(node.get());
            size_t count = 0;
            for (int i = 0; i < 256 && count < 48; i++) {
                if (src->children[i]) {
                    n48->index[i] = static_cast<uint8_t>(count);
                    n48->keys.push_back(static_cast<uint8_t>(i));
                    n48->children.push_back(std::move(src->children[i]));
                    count++;
                }
            }
        }
        return n48_ptr;
    }
};

// Out-of-line destructor for RadixTree::DumpNode. At this point DumpNode
// is a complete type, so libstdc++'s `is_destructible` trait evaluation
// for `std::vector<std::pair<uint8_t, DumpNode>>::~vector` succeeds on
// clang+libstdc++. See the comment block above the DumpNode definition.
#ifndef RANVIER_FUZZING
inline RadixTree::DumpNode::~DumpNode() = default;
#endif

} // namespace ranvier
