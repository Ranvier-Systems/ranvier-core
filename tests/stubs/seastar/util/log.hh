// Minimal Seastar stub for unit testing headers that include seastar/util/log.hh
// without requiring a full Seastar installation.
#pragma once

#include <utility>

namespace seastar {

class logger {
public:
    explicit logger(const char*) {}

    // No-op variadic templates — match the seastar::logger interface enough
    // to let production headers that call .warn() / .info() / .error() etc.
    // compile under the unit-test stub. The format string and args are
    // accepted and discarded.
    template <typename... Args>
    void log(int, const char*, Args&&...) const noexcept {}
    template <typename... Args>
    void error(const char*, Args&&...) const noexcept {}
    template <typename... Args>
    void warn(const char*, Args&&...) const noexcept {}
    template <typename... Args>
    void info(const char*, Args&&...) const noexcept {}
    template <typename... Args>
    void debug(const char*, Args&&...) const noexcept {}
    template <typename... Args>
    void trace(const char*, Args&&...) const noexcept {}
};

} // namespace seastar
