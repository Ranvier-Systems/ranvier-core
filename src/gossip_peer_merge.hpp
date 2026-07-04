// Ranvier Core - Gossip peer-set merge helper
//
// Pure, reactor-free logic for merging the configured/static peer list with
// DNS-discovered addresses under a hard size cap (Rule #4). Kept dependency-free
// (no Seastar) so it is unit-testable without a running reactor and reusable by
// gossip_protocol::refresh_peers().

#pragma once

#include <cstddef>
#include <functional>
#include <unordered_set>
#include <vector>

namespace ranvier {

// Merge `static_peers` and `discovered` into a deduplicated list capped at
// `cap`, with a deterministic admission priority: configured/static peers are
// admitted first, then DNS-discovered addresses. This guarantees a hostile or
// misconfigured resolver returning a flood of addresses can never evict a
// configured peer, and bounds the peer containers keyed off this set (Rule #4).
//
// `dropped` receives the number of *distinct* admissible addresses refused
// because the cap was already full; duplicates are collapsed and never counted
// as drops. The returned vector has at most `cap` entries and preserves
// admission order (static peers first).
template <typename Addr, typename Hash = std::hash<Addr>>
std::vector<Addr> merge_and_cap_peers(const std::vector<Addr>& static_peers,
                                      const std::vector<Addr>& discovered,
                                      std::size_t cap,
                                      std::size_t& dropped) {
    std::vector<Addr> merged;
    std::unordered_set<Addr, Hash> seen;
    dropped = 0;

    auto admit = [&](const Addr& addr) {
        if (!seen.insert(addr).second) {
            return;  // already encountered this address — duplicate, not a drop
        }
        if (merged.size() >= cap) {
            ++dropped;
            return;
        }
        merged.push_back(addr);
    };

    for (const auto& addr : static_peers) {
        admit(addr);
    }
    for (const auto& addr : discovered) {
        admit(addr);
    }
    return merged;
}

}  // namespace ranvier
