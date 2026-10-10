#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <type_traits>

#include "platform.h"

namespace hft {

inline constexpr std::size_t MAX_PRICE_LEVELS = 100000;
inline constexpr std::size_t MAX_ORDERS = 1000000;

// Two nodes fit in a 64-byte cache line. Links are pool indices;
// index zero terminates a list.
struct alignas(32) Order {
    std::uint64_t id;
    std::uint32_t price;
    std::uint32_t qty;
    std::uint32_t prev_order_idx;
    std::uint32_t next_order_idx;
    bool is_buy;
};
static_assert(sizeof(Order) == 32);
static_assert(std::is_trivially_copyable_v<Order>);

// Single-instrument, single-threaded book with FIFO queues at each price level.
// A pool index is valid until its order is released and may then be reused;
// callers must not retain stale indices. The feed handler owns exchange ID
// uniqueness and the mapping from IDs to pool indices. Exchange ID zero is valid.
class OrderBook {
    struct PriceLevel {
        std::uint32_t head;
        std::uint32_t tail;
    };

public:
    // The price window starts at base_price and spans at most MAX_PRICE_LEVELS
    // values, clipped at UINT32_MAX. The pool and level metadata share one
    // allocation whose pages are touched before use.
    explicit OrderBook(std::uint32_t base_price = 0,
                       std::uint32_t capacity = static_cast<std::uint32_t>(MAX_ORDERS - 1))
        : orders_(allocate_storage(capacity)),
          levels_(::new (static_cast<void*>(orders_ + static_cast<std::size_t>(capacity) + 1))
                      PriceLevel[2 * MAX_PRICE_LEVELS]{}),
          base_price_(base_price), capacity_(capacity) {}

    ~OrderBook() { free_prefaulted(orders_); }

    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // Return zero for an out-of-window price, zero quantity, or exhausted pool.
    // A rejected order leaves the book unchanged.
    [[nodiscard]] HFT_ALWAYS_INLINE std::uint32_t add_order(
        std::uint64_t id, std::uint32_t price, std::uint32_t qty, bool is_buy) noexcept {
        if (qty == 0 || !contains_price(price)) [[unlikely]] return 0;

        std::uint32_t idx;
        if (free_head_ != 0) {
            // Reuse released nodes before growing the working set.
            idx = free_head_;
            free_head_ = orders_[idx].next_order_idx;
        } else if (next_free_ <= capacity_) [[likely]] {
            idx = next_free_++;
        } else [[unlikely]] {
            return 0;
        }

        PriceLevel& level = level_for(price, is_buy);
        orders_[idx] = Order{id, price, qty, level.tail, 0, is_buy};
        if (level.tail != 0) {
            orders_[level.tail].next_order_idx = idx;
        } else {
            level.head = idx;
        }
        level.tail = idx;
        ++size_;
        return idx;
    }

    [[nodiscard]] HFT_ALWAYS_INLINE bool remove_order(std::uint32_t idx) noexcept {
        if (!is_live(idx)) [[unlikely]] return false;
        release_order(idx, orders_[idx]);
        return true;
    }

    // Partial execution preserves FIFO priority; full execution releases the node.
    // Reject zero or excessive quantities without modifying the order.
    [[nodiscard]] HFT_ALWAYS_INLINE bool execute_order(std::uint32_t idx,
                                                      std::uint32_t qty) noexcept {
        if (!is_live(idx) || qty == 0) [[unlikely]] return false;
        Order& order = orders_[idx];
        if (qty > order.qty) [[unlikely]] return false;
        if (qty == order.qty) {
            release_order(idx, order);
        } else {
            order.qty -= qty;
        }
        return true;
    }

    // The returned view remains valid until the corresponding node is modified.
    [[nodiscard]] HFT_ALWAYS_INLINE const Order* get_order(std::uint32_t idx) const noexcept {
        return is_live(idx) ? &orders_[idx] : nullptr;
    }

    [[nodiscard]] HFT_ALWAYS_INLINE std::uint32_t head(std::uint32_t price,
                                                     bool is_buy) const noexcept {
        return contains_price(price) ? level_for(price, is_buy).head : 0;
    }

    [[nodiscard]] HFT_ALWAYS_INLINE std::uint32_t tail(std::uint32_t price,
                                                     bool is_buy) const noexcept {
        return contains_price(price) ? level_for(price, is_buy).tail : 0;
    }

    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }

private:
    [[nodiscard]] static Order* allocate_storage(std::uint32_t capacity) {
        if (capacity >= MAX_ORDERS)
            throw std::invalid_argument("OrderBook capacity exceeds the pool limit");
        const std::size_t order_count = static_cast<std::size_t>(capacity) + 1;
        const std::size_t bytes = order_count * sizeof(Order) +
                                  2 * MAX_PRICE_LEVELS * sizeof(PriceLevel);
        void* const memory = alloc_prefaulted(bytes);
        // Start the array lifetime without writing the prefaulted pool again.
        return ::new (memory) Order[order_count];
    }

    [[nodiscard]] HFT_ALWAYS_INLINE bool contains_price(std::uint32_t price) const noexcept {
        // Widen before subtraction: a price below the base underflows outside the window.
        return static_cast<std::uint64_t>(price) - base_price_ < MAX_PRICE_LEVELS;
    }

    [[nodiscard]] HFT_ALWAYS_INLINE bool is_live(std::uint32_t idx) const noexcept {
        // next_free_ excludes nodes that have never been allocated.
        return idx != 0 && idx < next_free_ && orders_[idx].qty != 0;
    }

    [[nodiscard]] HFT_ALWAYS_INLINE PriceLevel& level_for(std::uint32_t price,
                                                        bool is_buy) noexcept {
        return levels_[(static_cast<std::uint64_t>(price) - base_price_) +
                       (is_buy ? 0 : MAX_PRICE_LEVELS)];
    }

    [[nodiscard]] HFT_ALWAYS_INLINE const PriceLevel& level_for(std::uint32_t price,
                                                              bool is_buy) const noexcept {
        return levels_[(static_cast<std::uint64_t>(price) - base_price_) +
                       (is_buy ? 0 : MAX_PRICE_LEVELS)];
    }

    HFT_ALWAYS_INLINE void release_order(std::uint32_t idx, Order& order) noexcept {
        PriceLevel& level = level_for(order.price, order.is_buy);
        if (order.prev_order_idx != 0)
            orders_[order.prev_order_idx].next_order_idx = order.next_order_idx;
        else
            level.head = order.next_order_idx;
        if (order.next_order_idx != 0)
            orders_[order.next_order_idx].prev_order_idx = order.prev_order_idx;
        else
            level.tail = order.prev_order_idx;

        order.qty = 0;
        order.prev_order_idx = 0;
        order.next_order_idx = free_head_;
        free_head_ = idx;
        --size_;
    }

    // Allocator state and storage pointers fit in one cache line.
    alignas(kCacheLine) Order* const orders_;
    PriceLevel* const levels_;
    const std::uint32_t base_price_;
    const std::uint32_t capacity_;
    std::uint32_t next_free_{1};
    std::uint32_t free_head_{0};
    std::uint32_t size_{0};
};

}  // namespace hft
