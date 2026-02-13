// Minimal Seastar stub for unit testing headers that include seastar/core/smp.hh
// without requiring a full Seastar installation.
#pragma once

#include <utility>

namespace seastar {

inline unsigned this_shard_id() { return 0; }

struct smp {
    template<typename Func>
    static auto submit_to(unsigned, Func&& func) {
        return func();
    }
};

} // namespace seastar
