// Ranvier Core - Shard count helper
//
// Replacement for the deprecated static `seastar::smp::count`.
//
// Seastar's suggested replacement, `seastar::this_smp_shard_count()`,
// dereferences a thread-local smp pointer that is only set on reactor
// threads. Unit tests construct services (e.g. RouterService) directly on
// the gtest main thread with no reactor running, where that call segfaults.
// The old static was 0 in that situation, so this helper preserves it.

#pragma once

#include <seastar/core/reactor.hh>
#include <seastar/core/smp.hh>

namespace ranvier {

// Number of reactor shards, or 0 when called off a reactor thread.
inline unsigned reactor_shard_count() noexcept {
    // smp::configure sets the thread-local smp pointer before allocating the
    // reactor on every shard, so engine_is_ready() implies this_smp() is valid.
    return seastar::engine_is_ready() ? seastar::this_smp_shard_count() : 0;
}

}  // namespace ranvier
