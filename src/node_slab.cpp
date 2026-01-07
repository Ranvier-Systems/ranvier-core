#include "node_slab.hpp"
#include "radix_tree.hpp"

#include <cstdlib>
#include <cstring>
#include <stdexcept>

// Use Seastar's aligned allocator when available
#ifdef HAVE_SEASTAR
#include <seastar/core/memory.hh>
#endif

namespace ranvier {

// =============================================================================
// Thread-Local Slab Storage
// =============================================================================

// Per-shard slab allocator (no synchronization needed)
thread_local NodeSlab* tl_node_slab = nullptr;

NodeSlab* get_node_slab() noexcept {
    return tl_node_slab;
}

void set_node_slab(NodeSlab* slab) noexcept {
    tl_node_slab = slab;
}

// =============================================================================
// Custom Deleter Implementation
// =============================================================================

void SlabNodeDeleter::operator()(Node* ptr) const noexcept {
    if (!ptr) return;

    // Invoke destructor manually (placement delete)
    // The correct destructor is called via virtual dispatch
    ptr->~Node();

    // Return memory to slab
    NodeSlab* slab = get_node_slab();
    if (slab) {
        slab->deallocate(ptr);
    } else {
        // Fallback: if no slab (shouldn't happen in normal operation),
        // we need to free the memory. Calculate the raw pointer.
        void* raw = static_cast<char*>(static_cast<void*>(ptr)) - sizeof(SlabHeader);
#ifdef HAVE_SEASTAR
        seastar::memory::free(raw);
#else
        std::free(raw);
#endif
    }
}

// =============================================================================
// Aligned Memory Allocation Helpers
// =============================================================================

namespace {

void* allocate_aligned_chunk(size_t size, size_t alignment) {
#ifdef HAVE_SEASTAR
    // Use Seastar's allocator for optimal NUMA-aware allocation
    return seastar::memory::allocate_aligned(alignment, size);
#else
    // Standard aligned allocation
    void* ptr = nullptr;
#if defined(_WIN32)
    ptr = _aligned_malloc(size, alignment);
#else
    if (posix_memalign(&ptr, alignment, size) != 0) {
        ptr = nullptr;
    }
#endif
    return ptr;
#endif
}

void free_aligned_chunk(void* ptr) {
#ifdef HAVE_SEASTAR
    seastar::memory::free(ptr);
#else
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
#endif
}

} // anonymous namespace

// =============================================================================
// NodeSlab Implementation
// =============================================================================

NodeSlab::NodeSlab()
    : pools_{
        Pool{SlabPoolConfig::NODE4_SLOT_SIZE, SlabPoolConfig::NODE4_SLOTS_PER_CHUNK},
        Pool{SlabPoolConfig::NODE16_SLOT_SIZE, SlabPoolConfig::NODE16_SLOTS_PER_CHUNK},
        Pool{SlabPoolConfig::NODE48_SLOT_SIZE, SlabPoolConfig::NODE48_SLOTS_PER_CHUNK},
        Pool{SlabPoolConfig::NODE256_SLOT_SIZE, SlabPoolConfig::NODE256_SLOTS_PER_CHUNK}
    }
{
    // Pre-allocate one chunk for the most common node type (Node4)
    // This avoids allocation latency on the first insert
    allocate_chunk(0);
}

NodeSlab::~NodeSlab() {
    // Free all chunks (intrusive free list requires no cleanup)
    for (auto& pool : pools_) {
        for (void* chunk : pool.chunks) {
            free_aligned_chunk(chunk);
        }
        pool.chunks.clear();
        pool.free_head = nullptr;
        pool.free_count = 0;
    }
}

void NodeSlab::allocate_chunk(size_t pool_index) {
    assert(pool_index < NUM_POOLS);
    Pool& pool = pools_[pool_index];

    // Allocate 2MB aligned chunk
    void* chunk = allocate_aligned_chunk(SLAB_CHUNK_SIZE, SLAB_ALIGNMENT);
    if (!chunk) {
        throw std::bad_alloc();
    }

    // Track the chunk for cleanup
    pool.chunks.push_back(chunk);

    // Initialize all slots and link them into the intrusive free list.
    // Each free slot stores a pointer to the next free slot in its first 8 bytes.
    // We iterate forward so the first slot becomes the head (LIFO for cache locality).
    char* base = static_cast<char*>(chunk);
    size_t slot_size = pool.slot_size;
    size_t num_slots = pool.slots_per_chunk;

    // Link all new slots into the free list (prepend to existing list)
    // The first slot in the chunk will point to the previous head
    for (size_t i = 0; i < num_slots; ++i) {
        void* slot = base + i * slot_size;
        // Store pointer to current head in this slot
        *static_cast<void**>(slot) = pool.free_head;
        // This slot becomes the new head
        pool.free_head = slot;
    }
    pool.free_count += num_slots;
}

void* NodeSlab::allocate(size_t pool_index) {
    assert(pool_index < NUM_POOLS);
    Pool& pool = pools_[pool_index];

    // Allocate new chunk if free list is empty
    if (pool.free_head == nullptr) {
        allocate_chunk(pool_index);
    }

    // Pop from intrusive free list (true O(1), no heap operations)
    void* slot = pool.free_head;
    pool.free_head = *static_cast<void**>(slot);  // Follow the next pointer
    pool.free_count--;

    // Initialize header
    auto* header = static_cast<SlabHeader*>(slot);
    header->pool_index = static_cast<uint8_t>(pool_index);
    std::memset(header->reserved, 0, sizeof(header->reserved));

    // Update statistics
    pool.allocated_count++;
    if (pool.allocated_count > pool.peak_count) {
        pool.peak_count = pool.allocated_count;
    }

    // Return pointer past the header (where the Node object will live)
    return static_cast<char*>(slot) + sizeof(SlabHeader);
}

void NodeSlab::deallocate(void* ptr) noexcept {
    if (!ptr) return;

    // Recover the raw slot pointer (before the header)
    void* slot = static_cast<char*>(ptr) - sizeof(SlabHeader);
    auto* header = static_cast<SlabHeader*>(slot);

    size_t pool_index = header->pool_index;

    // Bounds check even in release builds to prevent memory corruption
    // from causing cascading failures
    if (pool_index >= NUM_POOLS) [[unlikely]] {
        // Corrupted header - log and leak rather than corrupt further
        // In production, this indicates a serious bug that needs investigation
        return;
    }

    Pool& pool = pools_[pool_index];

    // Push to intrusive free list (true O(1), no heap operations, noexcept)
    *static_cast<void**>(slot) = pool.free_head;  // Point to current head
    pool.free_head = slot;                         // This slot becomes new head
    pool.free_count++;
    pool.allocated_count--;
}

NodeSlab::Stats NodeSlab::get_stats() const noexcept {
    Stats stats{};

    for (size_t i = 0; i < NUM_POOLS; ++i) {
        const Pool& pool = pools_[i];
        stats.total_chunks += pool.chunks.size();
        stats.allocated_nodes[i] = pool.allocated_count;
        stats.free_list_size[i] = pool.free_count;
        stats.peak_allocated[i] = pool.peak_count;
    }

    stats.total_bytes = stats.total_chunks * SLAB_CHUNK_SIZE;
    return stats;
}

// =============================================================================
// Factory Method Template Specializations
// =============================================================================

// Template specialization helper
namespace detail {

template<typename NodeT>
struct PoolIndexTraits;

template<>
struct PoolIndexTraits<Node4> {
    static constexpr size_t value = 0;
};

template<>
struct PoolIndexTraits<Node16> {
    static constexpr size_t value = 1;
};

template<>
struct PoolIndexTraits<Node48> {
    static constexpr size_t value = 2;
};

template<>
struct PoolIndexTraits<Node256> {
    static constexpr size_t value = 3;
};

} // namespace detail

// Explicit template instantiations for all node types
template<>
NodePtr NodeSlab::make_node<Node4>() {
    constexpr size_t pool_idx = detail::PoolIndexTraits<Node4>::value;
    void* mem = allocate(pool_idx);
    Node4* node = new (mem) Node4();
    return NodePtr(node);
}

template<>
NodePtr NodeSlab::make_node<Node16>() {
    constexpr size_t pool_idx = detail::PoolIndexTraits<Node16>::value;
    void* mem = allocate(pool_idx);
    Node16* node = new (mem) Node16();
    return NodePtr(node);
}

template<>
NodePtr NodeSlab::make_node<Node48>() {
    constexpr size_t pool_idx = detail::PoolIndexTraits<Node48>::value;
    void* mem = allocate(pool_idx);
    Node48* node = new (mem) Node48();
    return NodePtr(node);
}

template<>
NodePtr NodeSlab::make_node<Node256>() {
    constexpr size_t pool_idx = detail::PoolIndexTraits<Node256>::value;
    void* mem = allocate(pool_idx);
    Node256* node = new (mem) Node256();
    return NodePtr(node);
}

// =============================================================================
// Compile-Time Size Verification
// =============================================================================

// Verify that our slot sizes are sufficient for each node type
// These will cause compile errors if the sizes are wrong
static_assert(
    SlabPoolConfig::NODE4_SLOT_SIZE >= sizeof(SlabHeader) + sizeof(Node4),
    "NODE4_SLOT_SIZE too small"
);

static_assert(
    SlabPoolConfig::NODE16_SLOT_SIZE >= sizeof(SlabHeader) + sizeof(Node16),
    "NODE16_SLOT_SIZE too small"
);

static_assert(
    SlabPoolConfig::NODE48_SLOT_SIZE >= sizeof(SlabHeader) + sizeof(Node48),
    "NODE48_SLOT_SIZE too small"
);

static_assert(
    SlabPoolConfig::NODE256_SLOT_SIZE >= sizeof(SlabHeader) + sizeof(Node256),
    "NODE256_SLOT_SIZE too small"
);

} // namespace ranvier
