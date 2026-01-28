#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <seastar/core/future.hh>
#include <tokenizers_cpp.h>

namespace ranvier {

/**
 * Configuration for the tokenization cache.
 * Cache is shard-local (no cross-shard issues) and uses LRU eviction.
 */
struct TokenizationCacheConfig {
    bool enabled = true;           // Enable/disable caching
    size_t max_entries = 1000;     // Maximum cache entries (Rule #4: bounded container)
    size_t max_text_length = 8192; // Don't cache texts longer than this (avoid memory bloat)
};

/**
 * LRU cache for tokenization results.
 *
 * Design rationale:
 * - Shard-local (no cross-shard synchronization needed)
 * - O(1) lookup and insert via hash map + doubly-linked list
 * - Bounded size with LRU eviction (Rule #4)
 * - Lock-free (all operations on single shard)
 *
 * Expected hit rates:
 * - System messages: 80-90% (highly repetitive across requests)
 * - Role tags: 95%+ (e.g., "<|system|>\n" tokenized repeatedly)
 * - User queries: 10-30% (depends on traffic patterns)
 */
class TokenizationCache {
public:
    explicit TokenizationCache(TokenizationCacheConfig config = {})
        : _config(config) {}

    // Lookup cached tokens for text, returns nullptr if not found
    // Moves entry to front of LRU list on hit
    const std::vector<int32_t>* lookup(std::string_view text);

    // Insert tokenization result into cache
    // Evicts LRU entries if cache is full
    void insert(std::string_view text, std::vector<int32_t> tokens);

    // Cache statistics (lock-free, shard-local)
    uint64_t hits() const { return _hits; }
    uint64_t misses() const { return _misses; }
    size_t size() const { return _cache.size(); }
    size_t max_size() const { return _config.max_entries; }
    bool enabled() const { return _config.enabled; }

    // Clear the cache (for testing or hot-reload)
    void clear();

private:
    // Hash function for string_view keys
    struct StringHash {
        using is_transparent = void;
        size_t operator()(std::string_view sv) const noexcept {
            return std::hash<std::string_view>{}(sv);
        }
        size_t operator()(const std::string& s) const noexcept {
            return std::hash<std::string_view>{}(std::string_view(s));
        }
    };

    struct StringEqual {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const noexcept {
            return a == b;
        }
    };

    // LRU list entry: stores the actual text and tokens
    struct CacheEntry {
        std::string text;              // Owned copy of the text (key)
        std::vector<int32_t> tokens;   // Cached tokenization result
    };

    TokenizationCacheConfig _config;

    // LRU list: front = most recently used, back = least recently used
    std::list<CacheEntry> _lru_list;

    // Hash map: text -> iterator into LRU list
    // Uses transparent hashing to allow lookup with string_view
    std::unordered_map<std::string_view, std::list<CacheEntry>::iterator,
                       StringHash, StringEqual> _cache;

    // Statistics (lock-free, shard-local)
    uint64_t _hits = 0;
    uint64_t _misses = 0;
};

/**
 * TokenizerService wraps the HuggingFace tokenizers library.
 *
 * IMPORTANT: This service must be sharded (one instance per Seastar shard/core)
 * because the underlying tokenizers-cpp library is NOT thread-safe for concurrent
 * Encode() calls on the same instance. Each shard must have its own tokenizer.
 *
 * Usage pattern in Application:
 *   seastar::sharded<TokenizerService> _tokenizer;
 *   _tokenizer.start();
 *   _tokenizer.invoke_on_all([&json](TokenizerService& t) { t.load_from_json(json); });
 *
 * OPTIMIZATION: Use encode_cached() for hot paths to leverage LRU caching.
 * System messages have 80-90% cache hit rates, dramatically reducing tokenization overhead.
 */
class TokenizerService {
public:
    TokenizerService() = default;

    // Loads the tokenizer from a JSON string (or file content)
    // Must be called on each shard after start()
    void load_from_json(const std::string& json_content);

    // Configure the tokenization cache (call before or after load_from_json)
    void configure_cache(TokenizationCacheConfig config);

    // The main API: Text -> Integers (uncached, direct FFI call)
    // Accepts string_view for zero-copy tokenization from temporary_buffer
    // NOT thread-safe - must only be called from the owning shard
    std::vector<int32_t> encode(std::string_view text) const;

    // OPTIMIZED API: Text -> Integers with LRU caching
    // For hot paths (handle_proxy), use this to avoid redundant tokenization.
    // Returns pair<tokens, cache_hit> for metrics recording.
    // NOT thread-safe - must only be called from the owning shard
    std::pair<std::vector<int32_t>, bool> encode_cached(std::string_view text);

    // Check if ready
    bool is_loaded() const;

    // Get vocabulary size (for max_token_id validation)
    // Returns 0 if tokenizer is not loaded
    size_t vocab_size() const;

    // Cache statistics accessors (for metrics)
    uint64_t cache_hits() const { return _cache.hits(); }
    uint64_t cache_misses() const { return _cache.misses(); }
    size_t cache_size() const { return _cache.size(); }
    size_t cache_max_size() const { return _cache.max_size(); }
    bool cache_enabled() const { return _cache.enabled(); }

    // Clear the cache (for testing or config hot-reload)
    void clear_cache() { _cache.clear(); }

    // Seastar sharded service requirement: stop() must return future<>
    seastar::future<> stop() {
        _cache.clear();
        _impl.reset();
        return seastar::make_ready_future<>();
    }

private:
    std::unique_ptr<tokenizers::Tokenizer> _impl;
    TokenizationCache _cache;
};

} // namespace ranvier
