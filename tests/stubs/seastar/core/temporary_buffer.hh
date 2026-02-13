// Minimal Seastar stub for unit testing headers that include
// seastar/core/temporary_buffer.hh without requiring a full Seastar installation.
#pragma once

#include <cstddef>
#include <cstring>
#include <memory>

namespace seastar {

template<typename CharType>
class temporary_buffer {
    std::unique_ptr<CharType[]> _buffer;
    size_t _size = 0;
public:
    temporary_buffer() = default;

    explicit temporary_buffer(size_t size)
        : _buffer(new CharType[size]), _size(size) {}

    temporary_buffer(const CharType* data, size_t size)
        : _buffer(size ? new CharType[size] : nullptr), _size(size) {
        if (data && size > 0) std::memcpy(_buffer.get(), data, size);
    }

    temporary_buffer(temporary_buffer&&) noexcept = default;
    temporary_buffer& operator=(temporary_buffer&&) noexcept = default;

    const CharType* get() const noexcept { return _buffer.get(); }
    CharType* get_write() noexcept { return _buffer.get(); }
    size_t size() const noexcept { return _size; }
    bool empty() const noexcept { return _size == 0; }
};

} // namespace seastar
