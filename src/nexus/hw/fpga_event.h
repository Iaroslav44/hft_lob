#pragma once

#include <cstddef>
#include <cstdint>

#include "nexus/md/itch_parser.h"

namespace nexus::hw {

// DMA records use a fixed little-endian wire layout independent of host endianness.
// The caller owns a completed, CPU-readable buffer and has performed any required
// driver-specific DMA synchronization. This decoder does not read device MMIO.
inline constexpr std::size_t kFpgaAddOrderBytes = 32;
static_assert(sizeof(hft::MdAddOrder) == kFpgaAddOrderBytes);
static_assert(offsetof(hft::MdAddOrder, order_id) == 0);
static_assert(offsetof(hft::MdAddOrder, ts_ns) == 8);
static_assert(offsetof(hft::MdAddOrder, price) == 16);
static_assert(offsetof(hft::MdAddOrder, qty) == 20);
static_assert(offsetof(hft::MdAddOrder, locate) == 24);
static_assert(offsetof(hft::MdAddOrder, is_buy) == 26);

namespace detail {
template <class T>
[[nodiscard]] inline T read_event_le(const std::uint8_t* bytes) noexcept {
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(static_cast<T>(bytes[i]) << (8 * i));
    }
    return value;
}
}  // namespace detail

// Failure leaves the output unchanged. Reserved bytes must be zero so that a
// future wire format cannot be accepted silently as the current record type.
[[nodiscard]] inline bool decode_fpga_add_order(
    const void* data, std::size_t bytes, hft::MdAddOrder& output) noexcept {
    if (data == nullptr || bytes != kFpgaAddOrderBytes) return false;
    const auto* wire = static_cast<const std::uint8_t*>(data);
    if (wire[26] > 1 || wire[27] || wire[28] || wire[29] || wire[30] || wire[31])
        return false;

    hft::MdAddOrder decoded{};
    decoded.order_id = detail::read_event_le<std::uint64_t>(wire);
    decoded.ts_ns = detail::read_event_le<std::uint64_t>(wire + 8);
    decoded.price = detail::read_event_le<std::uint32_t>(wire + 16);
    decoded.qty = detail::read_event_le<std::uint32_t>(wire + 20);
    decoded.locate = detail::read_event_le<std::uint16_t>(wire + 24);
    decoded.is_buy = wire[26];
    if (decoded.qty == 0 || (decoded.ts_ns >> 48) != 0) return false;
    output = decoded;
    return true;
}

}  // namespace nexus::hw
