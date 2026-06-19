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
    void apply_digest(BackendId node, const std::vector<uint64_t>& hashes, TimePoint now) {
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

        entry.hashes = std::move(next);
        entry.last_seen = now;
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

    size_t prefix_count() const { return _holders.size(); }
    size_t node_count() const { return _by_node.size(); }
    uint64_t prefix_overflow() const { return _prefix_overflow; }
    uint64_t node_overflow() const { return _node_overflow; }

private:
    struct NodeEntry {
        absl::flat_hash_set<uint64_t> hashes;
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

    absl::flat_hash_map<uint64_t, absl::flat_hash_set<BackendId>> _holders;
    absl::flat_hash_map<BackendId, NodeEntry> _by_node;
    uint64_t _prefix_overflow = 0;
    uint64_t _node_overflow = 0;
};

}  // namespace ranvier
