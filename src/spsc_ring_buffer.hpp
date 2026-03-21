#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace ranvier {

// ============================================================================
// SPSC Ring Buffer
// ============================================================================
// Lock-free bounded single-producer single-consumer ring buffer for move-only
// types. Producer (reactor thread) writes at _tail; consumer (worker thread)
// reads from _head. No mutex, no CAS — SPSC guarantees one writer per index.
//
// Memory ordering: acquire/release on head/tail provide the happens-before
// relationship between producer writes and consumer reads. The producer
// writes the slot, then release-stores _tail. The consumer acquire-loads
// _tail to see the new slot data. Symmetric for the consumer side.
//
// Capacity is rounded up to the next power of two for efficient mask-based
// indexing. One slot is reserved as a sentinel to distinguish full from empty,
// so usable capacity is (rounded_capacity - 1).

template<typename T>
class SpscRingBuffer {
public:
    explicit SpscRingBuffer(size_t capacity)
        : _capacity(capacity)
        , _mask(capacity - 1)
        , _slots(std::make_unique<Slot[]>(capacity)) {
        // Capacity must be a power of two for mask-based indexing.
        // Round up if needed — caller provides desired capacity, we find
        // the next power of two >= that value.
        if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
            size_t p = 1;
            while (p < capacity) p <<= 1;
            _capacity = p;
            _mask = p - 1;
            _slots = std::make_unique<Slot[]>(p);
        }
    }

    // Producer only (reactor thread). Returns false if buffer is full.
    bool try_push(T&& item) {
        const size_t tail = _tail.load(std::memory_order_relaxed);
        const size_t next_tail = (tail + 1) & _mask;
        if (next_tail == _head.load(std::memory_order_acquire)) {
            return false;  // Full
        }
        _slots[tail].value = std::move(item);
        _tail.store(next_tail, std::memory_order_release);
        return true;
    }

    // Consumer only (worker thread). Returns false if buffer is empty.
    bool try_pop(T& item) {
        const size_t head = _head.load(std::memory_order_relaxed);
        if (head == _tail.load(std::memory_order_acquire)) {
            return false;  // Empty
        }
        item = std::move(_slots[head].value);
        _head.store((head + 1) & _mask, std::memory_order_release);
        return true;
    }

    // Consumer only. Drains up to max_count elements into output vector.
    // Returns number of elements drained.
    size_t drain(std::vector<T>& output, size_t max_count) {
        const size_t head = _head.load(std::memory_order_relaxed);
        const size_t tail = _tail.load(std::memory_order_acquire);
        const size_t available = (tail - head) & _mask;
        // When tail == head the ring is empty; available will be 0.
        // Note: usable capacity is _capacity - 1 (one slot is sentinel).
        const size_t count = std::min(available, max_count);
        output.reserve(output.size() + count);
        size_t h = head;
        for (size_t i = 0; i < count; ++i) {
            output.push_back(std::move(_slots[h].value));
            h = (h + 1) & _mask;
        }
        _head.store(h, std::memory_order_release);
        return count;
    }

    // Consumer only. Drains all elements.
    size_t drain_all(std::vector<T>& output) {
        return drain(output, _capacity);
    }

    // Lock-free size estimate. May be momentarily stale but safe to call
    // from any thread. Used for metrics/backpressure checks.
    size_t size_approx() const {
        const size_t tail = _tail.load(std::memory_order_relaxed);
        const size_t head = _head.load(std::memory_order_relaxed);
        return (tail - head) & _mask;
    }

    // Usable capacity (one slot reserved as sentinel for full/empty distinction).
    size_t capacity() const { return _capacity - 1; }

private:
    struct Slot {
        T value{};
    };

    size_t _capacity;
    size_t _mask;
    std::unique_ptr<Slot[]> _slots;

    // Placed on separate cache lines to avoid false sharing between
    // producer (reactor) and consumer (worker) threads.
    alignas(64) std::atomic<size_t> _head{0};
    alignas(64) std::atomic<size_t> _tail{0};
};

} // namespace ranvier
