// Ranvier Core - Tokenization Cache Unit Tests
//
// Tests for the LRU tokenization cache that optimizes repeated tokenization
// of system messages and role tags (Hard Rule #4: bounded container).

#include "tokenizer_service.hpp"
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace ranvier;

// =============================================================================
// TokenizationCacheConfig Tests
// =============================================================================

class TokenizationCacheConfigTest : public ::testing::Test {
protected:
    TokenizationCacheConfig config;
};

TEST_F(TokenizationCacheConfigTest, DefaultConfigIsEnabled) {
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.max_entries, 1000u);
    EXPECT_EQ(config.max_text_length, 65536u);
}

TEST_F(TokenizationCacheConfigTest, ConfigCanBeCustomized) {
    config.enabled = false;
    config.max_entries = 500;
    config.max_text_length = 4096;

    EXPECT_FALSE(config.enabled);
    EXPECT_EQ(config.max_entries, 500u);
    EXPECT_EQ(config.max_text_length, 4096u);
}

// =============================================================================
// TokenizationCache Basic Tests
// =============================================================================

class TokenizationCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        config.enabled = true;
        config.max_entries = 10;
        config.max_text_length = 1024;
    }

    TokenizationCacheConfig config;
};

TEST_F(TokenizationCacheTest, EmptyCacheReturnsMiss) {
    TokenizationCache cache(config);

    auto result = cache.lookup("hello world");
    EXPECT_EQ(result, nullptr);
    EXPECT_EQ(cache.misses(), 1u);
    EXPECT_EQ(cache.hits(), 0u);
}

TEST_F(TokenizationCacheTest, InsertAndLookupWorks) {
    TokenizationCache cache(config);

    std::vector<int32_t> tokens = {1, 2, 3, 4, 5};
    cache.insert("hello world", tokens);

    auto result = cache.lookup("hello world");
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(*result, tokens);
    EXPECT_EQ(cache.hits(), 1u);
    EXPECT_EQ(cache.size(), 1u);
}

TEST_F(TokenizationCacheTest, MissAfterInsertForDifferentKey) {
    TokenizationCache cache(config);

    std::vector<int32_t> tokens = {1, 2, 3};
    cache.insert("hello", tokens);

    auto result = cache.lookup("world");
    EXPECT_EQ(result, nullptr);
    EXPECT_EQ(cache.misses(), 1u);
}

TEST_F(TokenizationCacheTest, DisabledCacheAlwaysMisses) {
    config.enabled = false;
    TokenizationCache cache(config);

    cache.insert("hello", {1, 2, 3});
    auto result = cache.lookup("hello");

    EXPECT_EQ(result, nullptr);
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_FALSE(cache.enabled());
}

TEST_F(TokenizationCacheTest, SizeReturnsCorrectCount) {
    TokenizationCache cache(config);

    EXPECT_EQ(cache.size(), 0u);

    cache.insert("key1", {1});
    EXPECT_EQ(cache.size(), 1u);

    cache.insert("key2", {2});
    EXPECT_EQ(cache.size(), 2u);

    cache.insert("key3", {3});
    EXPECT_EQ(cache.size(), 3u);
}

TEST_F(TokenizationCacheTest, MaxSizeReturnsConfiguredLimit) {
    TokenizationCache cache(config);
    EXPECT_EQ(cache.max_size(), 10u);
}

// =============================================================================
// LRU Eviction Tests (Hard Rule #4)
// =============================================================================

class TokenizationCacheLRUTest : public ::testing::Test {
protected:
    void SetUp() override {
        config.enabled = true;
        config.max_entries = 3;  // Small cache for testing eviction
        config.max_text_length = 1024;
    }

    TokenizationCacheConfig config;
};

TEST_F(TokenizationCacheLRUTest, EvictsLRUEntryWhenFull) {
    TokenizationCache cache(config);

    // Fill cache to capacity
    cache.insert("key1", {1});
    cache.insert("key2", {2});
    cache.insert("key3", {3});
    EXPECT_EQ(cache.size(), 3u);

    // Insert a 4th entry - should evict key1 (LRU)
    cache.insert("key4", {4});
    EXPECT_EQ(cache.size(), 3u);

    // key1 should be evicted
    EXPECT_EQ(cache.lookup("key1"), nullptr);
    // key4 should be present
    ASSERT_NE(cache.lookup("key4"), nullptr);
}

TEST_F(TokenizationCacheLRUTest, LookupMovesToFrontOfLRU) {
    TokenizationCache cache(config);

    // Fill cache
    cache.insert("key1", {1});
    cache.insert("key2", {2});
    cache.insert("key3", {3});

    // Access key1, making it most recently used
    cache.lookup("key1");

    // Insert key4 - should evict key2 (now LRU since key1 was accessed)
    cache.insert("key4", {4});

    // key1 should still be present (was accessed)
    ASSERT_NE(cache.lookup("key1"), nullptr);
    // key2 should be evicted (LRU)
    EXPECT_EQ(cache.lookup("key2"), nullptr);
    // key3 and key4 should be present
    ASSERT_NE(cache.lookup("key3"), nullptr);
    ASSERT_NE(cache.lookup("key4"), nullptr);
}

TEST_F(TokenizationCacheLRUTest, UpdateExistingEntryMovesToFront) {
    TokenizationCache cache(config);

    // Fill cache
    cache.insert("key1", {1});
    cache.insert("key2", {2});
    cache.insert("key3", {3});

    // Update key1 with new tokens (should move to front)
    cache.insert("key1", {10, 11});

    // Insert key4 - should evict key2 (now LRU)
    cache.insert("key4", {4});

    // key1 should still be present with updated value
    auto result = cache.lookup("key1");
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(*result, (std::vector<int32_t>{10, 11}));

    // key2 should be evicted
    EXPECT_EQ(cache.lookup("key2"), nullptr);
}

TEST_F(TokenizationCacheLRUTest, CacheRemainsWithinBounds) {
    TokenizationCache cache(config);

    // Insert many more entries than capacity
    for (int i = 0; i < 100; ++i) {
        cache.insert("key" + std::to_string(i), {i});
    }

    // Cache should not exceed max_entries
    EXPECT_LE(cache.size(), config.max_entries);
}

// =============================================================================
// Text Length Limit Tests
// =============================================================================

class TokenizationCacheTextLengthTest : public ::testing::Test {
protected:
    void SetUp() override {
        config.enabled = true;
        config.max_entries = 100;
        config.max_text_length = 10;  // Very small for testing
    }

    TokenizationCacheConfig config;
};

TEST_F(TokenizationCacheTextLengthTest, ShortTextIsCached) {
    TokenizationCache cache(config);

    cache.insert("short", {1, 2, 3});
    EXPECT_EQ(cache.size(), 1u);
    ASSERT_NE(cache.lookup("short"), nullptr);
}

TEST_F(TokenizationCacheTextLengthTest, LongTextIsNotCached) {
    TokenizationCache cache(config);

    std::string long_text = "this is a very long text that exceeds the limit";
    cache.insert(long_text, {1, 2, 3, 4, 5});

    // Should not be cached
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.lookup(long_text), nullptr);
}

TEST_F(TokenizationCacheTextLengthTest, ExactLengthLimitIsCached) {
    TokenizationCache cache(config);

    std::string exact_length = "1234567890";  // Exactly 10 chars
    cache.insert(exact_length, {1, 2, 3});

    // Should be cached (at limit, not over)
    EXPECT_EQ(cache.size(), 1u);
}

TEST_F(TokenizationCacheTextLengthTest, OneByteTooLongIsNotCached) {
    TokenizationCache cache(config);

    std::string too_long = "12345678901";  // 11 chars, over limit
    cache.insert(too_long, {1, 2, 3});

    EXPECT_EQ(cache.size(), 0u);
}

// =============================================================================
// Large System Prefix Regression Tests
// =============================================================================
// Regression: before the max_text_length increase (8192 → 65536), large system
// prefixes used in boundary detection Strategy 4 were silently rejected by the
// cache, causing a blocking FFI call (5-13ms) on every request at ≥20 users.
// These tests enforce that the default config caches texts representative of
// real-world large system prefixes.

class LargeSystemPrefixCacheTest : public ::testing::Test {
protected:
    // Use DEFAULT config — the point is to verify the default accommodates
    // large system prefixes without requiring operator tuning.
    TokenizationCacheConfig default_config;
};

TEST_F(LargeSystemPrefixCacheTest, DefaultMaxTextCoversLargeSystemPrefix) {
    // A 2K-token system prefix at ~4 bytes/token ≈ 8KB text.
    // This must be cacheable with the default config to prevent the
    // boundary detection latency regression.
    EXPECT_GE(default_config.max_text_length, 8192u)
        << "Default max_text_length must accommodate 2K-token system prefixes "
           "(~8KB text) to prevent boundary detection regression";
}

TEST_F(LargeSystemPrefixCacheTest, DefaultMaxTextCoversXLargeSystemPrefix) {
    // An 8K-token system prefix at ~4 bytes/token ≈ 32KB text.
    // Stress benchmarks generate prefixes up to 8K tokens.
    EXPECT_GE(default_config.max_text_length, 32768u)
        << "Default max_text_length must accommodate 8K-token system prefixes "
           "(~32KB text) used in stress benchmarks";
}

TEST_F(LargeSystemPrefixCacheTest, CachesRealisticLargeSystemPrefix) {
    TokenizationCache cache(default_config);

    // Simulate a 16KB system prefix (4K tokens × ~4 bytes/token)
    std::string large_prefix(16384, 'A');
    std::vector<int32_t> tokens(4000);
    for (int i = 0; i < 4000; ++i) tokens[i] = i;

    cache.insert(large_prefix, tokens);
    EXPECT_EQ(cache.size(), 1u)
        << "16KB system prefix must be accepted by cache with default config";

    auto result = cache.lookup(large_prefix);
    ASSERT_NE(result, nullptr)
        << "16KB system prefix must be retrievable after insert";
    EXPECT_EQ(result->size(), 4000u);
}

TEST_F(LargeSystemPrefixCacheTest, CachesRealisticXLargeSystemPrefix) {
    TokenizationCache cache(default_config);

    // Simulate a 32KB system prefix (8K tokens × ~4 bytes/token)
    std::string xlarge_prefix(32768, 'B');
    std::vector<int32_t> tokens(8000);
    for (int i = 0; i < 8000; ++i) tokens[i] = i;

    cache.insert(xlarge_prefix, tokens);
    EXPECT_EQ(cache.size(), 1u)
        << "32KB system prefix must be accepted by cache with default config";

    auto result = cache.lookup(xlarge_prefix);
    ASSERT_NE(result, nullptr)
        << "32KB system prefix must be retrievable after insert";
    EXPECT_EQ(result->size(), 8000u);
}

TEST_F(LargeSystemPrefixCacheTest, OldDefaultWouldHaveRejectedLargePrefix) {
    // Prove that the old default (8192) was the root cause: a cache configured
    // with the old limit silently drops large prefixes.
    TokenizationCacheConfig old_config;
    old_config.max_text_length = 8192;  // The old default
    TokenizationCache cache(old_config);

    std::string large_prefix(16384, 'C');  // 16KB > 8192
    cache.insert(large_prefix, {1, 2, 3});
    EXPECT_EQ(cache.size(), 0u)
        << "Old default (8192) would reject 16KB prefix — this was the regression";
}

TEST_F(LargeSystemPrefixCacheTest, MultipleLargePrefixesCoexist) {
    // Stress benchmark generates 5 unique large prefixes.
    // All 5 should fit in the cache simultaneously.
    TokenizationCache cache(default_config);

    for (int i = 0; i < 5; ++i) {
        // Varying sizes: 8KB, 12KB, 16KB, 24KB, 32KB
        size_t size = (i + 2) * 4096;
        if (size > default_config.max_text_length) {
            // Skip sizes exceeding the limit (tests the boundary)
            continue;
        }
        std::string prefix(size, static_cast<char>('A' + i));
        cache.insert(prefix, {static_cast<int32_t>(i)});
    }

    // All inserted entries should be present (cache has 1000 entry capacity)
    EXPECT_GE(cache.size(), 4u)
        << "At least 4 of the 5 benchmark-sized prefixes must be cacheable";
}

// =============================================================================
// Clear Tests
// =============================================================================

class TokenizationCacheClearTest : public ::testing::Test {
protected:
    void SetUp() override {
        config.enabled = true;
        config.max_entries = 10;
        config.max_text_length = 1024;
    }

    TokenizationCacheConfig config;
};

TEST_F(TokenizationCacheClearTest, ClearRemovesAllEntries) {
    TokenizationCache cache(config);

    cache.insert("key1", {1});
    cache.insert("key2", {2});
    cache.insert("key3", {3});
    EXPECT_EQ(cache.size(), 3u);

    cache.clear();

    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.lookup("key1"), nullptr);
    EXPECT_EQ(cache.lookup("key2"), nullptr);
    EXPECT_EQ(cache.lookup("key3"), nullptr);
}

TEST_F(TokenizationCacheClearTest, ClearPreservesStatistics) {
    TokenizationCache cache(config);

    cache.insert("key1", {1});
    cache.lookup("key1");  // hit
    cache.lookup("missing");  // miss

    uint64_t hits_before = cache.hits();
    uint64_t misses_before = cache.misses();

    cache.clear();

    // Stats should be preserved (cumulative for metrics)
    EXPECT_EQ(cache.hits(), hits_before);
    EXPECT_EQ(cache.misses(), misses_before);
}

TEST_F(TokenizationCacheClearTest, CacheWorksAfterClear) {
    TokenizationCache cache(config);

    cache.insert("key1", {1});
    cache.clear();

    // Should work normally after clear
    cache.insert("key2", {2});
    EXPECT_EQ(cache.size(), 1u);
    ASSERT_NE(cache.lookup("key2"), nullptr);
}

// =============================================================================
// Statistics Tests
// =============================================================================

class TokenizationCacheStatsTest : public ::testing::Test {
protected:
    void SetUp() override {
        config.enabled = true;
        config.max_entries = 10;
        config.max_text_length = 1024;
    }

    TokenizationCacheConfig config;
};

TEST_F(TokenizationCacheStatsTest, HitsAndMissesAccumulate) {
    TokenizationCache cache(config);

    cache.insert("key1", {1});

    // Generate some hits and misses
    cache.lookup("key1");  // hit
    cache.lookup("key1");  // hit
    cache.lookup("missing1");  // miss
    cache.lookup("key1");  // hit
    cache.lookup("missing2");  // miss
    cache.lookup("missing3");  // miss

    EXPECT_EQ(cache.hits(), 3u);
    EXPECT_EQ(cache.misses(), 3u);
}

TEST_F(TokenizationCacheStatsTest, DisabledCacheCountsMisses) {
    config.enabled = false;
    TokenizationCache cache(config);

    cache.insert("key1", {1});
    cache.lookup("key1");
    cache.lookup("key1");

    // All lookups are misses when disabled
    EXPECT_EQ(cache.hits(), 0u);
    EXPECT_EQ(cache.misses(), 2u);
}

// =============================================================================
// Edge Cases Tests
// =============================================================================

class TokenizationCacheEdgeCasesTest : public ::testing::Test {
protected:
    TokenizationCacheConfig config;
};

TEST_F(TokenizationCacheEdgeCasesTest, EmptyStringCanBeCached) {
    config.enabled = true;
    config.max_entries = 10;
    config.max_text_length = 1024;
    TokenizationCache cache(config);

    cache.insert("", {});
    EXPECT_EQ(cache.size(), 1u);

    auto result = cache.lookup("");
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->empty());
}

TEST_F(TokenizationCacheEdgeCasesTest, EmptyTokensCanBeCached) {
    config.enabled = true;
    config.max_entries = 10;
    config.max_text_length = 1024;
    TokenizationCache cache(config);

    cache.insert("empty_tokens", {});
    EXPECT_EQ(cache.size(), 1u);

    auto result = cache.lookup("empty_tokens");
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->empty());
}

TEST_F(TokenizationCacheEdgeCasesTest, LargeTokenVectorWorks) {
    config.enabled = true;
    config.max_entries = 10;
    config.max_text_length = 1024;
    TokenizationCache cache(config);

    std::vector<int32_t> large_tokens(10000);
    for (int i = 0; i < 10000; ++i) {
        large_tokens[i] = i;
    }

    cache.insert("large", large_tokens);
    auto result = cache.lookup("large");
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->size(), 10000u);
}

TEST_F(TokenizationCacheEdgeCasesTest, ZeroCapacityCacheWorks) {
    config.enabled = true;
    config.max_entries = 0;
    config.max_text_length = 1024;
    TokenizationCache cache(config);

    // Should not crash, just not cache anything
    cache.insert("key1", {1});
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.lookup("key1"), nullptr);
}

// =============================================================================
// String View Handling Tests
// =============================================================================

class TokenizationCacheStringViewTest : public ::testing::Test {
protected:
    void SetUp() override {
        config.enabled = true;
        config.max_entries = 10;
        config.max_text_length = 1024;
    }

    TokenizationCacheConfig config;
};

TEST_F(TokenizationCacheStringViewTest, LookupWithStringViewWorks) {
    TokenizationCache cache(config);

    cache.insert("hello world", {1, 2, 3});

    // Lookup with string_view from different source
    std::string key = "hello world";
    std::string_view sv(key);

    auto result = cache.lookup(sv);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->size(), 3u);
}

TEST_F(TokenizationCacheStringViewTest, InsertWithStringViewCopiesText) {
    TokenizationCache cache(config);

    {
        std::string temp = "temporary key";
        std::string_view sv(temp);
        cache.insert(sv, {1, 2, 3});
    }  // temp goes out of scope

    // Should still be able to find it (cache owns a copy)
    auto result = cache.lookup("temporary key");
    ASSERT_NE(result, nullptr);
}

// =============================================================================
// Type Traits Tests
// =============================================================================

TEST(TokenizationCacheTypeTraitsTest, ConfigIsCopyable) {
    EXPECT_TRUE(std::is_copy_constructible_v<TokenizationCacheConfig>);
    EXPECT_TRUE(std::is_copy_assignable_v<TokenizationCacheConfig>);
}

TEST(TokenizationCacheTypeTraitsTest, CacheIsMovable) {
    EXPECT_TRUE(std::is_move_constructible_v<TokenizationCache>);
    EXPECT_TRUE(std::is_move_assignable_v<TokenizationCache>);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
