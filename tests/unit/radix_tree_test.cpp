/**
 * Unit tests for RadixTree
 *
 * Tests the Adaptive Radix Tree implementation used for token prefix routing.
 * The RadixTree supports:
 * - O(k) insert/lookup where k is prefix length
 * - Path compression (storing multiple tokens in node prefix)
 * - Longest prefix matching
 * - Node growth (Node4 -> Node256)
 */

#include <gtest/gtest.h>
#include "radix_tree.hpp"

#include <vector>
#include <optional>
#include <thread>
#include <chrono>

using namespace ranvier;

class RadixTreeTest : public ::testing::Test {
protected:
    // Use block_alignment=1 to disable alignment for existing tests
    RadixTree tree{1};

    // Helper to create token vectors
    std::vector<TokenId> tokens(std::initializer_list<TokenId> list) {
        return std::vector<TokenId>(list);
    }
};

// =============================================================================
// Basic Insert and Lookup Tests
// =============================================================================

TEST_F(RadixTreeTest, EmptyTreeLookupReturnsNullopt) {
    auto result = tree.lookup(tokens({1, 2, 3}));
    EXPECT_FALSE(result.has_value());
}

TEST_F(RadixTreeTest, InsertAndLookupSingleToken) {
    tree.insert(tokens({42}), 1);

    auto result = tree.lookup(tokens({42}));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 1);
}

TEST_F(RadixTreeTest, InsertAndLookupMultipleTokens) {
    tree.insert(tokens({1, 2, 3, 4, 5}), 99);

    auto result = tree.lookup(tokens({1, 2, 3, 4, 5}));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 99);
}

TEST_F(RadixTreeTest, LookupNonexistentKeyReturnsNullopt) {
    tree.insert(tokens({1, 2, 3}), 1);

    auto result = tree.lookup(tokens({4, 5, 6}));
    EXPECT_FALSE(result.has_value());
}

TEST_F(RadixTreeTest, InsertEmptyTokenSequence) {
    tree.insert(tokens({}), 1);

    // Empty sequence should not create a route (no tokens to match)
    auto result = tree.lookup(tokens({}));
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// Prefix Matching Tests (Longest Prefix Match)
// =============================================================================

TEST_F(RadixTreeTest, LongestPrefixMatchReturnsCorrectBackend) {
    // Insert a short prefix
    tree.insert(tokens({1, 2}), 10);
    // Insert a longer prefix
    tree.insert(tokens({1, 2, 3, 4}), 20);

    // Lookup with exact match to longer prefix
    auto result = tree.lookup(tokens({1, 2, 3, 4}));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 20);
}

TEST_F(RadixTreeTest, LongestPrefixMatchFallsBackToShorterPrefix) {
    // Insert a prefix
    tree.insert(tokens({1, 2}), 10);

    // Lookup with longer sequence should match the prefix
    auto result = tree.lookup(tokens({1, 2, 3, 4, 5}));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 10);
}

TEST_F(RadixTreeTest, PartialPrefixMatchReturnsNullopt) {
    // Insert "Apple" (tokens 1,2,3,4,5)
    tree.insert(tokens({1, 2, 3, 4, 5}), 1);

    // Lookup "App" should NOT match (input shorter than stored prefix)
    auto result = tree.lookup(tokens({1, 2, 3}));
    EXPECT_FALSE(result.has_value());
}

TEST_F(RadixTreeTest, DivergentPrefixReturnsNullopt) {
    // Insert tokens {1, 2, 3}
    tree.insert(tokens({1, 2, 3}), 1);

    // Lookup {1, 2, 99} - diverges at position 2
    auto result = tree.lookup(tokens({1, 2, 99}));
    EXPECT_FALSE(result.has_value());
}

// =============================================================================
// Node Splitting Tests
// =============================================================================

TEST_F(RadixTreeTest, NodeSplitOnDivergentInsert) {
    // Insert "Apple" -> backend 1
    tree.insert(tokens({1, 2, 3, 4, 5}), 1);  // A-p-p-l-e

    // Insert "Apply" -> backend 2 (diverges at position 4)
    tree.insert(tokens({1, 2, 3, 4, 99}), 2); // A-p-p-l-y

    // Both should be retrievable
    auto apple = tree.lookup(tokens({1, 2, 3, 4, 5}));
    ASSERT_TRUE(apple.has_value());
    EXPECT_EQ(apple.value(), 1);

    auto apply = tree.lookup(tokens({1, 2, 3, 4, 99}));
    ASSERT_TRUE(apply.has_value());
    EXPECT_EQ(apply.value(), 2);
}

TEST_F(RadixTreeTest, NodeSplitAtBeginning) {
    // Insert {1, 2, 3}
    tree.insert(tokens({1, 2, 3}), 1);

    // Insert {99, 2, 3} - diverges at first token
    tree.insert(tokens({99, 2, 3}), 2);

    auto first = tree.lookup(tokens({1, 2, 3}));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first.value(), 1);

    auto second = tree.lookup(tokens({99, 2, 3}));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second.value(), 2);
}

TEST_F(RadixTreeTest, InsertShorterPrefixAfterLonger) {
    // Insert longer first
    tree.insert(tokens({1, 2, 3, 4, 5}), 1);

    // Insert shorter prefix (causes split)
    tree.insert(tokens({1, 2}), 2);

    // Both should work
    auto longer = tree.lookup(tokens({1, 2, 3, 4, 5}));
    ASSERT_TRUE(longer.has_value());
    EXPECT_EQ(longer.value(), 1);

    auto shorter = tree.lookup(tokens({1, 2}));
    ASSERT_TRUE(shorter.has_value());
    EXPECT_EQ(shorter.value(), 2);
}

// =============================================================================
// Multiple Routes Tests
// =============================================================================

TEST_F(RadixTreeTest, MultipleDistinctRoutes) {
    tree.insert(tokens({1, 2, 3}), 1);
    tree.insert(tokens({4, 5, 6}), 2);
    tree.insert(tokens({7, 8, 9}), 3);

    EXPECT_EQ(tree.lookup(tokens({1, 2, 3})).value(), 1);
    EXPECT_EQ(tree.lookup(tokens({4, 5, 6})).value(), 2);
    EXPECT_EQ(tree.lookup(tokens({7, 8, 9})).value(), 3);
}

TEST_F(RadixTreeTest, OverwriteExistingRoute) {
    tree.insert(tokens({1, 2, 3}), 1);
    tree.insert(tokens({1, 2, 3}), 99);  // Overwrite

    auto result = tree.lookup(tokens({1, 2, 3}));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 99);
}

TEST_F(RadixTreeTest, CommonPrefixWithDifferentSuffixes) {
    // All share prefix {1, 2}
    tree.insert(tokens({1, 2, 10}), 10);
    tree.insert(tokens({1, 2, 20}), 20);
    tree.insert(tokens({1, 2, 30}), 30);

    EXPECT_EQ(tree.lookup(tokens({1, 2, 10})).value(), 10);
    EXPECT_EQ(tree.lookup(tokens({1, 2, 20})).value(), 20);
    EXPECT_EQ(tree.lookup(tokens({1, 2, 30})).value(), 30);
}

// =============================================================================
// Node Growth Tests (Node4 -> Node16 -> Node48 -> Node256)
// =============================================================================

TEST_F(RadixTreeTest, Node4ToNode16Growth) {
    // Insert 5 routes - triggers Node4 -> Node16 growth
    tree.insert(tokens({1, 10}), 10);
    tree.insert(tokens({1, 20}), 20);
    tree.insert(tokens({1, 30}), 30);
    tree.insert(tokens({1, 40}), 40);
    tree.insert(tokens({1, 50}), 50);  // 5th child triggers growth to Node16

    // All should still be retrievable
    EXPECT_EQ(tree.lookup(tokens({1, 10})).value(), 10);
    EXPECT_EQ(tree.lookup(tokens({1, 20})).value(), 20);
    EXPECT_EQ(tree.lookup(tokens({1, 30})).value(), 30);
    EXPECT_EQ(tree.lookup(tokens({1, 40})).value(), 40);
    EXPECT_EQ(tree.lookup(tokens({1, 50})).value(), 50);
}

TEST_F(RadixTreeTest, Node16ToNode48Growth) {
    // Insert 17 routes - triggers Node4 -> Node16 -> Node48 growth
    for (int i = 0; i < 17; i++) {
        tree.insert(tokens({1, static_cast<TokenId>(i)}), static_cast<BackendId>(i));
    }

    // Verify all are retrievable
    for (int i = 0; i < 17; i++) {
        auto result = tree.lookup(tokens({1, static_cast<TokenId>(i)}));
        ASSERT_TRUE(result.has_value()) << "Failed for i=" << i;
        EXPECT_EQ(result.value(), static_cast<BackendId>(i));
    }
}

TEST_F(RadixTreeTest, Node48ToNode256Growth) {
    // Insert 49 routes - triggers full growth chain to Node256
    for (int i = 0; i < 49; i++) {
        tree.insert(tokens({1, static_cast<TokenId>(i)}), static_cast<BackendId>(i % 128));
    }

    // Verify all are retrievable
    for (int i = 0; i < 49; i++) {
        auto result = tree.lookup(tokens({1, static_cast<TokenId>(i)}));
        ASSERT_TRUE(result.has_value()) << "Failed for i=" << i;
        EXPECT_EQ(result.value(), static_cast<BackendId>(i % 128));
    }
}

TEST_F(RadixTreeTest, FullNode256) {
    // Insert 100 routes to stress Node256
    for (int i = 0; i < 100; i++) {
        tree.insert(tokens({1, static_cast<TokenId>(i)}), static_cast<BackendId>(i % 128));
    }

    // Verify all are retrievable
    for (int i = 0; i < 100; i++) {
        auto result = tree.lookup(tokens({1, static_cast<TokenId>(i)}));
        ASSERT_TRUE(result.has_value()) << "Failed for i=" << i;
        EXPECT_EQ(result.value(), static_cast<BackendId>(i % 128));
    }
}

TEST_F(RadixTreeTest, MixedDepthWithNodeGrowth) {
    // Test node growth at different tree depths
    // First level: many children (triggers growth)
    for (int i = 0; i < 20; i++) {
        tree.insert(tokens({static_cast<TokenId>(i), 1, 2}), static_cast<BackendId>(i));
    }

    // Second level: add more children under one branch
    for (int i = 0; i < 20; i++) {
        tree.insert(tokens({0, static_cast<TokenId>(i + 100), 2}), static_cast<BackendId>(i + 50));
    }

    // Verify first level routes
    for (int i = 0; i < 20; i++) {
        auto result = tree.lookup(tokens({static_cast<TokenId>(i), 1, 2}));
        ASSERT_TRUE(result.has_value()) << "First level failed for i=" << i;
        EXPECT_EQ(result.value(), static_cast<BackendId>(i));
    }

    // Verify second level routes
    for (int i = 0; i < 20; i++) {
        auto result = tree.lookup(tokens({0, static_cast<TokenId>(i + 100), 2}));
        ASSERT_TRUE(result.has_value()) << "Second level failed for i=" << i;
        EXPECT_EQ(result.value(), static_cast<BackendId>(i + 50));
    }
}

TEST_F(RadixTreeTest, NodeGrowthWithChildUpdates) {
    // Insert routes, trigger growth, then update children
    for (int i = 0; i < 20; i++) {
        tree.insert(tokens({1, static_cast<TokenId>(i * 10)}), static_cast<BackendId>(i));
    }

    // Now insert deeper routes that require updating child pointers
    for (int i = 0; i < 20; i++) {
        // Add longer routes under existing prefixes
        tree.insert(tokens({1, static_cast<TokenId>(i * 10), 99, 100}), static_cast<BackendId>(i + 100));
    }

    // Verify shorter routes still work (longest prefix match)
    for (int i = 0; i < 20; i++) {
        auto result = tree.lookup(tokens({1, static_cast<TokenId>(i * 10)}));
        ASSERT_TRUE(result.has_value()) << "Short route failed for i=" << i;
        EXPECT_EQ(result.value(), static_cast<BackendId>(i));
    }

    // Verify longer routes work
    for (int i = 0; i < 20; i++) {
        auto result = tree.lookup(tokens({1, static_cast<TokenId>(i * 10), 99, 100}));
        ASSERT_TRUE(result.has_value()) << "Long route failed for i=" << i;
        EXPECT_EQ(result.value(), static_cast<BackendId>(i + 100));
    }
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(RadixTreeTest, SingleTokenDifferentBackends) {
    // Different single tokens map to different backends
    tree.insert(tokens({1}), 1);
    tree.insert(tokens({2}), 2);
    tree.insert(tokens({3}), 3);

    EXPECT_EQ(tree.lookup(tokens({1})).value(), 1);
    EXPECT_EQ(tree.lookup(tokens({2})).value(), 2);
    EXPECT_EQ(tree.lookup(tokens({3})).value(), 3);
}

TEST_F(RadixTreeTest, LongTokenSequence) {
    // Test with a long sequence (simulating a long system prompt)
    std::vector<TokenId> long_seq;
    for (int i = 0; i < 1000; i++) {
        long_seq.push_back(static_cast<TokenId>(i));
    }

    tree.insert(long_seq, 42);

    auto result = tree.lookup(long_seq);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 42);
}

TEST_F(RadixTreeTest, LongSequenceWithPrefixMatch) {
    // Insert long sequence
    std::vector<TokenId> long_seq;
    for (int i = 0; i < 100; i++) {
        long_seq.push_back(static_cast<TokenId>(i));
    }
    tree.insert(long_seq, 42);

    // Lookup with even longer sequence should match
    std::vector<TokenId> longer_seq = long_seq;
    longer_seq.push_back(999);
    longer_seq.push_back(888);

    auto result = tree.lookup(longer_seq);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 42);
}

TEST_F(RadixTreeTest, NegativeTokenIds) {
    // Token IDs can be negative (depending on tokenizer)
    tree.insert(tokens({-1, -2, -3}), 1);

    auto result = tree.lookup(tokens({-1, -2, -3}));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 1);
}

TEST_F(RadixTreeTest, MaxValueTokenIds) {
    // Test with max int32 values
    tree.insert(tokens({INT32_MAX, INT32_MIN, 0}), 1);

    auto result = tree.lookup(tokens({INT32_MAX, INT32_MIN, 0}));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 1);
}

// =============================================================================
// Hierarchical Prefix Tests (Real-world scenario)
// =============================================================================

TEST_F(RadixTreeTest, HierarchicalPrefixRouting) {
    // Simulate system prompts with shared prefixes
    // "You are a helpful assistant" -> GPU 1
    tree.insert(tokens({100, 200, 300, 400, 500}), 1);

    // "You are a helpful assistant. Help with code" -> GPU 2
    tree.insert(tokens({100, 200, 300, 400, 500, 600, 700}), 2);

    // "You are a helpful assistant. Help with math" -> GPU 3
    tree.insert(tokens({100, 200, 300, 400, 500, 600, 800}), 3);

    // Query with just the base prompt
    auto base = tree.lookup(tokens({100, 200, 300, 400, 500}));
    ASSERT_TRUE(base.has_value());
    EXPECT_EQ(base.value(), 1);

    // Query with code extension
    auto code = tree.lookup(tokens({100, 200, 300, 400, 500, 600, 700}));
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(code.value(), 2);

    // Query with math extension
    auto math = tree.lookup(tokens({100, 200, 300, 400, 500, 600, 800}));
    ASSERT_TRUE(math.has_value());
    EXPECT_EQ(math.value(), 3);

    // Query with unknown extension should fall back to longest known prefix
    auto unknown = tree.lookup(tokens({100, 200, 300, 400, 500, 600, 999}));
    ASSERT_TRUE(unknown.has_value());
    EXPECT_EQ(unknown.value(), 1);  // Falls back to base prompt's backend
}

// =============================================================================
// Span Interface Tests
// =============================================================================

TEST_F(RadixTreeTest, InsertWithSpan) {
    std::vector<TokenId> vec = {1, 2, 3, 4, 5};
    std::span<const TokenId> span_view(vec);

    tree.insert(span_view, 42);

    auto result = tree.lookup(span_view);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 42);
}

TEST_F(RadixTreeTest, LookupWithSubspan) {
    std::vector<TokenId> vec = {1, 2, 3, 4, 5};
    tree.insert(vec, 42);

    // Lookup using a subspan of a larger vector
    std::vector<TokenId> larger = {1, 2, 3, 4, 5, 6, 7, 8};
    std::span<const TokenId> subspan(larger.data(), 5);

    auto result = tree.lookup(subspan);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 42);
}

// =============================================================================
// Block Alignment Tests (vLLM PagedAttention compatibility)
// =============================================================================

class RadixTreeBlockAlignmentTest : public ::testing::Test {
protected:
    // Helper to create token vectors
    std::vector<TokenId> tokens(size_t count, TokenId start = 1) {
        std::vector<TokenId> result;
        result.reserve(count);
        for (size_t i = 0; i < count; i++) {
            result.push_back(start + static_cast<TokenId>(i));
        }
        return result;
    }
};

TEST_F(RadixTreeBlockAlignmentTest, DefaultBlockAlignmentIs16) {
    RadixTree tree;  // Default block_alignment = 16

    // Insert 16 tokens - should succeed (exactly one block)
    auto vec16 = tokens(16);
    tree.insert(vec16, 1);

    auto result = tree.lookup(vec16);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 1);
}

TEST_F(RadixTreeBlockAlignmentTest, TokensBelowBlockAlignmentNotInserted) {
    RadixTree tree;  // Default block_alignment = 16

    // Insert fewer than 16 tokens - should be skipped
    auto vec10 = tokens(10);
    tree.insert(vec10, 1);

    // Lookup should return nullopt since nothing was inserted
    auto result = tree.lookup(vec10);
    EXPECT_FALSE(result.has_value());
}

TEST_F(RadixTreeBlockAlignmentTest, TokensTruncatedToBlockBoundary) {
    RadixTree tree;  // Default block_alignment = 16

    // Insert 20 tokens - should truncate to 16
    auto vec20 = tokens(20);
    tree.insert(vec20, 42);

    // Lookup with exactly 16 tokens should find it
    auto vec16 = tokens(16);
    auto result16 = tree.lookup(vec16);
    ASSERT_TRUE(result16.has_value());
    EXPECT_EQ(result16.value(), 42);

    // Lookup with 20 tokens should also match (longest prefix)
    auto result20 = tree.lookup(vec20);
    ASSERT_TRUE(result20.has_value());
    EXPECT_EQ(result20.value(), 42);

    // Lookup with 17 tokens should match at 16
    auto vec17 = tokens(17);
    auto result17 = tree.lookup(vec17);
    ASSERT_TRUE(result17.has_value());
    EXPECT_EQ(result17.value(), 42);
}

TEST_F(RadixTreeBlockAlignmentTest, MultipleBlocksInserted) {
    RadixTree tree;  // Default block_alignment = 16

    // Insert 32 tokens (exactly 2 blocks)
    auto vec32 = tokens(32);
    tree.insert(vec32, 1);

    // Should be inserted at full 32 length
    auto result = tree.lookup(vec32);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 1);
}

TEST_F(RadixTreeBlockAlignmentTest, TokensTruncatedToMultipleBlocks) {
    RadixTree tree;  // Default block_alignment = 16

    // Insert 47 tokens - should truncate to 32 (2 full blocks)
    auto vec47 = tokens(47);
    tree.insert(vec47, 99);

    // Lookup at 32 should find it
    auto vec32 = tokens(32);
    auto result32 = tree.lookup(vec32);
    ASSERT_TRUE(result32.has_value());
    EXPECT_EQ(result32.value(), 99);

    // Lookup at 33-47 should also match via longest prefix
    auto result47 = tree.lookup(vec47);
    ASSERT_TRUE(result47.has_value());
    EXPECT_EQ(result47.value(), 99);

    // Lookup at 48 (different tokens beyond 47) should still match at 32
    auto vec48 = tokens(48);
    auto result48 = tree.lookup(vec48);
    ASSERT_TRUE(result48.has_value());
    EXPECT_EQ(result48.value(), 99);
}

TEST_F(RadixTreeBlockAlignmentTest, CustomBlockAlignment) {
    RadixTree tree(8);  // Custom block_alignment = 8

    // Insert 10 tokens - should truncate to 8
    auto vec10 = tokens(10);
    tree.insert(vec10, 1);

    // Lookup at 8 should find it
    auto vec8 = tokens(8);
    auto result = tree.lookup(vec8);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 1);
}

TEST_F(RadixTreeBlockAlignmentTest, BlockAlignmentOf1DisablesAlignment) {
    RadixTree tree(1);  // block_alignment = 1 effectively disables alignment

    // Any length should work
    auto vec3 = tokens(3);
    tree.insert(vec3, 1);

    auto result = tree.lookup(vec3);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 1);
}

TEST_F(RadixTreeBlockAlignmentTest, MultipleRoutesWithAlignment) {
    RadixTree tree;  // Default block_alignment = 16

    // Insert routes at different aligned lengths
    auto vec16 = tokens(16, 1);
    auto vec32 = tokens(32, 1);
    auto vec48 = tokens(48, 1);

    tree.insert(vec16, 1);
    tree.insert(vec32, 2);
    tree.insert(vec48, 3);

    // Each should be found at exact length
    EXPECT_EQ(tree.lookup(vec16).value(), 1);
    EXPECT_EQ(tree.lookup(vec32).value(), 2);
    EXPECT_EQ(tree.lookup(vec48).value(), 3);

    // Lookup with 20 tokens should match at 16
    auto vec20 = tokens(20, 1);
    EXPECT_EQ(tree.lookup(vec20).value(), 1);

    // Lookup with 40 tokens should match at 32
    auto vec40 = tokens(40, 1);
    EXPECT_EQ(tree.lookup(vec40).value(), 2);
}

TEST_F(RadixTreeBlockAlignmentTest, OverwriteAtAlignedBoundary) {
    RadixTree tree;  // Default block_alignment = 16

    // Insert 20 tokens -> stored at 16
    auto vec20 = tokens(20);
    tree.insert(vec20, 1);

    // Insert 18 tokens -> also stored at 16 (overwrites)
    auto vec18 = tokens(18);
    tree.insert(vec18, 99);

    // Both truncate to 16, so lookup at 16 should return 99
    auto vec16 = tokens(16);
    auto result = tree.lookup(vec16);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 99);
}

TEST_F(RadixTreeBlockAlignmentTest, EmptyInsertWithAlignment) {
    RadixTree tree;  // Default block_alignment = 16

    // Empty token sequence should not insert anything
    std::vector<TokenId> empty;
    tree.insert(empty, 1);

    auto result = tree.lookup(empty);
    EXPECT_FALSE(result.has_value());
}

TEST_F(RadixTreeBlockAlignmentTest, LargeBlockAlignment) {
    RadixTree tree(64);  // Large block alignment

    // 63 tokens should not insert
    auto vec63 = tokens(63);
    tree.insert(vec63, 1);
    EXPECT_FALSE(tree.lookup(vec63).has_value());

    // 64 tokens should insert
    auto vec64 = tokens(64);
    tree.insert(vec64, 2);
    ASSERT_TRUE(tree.lookup(vec64).has_value());
    EXPECT_EQ(tree.lookup(vec64).value(), 2);

    // 100 tokens should truncate to 64
    auto vec100 = tokens(100);
    tree.insert(vec100, 3);
    auto result = tree.lookup(vec64);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 3);  // Overwrote previous value
}

// =============================================================================
// LRU Infrastructure Tests (route_count, for_each_leaf, TTL, eviction)
// =============================================================================

class RadixTreeLRUTest : public ::testing::Test {
protected:
    // Use block_alignment=1 to disable alignment for cleaner tests
    RadixTree tree{1};

    // Helper to create token vectors
    std::vector<TokenId> tokens(std::initializer_list<TokenId> list) {
        return std::vector<TokenId>(list);
    }

    std::vector<TokenId> tokens(size_t count, TokenId start = 1) {
        std::vector<TokenId> result;
        result.reserve(count);
        for (size_t i = 0; i < count; i++) {
            result.push_back(start + static_cast<TokenId>(i));
        }
        return result;
    }
};

// -----------------------------------------------------------------------------
// route_count() Tests
// -----------------------------------------------------------------------------

TEST_F(RadixTreeLRUTest, RouteCountStartsAtZero) {
    EXPECT_EQ(tree.route_count(), 0u);
}

TEST_F(RadixTreeLRUTest, RouteCountIncrementsOnInsert) {
    tree.insert(tokens({1, 2, 3}), 1);
    EXPECT_EQ(tree.route_count(), 1u);

    tree.insert(tokens({4, 5, 6}), 2);
    EXPECT_EQ(tree.route_count(), 2u);

    tree.insert(tokens({7, 8, 9}), 3);
    EXPECT_EQ(tree.route_count(), 3u);
}

TEST_F(RadixTreeLRUTest, RouteCountIncrementsOnOverwrite) {
    // Note: route_count is approximate - it doesn't detect overwrites
    tree.insert(tokens({1, 2, 3}), 1);
    EXPECT_EQ(tree.route_count(), 1u);

    tree.insert(tokens({1, 2, 3}), 99);  // Overwrite
    // Count increases even on overwrite (documented as approximate)
    EXPECT_EQ(tree.route_count(), 2u);
}

TEST_F(RadixTreeLRUTest, RouteCountManyRoutes) {
    for (int i = 0; i < 100; i++) {
        tree.insert(tokens({static_cast<TokenId>(i), 1, 2}), static_cast<BackendId>(i));
    }
    EXPECT_EQ(tree.route_count(), 100u);
}

// -----------------------------------------------------------------------------
// for_each_leaf() Tests
// -----------------------------------------------------------------------------

TEST_F(RadixTreeLRUTest, ForEachLeafEmptyTree) {
    int count = 0;
    tree.for_each_leaf([&](const RouteEntry& entry) {
        count++;
    });
    EXPECT_EQ(count, 0);
}

TEST_F(RadixTreeLRUTest, ForEachLeafSingleRoute) {
    tree.insert(tokens({1, 2, 3}), 42);

    std::vector<RouteEntry> entries;
    tree.for_each_leaf([&](const RouteEntry& entry) {
        entries.push_back(entry);
    });

    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].backend, 42);
    EXPECT_EQ(entries[0].tokens, tokens({1, 2, 3}));
}

TEST_F(RadixTreeLRUTest, ForEachLeafMultipleRoutes) {
    tree.insert(tokens({1, 2, 3}), 1);
    tree.insert(tokens({4, 5, 6}), 2);
    tree.insert(tokens({7, 8, 9}), 3);

    std::vector<BackendId> backends;
    tree.for_each_leaf([&](const RouteEntry& entry) {
        backends.push_back(entry.backend);
    });

    EXPECT_EQ(backends.size(), 3u);
    // Sort to check all backends are present (traversal order may vary)
    std::sort(backends.begin(), backends.end());
    EXPECT_EQ(backends, (std::vector<BackendId>{1, 2, 3}));
}

TEST_F(RadixTreeLRUTest, ForEachLeafWithSharedPrefix) {
    // Routes with shared prefix
    tree.insert(tokens({1, 2, 10}), 10);
    tree.insert(tokens({1, 2, 20}), 20);
    tree.insert(tokens({1, 2, 30}), 30);

    std::vector<BackendId> backends;
    tree.for_each_leaf([&](const RouteEntry& entry) {
        backends.push_back(entry.backend);
    });

    EXPECT_EQ(backends.size(), 3u);
    std::sort(backends.begin(), backends.end());
    EXPECT_EQ(backends, (std::vector<BackendId>{10, 20, 30}));
}

TEST_F(RadixTreeLRUTest, ForEachLeafHierarchicalRoutes) {
    // Hierarchical routes (short and long prefixes)
    tree.insert(tokens({1, 2}), 1);
    tree.insert(tokens({1, 2, 3, 4}), 2);

    std::vector<RouteEntry> entries;
    tree.for_each_leaf([&](const RouteEntry& entry) {
        entries.push_back(entry);
    });

    EXPECT_EQ(entries.size(), 2u);
}

// -----------------------------------------------------------------------------
// last_accessed Timestamp Tests
// -----------------------------------------------------------------------------

TEST_F(RadixTreeLRUTest, LastAccessedUpdatedOnLookup) {
    tree.insert(tokens({1, 2, 3}), 1);

    // Get initial timestamp
    std::chrono::steady_clock::time_point initial_time;
    tree.for_each_leaf([&](const RouteEntry& entry) {
        initial_time = entry.last_accessed;
    });

    // Small delay to ensure time difference
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Perform lookup
    tree.lookup(tokens({1, 2, 3}));

    // Check timestamp was updated
    std::chrono::steady_clock::time_point after_lookup;
    tree.for_each_leaf([&](const RouteEntry& entry) {
        after_lookup = entry.last_accessed;
    });

    EXPECT_GT(after_lookup, initial_time);
}

TEST_F(RadixTreeLRUTest, LastAccessedUpdatedOnLongestPrefixMatch) {
    tree.insert(tokens({1, 2}), 1);

    // Get initial timestamp
    std::chrono::steady_clock::time_point initial_time;
    tree.for_each_leaf([&](const RouteEntry& entry) {
        initial_time = entry.last_accessed;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Lookup with longer sequence (triggers longest prefix match)
    tree.lookup(tokens({1, 2, 3, 4, 5}));

    // Check timestamp was updated
    std::chrono::steady_clock::time_point after_lookup;
    tree.for_each_leaf([&](const RouteEntry& entry) {
        after_lookup = entry.last_accessed;
    });

    EXPECT_GT(after_lookup, initial_time);
}

// -----------------------------------------------------------------------------
// remove_expired() Tests
// -----------------------------------------------------------------------------

TEST_F(RadixTreeLRUTest, RemoveExpiredNoExpiredRoutes) {
    tree.insert(tokens({1, 2, 3}), 1);
    tree.insert(tokens({4, 5, 6}), 2);

    // Cutoff in the past - nothing should be expired
    auto past = std::chrono::steady_clock::now() - std::chrono::hours(1);
    size_t removed = tree.remove_expired(past);

    EXPECT_EQ(removed, 0u);
    EXPECT_EQ(tree.route_count(), 2u);
}

TEST_F(RadixTreeLRUTest, RemoveExpiredAllExpired) {
    tree.insert(tokens({1, 2, 3}), 1);
    tree.insert(tokens({4, 5, 6}), 2);

    // Cutoff in the future - all should be expired
    auto future = std::chrono::steady_clock::now() + std::chrono::hours(1);
    size_t removed = tree.remove_expired(future);

    EXPECT_EQ(removed, 2u);
    EXPECT_EQ(tree.route_count(), 0u);

    // Verify routes are gone
    EXPECT_FALSE(tree.lookup(tokens({1, 2, 3})).has_value());
    EXPECT_FALSE(tree.lookup(tokens({4, 5, 6})).has_value());
}

TEST_F(RadixTreeLRUTest, RemoveExpiredPartialExpiration) {
    tree.insert(tokens({1, 2, 3}), 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto middle = std::chrono::steady_clock::now();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    tree.insert(tokens({4, 5, 6}), 2);

    // Only the first route should be expired
    size_t removed = tree.remove_expired(middle);

    EXPECT_EQ(removed, 1u);
    EXPECT_EQ(tree.route_count(), 1u);

    // First route should be gone, second should remain
    EXPECT_FALSE(tree.lookup(tokens({1, 2, 3})).has_value());
    EXPECT_TRUE(tree.lookup(tokens({4, 5, 6})).has_value());
}

TEST_F(RadixTreeLRUTest, RemoveExpiredUpdatesRouteCount) {
    for (int i = 0; i < 10; i++) {
        tree.insert(tokens({static_cast<TokenId>(i), 1, 2}), static_cast<BackendId>(i));
    }
    EXPECT_EQ(tree.route_count(), 10u);

    // Expire all
    auto future = std::chrono::steady_clock::now() + std::chrono::hours(1);
    size_t removed = tree.remove_expired(future);

    EXPECT_EQ(removed, 10u);
    EXPECT_EQ(tree.route_count(), 0u);
}

// -----------------------------------------------------------------------------
// evict_oldest() Tests
// -----------------------------------------------------------------------------

TEST_F(RadixTreeLRUTest, EvictOldestEmptyTree) {
    bool evicted = tree.evict_oldest();
    EXPECT_FALSE(evicted);
}

TEST_F(RadixTreeLRUTest, EvictOldestSingleRoute) {
    tree.insert(tokens({1, 2, 3}), 1);
    EXPECT_EQ(tree.route_count(), 1u);

    bool evicted = tree.evict_oldest();

    EXPECT_TRUE(evicted);
    EXPECT_EQ(tree.route_count(), 0u);
    EXPECT_FALSE(tree.lookup(tokens({1, 2, 3})).has_value());
}

TEST_F(RadixTreeLRUTest, EvictOldestEvictsOldest) {
    tree.insert(tokens({1, 2, 3}), 1);  // Oldest

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    tree.insert(tokens({4, 5, 6}), 2);  // Newer

    EXPECT_EQ(tree.route_count(), 2u);

    bool evicted = tree.evict_oldest();

    EXPECT_TRUE(evicted);
    EXPECT_EQ(tree.route_count(), 1u);

    // Oldest (backend 1) should be evicted, newer (backend 2) should remain
    EXPECT_FALSE(tree.lookup(tokens({1, 2, 3})).has_value());
    EXPECT_TRUE(tree.lookup(tokens({4, 5, 6})).has_value());
}

TEST_F(RadixTreeLRUTest, EvictOldestUpdatesRouteCount) {
    tree.insert(tokens({1, 2, 3}), 1);
    tree.insert(tokens({4, 5, 6}), 2);
    tree.insert(tokens({7, 8, 9}), 3);
    EXPECT_EQ(tree.route_count(), 3u);

    tree.evict_oldest();
    EXPECT_EQ(tree.route_count(), 2u);

    tree.evict_oldest();
    EXPECT_EQ(tree.route_count(), 1u);

    tree.evict_oldest();
    EXPECT_EQ(tree.route_count(), 0u);

    // No more to evict
    bool evicted = tree.evict_oldest();
    EXPECT_FALSE(evicted);
    EXPECT_EQ(tree.route_count(), 0u);
}

TEST_F(RadixTreeLRUTest, EvictOldestRespectsLookupUpdates) {
    // Insert three routes with delays
    tree.insert(tokens({1, 2, 3}), 1);  // Initially oldest
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    tree.insert(tokens({4, 5, 6}), 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    tree.insert(tokens({7, 8, 9}), 3);  // Initially newest

    // Access the oldest route to make it newest
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    tree.lookup(tokens({1, 2, 3}));

    // Now route 2 should be the oldest
    tree.evict_oldest();

    // Route 2 should be evicted, routes 1 and 3 should remain
    EXPECT_TRUE(tree.lookup(tokens({1, 2, 3})).has_value());
    EXPECT_FALSE(tree.lookup(tokens({4, 5, 6})).has_value());
    EXPECT_TRUE(tree.lookup(tokens({7, 8, 9})).has_value());
}

TEST_F(RadixTreeLRUTest, EvictOldestWithManyRoutes) {
    // Insert 100 routes
    for (int i = 0; i < 100; i++) {
        tree.insert(tokens({static_cast<TokenId>(i), 1, 2}), static_cast<BackendId>(i));
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    EXPECT_EQ(tree.route_count(), 100u);

    // Evict 50 routes
    for (int i = 0; i < 50; i++) {
        EXPECT_TRUE(tree.evict_oldest());
    }

    EXPECT_EQ(tree.route_count(), 50u);
}

// -----------------------------------------------------------------------------
// Combined LRU Operations Tests
// -----------------------------------------------------------------------------

TEST_F(RadixTreeLRUTest, CombinedExpireAndEvict) {
    // Insert routes at different times
    tree.insert(tokens({1, 2, 3}), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    tree.insert(tokens({4, 5, 6}), 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto cutoff = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    tree.insert(tokens({7, 8, 9}), 3);

    EXPECT_EQ(tree.route_count(), 3u);

    // Expire old routes
    size_t expired = tree.remove_expired(cutoff);
    EXPECT_EQ(expired, 2u);
    EXPECT_EQ(tree.route_count(), 1u);

    // Only newest route should remain
    EXPECT_FALSE(tree.lookup(tokens({1, 2, 3})).has_value());
    EXPECT_FALSE(tree.lookup(tokens({4, 5, 6})).has_value());
    EXPECT_TRUE(tree.lookup(tokens({7, 8, 9})).has_value());

    // Evict the remaining route
    EXPECT_TRUE(tree.evict_oldest());
    EXPECT_EQ(tree.route_count(), 0u);
}

TEST_F(RadixTreeLRUTest, RouteEntryContainsCorrectData) {
    tree.insert(tokens({10, 20, 30}), 42);

    tree.for_each_leaf([&](const RouteEntry& entry) {
        EXPECT_EQ(entry.backend, 42);
        EXPECT_EQ(entry.tokens.size(), 3u);
        EXPECT_EQ(entry.tokens[0], 10);
        EXPECT_EQ(entry.tokens[1], 20);
        EXPECT_EQ(entry.tokens[2], 30);
        // Timestamp should be recent
        auto now = std::chrono::steady_clock::now();
        auto age = now - entry.last_accessed;
        EXPECT_LT(age, std::chrono::seconds(1));
    });
}
