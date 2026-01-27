#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <seastar/core/future.hh>
#include <tokenizers_cpp.h>

namespace ranvier {

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
 */
class TokenizerService {
public:
    TokenizerService() = default;

    // Loads the tokenizer from a JSON string (or file content)
    // Must be called on each shard after start()
    void load_from_json(const std::string& json_content);

    // The main API: Text -> Integers
    // Accepts string_view for zero-copy tokenization from temporary_buffer
    // NOT thread-safe - must only be called from the owning shard
    std::vector<int32_t> encode(std::string_view text) const;

    // Check if ready
    bool is_loaded() const;

    // Get vocabulary size (for max_token_id validation)
    // Returns 0 if tokenizer is not loaded
    size_t vocab_size() const;

    // Seastar sharded service requirement: stop() must return future<>
    seastar::future<> stop() {
        _impl.reset();
        return seastar::make_ready_future<>();
    }

private:
    std::unique_ptr<tokenizers::Tokenizer> _impl;
};

} // namespace ranvier
