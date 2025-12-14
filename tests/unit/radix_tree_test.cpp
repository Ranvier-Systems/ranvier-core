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

using namespace ranvier;

class RadixTreeTest : public ::testing::Test {
protected:
    RadixTree tree;

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

    auto result = tree.lookup(tokens({}));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 1);
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
// Node Growth Tests (Node4 -> Node256)
// =============================================================================

TEST_F(RadixTreeTest, NodeGrowthBeyondFourChildren) {
    // Insert 5 routes with same first token but different second tokens
    // This should trigger Node4 -> Node256 growth
    tree.insert(tokens({1, 10}), 10);
    tree.insert(tokens({1, 20}), 20);
    tree.insert(tokens({1, 30}), 30);
    tree.insert(tokens({1, 40}), 40);
    tree.insert(tokens({1, 50}), 50);  // 5th child triggers growth

    // All should still be retrievable
    EXPECT_EQ(tree.lookup(tokens({1, 10})).value(), 10);
    EXPECT_EQ(tree.lookup(tokens({1, 20})).value(), 20);
    EXPECT_EQ(tree.lookup(tokens({1, 30})).value(), 30);
    EXPECT_EQ(tree.lookup(tokens({1, 40})).value(), 40);
    EXPECT_EQ(tree.lookup(tokens({1, 50})).value(), 50);
}

TEST_F(RadixTreeTest, ManyChildrenAfterGrowth) {
    // Insert many routes to stress test node growth
    for (int i = 0; i < 20; i++) {
        tree.insert(tokens({1, static_cast<TokenId>(i * 10)}), static_cast<BackendId>(i));
    }

    // Verify all are retrievable
    for (int i = 0; i < 20; i++) {
        auto result = tree.lookup(tokens({1, static_cast<TokenId>(i * 10)}));
        ASSERT_TRUE(result.has_value()) << "Failed for i=" << i;
        EXPECT_EQ(result.value(), static_cast<BackendId>(i));
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
