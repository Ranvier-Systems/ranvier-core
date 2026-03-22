#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace ranvier {

// ============================================================================
// MPSC Ring Buffer
// ============================================================================
// Lock-free bounded multi-producer single-consumer ring buffer for move-only
// types. Multiple producer threads (reactor shards) CAS-compete for tail
// slots; a single consumer thread (flush timer / seastar::async worker)
// reads from head.
//
// Design based on Dmitry Vyukov's bounded MPMC queue, simplified for
// single-consumer. Each slot carries a sequence number that coordinates
// the producer-consumer handoff without per-slot mutexes:
//
//   - Producer: CAS-reserves a slot by advancing _tail, then writes data
//     and publishes via release-store on the slot's sequence number.
//   - Consumer: checks the slot's sequence number to confirm the producer
//     has finished writing before reading.
//
// Capacity is rounded up to the next power of two for efficient mask-based
// indexing. Usable capacity equals the rounded value (no sentinel slot
// needed — sequence numbers distinguish full from empty).

template<typename T>
class MpscRingBuffer {
public:
    explicit MpscRingBuffer(size_t capacity) {
        // Round up to next power of two.
        size_t p = 1;
        while (p < capacity) p <<= 1;
        _capacity = p;
        _mask = p - 1;
        _slots = std::make_unique<Slot[]>(p);

        // Initialize sequence numbers: slot[i].seq = i.
        // This means all slots start as "empty and ready for producer i to claim."
        for (size_t i = 0; i < p; ++i) {
            _slots[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    // Thread-safe for multiple producers. Returns false if buffer is full.
    bool try_push(T&& item) {
        size_t tail = _tail.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = _slots[tail & _mask];
            size_t seq = slot.seq.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(tail);

            if (diff == 0) {
                // Slot is ready for writing at this tail position.
                // Try to claim it by advancing _tail.
                if (_tail.compare_exchange_weak(tail, tail + 1,
                        std::memory_order_relaxed)) {
                    // Slot claimed. Write data and publish.
                    slot.value = std::move(item);
                    slot.seq.store(tail + 1, std::memory_order_release);
                    return true;
                }
                // CAS failed — another producer won. tail is updated, retry.
            } else if (diff < 0) {
                // Slot still occupied by consumer (hasn't been freed yet).
                // Buffer is full.
                return false;
            } else {
                // diff > 0: another producer already claimed this slot but
                // we're looking at a stale tail. Reload and retry.
                tail = _tail.load(std::memory_order_relaxed);
            }
        }
    }

    // Consumer only (single thread). Returns false if buffer is empty.
    bool try_pop(T& item) {
        Slot& slot = _slots[_head & _mask];
        size_t seq = slot.seq.load(std::memory_order_acquire);
        intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(_head + 1);

        if (diff < 0) {
            return false;  // Empty (producer hasn't written here yet)
        }

        // Slot is ready for reading.
        item = std::move(slot.value);
        slot.seq.store(_head + _capacity, std::memory_order_release);
        ++_head;
        _head_approx.store(_head, std::memory_order_relaxed);
        return true;
    }

    // Consumer only. Drains up to max_count elements into output vector.
    // Returns number of elements drained.
    size_t drain(std::vector<T>& output, size_t max_count) {
        size_t count = 0;
        // Pre-scan how many are ready to avoid per-element reserve.
        output.reserve(output.size() + std::min(max_count, _capacity));
        while (count < max_count) {
            Slot& slot = _slots[_head & _mask];
            size_t seq = slot.seq.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(_head + 1);

            if (diff < 0) {
                break;  // No more ready slots
            }

            output.push_back(std::move(slot.value));
            slot.seq.store(_head + _capacity, std::memory_order_release);
            ++_head;
            ++count;
        }
        if (count > 0) {
            _head_approx.store(_head, std::memory_order_relaxed);
        }
        return count;
    }

    // Consumer only. Drains all available elements.
    size_t drain_all(std::vector<T>& output) {
        return drain(output, _capacity);
    }

    // Lock-free size estimate. May be momentarily stale but safe to call
    // from any thread. Used for metrics/backpressure checks.
    size_t size_approx() const {
        size_t tail = _tail.load(std::memory_order_relaxed);
        size_t head = _head_approx.load(std::memory_order_relaxed);
        return (tail >= head) ? (tail - head) : 0;
    }

    // Usable capacity (all slots usable — sequence numbers handle full/empty).
    size_t capacity() const { return _capacity; }

private:
    struct Slot {
        std::atomic<size_t> seq{0};
        T value{};
    };

    size_t _capacity;
    size_t _mask;
    std::unique_ptr<Slot[]> _slots;

    // _tail is contended by multiple producers — CAS target.
    alignas(64) std::atomic<size_t> _tail{0};

    // _head is consumer-only (not atomic for the consumer's own use).
    // _head_approx is a relaxed mirror published for size_approx() readers.
    alignas(64) size_t _head{0};
    std::atomic<size_t> _head_approx{0};
};

} // namespace ranvier
