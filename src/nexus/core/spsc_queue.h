#pragma once

#include <atomic>
#include <cstddef>
#include <limits>
#include <type_traits>
#include <utility>

#include "nexus/core/platform.h"

namespace hft {

// A bounded single-producer, single-consumer queue with all Capacity slots usable.
// Unsigned counters may wrap: their distance never exceeds Capacity, and a mask
// selects the slot. Queue operations never wait for the other thread.
// Callbacks must not retain slot references or re-enter the same queue endpoint.
// Both threads must finish accessing the queue before its destruction.
template <typename T, std::size_t Capacity>
class SPSCQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two and at least two");
    // Trivial default construction permits implicit object creation by aligned_alloc
    // in C++20. No constructors or destructors run on the hot path.
    static_assert(std::is_trivially_default_constructible_v<T> &&
                  std::is_trivially_copyable_v<T> && std::is_trivially_copy_assignable_v<T> &&
                  std::is_trivially_destructible_v<T> &&
                  std::is_same_v<T, std::remove_cv_t<T>>,
                  "T must be unqualified, trivially default constructible, and trivially copyable");
    static_assert(alignof(T) <= kHugePage, "Slot alignment exceeds buffer alignment");
    static_assert(Capacity <= std::numeric_limits<std::size_t>::max() / sizeof(T),
                  "Buffer size overflows size_t");
    static_assert(std::atomic<std::size_t>::is_always_lock_free);

    static constexpr std::size_t kMask = Capacity - 1;

public:
    SPSCQueue()
        : buffer_prod_(static_cast<T*>(alloc_prefaulted(sizeof(T) * Capacity))),
          buffer_cons_(buffer_prod_) {}

    ~SPSCQueue() { free_prefaulted(buffer_prod_); }

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    [[nodiscard]] HFT_ALWAYS_INLINE bool push(const T& item) noexcept {
        return try_emplace([&item](T& slot) noexcept { slot = item; });
    }

    // Fill a slot in place and publish it immediately after the callback returns.
    // A full queue returns false without invoking the callback.
    template <class Writer>
    [[nodiscard]] HFT_ALWAYS_INLINE bool try_emplace(Writer&& write) noexcept {
        static_assert(std::is_nothrow_invocable_v<Writer&&, T&>,
                      "The writer callback must be noexcept");
        const std::size_t t = tail_.load(std::memory_order_relaxed);

        // Read the consumer's line only when the cached snapshot reports a full queue.
        if (t - head_cache_ == Capacity) [[unlikely]] {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (t - head_cache_ == Capacity) return false;
        }

        std::forward<Writer>(write)(buffer_prod_[t & kMask]);
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Fill up to max_count available slots, then publish the entire batch once.
    // The callback receives the slot and its zero-based offset within this batch.
    // A partial batch is valid; a zero request or a full queue invokes no callbacks.
    // Publication is deferred until all callbacks return, so they must not wait
    // for the consumer to observe this batch. Single-message APIs publish immediately.
    template <class Writer>
    [[nodiscard]] HFT_ALWAYS_INLINE std::size_t try_produce_batch(
        std::size_t max_count, Writer&& write) noexcept {
        static_assert(std::is_nothrow_invocable_v<Writer&, T&, std::size_t>,
                      "The batch writer callback must be noexcept");
        if (max_count == 0) return 0;
        const std::size_t requested = max_count < Capacity ? max_count : Capacity;
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        std::size_t available = Capacity - (t - head_cache_);
        if (available < requested) {
            head_cache_ = head_.load(std::memory_order_acquire);
            available = Capacity - (t - head_cache_);
        }
        const std::size_t count = requested < available ? requested : available;
        if (count == 0) return 0;

        T* const buffer = buffer_prod_;
        for (std::size_t i = 0; i < count; ++i) write(buffer[(t + i) & kMask], i);
        tail_.store(t + count, std::memory_order_release);
        return count;
    }

    [[nodiscard]] HFT_ALWAYS_INLINE bool pop(T& item) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);

        if (h == tail_cache_) [[unlikely]] {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cache_) return false;
        }

        item = buffer_cons_[h & kMask];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Drain the published snapshot observed by the acquire load.
    // One acquire/release pair per batch; references are valid only within the callback.
    template <class Fn>
    HFT_ALWAYS_INLINE std::size_t consume_all(Fn&& fn) noexcept {
        static_assert(std::is_nothrow_invocable_v<Fn&, const T&>,
                      "The consumer callback must be noexcept");
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_cache_ = tail_.load(std::memory_order_acquire);
        if (h == t) return 0;

        T* const buf = buffer_cons_;
        for (std::size_t i = h; i != t; ++i) fn(std::as_const(buf[i & kMask]));

        head_.store(t, std::memory_order_release);
        return t - h;
    }

private:
    // Producer-owned state; the consumer reads only tail_.
    alignas(kFalseSharingRange) std::atomic<std::size_t> tail_{0};
    std::size_t head_cache_{0};
    T* const buffer_prod_;

    // Consumer-owned state, including its own buffer pointer.
    alignas(kFalseSharingRange) std::atomic<std::size_t> head_{0};
    std::size_t tail_cache_{0};
    T* const buffer_cons_;
};

}  // namespace hft
