#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>
#include <new>
#include <cassert>

/*
Shard-Local Slab Allocator for RadixTree Nodes

Design Philosophy:
- Pre-allocates large contiguous chunks (2MB) using aligned memory allocation
- Four size-classed pools: one for each NodeType (Node4, Node16, Node48, Node256)
- Nodes of the same type are physically adjacent in memory for L1/L2 cache locality
- O(1) allocation and deallocation via free list
- Per-shard isolation: thread_local, no mutexes or atomics
- Custom deleter returns memory to slab instead of calling delete

Memory Layout:
┌─────────────────────────────────────────────────────────────────────────────┐
│                              2MB Chunk (Node4 Pool)                        │
├────────┬────────┬────────┬────────┬────────┬────────┬────────┬─────────────┤
│ Header │ Node4  │ Header │ Node4  │ Header │ Node4  │  ...   │   (free)    │
│ 8 bytes│128 bytes│8 bytes│128 bytes│8 bytes│128 bytes│        │             │
└────────┴────────┴────────┴────────┴────────┴────────┴────────┴─────────────┘

Each slot = SlabHeader (8 bytes) + Node storage (size varies by type)
Free list maintains O(1) allocation by reusing deallocated slots.
*/

namespace ranvier {

// Forward declarations from radix_tree.hpp
struct Node;
struct Node4;
struct Node16;
struct Node48;
struct Node256;
enum class NodeType : uint8_t;

// =============================================================================
// Constants
// =============================================================================

// Chunk size for pre-allocation (2MB = huge page size on many systems)
inline constexpr size_t SLAB_CHUNK_SIZE = 2 * 1024 * 1024;

// Memory alignment (64 bytes = cache line size on most x86_64 systems)
inline constexpr size_t SLAB_ALIGNMENT = 64;

// =============================================================================
// Slab Header
// =============================================================================

// Header stored before each allocated object to identify its pool
// This allows the custom deleter to return memory to the correct pool
struct alignas(8) SlabHeader {
    uint8_t pool_index;  // Which pool this allocation belongs to (0-3)
    uint8_t reserved[7]; // Padding for 8-byte alignment
};

static_assert(sizeof(SlabHeader) == 8, "SlabHeader must be 8 bytes");

// =============================================================================
// Forward Declaration
// =============================================================================

class NodeSlab;

// Thread-local slab accessor (defined in node_slab.cpp)
NodeSlab* get_node_slab() noexcept;
void set_node_slab(NodeSlab* slab) noexcept;

// =============================================================================
// Custom Deleter
// =============================================================================

// Custom deleter for std::unique_ptr that returns memory to the slab's free list
// instead of calling delete. This enables O(1) deallocation.
struct SlabNodeDeleter {
    void operator()(Node* ptr) const noexcept;
};

// Type alias for slab-allocated node pointers
// This replaces std::unique_ptr<Node> throughout RadixTree
using NodePtr = std::unique_ptr<Node, SlabNodeDeleter>;

// =============================================================================
// Pool Configuration
// =============================================================================

// Slot sizes for each node type (including SlabHeader)
// These are computed at compile time and rounded up for cache alignment
struct SlabPoolConfig {
    // Slot sizes must accommodate SlabHeader + sizeof(NodeType) + alignment padding
    // Values are determined empirically from sizeof() and aligned to cache lines

    // Node4:   ~112 bytes object -> 128 byte slot + 8 header = 136, round to 192
    // Node16:  ~112 bytes object -> 128 byte slot + 8 header = 136, round to 192
    // Node48:  ~368 bytes object -> 384 byte slot + 8 header = 392, round to 448
    // Node256: ~2112 bytes object -> 2176 byte slot + 8 header = 2184, round to 2240

    static constexpr size_t NODE4_SLOT_SIZE = 192;
    static constexpr size_t NODE16_SLOT_SIZE = 192;
    static constexpr size_t NODE48_SLOT_SIZE = 448;
    static constexpr size_t NODE256_SLOT_SIZE = 2240;

    // Slots per 2MB chunk for each type
    static constexpr size_t NODE4_SLOTS_PER_CHUNK = SLAB_CHUNK_SIZE / NODE4_SLOT_SIZE;
    static constexpr size_t NODE16_SLOTS_PER_CHUNK = SLAB_CHUNK_SIZE / NODE16_SLOT_SIZE;
    static constexpr size_t NODE48_SLOTS_PER_CHUNK = SLAB_CHUNK_SIZE / NODE48_SLOT_SIZE;
    static constexpr size_t NODE256_SLOTS_PER_CHUNK = SLAB_CHUNK_SIZE / NODE256_SLOT_SIZE;

    static constexpr size_t slot_size(size_t pool_index) {
        constexpr size_t sizes[] = {
            NODE4_SLOT_SIZE,
            NODE16_SLOT_SIZE,
            NODE48_SLOT_SIZE,
            NODE256_SLOT_SIZE
        };
        return sizes[pool_index];
    }

    static constexpr size_t slots_per_chunk(size_t pool_index) {
        constexpr size_t counts[] = {
            NODE4_SLOTS_PER_CHUNK,
            NODE16_SLOTS_PER_CHUNK,
            NODE48_SLOTS_PER_CHUNK,
            NODE256_SLOTS_PER_CHUNK
        };
        return counts[pool_index];
    }
};

// =============================================================================
// NodeSlab Class
// =============================================================================

class NodeSlab {
public:
    // Number of pools (one per NodeType)
    static constexpr size_t NUM_POOLS = 4;

    // -------------------------------------------------------------------------
    // Construction / Destruction
    // -------------------------------------------------------------------------

    NodeSlab();
    ~NodeSlab();

    // Non-copyable, non-movable (slab owns aligned memory)
    NodeSlab(const NodeSlab&) = delete;
    NodeSlab& operator=(const NodeSlab&) = delete;
    NodeSlab(NodeSlab&&) = delete;
    NodeSlab& operator=(NodeSlab&&) = delete;

    // -------------------------------------------------------------------------
    // Factory Methods
    // -------------------------------------------------------------------------

    // Allocate and construct a node of the specified type
    // Returns a unique_ptr with custom deleter that returns memory to slab
    template<typename NodeT>
    NodePtr make_node();

    // -------------------------------------------------------------------------
    // Low-Level Allocation (for use by deleter)
    // -------------------------------------------------------------------------

    // Allocate raw memory from the appropriate pool
    // Returns pointer to usable memory (after SlabHeader)
    void* allocate(size_t pool_index);

    // Return memory to the pool's free list
    // ptr must point to memory returned by allocate()
    void deallocate(void* ptr) noexcept;

    // -------------------------------------------------------------------------
    // Statistics
    // -------------------------------------------------------------------------

    struct Stats {
        size_t total_chunks;           // Total 2MB chunks allocated
        size_t total_bytes;            // Total bytes allocated from system
        size_t allocated_nodes[4];     // Nodes currently allocated per pool
        size_t free_list_size[4];      // Slots in free list per pool
        size_t peak_allocated[4];      // Peak allocation per pool
    };

    Stats get_stats() const noexcept;

private:
    // -------------------------------------------------------------------------
    // Pool Structure
    // -------------------------------------------------------------------------

    struct Pool {
        std::vector<void*> free_list;   // Stack of free slots (LIFO for cache locality)
        std::vector<void*> chunks;       // Owned chunk pointers (for cleanup)
        size_t slot_size;                // Size of each slot in this pool
        size_t slots_per_chunk;          // Number of slots per 2MB chunk
        size_t allocated_count = 0;      // Currently allocated nodes
        size_t peak_count = 0;           // Peak allocated nodes

        Pool(size_t slot_sz, size_t slots)
            : slot_size(slot_sz), slots_per_chunk(slots) {
            // Pre-reserve free list for one chunk
            free_list.reserve(slots);
        }
    };

    Pool pools_[NUM_POOLS];

    // -------------------------------------------------------------------------
    // Internal Methods
    // -------------------------------------------------------------------------

    // Allocate a new 2MB chunk and add slots to the free list
    void allocate_chunk(size_t pool_index);

    // Map NodeType enum to pool index
    static constexpr size_t pool_index_for_type(NodeType type) {
        return static_cast<size_t>(type);
    }

    // Get NodeType enum from pool index
    static constexpr NodeType type_for_pool_index(size_t idx) {
        return static_cast<NodeType>(idx);
    }
};

// =============================================================================
// Template Implementation
// =============================================================================

// Helper trait to map NodeT to its pool index
template<typename NodeT>
struct NodePoolIndex;

// Explicit specialization declarations for make_node<T>()
// These are defined in node_slab.cpp after radix_tree.hpp includes the node definitions
template<> NodePtr NodeSlab::make_node<Node4>();
template<> NodePtr NodeSlab::make_node<Node16>();
template<> NodePtr NodeSlab::make_node<Node48>();
template<> NodePtr NodeSlab::make_node<Node256>();

} // namespace ranvier
