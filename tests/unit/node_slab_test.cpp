// Ranvier Core - Node Slab Allocator Unit Tests
//
// Dedicated tests for the per-shard memory pool used by RadixTree nodes.
// Tests allocate/free round-trips, free-list integrity, peak tracking,
// pool auto-growth on exhaustion, and make_node factory methods.

#include "node_slab.hpp"
#include "radix_tree.hpp"
#include <gtest/gtest.h>
#include <cstddef>
#include <vector>

using namespace ranvier;

// =============================================================================
// SlabPoolConfig Compile-Time Tests
// =============================================================================

class SlabPoolConfigTest : public ::testing::Test {};

TEST_F(SlabPoolConfigTest, SlotSizesCacheLineAligned) {
    EXPECT_EQ(SlabPoolConfig::NODE4_SLOT_SIZE % SLAB_ALIGNMENT, 0u);
    EXPECT_EQ(SlabPoolConfig::NODE16_SLOT_SIZE % SLAB_ALIGNMENT, 0u);
    EXPECT_EQ(SlabPoolConfig::NODE48_SLOT_SIZE % SLAB_ALIGNMENT, 0u);
    EXPECT_EQ(SlabPoolConfig::NODE256_SLOT_SIZE % SLAB_ALIGNMENT, 0u);
}

TEST_F(SlabPoolConfigTest, SlotsPerChunkConsistentWithChunkSize) {
    EXPECT_EQ(SlabPoolConfig::NODE4_SLOTS_PER_CHUNK,
              SLAB_CHUNK_SIZE / SlabPoolConfig::NODE4_SLOT_SIZE);
    EXPECT_EQ(SlabPoolConfig::NODE16_SLOTS_PER_CHUNK,
              SLAB_CHUNK_SIZE / SlabPoolConfig::NODE16_SLOT_SIZE);
    EXPECT_EQ(SlabPoolConfig::NODE48_SLOTS_PER_CHUNK,
              SLAB_CHUNK_SIZE / SlabPoolConfig::NODE48_SLOT_SIZE);
    EXPECT_EQ(SlabPoolConfig::NODE256_SLOTS_PER_CHUNK,
              SLAB_CHUNK_SIZE / SlabPoolConfig::NODE256_SLOT_SIZE);
}

TEST_F(SlabPoolConfigTest, SlotSizeAccessor) {
    EXPECT_EQ(SlabPoolConfig::slot_size(0), SlabPoolConfig::NODE4_SLOT_SIZE);
    EXPECT_EQ(SlabPoolConfig::slot_size(1), SlabPoolConfig::NODE16_SLOT_SIZE);
    EXPECT_EQ(SlabPoolConfig::slot_size(2), SlabPoolConfig::NODE48_SLOT_SIZE);
    EXPECT_EQ(SlabPoolConfig::slot_size(3), SlabPoolConfig::NODE256_SLOT_SIZE);
}

TEST_F(SlabPoolConfigTest, SlotsPerChunkAccessor) {
    EXPECT_EQ(SlabPoolConfig::slots_per_chunk(0), SlabPoolConfig::NODE4_SLOTS_PER_CHUNK);
    EXPECT_EQ(SlabPoolConfig::slots_per_chunk(1), SlabPoolConfig::NODE16_SLOTS_PER_CHUNK);
    EXPECT_EQ(SlabPoolConfig::slots_per_chunk(2), SlabPoolConfig::NODE48_SLOTS_PER_CHUNK);
    EXPECT_EQ(SlabPoolConfig::slots_per_chunk(3), SlabPoolConfig::NODE256_SLOTS_PER_CHUNK);
}

TEST_F(SlabPoolConfigTest, NodeSizesAreLargerSlots) {
    // Slot sizes must accommodate SlabHeader + Node + alignment
    EXPECT_GT(SlabPoolConfig::NODE4_SLOT_SIZE, sizeof(SlabHeader));
    EXPECT_GT(SlabPoolConfig::NODE256_SLOT_SIZE, SlabPoolConfig::NODE4_SLOT_SIZE);
}

// =============================================================================
// NodeSlab Low-Level Allocate/Deallocate Tests
// =============================================================================

class NodeSlabAllocateTest : public ::testing::Test {
protected:
    void SetUp() override {
        slab = std::make_unique<NodeSlab>();
        set_node_slab(slab.get());
    }
    void TearDown() override {
        set_node_slab(nullptr);
        slab.reset();
    }
    std::unique_ptr<NodeSlab> slab;
};

TEST_F(NodeSlabAllocateTest, ConstructorPreAllocatesOneChunk) {
    auto stats = slab->get_stats();
    // Constructor pre-allocates one chunk for pool 0 (Node4)
    EXPECT_GE(stats.total_chunks, 1u);
    EXPECT_GE(stats.total_bytes, SLAB_CHUNK_SIZE);
    // Pool 0 should have free slots from the pre-allocated chunk
    EXPECT_GT(stats.free_list_size[0], 0u);
}

TEST_F(NodeSlabAllocateTest, AllocateReturnsNonNull) {
    void* ptr = slab->allocate(0);
    ASSERT_NE(ptr, nullptr);
    slab->deallocate(ptr);
}

TEST_F(NodeSlabAllocateTest, AllocateFromEachPool) {
    for (size_t pool = 0; pool < NodeSlab::NUM_POOLS; ++pool) {
        void* ptr = slab->allocate(pool);
        ASSERT_NE(ptr, nullptr) << "Pool " << pool << " returned null";
        slab->deallocate(ptr);
    }
}

TEST_F(NodeSlabAllocateTest, AllocateUpdatesStats) {
    auto before = slab->get_stats();
    void* ptr = slab->allocate(0);
    auto after = slab->get_stats();

    EXPECT_EQ(after.allocated_nodes[0], before.allocated_nodes[0] + 1);
    EXPECT_EQ(after.free_list_size[0], before.free_list_size[0] - 1);

    slab->deallocate(ptr);
}

TEST_F(NodeSlabAllocateTest, DeallocateUpdatesStats) {
    void* ptr = slab->allocate(0);
    auto before = slab->get_stats();
    slab->deallocate(ptr);
    auto after = slab->get_stats();

    EXPECT_EQ(after.allocated_nodes[0], before.allocated_nodes[0] - 1);
    EXPECT_EQ(after.free_list_size[0], before.free_list_size[0] + 1);
}

TEST_F(NodeSlabAllocateTest, DeallocateNullIsSafe) {
    // Should not crash
    slab->deallocate(nullptr);
}

TEST_F(NodeSlabAllocateTest, AllocateDeallocateRoundTrip) {
    auto initial = slab->get_stats();

    void* ptr = slab->allocate(0);
    slab->deallocate(ptr);

    auto final_stats = slab->get_stats();
    EXPECT_EQ(final_stats.allocated_nodes[0], initial.allocated_nodes[0]);
    EXPECT_EQ(final_stats.free_list_size[0], initial.free_list_size[0]);
}

// =============================================================================
// Free-List Integrity Tests
// =============================================================================

class NodeSlabFreeListTest : public ::testing::Test {
protected:
    void SetUp() override {
        slab = std::make_unique<NodeSlab>();
        set_node_slab(slab.get());
    }
    void TearDown() override {
        set_node_slab(nullptr);
        slab.reset();
    }
    std::unique_ptr<NodeSlab> slab;
};

TEST_F(NodeSlabFreeListTest, ReallocateReturnsPreviouslyFreedSlot) {
    // Allocate, free, reallocate — the slab should reuse the freed slot
    void* first = slab->allocate(0);
    slab->deallocate(first);
    void* second = slab->allocate(0);
    // Free list is LIFO — the most recently freed slot should be reused
    EXPECT_EQ(first, second);
    slab->deallocate(second);
}

TEST_F(NodeSlabFreeListTest, MultipleAllocationsAreUnique) {
    constexpr size_t N = 50;
    std::vector<void*> ptrs;
    ptrs.reserve(N);

    for (size_t i = 0; i < N; ++i) {
        void* p = slab->allocate(0);
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
    }

    // All pointers should be distinct
    for (size_t i = 0; i < N; ++i) {
        for (size_t j = i + 1; j < N; ++j) {
            EXPECT_NE(ptrs[i], ptrs[j]) << "Duplicate at indices " << i << " and " << j;
        }
    }

    for (void* p : ptrs) {
        slab->deallocate(p);
    }
}

TEST_F(NodeSlabFreeListTest, FreeListIntegrityAfterCycles) {
    // Allocate a batch, free in reverse, allocate again
    constexpr size_t N = 20;
    std::vector<void*> first_batch(N);
    for (size_t i = 0; i < N; ++i) {
        first_batch[i] = slab->allocate(1);  // Pool 1 (Node16)
    }

    // Free all in reverse order
    for (size_t i = N; i > 0; --i) {
        slab->deallocate(first_batch[i - 1]);
    }

    auto mid_stats = slab->get_stats();
    EXPECT_EQ(mid_stats.allocated_nodes[1], 0u);

    // Reallocate same count — all should succeed
    std::vector<void*> second_batch(N);
    for (size_t i = 0; i < N; ++i) {
        second_batch[i] = slab->allocate(1);
        ASSERT_NE(second_batch[i], nullptr);
    }

    auto after_stats = slab->get_stats();
    EXPECT_EQ(after_stats.allocated_nodes[1], N);

    for (void* p : second_batch) {
        slab->deallocate(p);
    }
}

TEST_F(NodeSlabFreeListTest, PoolsAreIndependent) {
    void* p0 = slab->allocate(0);
    void* p2 = slab->allocate(2);

    auto stats = slab->get_stats();
    EXPECT_EQ(stats.allocated_nodes[0], 1u);
    EXPECT_EQ(stats.allocated_nodes[2], 1u);
    EXPECT_EQ(stats.allocated_nodes[1], 0u);
    EXPECT_EQ(stats.allocated_nodes[3], 0u);

    slab->deallocate(p0);
    slab->deallocate(p2);
}

// =============================================================================
// Peak Usage Tracking Tests
// =============================================================================

class NodeSlabPeakTest : public ::testing::Test {
protected:
    void SetUp() override {
        slab = std::make_unique<NodeSlab>();
        set_node_slab(slab.get());
    }
    void TearDown() override {
        set_node_slab(nullptr);
        slab.reset();
    }
    std::unique_ptr<NodeSlab> slab;
};

TEST_F(NodeSlabPeakTest, PeakTracksHighWaterMark) {
    void* a = slab->allocate(0);
    void* b = slab->allocate(0);
    void* c = slab->allocate(0);
    // Peak should be 3
    EXPECT_EQ(slab->get_stats().peak_allocated[0], 3u);

    slab->deallocate(c);
    slab->deallocate(b);
    // Peak remains 3 even after freeing
    EXPECT_EQ(slab->get_stats().peak_allocated[0], 3u);
    EXPECT_EQ(slab->get_stats().allocated_nodes[0], 1u);

    slab->deallocate(a);
    EXPECT_EQ(slab->get_stats().peak_allocated[0], 3u);
    EXPECT_EQ(slab->get_stats().allocated_nodes[0], 0u);
}

TEST_F(NodeSlabPeakTest, PeakUpdatesOnNewHighWaterMark) {
    void* a = slab->allocate(0);
    void* b = slab->allocate(0);
    EXPECT_EQ(slab->get_stats().peak_allocated[0], 2u);

    slab->deallocate(a);
    slab->deallocate(b);

    // Allocate 3 this time — new peak
    void* c = slab->allocate(0);
    void* d = slab->allocate(0);
    void* e = slab->allocate(0);
    EXPECT_EQ(slab->get_stats().peak_allocated[0], 3u);

    slab->deallocate(c);
    slab->deallocate(d);
    slab->deallocate(e);
}

TEST_F(NodeSlabPeakTest, PeakTrackedPerPool) {
    void* p0 = slab->allocate(0);
    void* p1a = slab->allocate(1);
    void* p1b = slab->allocate(1);

    EXPECT_EQ(slab->get_stats().peak_allocated[0], 1u);
    EXPECT_EQ(slab->get_stats().peak_allocated[1], 2u);

    slab->deallocate(p0);
    slab->deallocate(p1a);
    slab->deallocate(p1b);
}

// =============================================================================
// Pool Exhaustion / Auto-Growth Tests
// =============================================================================

class NodeSlabGrowthTest : public ::testing::Test {
protected:
    void SetUp() override {
        slab = std::make_unique<NodeSlab>();
        set_node_slab(slab.get());
    }
    void TearDown() override {
        set_node_slab(nullptr);
        slab.reset();
    }
    std::unique_ptr<NodeSlab> slab;
};

TEST_F(NodeSlabGrowthTest, AutoGrowsWhenPoolExhausted) {
    auto initial = slab->get_stats();
    size_t initial_free = initial.free_list_size[0];

    // Exhaust all free slots in pool 0
    std::vector<void*> ptrs;
    ptrs.reserve(initial_free + 1);
    for (size_t i = 0; i < initial_free; ++i) {
        ptrs.push_back(slab->allocate(0));
    }

    auto exhausted = slab->get_stats();
    EXPECT_EQ(exhausted.free_list_size[0], 0u);

    // Next allocation triggers new chunk
    void* extra = slab->allocate(0);
    ASSERT_NE(extra, nullptr);
    ptrs.push_back(extra);

    auto grown = slab->get_stats();
    EXPECT_GT(grown.total_chunks, initial.total_chunks);
    EXPECT_GT(grown.free_list_size[0], 0u);

    for (void* p : ptrs) {
        slab->deallocate(p);
    }
}

TEST_F(NodeSlabGrowthTest, LazyChunkAllocationForNonDefaultPools) {
    auto initial = slab->get_stats();
    // Pool 3 (Node256) has no pre-allocated chunk
    EXPECT_EQ(initial.free_list_size[3], 0u);

    // First allocation from pool 3 triggers chunk allocation
    void* ptr = slab->allocate(3);
    ASSERT_NE(ptr, nullptr);

    auto after = slab->get_stats();
    EXPECT_GT(after.total_chunks, initial.total_chunks);
    EXPECT_GT(after.free_list_size[3], 0u);

    slab->deallocate(ptr);
}

TEST_F(NodeSlabGrowthTest, TotalBytesTracksChunkCount) {
    auto stats = slab->get_stats();
    EXPECT_EQ(stats.total_bytes, stats.total_chunks * SLAB_CHUNK_SIZE);
}

// =============================================================================
// make_node Factory Method Tests
// =============================================================================

class NodeSlabMakeNodeTest : public ::testing::Test {
protected:
    void SetUp() override {
        slab = std::make_unique<NodeSlab>();
        set_node_slab(slab.get());
    }
    void TearDown() override {
        set_node_slab(nullptr);
        slab.reset();
    }
    std::unique_ptr<NodeSlab> slab;
};

TEST_F(NodeSlabMakeNodeTest, MakeNode4ReturnsValidPtr) {
    auto node = slab->make_node<Node4>();
    ASSERT_NE(node.get(), nullptr);
    EXPECT_EQ(slab->get_stats().allocated_nodes[0], 1u);
}

TEST_F(NodeSlabMakeNodeTest, MakeNode16ReturnsValidPtr) {
    auto node = slab->make_node<Node16>();
    ASSERT_NE(node.get(), nullptr);
    EXPECT_EQ(slab->get_stats().allocated_nodes[1], 1u);
}

TEST_F(NodeSlabMakeNodeTest, MakeNode48ReturnsValidPtr) {
    auto node = slab->make_node<Node48>();
    ASSERT_NE(node.get(), nullptr);
    EXPECT_EQ(slab->get_stats().allocated_nodes[2], 1u);
}

TEST_F(NodeSlabMakeNodeTest, MakeNode256ReturnsValidPtr) {
    auto node = slab->make_node<Node256>();
    ASSERT_NE(node.get(), nullptr);
    EXPECT_EQ(slab->get_stats().allocated_nodes[3], 1u);
}

TEST_F(NodeSlabMakeNodeTest, CustomDeleterReturnsToPools) {
    {
        auto node = slab->make_node<Node4>();
        EXPECT_EQ(slab->get_stats().allocated_nodes[0], 1u);
    }
    // After NodePtr goes out of scope, custom deleter returns to slab
    EXPECT_EQ(slab->get_stats().allocated_nodes[0], 0u);
}

TEST_F(NodeSlabMakeNodeTest, MultipleNodeTypesCoexist) {
    auto n4 = slab->make_node<Node4>();
    auto n16 = slab->make_node<Node16>();
    auto n48 = slab->make_node<Node48>();
    auto n256 = slab->make_node<Node256>();

    auto stats = slab->get_stats();
    EXPECT_EQ(stats.allocated_nodes[0], 1u);
    EXPECT_EQ(stats.allocated_nodes[1], 1u);
    EXPECT_EQ(stats.allocated_nodes[2], 1u);
    EXPECT_EQ(stats.allocated_nodes[3], 1u);
}

// =============================================================================
// Thread-Local Slab Accessor Tests
// =============================================================================

class NodeSlabAccessorTest : public ::testing::Test {};

TEST_F(NodeSlabAccessorTest, DefaultIsNull) {
    set_node_slab(nullptr);
    EXPECT_EQ(get_node_slab(), nullptr);
}

TEST_F(NodeSlabAccessorTest, SetAndGet) {
    NodeSlab slab;
    set_node_slab(&slab);
    EXPECT_EQ(get_node_slab(), &slab);
    set_node_slab(nullptr);
}
