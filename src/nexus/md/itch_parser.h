#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "nexus/core/platform.h"

namespace hft {

// NASDAQ TotalView-ITCH 5.0 Add Order ('A'), exactly 36 wire bytes.
// This structure defines offsets only; do not cast a receive buffer to it.
// Multibyte fields use big-endian encoding.
#pragma pack(push, 1)
struct ItchAddOrder {
    char          message_type;
    std::uint16_t stock_locate;
    std::uint16_t tracking_number;
    std::uint8_t  timestamp_nanoseconds[6];
    std::uint64_t order_reference_number;
    char          buy_sell_indicator;
    std::uint32_t shares;
    char          stock[8];
    std::uint32_t price;
};
#pragma pack(pop)
static_assert(sizeof(ItchAddOrder) == 36);
static_assert(offsetof(ItchAddOrder, stock_locate) == 1);
static_assert(offsetof(ItchAddOrder, timestamp_nanoseconds) == 5);
static_assert(offsetof(ItchAddOrder, order_reference_number) == 11);
static_assert(offsetof(ItchAddOrder, buy_sell_indicator) == 19);
static_assert(offsetof(ItchAddOrder, shares) == 20);
static_assert(offsetof(ItchAddOrder, stock) == 24);
static_assert(offsetof(ItchAddOrder, price) == 32);

// Host-endian message. The 32-byte alignment keeps each queue element
// within a single 64-byte cache line.
struct alignas(32) MdAddOrder {
    std::uint64_t order_id;
    std::uint64_t ts_ns;
    std::uint32_t price;
    std::uint32_t qty;
    std::uint16_t locate;
    std::uint8_t  is_buy;
};
static_assert(sizeof(MdAddOrder) == 32);
static_assert(std::is_trivially_copyable_v<MdAddOrder>);

// Fast path for a validated Add Order: p addresses at least 36 readable bytes,
// the side is 'B' or 'S', and the quantity is nonzero.
// memcpy-based loads permit unaligned input without violating aliasing rules.
// The input range and out must not overlap.
HFT_ALWAYS_INLINE void decode_add_order(const char* p, MdAddOrder& out) noexcept {
    // The 8-byte timestamp load stays inside the message; discard the two ID bytes.
    out.locate   = load_be<std::uint16_t>(p + offsetof(ItchAddOrder, stock_locate));
    out.ts_ns    = load_be<std::uint64_t>(p + offsetof(ItchAddOrder, timestamp_nanoseconds)) >> 16;
    out.order_id = load_be<std::uint64_t>(p + offsetof(ItchAddOrder, order_reference_number));
    out.is_buy   = static_cast<std::uint8_t>(p[offsetof(ItchAddOrder, buy_sell_indicator)] == 'B');
    out.qty      = load_be<std::uint32_t>(p + offsetof(ItchAddOrder, shares));
    out.price    = load_be<std::uint32_t>(p + offsetof(ItchAddOrder, price));
}

// Validate the buffer extent and supported message fields, leaving out unchanged
// on failure. Only the first message is read; transport framing is the caller's
// responsibility. The input range and out must not overlap.
[[nodiscard]] HFT_ALWAYS_INLINE bool parse_itch_add_order(
    const char* p, std::size_t bytes, MdAddOrder& out) noexcept {
    if (p == nullptr || bytes < sizeof(ItchAddOrder)) [[unlikely]] return false;
    if (p[0] != 'A') [[unlikely]] return false;
    const char side = p[offsetof(ItchAddOrder, buy_sell_indicator)];
    if (side != 'B' && side != 'S') [[unlikely]] return false;
    if (load_be<std::uint32_t>(p + offsetof(ItchAddOrder, shares)) == 0) [[unlikely]]
        return false;
    decode_add_order(p, out);
    return true;
}

// Test encoder. Preconditions: p addresses at least 36 writable bytes,
// ts_ns fits in 48 bits, and shares is nonzero.
inline void encode_add_order(char* p, std::uint16_t locate, std::uint64_t ts_ns,
                             std::uint64_t order_id, bool is_buy, std::uint32_t shares,
                             const char (&stock)[8], std::uint32_t price) noexcept {
    p[0] = 'A';
    store_be<std::uint16_t>(p + offsetof(ItchAddOrder, stock_locate), locate);
    store_be<std::uint16_t>(p + offsetof(ItchAddOrder, tracking_number), 0);
    store_be48(p + offsetof(ItchAddOrder, timestamp_nanoseconds), ts_ns);
    store_be<std::uint64_t>(p + offsetof(ItchAddOrder, order_reference_number), order_id);
    p[offsetof(ItchAddOrder, buy_sell_indicator)] = is_buy ? 'B' : 'S';
    store_be<std::uint32_t>(p + offsetof(ItchAddOrder, shares), shares);
    std::memcpy(p + offsetof(ItchAddOrder, stock), stock, 8);
    store_be<std::uint32_t>(p + offsetof(ItchAddOrder, price), price);
}

}  // namespace hft
