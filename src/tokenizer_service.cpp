#include "tokenizer_service.hpp"

#include <stdexcept>

namespace ranvier {

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

bool TokenizerService::is_loaded() const {
    return _impl != nullptr;
}

size_t TokenizerService::vocab_size() const {
    if (!_impl) return 0;
    return _impl->GetVocabSize();
}

} // namespace ranvier
