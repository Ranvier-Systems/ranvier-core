// Minimal Seastar stub for unit testing headers that include seastar/util/log.hh
// without requiring a full Seastar installation.
#pragma once

namespace seastar {

class logger {
public:
    explicit logger(const char*) {}
};

} // namespace seastar
