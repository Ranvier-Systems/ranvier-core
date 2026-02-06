// Minimal fmt stub for unit testing headers that include fmt/format.h
// without requiring the real libfmt. Delegates to C++20 std::format.
#pragma once

#include <format>
#include <string>

namespace fmt {

template<typename... Args>
inline std::string format(std::format_string<Args...> s, Args&&... args) {
    return std::format(std::move(s), std::forward<Args>(args)...);
}

} // namespace fmt
