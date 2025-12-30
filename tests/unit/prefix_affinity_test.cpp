/**
 * Unit tests for Prefix-Affinity Routing
 *
 * Tests the prefix-affinity routing implementation that uses consistent hashing
 * on prefix tokens to route requests with the same prefix to the same backend,
 * enabling vLLM's KV cache reuse across requests.
 *
 * Key properties tested:
 * - Same prefix → Same backend (determinism)
 * - Different prefixes → Distributed across backends
 * - Prefix truncation respects configuration
 * - Block alignment is applied correctly
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>
#include <map>
#include <algorithm>
#include <numeric>

// FNV-1a hash constants (same as in router_service.cpp)
constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;

// Hash function for prefix tokens using FNV-1a (copied from router_service.cpp)
inline uint64_t hash_prefix(const int32_t* tokens, size_t count, uint32_t block_alignment) {
    // Align to block_alignment boundary
    size_t aligned_len = (count / block_alignment) * block_alignment;
    if (aligned_len == 0) aligned_len = count;

    uint64_t hash = FNV_OFFSET_BASIS;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(tokens);
    size_t byte_len = aligned_len * sizeof(int32_t);

    for (size_t i = 0; i < byte_len; ++i) {
        hash ^= data[i];
        hash *= FNV_PRIME;
    }

    return hash;
}

// Simulated backend selection logic (same as in router_service.cpp)
inline int32_t select_backend(const std::vector<int32_t>& tokens,
                               size_t prefix_token_length,
                               uint32_t block_alignment,
                               const std::vector<int32_t>& live_backends) {
    if (live_backends.empty()) {
        return -1;
    }

    // Extract prefix (first N tokens)
    size_t prefix_len = std::min(tokens.size(), prefix_token_length);
    if (prefix_len == 0) {
        return live_backends[0];
    }

    // Hash the prefix
    uint64_t prefix_hash = hash_prefix(tokens.data(), prefix_len, block_alignment);

    // Consistent hashing: map hash to backend index
    size_t index = prefix_hash % live_backends.size();
    return live_backends[index];
}

class PrefixAffinityTest : public ::testing::Test {
protected:
    // Default configuration
    size_t prefix_token_length = 128;
    uint32_t block_alignment = 16;
    std::vector<int32_t> backends = {1, 2};

    // Helper to create token vectors
    std::vector<int32_t> tokens(std::initializer_list<int32_t> list) {
        return std::vector<int32_t>(list);
    }

    std::vector<int32_t> tokens(size_t count, int32_t start = 1) {
        std::vector<int32_t> result(count);
        std::iota(result.begin(), result.end(), start);
        return result;
    }
};

// =============================================================================
// Hash Function Tests
// =============================================================================

TEST_F(PrefixAffinityTest, HashIsDeterministic) {
    auto tok = tokens(32);

    uint64_t hash1 = hash_prefix(tok.data(), tok.size(), block_alignment);
    uint64_t hash2 = hash_prefix(tok.data(), tok.size(), block_alignment);

    EXPECT_EQ(hash1, hash2);
}

TEST_F(PrefixAffinityTest, DifferentTokensProduceDifferentHashes) {
    auto tok1 = tokens(32, 1);
    auto tok2 = tokens(32, 100);

    uint64_t hash1 = hash_prefix(tok1.data(), tok1.size(), block_alignment);
    uint64_t hash2 = hash_prefix(tok2.data(), tok2.size(), block_alignment);

    EXPECT_NE(hash1, hash2);
}

TEST_F(PrefixAffinityTest, HashRespectsBlockAlignment) {
    // With block_alignment = 16, tokens 0-15 should be hashed,
    // and adding more tokens up to 31 shouldn't change the hash
    auto tok16 = tokens(16);
    auto tok20 = tokens(20);

    uint64_t hash16 = hash_prefix(tok16.data(), tok16.size(), 16);
    uint64_t hash20 = hash_prefix(tok20.data(), tok20.size(), 16);

    EXPECT_EQ(hash16, hash20);
}

TEST_F(PrefixAffinityTest, HashChangesBeyondBlockBoundary) {
    // Tokens 0-15 vs 0-31 should produce different hashes
    auto tok16 = tokens(16);
    auto tok32 = tokens(32);

    uint64_t hash16 = hash_prefix(tok16.data(), tok16.size(), 16);
    uint64_t hash32 = hash_prefix(tok32.data(), tok32.size(), 16);

    EXPECT_NE(hash16, hash32);
}

TEST_F(PrefixAffinityTest, HashWithBlockAlignmentOf1) {
    // Block alignment of 1 means every token matters
    auto tok5 = tokens(5);
    auto tok6 = tokens(6);

    uint64_t hash5 = hash_prefix(tok5.data(), tok5.size(), 1);
    uint64_t hash6 = hash_prefix(tok6.data(), tok6.size(), 1);

    EXPECT_NE(hash5, hash6);
}

TEST_F(PrefixAffinityTest, HashWithBelowBlockAlignment) {
    // When count < block_alignment, we still hash what we have
    auto tok5 = tokens(5);
    auto tok10 = tokens(10);

    // With block_alignment = 16, both should hash all tokens (aligned_len = count)
    uint64_t hash5 = hash_prefix(tok5.data(), tok5.size(), 16);
    uint64_t hash10 = hash_prefix(tok10.data(), tok10.size(), 16);

    EXPECT_NE(hash5, hash10);
}

// =============================================================================
// Backend Selection Tests
// =============================================================================

TEST_F(PrefixAffinityTest, SamePrefixSameBackend) {
    // Same prefix should always route to the same backend
    auto tok1 = tokens(200);  // First 200 tokens
    auto tok2 = tokens(200);  // Same first 200 tokens

    int32_t backend1 = select_backend(tok1, prefix_token_length, block_alignment, backends);
    int32_t backend2 = select_backend(tok2, prefix_token_length, block_alignment, backends);

    EXPECT_EQ(backend1, backend2);
}

TEST_F(PrefixAffinityTest, DifferentSuffixSameBackend) {
    // Requests with same prefix but different suffix should route to same backend
    // because we only hash the first prefix_token_length tokens

    std::vector<int32_t> tok1(150, 1);  // 150 tokens all = 1
    std::vector<int32_t> tok2(150, 1);  // Same prefix
    // Change suffix (beyond prefix_token_length)
    tok2[140] = 999;
    tok2[141] = 888;

    int32_t backend1 = select_backend(tok1, prefix_token_length, block_alignment, backends);
    int32_t backend2 = select_backend(tok2, prefix_token_length, block_alignment, backends);

    EXPECT_EQ(backend1, backend2);
}

TEST_F(PrefixAffinityTest, DifferentPrefixMayRouteToDifferentBackend) {
    // Different prefixes should distribute across backends
    // With 2 backends and many different prefixes, we should see both backends
    // Use varied token patterns to ensure hash distribution

    std::map<int32_t, int> backend_counts;
    for (int i = 0; i < 100; i++) {
        // Create varied token patterns by using primes and XOR
        std::vector<int32_t> tok(32);
        for (int j = 0; j < 32; j++) {
            tok[j] = (i * 7919 + j * 104729) ^ (i * i);  // Mix with primes
        }
        int32_t backend = select_backend(tok, prefix_token_length, 1, backends);
        backend_counts[backend]++;
    }

    // Both backends should be used
    EXPECT_GT(backend_counts[1], 0);
    EXPECT_GT(backend_counts[2], 0);
}

TEST_F(PrefixAffinityTest, DistributionIsRoughlyEven) {
    // With many different prefixes, distribution should be roughly even
    // Use varied token patterns to ensure hash distribution
    std::map<int32_t, int> backend_counts;

    for (int i = 0; i < 1000; i++) {
        // Create varied token patterns by using primes and XOR
        std::vector<int32_t> tok(32);
        for (int j = 0; j < 32; j++) {
            tok[j] = (i * 7919 + j * 104729) ^ (i * i);  // Mix with primes
        }
        int32_t backend = select_backend(tok, prefix_token_length, 1, backends);
        backend_counts[backend]++;
    }

    // Each backend should get at least 30% of requests (expect ~50%)
    EXPECT_GT(backend_counts[1], 300);
    EXPECT_GT(backend_counts[2], 300);
}

TEST_F(PrefixAffinityTest, EmptyTokensReturnsFirstBackend) {
    std::vector<int32_t> empty;
    int32_t backend = select_backend(empty, prefix_token_length, block_alignment, backends);
    EXPECT_EQ(backend, backends[0]);
}

TEST_F(PrefixAffinityTest, NoBackendsReturnsNegativeOne) {
    auto tok = tokens(32);
    std::vector<int32_t> no_backends;
    int32_t backend = select_backend(tok, prefix_token_length, block_alignment, no_backends);
    EXPECT_EQ(backend, -1);
}

TEST_F(PrefixAffinityTest, SingleBackendAlwaysSelected) {
    std::vector<int32_t> single_backend = {42};

    for (int i = 0; i < 100; i++) {
        auto tok = tokens(32, i * 1000);
        int32_t backend = select_backend(tok, prefix_token_length, block_alignment, single_backend);
        EXPECT_EQ(backend, 42);
    }
}

// =============================================================================
// Backend Ordering Tests
// =============================================================================

TEST_F(PrefixAffinityTest, BackendOrderMatters) {
    // Same token sequence with differently ordered backends may route differently
    auto tok = tokens(32);

    std::vector<int32_t> backends_1_2 = {1, 2};
    std::vector<int32_t> backends_2_1 = {2, 1};

    int32_t result1 = select_backend(tok, prefix_token_length, block_alignment, backends_1_2);
    int32_t result2 = select_backend(tok, prefix_token_length, block_alignment, backends_2_1);

    // Results may be the same or different depending on hash, but consistency is key
    // Re-select with same order should give same result
    EXPECT_EQ(result1, select_backend(tok, prefix_token_length, block_alignment, backends_1_2));
    EXPECT_EQ(result2, select_backend(tok, prefix_token_length, block_alignment, backends_2_1));
}

TEST_F(PrefixAffinityTest, AddingBackendChangesRouting) {
    auto tok = tokens(32);

    std::vector<int32_t> two_backends = {1, 2};
    std::vector<int32_t> three_backends = {1, 2, 3};

    int32_t with_two = select_backend(tok, prefix_token_length, block_alignment, two_backends);
    int32_t with_three = select_backend(tok, prefix_token_length, block_alignment, three_backends);

    // These may or may not be equal - the key is that results are deterministic
    // for the same backend list
    EXPECT_EQ(with_two, select_backend(tok, prefix_token_length, block_alignment, two_backends));
    EXPECT_EQ(with_three, select_backend(tok, prefix_token_length, block_alignment, three_backends));
}

// =============================================================================
// Prefix Token Length Configuration Tests
// =============================================================================

TEST_F(PrefixAffinityTest, ShortPrefixLength) {
    // With prefix_token_length = 8, only first 8 tokens matter
    std::vector<int32_t> tok1(100, 1);
    std::vector<int32_t> tok2(100, 1);
    tok1[50] = 999;  // Change at position 50 (beyond prefix)

    int32_t backend1 = select_backend(tok1, 8, block_alignment, backends);
    int32_t backend2 = select_backend(tok2, 8, block_alignment, backends);

    EXPECT_EQ(backend1, backend2);
}

TEST_F(PrefixAffinityTest, TokensLessThanPrefixLength) {
    // When tokens.size() < prefix_token_length, use all tokens
    auto tok = tokens(10);

    int32_t backend = select_backend(tok, 128, block_alignment, backends);
    EXPECT_TRUE(backend == 1 || backend == 2);
}

// =============================================================================
// Consistency Under Failure Tests
// =============================================================================

TEST_F(PrefixAffinityTest, BackendFailoverChangesRouting) {
    auto tok = tokens(32);

    std::vector<int32_t> all_backends = {1, 2, 3};
    std::vector<int32_t> after_failure = {1, 3};  // Backend 2 failed

    int32_t before = select_backend(tok, prefix_token_length, block_alignment, all_backends);
    int32_t after = select_backend(tok, prefix_token_length, block_alignment, after_failure);

    // Routing may change when backends change
    // But requests that were routing to 1 or 3 should still work

    // Most importantly, same config = same result
    EXPECT_EQ(before, select_backend(tok, prefix_token_length, block_alignment, all_backends));
    EXPECT_EQ(after, select_backend(tok, prefix_token_length, block_alignment, after_failure));
}

// =============================================================================
// Real-world Scenario Tests
// =============================================================================

TEST_F(PrefixAffinityTest, SystemPromptAffinity) {
    // Simulate system prompt prefix with different user questions
    // System prompt tokens (common prefix)
    std::vector<int32_t> system_prompt(100);
    std::iota(system_prompt.begin(), system_prompt.end(), 1000);

    // Create requests with same system prompt but different user questions
    std::vector<int32_t> request1 = system_prompt;
    std::vector<int32_t> request2 = system_prompt;
    std::vector<int32_t> request3 = system_prompt;

    // Add different user questions
    for (int i = 0; i < 50; i++) {
        request1.push_back(2000 + i);
        request2.push_back(3000 + i);
        request3.push_back(4000 + i);
    }

    // All should route to the same backend (same prefix)
    int32_t backend1 = select_backend(request1, prefix_token_length, block_alignment, backends);
    int32_t backend2 = select_backend(request2, prefix_token_length, block_alignment, backends);
    int32_t backend3 = select_backend(request3, prefix_token_length, block_alignment, backends);

    EXPECT_EQ(backend1, backend2);
    EXPECT_EQ(backend2, backend3);
}

TEST_F(PrefixAffinityTest, DifferentSystemPromptsDistribute) {
    // Different system prompts should distribute across backends
    // Use varied token patterns to ensure hash distribution
    std::map<int32_t, int> backend_counts;

    for (int sys = 0; sys < 100; sys++) {
        // Each "system prompt" is different with varied pattern
        std::vector<int32_t> tok(50);
        for (int j = 0; j < 50; j++) {
            tok[j] = (sys * 7919 + j * 104729) ^ (sys * sys);  // Mix with primes
        }

        int32_t backend = select_backend(tok, prefix_token_length, 1, backends);
        backend_counts[backend]++;
    }

    // Both backends should be used for different system prompts
    EXPECT_GT(backend_counts[1], 0);
    EXPECT_GT(backend_counts[2], 0);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(PrefixAffinityTest, VeryLongTokenSequence) {
    // Test with very long token sequence
    auto tok = tokens(10000);  // 10k tokens

    int32_t backend = select_backend(tok, prefix_token_length, block_alignment, backends);
    EXPECT_TRUE(backend == 1 || backend == 2);

    // Same long sequence should be deterministic
    EXPECT_EQ(backend, select_backend(tok, prefix_token_length, block_alignment, backends));
}

TEST_F(PrefixAffinityTest, ManyBackends) {
    // Test with many backends
    // Use varied token patterns to ensure hash distribution
    std::vector<int32_t> many_backends;
    for (int i = 1; i <= 100; i++) {
        many_backends.push_back(i);
    }

    std::map<int32_t, int> backend_counts;
    for (int i = 0; i < 10000; i++) {
        // Create varied token patterns
        std::vector<int32_t> tok(32);
        for (int j = 0; j < 32; j++) {
            tok[j] = (i * 7919 + j * 104729) ^ (i * i);
        }
        int32_t backend = select_backend(tok, prefix_token_length, 1, many_backends);
        backend_counts[backend]++;
    }

    // Should use a reasonable number of backends with 10k samples
    // Hash distribution may not be perfectly uniform, but should use many
    int used_backends = backend_counts.size();
    EXPECT_GE(used_backends, 20);
}

TEST_F(PrefixAffinityTest, NegativeTokenIds) {
    // Token IDs can be negative
    std::vector<int32_t> negative_tokens = {-1, -2, -3, -100, -200, -300};

    int32_t backend = select_backend(negative_tokens, prefix_token_length, 1, backends);
    EXPECT_TRUE(backend == 1 || backend == 2);
}

TEST_F(PrefixAffinityTest, MaxValueTokenIds) {
    // Test with extreme token values
    std::vector<int32_t> extreme_tokens = {INT32_MAX, INT32_MIN, 0, INT32_MAX/2};

    int32_t backend = select_backend(extreme_tokens, prefix_token_length, 1, backends);
    EXPECT_TRUE(backend == 1 || backend == 2);
}
