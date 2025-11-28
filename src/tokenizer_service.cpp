#include "tokenizer_service.hpp"

#include <stdexcept>

namespace ranvier {

TokenizerService::TokenizerService() : _impl(nullptr) {}

void TokenizerService::load_from_json(const std::string& json_content) {
    _impl = tokenizers::Tokenizer::FromBlobJSON(json_content);
}

std::vector<int32_t> TokenizerService::encode(const std::string& text) const {
    if (!_impl) return {};
    return _impl->Encode(text);
}

bool TokenizerService::is_loaded() const {
    return _impl != nullptr;
}

} // namespace ranvier
