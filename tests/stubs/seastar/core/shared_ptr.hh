// Minimal Seastar stub for unit testing headers that include
// seastar/core/shared_ptr.hh without requiring a full Seastar installation.
//
// Provides foreign_ptr and make_foreign for cross-shard pointer semantics.
#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace seastar {

template<typename PtrType>
class foreign_ptr {
    PtrType _ptr;
public:
    foreign_ptr() = default;
    explicit foreign_ptr(PtrType ptr) : _ptr(std::move(ptr)) {}
    foreign_ptr(foreign_ptr&&) noexcept = default;
    foreign_ptr& operator=(foreign_ptr&&) noexcept = default;

    auto& operator*() { return *_ptr; }
    const auto& operator*() const { return *_ptr; }
    auto* operator->() { return &*_ptr; }
};

template<typename T>
foreign_ptr<T> make_foreign(T ptr) {
    return foreign_ptr<T>(std::move(ptr));
}

template<typename T>
class lw_shared_ptr {};

} // namespace seastar
