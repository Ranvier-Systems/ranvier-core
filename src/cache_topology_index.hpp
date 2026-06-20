// Ranvier Core - Cluster cache-topology index (BACKLOG §21 P2)
//
// Shard-0-only reverse index: prefix fingerprint -> the set of cluster nodes
// (BackendIds) that currently report it in their hot-prefix top-K. Built from
// HOT_PREFIX_DIGEST gossip — one digest per node, latest-value-wins. It answers
// "how many live nodes hold this hot prefix", the basis for "sole holder ==
// exactly one node => unsafe to reap" (surfaced in P2 slice 3).
//
// Maintained on shard 0 only (the gossip home); no Seastar dependency, so it is
// unit-testable without a reactor and updated lock-free (Rule #1). Timestamps
// are passed in by the caller (no clock to inject) — the gossip layer supplies
// steady_clock::now().
//
// Bounded (Rule #4): `_holders` capped at MAX_HOT_PREFIXES (a brand-new prefix
// is rejected at the cap and counted); `_by_node` capped at MAX_NODES. Both
// self-clean — a prefix with no holders, or a node with no prefixes, is erased,
// so a dead node never lingers to inflate holder counts.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "types.hpp"

namespace ranvier {

class CacheTopologyIndex {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    static constexpr size_t MAX_HOT_PREFIXES = 16384;  // ~ K * max_nodes ceiling
    static constexpr size_t MAX_NODES = 1024;          // matches gossip MAX_PEERS

    // Replace `node`'s entire contribution with `hashes` (per-node set-replace:
    // a node's latest digest wholly supersedes its prior one). O(|old| + |new|),
    // both bounded by the gossip digest cap.
    //
    // `verified` (BACKLOG §21 Phase 5c) is the subset of `hashes` the node reports
    // verified-resident in its own KV cache (native KV-event stream fresh). It is
    // intersected with the ACCEPTED membership, so a hash rejected at the cap — or
    // a malformed peer claiming verified outside its membership — never counts
    // verified. Empty `verified` => membership-only (old peer / no native trust).
    void apply_digest(BackendId node, const std::vector<uint64_t>& hashes,
                      const std::vector<uint64_t>& verified, TimePoint now) {
        auto node_it = _by_node.find(node);
        if (node_it == _by_node.end()) {
            if (_by_node.size() >= MAX_NODES) {  // Rule #4: bound node fan-out
                ++_node_overflow;
                return;
            }
            node_it = _by_node.emplace(node, NodeEntry{}).first;
        }
        NodeEntry& entry = node_it->second;

        absl::flat_hash_set<uint64_t> next;
        next.reserve(hashes.size());
        for (uint64_t h : hashes) next.insert(h);

        // Remove the node from prefixes it no longer holds.
        for (uint64_t h : entry.hashes) {
            if (!next.contains(h)) {
                remove_holder(node, h);
            }
        }
        // Add the node to prefixes newly held. A brand-new prefix rejected at the
        // cap is dropped from `next` too, keeping `_by_node` consistent with
        // `_holders` (erase deferred — never mutate `next` mid-iteration).
        std::vector<uint64_t> rejected;
        for (uint64_t h : next) {
            if (!entry.hashes.contains(h) && !add_holder(node, h)) {
                rejected.push_back(h);
            }
        }
        for (uint64_t h : rejected) next.erase(h);

        // Verified tier: the subset of ACCEPTED membership (`next`) the node marks
        // verified. Intersecting with `next` enforces verified ⊆ membership, so a
        // cap-rejected/absent hash can never inflate the verified holder count.
        absl::flat_hash_set<uint64_t> next_verified;
        next_verified.reserve(verified.size());
        for (uint64_t h : verified) {
            if (next.contains(h)) next_verified.insert(h);
        }
        // Verified removals (prefixes the node no longer verifies).
        for (uint64_t h : entry.verified) {
            if (!next_verified.contains(h)) {
                remove_verified_holder(node, h);
            }
        }
        // Verified additions. Each `h` is in `next` (accepted membership), so its
        // `_holders` key exists and `_verified_holders` keeps within the membership
        // keyspace — implicitly bounded by MAX_HOT_PREFIXES (Rule #4).
        for (uint64_t h : next_verified) {
            if (!entry.verified.contains(h)) {
                add_verified_holder(node, h);
            }
        }

        entry.hashes = std::move(next);
        entry.verified = std::move(next_verified);
        entry.last_seen = now;
    }

    // Membership-only convenience (no native-verified subset). Used by callers
    // that have no residency information and by the membership-tier unit tests.
    void apply_digest(BackendId node, const std::vector<uint64_t>& hashes, TimePoint now) {
        apply_digest(node, hashes, {}, now);
    }

    // Drop a node entirely (peer death). Removing a dead node is what keeps the
    // sole-holder signal honest — a stale dead holder is the dangerous
    // false-negative ("looks shared, is actually sole").
    void evict_node(BackendId node) {
        auto it = _by_node.find(node);
        if (it == _by_node.end()) return;
        for (uint64_t h : it->second.hashes) {
            remove_holder(node, h);
        }
        for (uint64_t h : it->second.verified) {
            remove_verified_holder(node, h);
        }
        _by_node.erase(it);
    }

    // TTL belt-and-suspenders to peer-death eviction: drop nodes not refreshed
    // since `cutoff`. Returns the count evicted. Bounded by node count.
    size_t age_out(TimePoint cutoff) {
        std::vector<BackendId> dead;
        for (const auto& [node, entry] : _by_node) {
            if (entry.last_seen < cutoff) dead.push_back(node);
        }
        for (BackendId node : dead) evict_node(node);
        return dead.size();
    }

    // ---- Queries (consumed by P2 slice 3) ----

    // Nodes currently holding `hash`, or nullptr if none.
    const absl::flat_hash_set<BackendId>* holders_of(uint64_t hash) const {
        auto it = _holders.find(hash);
        return it == _holders.end() ? nullptr : &it->second;
    }
    // Holder count for `hash` (0 if none). == 1 means sole-held => unsafe to reap.
    size_t holder_count(uint64_t hash) const {
        auto it = _holders.find(hash);
        return it == _holders.end() ? 0 : it->second.size();
    }

    // BACKLOG §21 Phase 5c: nodes that report `hash` verified-resident, or nullptr.
    // A subset of holders_of(hash) (verified ⊆ membership).
    const absl::flat_hash_set<BackendId>* verified_holders_of(uint64_t hash) const {
        auto it = _verified_holders.find(hash);
        return it == _verified_holders.end() ? nullptr : &it->second;
    }
    // Count of holders whose contribution marks `hash` verified-resident (0 if
    // none). == 1 means verified-sole-held => the automated-reaping gate (5d).
    size_t verified_holder_count(uint64_t hash) const {
        auto it = _verified_holders.find(hash);
        return it == _verified_holders.end() ? 0 : it->second.size();
    }

    size_t prefix_count() const { return _holders.size(); }
    size_t node_count() const { return _by_node.size(); }
    uint64_t prefix_overflow() const { return _prefix_overflow; }
    uint64_t node_overflow() const { return _node_overflow; }

private:
    struct NodeEntry {
        absl::flat_hash_set<uint64_t> hashes;    // membership (this node's hot top-K)
        absl::flat_hash_set<uint64_t> verified;  // 5c: subset verified-resident; ⊆ hashes
        TimePoint last_seen{};
    };

    // Add `node` to `hash`'s holder set. Returns false iff a brand-new prefix is
    // rejected at MAX_HOT_PREFIXES (Rule #4). An existing prefix never counts
    // against the cap (the cap bounds distinct prefixes, not holders).
    bool add_holder(BackendId node, uint64_t hash) {
        auto it = _holders.find(hash);
        if (it == _holders.end()) {
            if (_holders.size() >= MAX_HOT_PREFIXES) {
                ++_prefix_overflow;
                return false;
            }
            it = _holders.emplace(hash, absl::flat_hash_set<BackendId>{}).first;
        }
        it->second.insert(node);
        return true;
    }
    void remove_holder(BackendId node, uint64_t hash) {
        auto it = _holders.find(hash);
        if (it == _holders.end()) return;
        it->second.erase(node);
        if (it->second.empty()) _holders.erase(it);  // self-clean
    }

    // Verified-tier holder maintenance (BACKLOG §21 Phase 5c). No cap check: a
    // verified hash is always an accepted member (verified ⊆ accepted membership),
    // so `_verified_holders` keys ⊆ `_holders` keys ≤ MAX_HOT_PREFIXES (Rule #4).
    void add_verified_holder(BackendId node, uint64_t hash) {
        _verified_holders[hash].insert(node);
    }
    void remove_verified_holder(BackendId node, uint64_t hash) {
        auto it = _verified_holders.find(hash);
        if (it == _verified_holders.end()) return;
        it->second.erase(node);
        if (it->second.empty()) _verified_holders.erase(it);  // self-clean
    }

    absl::flat_hash_map<uint64_t, absl::flat_hash_set<BackendId>> _holders;
    // Verified-resident reverse index (5c): hash -> nodes reporting it resident.
    // Parallel to _holders; keys are always a subset of _holders' keys.
    absl::flat_hash_map<uint64_t, absl::flat_hash_set<BackendId>> _verified_holders;
    absl::flat_hash_map<BackendId, NodeEntry> _by_node;
    uint64_t _prefix_overflow = 0;
    uint64_t _node_overflow = 0;
};

}  // namespace ranvier
