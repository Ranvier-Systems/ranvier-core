#include "tokenizer_service.hpp"

#include <stdexcept>

namespace ranvier {

TokenizerService::TokenizerService() : _impl(nullptr) {}

void TokenizerService::load_from_json(const std::string& json_content) {
    _impl = tokenizers::Tokenizer::FromBlobJSON(json_content);
}

std::vector<int32_t> TokenizerService::encode(std::string_view text) const {
    if (!_impl) return {};
    // The tokenizers library accepts std::string, so we construct one from the view.
    // This is a single copy at the tokenization boundary, but we've eliminated
    // all earlier copies in the request path (NIC -> temporary_buffer -> string_view).
    // The tokenizer itself may need contiguous null-terminated input internally.
    return _impl->Encode(std::string(text));
}

bool TokenizerService::is_loaded() const {
    return _impl != nullptr;
}

} // namespace ranvier
