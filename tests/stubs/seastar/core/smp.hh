// Minimal Seastar stub for unit testing headers that include seastar/core/smp.hh
// without requiring a full Seastar installation.
#pragma once

namespace seastar {

inline unsigned this_shard_id() { return 0; }

} // namespace seastar
