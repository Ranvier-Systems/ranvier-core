// Ranvier Core - Space-Saving bounded top-K frequency summary
//
// Fixed-capacity approximate "heavy hitters" tracker (Metwally, Agrawal &
// El Abbadi, "Efficient Computation of Frequent and Top-k Elements in Data
// Streams", 2005). Tracks the K most frequently `touch()`ed keys using exactly
// K slots — bounded by construction (Rule #4: the only growth site is guarded by
// a `size() < capacity` check, so the structure can never exceed K entries).
//
// Counts are approximate with a one-sided error: for any tracked entry the true
// frequency lies in `[count - error, count]`, and a key whose true frequency
// exceeds `total / K` is guaranteed to be retained. This is exactly the property
// the cache-aware-autoscaler hot-prefix signal needs — the *set* of hot prefixes,
// not exact counts (see docs/architecture/cache-aware-autoscaler-telemetry.md and
// BACKLOG §21).
//
// Reactor-free / no Seastar deps: this header is used both on the shard-local hot
// path (one `touch()` per routed request) and in reactor-free unit tests. `touch()`
// is O(1) on a hit or while filling, and O(capacity) only on an eviction (a miss
// once full). At the default capacity that eviction is a sub-microsecond integer
// scan, far under the Seastar task quota, so it does not risk a reactor stall
// (Rule #17). If capacity is ever raised by orders of magnitude, replace the
// eviction scan with the O(1) bucket-list variant of Space-Saving.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <absl/container/flat_hash_map.h>

namespace ranvier {

class StreamSummary {
public:
    static constexpr size_t kDefaultCapacity = 128;

    struct Entry {
        uint64_t key = 0;
        uint64_t count = 0;   // upper bound on the key's true frequency
        uint64_t error = 0;   // max over-estimation; true frequency >= count - error
    };

    explicit StreamSummary(size_t capacity = kDefaultCapacity)
        : _capacity(capacity == 0 ? 1 : capacity) {
        _slots.reserve(_capacity);
        _index.reserve(_capacity);
    }

    // Record one occurrence of `key`. O(1) on a hit or while the summary is still
    // filling; O(capacity) on an eviction (a miss once full).
    void touch(uint64_t key) {
        ++_total;

        if (auto it = _index.find(key); it != _index.end()) {
            ++_slots[it->second].count;
            return;
        }

        if (_slots.size() < _capacity) {
            // Rule #4: bounded — reached only while size() < _capacity.
            _index.emplace(key, _slots.size());
            _slots.push_back(Entry{key, 1, 0});
            return;
        }

        // Full: evict the minimum-count slot and reuse it for the new key. The
        // evicted count becomes the new entry's error bound (Space-Saving rule).
        const size_t victim = min_slot();
        Entry& slot = _slots[victim];
        _index.erase(slot.key);
        slot.error = slot.count;
        slot.key = key;
        ++slot.count;
        _index.emplace(key, victim);
    }

    // Total number of touch() calls (the stream length), not capped by capacity.
    uint64_t total() const { return _total; }
    size_t size() const { return _slots.size(); }
    size_t capacity() const { return _capacity; }

    // Entries sorted by count descending (size <= capacity). Used by the
    // cross-shard merge / export path; returns an owned copy by design so callers
    // on another shard reallocate locally (Rule #14).
    std::vector<Entry> snapshot() const {
        std::vector<Entry> out(_slots.begin(), _slots.end());
        std::sort(out.begin(), out.end(),
                  [](const Entry& a, const Entry& b) { return a.count > b.count; });
        return out;
    }

    void clear() {
        _slots.clear();
        _index.clear();
        _total = 0;
    }

private:
    // Index of the slot with the smallest count (the eviction victim). O(size).
    size_t min_slot() const {
        size_t min_i = 0;
        for (size_t i = 1; i < _slots.size(); ++i) {
            if (_slots[i].count < _slots[min_i].count) {
                min_i = i;
            }
        }
        return min_i;
    }

    size_t _capacity;
    uint64_t _total = 0;
    std::vector<Entry> _slots;                     // invariant: size() <= _capacity
    absl::flat_hash_map<uint64_t, size_t> _index;  // key -> slot index in _slots
};

}  // namespace ranvier
