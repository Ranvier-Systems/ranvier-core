// Minimal Seastar stub for unit testing headers that include seastar/core/future.hh
// without requiring a full Seastar installation.
#pragma once

namespace seastar {

template<typename T = void>
class future {
public:
    future() = default;
};

template<typename T = void>
future<T> make_ready_future() { return future<T>{}; }

} // namespace seastar
