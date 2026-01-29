#include "tokenizer_service.hpp"
#include "shard_load_balancer.hpp"

#include <stdexcept>

#include <seastar/util/log.hh>

namespace ranvier {

static seastar::logger log_tokenizer("tokenizer");

// ============================================================================
// TokenizationCache Implementation
// ============================================================================

const std::vector<int32_t>* TokenizationCache::lookup(std::string_view text) {
    if (!_config.enabled) {
        ++_misses;
        return nullptr;
    }

    auto it = _cache.find(text);
    if (it == _cache.end()) {
        ++_misses;
        return nullptr;
    }

    ++_hits;

    // Move to front of LRU list (most recently used)
    auto list_it = it->second;
    if (list_it != _lru_list.begin()) {
        _lru_list.splice(_lru_list.begin(), _lru_list, list_it);
    }

    return &list_it->tokens;
}

void TokenizationCache::insert(std::string_view text, std::vector<int32_t> tokens) {
    if (!_config.enabled) {
        return;
    }

    // Don't cache if max_entries is 0 (effectively disabled)
    if (_config.max_entries == 0) {
        return;
    }

    // Don't cache very long texts (memory optimization)
    if (text.size() > _config.max_text_length) {
        return;
    }

    // Check if already in cache (shouldn't happen in normal flow, but be safe)
    auto existing = _cache.find(text);
    if (existing != _cache.end()) {
        // Update existing entry and move to front
        existing->second->tokens = std::move(tokens);
        _lru_list.splice(_lru_list.begin(), _lru_list, existing->second);
        return;
    }

    // Evict LRU entries if at capacity (Rule #4: bounded container)
    while (_cache.size() >= _config.max_entries && !_lru_list.empty()) {
        // Remove from back (least recently used)
        auto& lru_entry = _lru_list.back();
        _cache.erase(std::string_view(lru_entry.text));
        _lru_list.pop_back();
    }

    // Insert new entry at front (most recently used)
    _lru_list.push_front(CacheEntry{std::string(text), std::move(tokens)});
    auto new_it = _lru_list.begin();

    // Map text to list iterator (use string_view pointing to owned string)
    _cache.emplace(std::string_view(new_it->text), new_it);
}

void TokenizationCache::clear() {
    _cache.clear();
    _lru_list.clear();
    // Don't reset stats - they're cumulative for metrics
}

// ============================================================================
// TokenizerService Implementation
// ============================================================================

void TokenizerService::load_from_json(const std::string& json_content) {
    _impl = tokenizers::Tokenizer::FromBlobJSON(json_content);
}

void TokenizerService::configure_cache(TokenizationCacheConfig config) {
    _cache = TokenizationCache(config);
}

std::vector<int32_t> TokenizerService::encode(std::string_view text) const {
    if (!_impl) return {};
    // The tokenizers library accepts std::string, so we construct one from the view.
    // This is a single copy at the tokenization boundary, but we've eliminated
    // all earlier copies in the request path (NIC -> temporary_buffer -> string_view).
    // The tokenizer itself may need contiguous null-terminated input internally.
    return _impl->Encode(std::string(text));
}

std::pair<std::vector<int32_t>, bool> TokenizerService::encode_cached(std::string_view text) {
    // Fast path: check cache first
    const auto* cached = _cache.lookup(text);
    if (cached) {
        // Return copy of cached tokens (cache retains ownership)
        return {*cached, true};
    }

    // Cache miss: perform actual tokenization
    if (!_impl) {
        return {{}, false};
    }

    auto tokens = _impl->Encode(std::string(text));

    // Insert into cache for future lookups
    // Note: We make a copy here because tokens will be moved into the return value
    _cache.insert(text, tokens);

    return {std::move(tokens), false};
}

void TokenizerService::configure_cross_shard(CrossShardTokenizationConfig config) {
    _cross_shard_config = config;
    if (config.enabled) {
        log_tokenizer.info("Cross-shard tokenization enabled on shard {} "
                          "(min_text_length={}, max_text_length={})",
                          seastar::this_shard_id(),
                          config.min_text_length,
                          config.max_text_length);
    }
}

void TokenizerService::set_cross_shard_refs(
    seastar::sharded<ShardLoadBalancer>* load_balancer,
    seastar::sharded<TokenizerService>* tokenizer) {
    _load_balancer = load_balancer;
    _tokenizer_sharded = tokenizer;
    log_tokenizer.info("Cross-shard tokenization refs set on shard {} (lb={}, tok={})",
                       seastar::this_shard_id(),
                       load_balancer != nullptr,
                       tokenizer != nullptr);
}

bool TokenizerService::should_dispatch_cross_shard(size_t text_length) const {
    // Cross-shard must be enabled
    if (!_cross_shard_config.enabled) {
        log_tokenizer.trace("Cross-shard skip: disabled");
        return false;
    }

    // Must have references set up
    if (!_load_balancer || !_tokenizer_sharded) {
        log_tokenizer.trace("Cross-shard skip: refs not set (lb={}, tok={})",
                           _load_balancer != nullptr, _tokenizer_sharded != nullptr);
        return false;
    }

    // Only dispatch if we have multiple shards
    if (seastar::smp::count <= 1) {
        log_tokenizer.trace("Cross-shard skip: single shard");
        return false;
    }

    // Check text length bounds
    if (text_length < _cross_shard_config.min_text_length) {
        log_tokenizer.trace("Cross-shard skip: text too small ({} < {})",
                           text_length, _cross_shard_config.min_text_length);
        return false;
    }
    if (text_length > _cross_shard_config.max_text_length) {
        log_tokenizer.trace("Cross-shard skip: text too large ({} > {})",
                           text_length, _cross_shard_config.max_text_length);
        return false;
    }

    log_tokenizer.trace("Cross-shard eligible: text_length={}", text_length);
    return true;
}

uint32_t TokenizerService::select_tokenization_shard() const {
    if (!_load_balancer) {
        return seastar::this_shard_id();
    }

    // Use P2C to select least-loaded shard
    uint32_t selected = _load_balancer->local().select_shard();
    uint32_t local = seastar::this_shard_id();
    log_tokenizer.trace("P2C selected shard {} (local={})", selected, local);
    return selected;
}

seastar::future<TokenizationResult> TokenizerService::encode_cached_async(std::string_view text) {
    uint32_t local_shard = seastar::this_shard_id();

    log_tokenizer.trace("encode_cached_async called: text_length={}", text.size());

    // Fast path: check local cache first (no async overhead for hits)
    const auto* cached = _cache.lookup(text);
    if (cached) {
        log_tokenizer.trace("Cache hit for text_length={}", text.size());
        TokenizationResult result;
        result.tokens = *cached;  // Copy from cache
        result.cache_hit = true;
        result.cross_shard = false;
        result.source_shard = local_shard;
        return seastar::make_ready_future<TokenizationResult>(std::move(result));
    }

    // Cache miss - check if tokenizer is loaded
    if (!_impl) {
        return seastar::make_ready_future<TokenizationResult>(TokenizationResult{});
    }

    // Decide whether to dispatch cross-shard
    if (!should_dispatch_cross_shard(text.size())) {
        // Tokenize locally (blocks reactor but avoids cross-shard overhead)
        ++_cross_shard_local_fallbacks;

        TokenizationResult result;
        try {
            result.tokens = _impl->Encode(std::string(text));
        } catch (const std::exception& e) {
            // Rule #9: Log exceptions at warn level with context
            log_tokenizer.warn("Local tokenization failed on shard {}: {}",
                              local_shard, e.what());
            return seastar::make_ready_future<TokenizationResult>(TokenizationResult{});
        }
        result.cache_hit = false;
        result.cross_shard = false;
        result.source_shard = local_shard;

        // Cache locally for future hits
        _cache.insert(text, result.tokens);

        return seastar::make_ready_future<TokenizationResult>(std::move(result));
    }

    // Select target shard via P2C
    uint32_t target_shard = select_tokenization_shard();

    if (target_shard == local_shard) {
        // P2C selected local shard - process locally
        ++_cross_shard_local_fallbacks;

        TokenizationResult result;
        try {
            result.tokens = _impl->Encode(std::string(text));
        } catch (const std::exception& e) {
            log_tokenizer.warn("Local tokenization failed on shard {}: {}",
                              local_shard, e.what());
            return seastar::make_ready_future<TokenizationResult>(TokenizationResult{});
        }
        result.cache_hit = false;
        result.cross_shard = false;
        result.source_shard = local_shard;

        _cache.insert(text, result.tokens);

        return seastar::make_ready_future<TokenizationResult>(std::move(result));
    }

    // Cross-shard dispatch: copy text for transfer (Rule #14: safe cross-shard data)
    ++_cross_shard_dispatches;

    // IMPORTANT: We must copy the text into an owned string because:
    // 1. The original string_view may point to temporary_buffer that could be freed
    // 2. Rule #14: Cannot pass references across shard boundaries
    std::string text_copy(text);

    // Capture sharded pointer by value (not `this`) to avoid cross-shard memory access
    auto tokenizer_sharded = _tokenizer_sharded;

    return seastar::smp::submit_to(target_shard,
        [tokenizer_sharded, text_copy = std::move(text_copy), target_shard]() mutable
            -> std::pair<std::vector<int32_t>, bool> {
            // On target shard: get local TokenizerService and check cache
            auto& target_tokenizer = tokenizer_sharded->local();

            const auto* target_cached = target_tokenizer._cache.lookup(text_copy);
            if (target_cached) {
                // Target cache hit - return copy of cached tokens
                return {*target_cached, true};
            }

            // Target cache miss - perform tokenization (blocks target reactor)
            if (!target_tokenizer._impl) {
                return {{}, false};
            }

            std::vector<int32_t> tokens;
            try {
                tokens = target_tokenizer._impl->Encode(text_copy);
            } catch (const std::exception& e) {
                log_tokenizer.warn("Cross-shard tokenization failed on shard {}: {}",
                                  target_shard, e.what());
                return {{}, false};
            }

            // Cache on target shard for future cross-shard requests
            target_tokenizer._cache.insert(text_copy, tokens);

            return {std::move(tokens), false};
        })
        .then([this, local_shard, target_shard, text = std::string(text)]
              (std::pair<std::vector<int32_t>, bool> remote_result) {
            // Back on calling shard: package result and cache locally

            TokenizationResult result;
            result.tokens = std::move(remote_result.first);
            result.cache_hit = remote_result.second;  // Was it a cache hit on target?
            result.cross_shard = true;
            result.source_shard = target_shard;

            // Cache locally so future requests on this shard hit local cache
            if (!result.tokens.empty()) {
                _cache.insert(text, result.tokens);
            }

            return result;
        });
}

bool TokenizerService::is_loaded() const {
    return _impl != nullptr;
}

size_t TokenizerService::vocab_size() const {
    if (!_impl) return 0;
    return _impl->GetVocabSize();
}

} // namespace ranvier
