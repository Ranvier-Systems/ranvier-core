#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <seastar/core/future.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/smp.hh>
#include <tokenizers_cpp.h>

#include "tokenizer_thread_pool.hpp"

namespace ranvier {

// Forward declaration for cross-shard dispatch
class ShardLoadBalancer;

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
 * Configuration for cross-shard tokenization offloading.
 *
 * On cache miss, instead of blocking the local reactor for 5-13ms during FFI,
 * dispatch to the least-loaded shard via P2C. The calling shard's reactor is
 * freed to handle other requests while waiting.
 *
 * Trade-offs:
 * - Pro: Unblocks reactor on calling shard during tokenization
 * - Pro: Uses existing P2C infrastructure, no thread-safety issues
 * - Con: Cross-shard latency (~1-10μs) + string copy overhead
 * - Con: Cache locality reduced (each shard builds own cache)
 */
struct CrossShardTokenizationConfig {
    bool enabled = true;  // Enabled by default; requires set_cross_shard_refs() to work

    // Minimum text length (bytes) to consider cross-shard dispatch.
    // Short texts tokenize quickly; cross-shard overhead may exceed benefit.
    // Default 64 bytes - even small texts benefit from reactor unblocking.
    size_t min_text_length = 64;

    // Maximum text length (bytes) for cross-shard dispatch.
    // Very long texts would require large string copies across shards.
    // Default 32KB; longer texts tokenize locally to avoid copy overhead.
    size_t max_text_length = 32768;
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
    // Uses absl::flat_hash_map for better cache locality and lookup performance
    absl::flat_hash_map<std::string_view, std::list<CacheEntry>::iterator,
                        StringHash, StringEqual> _cache;

    // Statistics (lock-free, shard-local)
    uint64_t _hits = 0;
    uint64_t _misses = 0;
};

/**
 * Result from async tokenization with source information.
 */
struct TokenizationResult {
    std::vector<int32_t> tokens;
    bool cache_hit = false;       // True if served from local cache
    bool cross_shard = false;     // True if tokenized on a different shard
    uint32_t source_shard = 0;    // Shard that performed tokenization
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
 *
 * ASYNC OPTIMIZATION: Use encode_cached_async() with cross-shard dispatch enabled
 * to offload cache-miss tokenization to the least-loaded shard via P2C. This frees
 * the calling shard's reactor during the 5-13ms FFI call.
 */
class TokenizerService {
public:
    TokenizerService() = default;

    // Loads the tokenizer from a JSON string (or file content)
    // Must be called on each shard after start()
    void load_from_json(const std::string& json_content);

    // Configure the tokenization cache (call before or after load_from_json)
    void configure_cache(TokenizationCacheConfig config);

    // Configure cross-shard tokenization offloading
    void configure_cross_shard(CrossShardTokenizationConfig config);

    // Set references for cross-shard dispatch (call after services are started)
    // load_balancer: P2C load balancer for shard selection
    // tokenizer: sharded tokenizer service (self-reference for cross-shard calls)
    void set_cross_shard_refs(
        seastar::sharded<ShardLoadBalancer>* load_balancer,
        seastar::sharded<TokenizerService>* tokenizer);

    // Set reference to thread pool for encode_threaded_async()
    // Call after thread pool workers are started
    void set_thread_pool_ref(seastar::sharded<TokenizerThreadPool>* thread_pool);

    // The main API: Text -> Integers (uncached, direct FFI call)
    // Accepts string_view for zero-copy tokenization from temporary_buffer
    // NOT thread-safe - must only be called from the owning shard
    std::vector<int32_t> encode(std::string_view text) const;

    // OPTIMIZED API: Text -> Integers with LRU caching (SYNCHRONOUS)
    // For hot paths (handle_proxy), use this to avoid redundant tokenization.
    // Returns pair<tokens, cache_hit> for metrics recording.
    // NOT thread-safe - must only be called from the owning shard
    // WARNING: Blocks reactor for 5-13ms on cache miss. Prefer encode_cached_async().
    std::pair<std::vector<int32_t>, bool> encode_cached(std::string_view text);

    // ASYNC OPTIMIZED API: Text -> Integers with caching + cross-shard dispatch
    // On cache hit: returns immediately (no async overhead)
    // On cache miss with cross-shard enabled: dispatches to least-loaded shard,
    //   freeing the calling reactor to handle other requests during FFI.
    // On cache miss with cross-shard disabled: tokenizes locally (blocks reactor).
    //
    // The result includes metadata about where tokenization occurred for metrics.
    seastar::future<TokenizationResult> encode_cached_async(std::string_view text);

    // THREAD POOL API: Text -> Integers with caching + dedicated thread pool
    // This is the highest-performance path for non-blocking tokenization.
    //
    // Priority order:
    // 1. Local cache hit → immediate return
    // 2. Thread pool available → submit to worker thread (non-blocking)
    // 3. Cross-shard dispatch available → dispatch via P2C (frees local reactor)
    // 4. Fallback → local tokenization (blocks reactor)
    //
    // Requires set_thread_pool_ref() to be called after thread pool is started.
    seastar::future<TokenizationResult> encode_threaded_async(std::string_view text);

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

    // Cross-shard dispatch statistics (for metrics)
    uint64_t cross_shard_dispatches() const { return _cross_shard_dispatches; }
    uint64_t cross_shard_local_fallbacks() const { return _cross_shard_local_fallbacks; }
    bool cross_shard_enabled() const { return _cross_shard_config.enabled; }

    // Thread pool dispatch statistics (for metrics)
    uint64_t thread_pool_dispatches() const { return _thread_pool_dispatches; }
    uint64_t thread_pool_fallbacks() const { return _thread_pool_fallbacks; }
    bool thread_pool_enabled() const {
        return _thread_pool && _thread_pool->local().enabled();
    }

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

    // Cross-shard dispatch configuration and references
    CrossShardTokenizationConfig _cross_shard_config;
    seastar::sharded<ShardLoadBalancer>* _load_balancer = nullptr;
    seastar::sharded<TokenizerService>* _tokenizer_sharded = nullptr;

    // Cross-shard dispatch statistics (shard-local, lock-free)
    uint64_t _cross_shard_dispatches = 0;      // Cache misses dispatched to other shards
    uint64_t _cross_shard_local_fallbacks = 0; // Cache misses processed locally (no eligible target)

    // Thread pool reference and statistics
    seastar::sharded<TokenizerThreadPool>* _thread_pool = nullptr;
    uint64_t _thread_pool_dispatches = 0;    // Cache misses dispatched to thread pool
    uint64_t _thread_pool_fallbacks = 0;     // Thread pool unavailable, fell back to other methods

    // Select target shard for cross-shard dispatch using P2C
    // Returns local shard ID if cross-shard is disabled or not beneficial
    uint32_t select_tokenization_shard() const;

    // Check if text qualifies for cross-shard dispatch
    bool should_dispatch_cross_shard(size_t text_length) const;
};

} // namespace ranvier
