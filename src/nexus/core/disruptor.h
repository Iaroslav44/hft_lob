#pragma once
#include <atomic>
#include <bit>
#include <cstddef>
#include <vector>
#include <cstdint>
#include <limits>
#include <thread>
#include "nexus/core/platform.h"

namespace nexus::core {

using hft::kCacheLine;
using hft::cpu_relax;

// Exactly one producer and one consumer own this ring. The producer must finish
// next() -> get(sequence) -> publish(sequence) before claiming another sequence.
// The consumer processes sequences in order, acquiring availability before
// access and releasing each sequence only after every access has finished.
// Sequences wrap modulo 2^64; signed comparisons do not establish availability.
template <typename T, std::size_t Capacity>
class DisruptorRingBuffer {
    static_assert(Capacity >= 1 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a nonzero power of two");
    static_assert(Capacity <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()),
                  "Capacity must be smaller than half the sequence space");
    static_assert(std::atomic<int64_t>::is_always_lock_free);
    alignas(kCacheLine) T buffer_[Capacity];
    alignas(kCacheLine) std::atomic<int64_t> cursor_{-1};
    alignas(kCacheLine) std::atomic<int64_t> consumer_sequence_{-1};

public:
    DisruptorRingBuffer() = default;

    int64_t next() {
        const std::uint64_t next_seq =
            static_cast<std::uint64_t>(cursor_.load(std::memory_order_relaxed)) + 1;
        // Modular distance remains bounded across signed and unsigned rollover.
        // Acquire pairs with reclamation before the producer may reuse this slot.
        while (next_seq - static_cast<std::uint64_t>(
                   consumer_sequence_.load(std::memory_order_acquire)) > Capacity) {
            cpu_relax();
        }
        return std::bit_cast<int64_t>(next_seq);
    }

    void publish(int64_t seq) {
        cursor_.store(seq, std::memory_order_release);
    }

    T& get(int64_t seq) {
        return buffer_[seq & (Capacity - 1)];
    }

    const T& get(int64_t seq) const {
        return buffer_[seq & (Capacity - 1)];
    }

    int64_t get_cursor() const {
        return cursor_.load(std::memory_order_acquire);
    }

    // Query only the next unconsumed sequence, never an already reclaimed slot.
    // A published sequence trails the cursor by at most Capacity - 1; a cursor
    // still preceding that sequence has a modular distance outside this range.
    [[nodiscard]] bool is_published(int64_t seq) const {
        const auto cursor = static_cast<std::uint64_t>(get_cursor());
        return cursor - static_cast<std::uint64_t>(seq) < Capacity;
    }

    void set_consumer_sequence(int64_t seq) {
        consumer_sequence_.store(seq, std::memory_order_release);
    }
};

} // namespace nexus::core
