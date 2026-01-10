#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <tokenizers_cpp.h>

namespace ranvier {

class TokenizerService {
public:
    TokenizerService();
    // Loads the tokenizer from a JSON string (or file content)
    void load_from_json(const std::string& json_content);

    // The main API: Text -> Integers
    // Accepts string_view for zero-copy tokenization from temporary_buffer
    std::vector<int32_t> encode(std::string_view text) const;

    // Check if ready
    bool is_loaded() const;

private:
    std::unique_ptr<tokenizers::Tokenizer> _impl;
};

} // namespace ranvier
