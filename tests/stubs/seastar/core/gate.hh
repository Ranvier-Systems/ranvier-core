// Minimal Seastar stub for unit testing headers that include seastar/core/gate.hh
// without requiring a full Seastar installation.
#pragma once

#include <stdexcept>

namespace seastar {

class gate_closed_exception : public std::exception {
public:
    const char* what() const noexcept override { return "gate closed"; }
};

class gate {
public:
    class holder {
    public:
        holder() = default;
    };

    holder hold() { return holder{}; }
    bool is_closed() const { return false; }
};

} // namespace seastar
