#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "nexus/hw/fpga_event.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

bool same_fields(const hft::MdAddOrder& a, const hft::MdAddOrder& b) {
    return a.order_id == b.order_id && a.ts_ns == b.ts_ns && a.price == b.price &&
           a.qty == b.qty && a.locate == b.locate && a.is_buy == b.is_buy;
}
}  // namespace

int main() {
    // Deliberately unaligned input with distinct byte values in every field.
    std::array<std::uint8_t, 33> storage = {
        0xee,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x81,
        0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0, 0,
        0x40, 0x30, 0x20, 0x10, 0x78, 0x56, 0x34, 0x12,
        0xbc, 0x9a, 1, 0, 0, 0, 0, 0
    };
    auto* wire = storage.data() + 1;
    hft::MdAddOrder result{};
    require(nexus::hw::decode_fpga_add_order(wire, 32, result), "Valid DMA record rejected");
    require(result.order_id == UINT64_C(0x8102030405060708), "Order ID byte order");
    require(result.ts_ns == UINT64_C(0x112233445566), "Timestamp byte order");
    require(result.price == UINT32_C(0x10203040) && result.qty == UINT32_C(0x12345678), "Price/quantity byte order");
    require(result.locate == UINT16_C(0x9abc) && result.is_buy == 1, "Locate/side byte order");
    const hft::MdAddOrder saved = result;

    for (std::size_t bytes : {std::size_t{0}, std::size_t{31}, std::size_t{33}}) {
        require(!nexus::hw::decode_fpga_add_order(wire, bytes, result), "Invalid record extent accepted");
        require(same_fields(result, saved), "Failed decode modified output");
    }
    require(!nexus::hw::decode_fpga_add_order(nullptr, 32, result), "Null record accepted");
    for (std::size_t reserved = 27; reserved < 32; ++reserved) {
        wire[reserved] = 1;
        require(!nexus::hw::decode_fpga_add_order(wire, 32, result), "Reserved byte accepted");
        wire[reserved] = 0;
    }
    wire[26] = 2;
    require(!nexus::hw::decode_fpga_add_order(wire, 32, result), "Invalid side accepted");
    wire[26] = 0;
    wire[14] = 1;
    require(!nexus::hw::decode_fpga_add_order(wire, 32, result), "Timestamp exceeds 48 bits");
    wire[14] = 0;
    for (std::size_t i = 20; i < 24; ++i) wire[i] = 0;
    require(!nexus::hw::decode_fpga_add_order(wire, 32, result), "Zero quantity accepted");
    require(same_fields(result, saved), "Rejected payload modified output");
    wire[20] = 1;
    require(nexus::hw::decode_fpga_add_order(wire, 32, result) && result.is_buy == 0,
            "Valid sell record rejected");
    return 0;
}
