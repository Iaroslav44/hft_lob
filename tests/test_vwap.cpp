#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>

#include "nexus/quant/avx2_vwap.h"

namespace {

using nexus::quant::calculate_vwap_int_avx2;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "%s:%d: check failed: %s\n",                    \
                         __FILE__, __LINE__, #condition);                        \
            std::abort();                                                       \
        }                                                                       \
    } while (false)

constexpr auto kMax = std::numeric_limits<std::uint32_t>::max();

// Use a scalar sum and a division-free search to check the floor independently
// of the production vector reduction and final integer division.
std::uint64_t oracle(const std::uint32_t* prices, const std::uint32_t* volumes,
                     std::size_t count) {
    using Wide = __uint128_t;
    Wide value = 0;
    Wide volume = 0;
    for (std::size_t i = 0; i < count; ++i) {
        value += static_cast<Wide>(prices[i]) * volumes[i];
        volume += volumes[i];
    }
    if (volume == 0) return 0;

    std::uint64_t low = 0;
    std::uint64_t high = std::uint64_t{kMax} + 1;
    while (high - low > 1) {
        const std::uint64_t middle = low + (high - low) / 2;
        if (static_cast<Wide>(middle) * volume <= value)
            low = middle;
        else
            high = middle;
    }
    return low;
}

void check_against_oracle(const std::uint32_t* prices, const std::uint32_t* volumes,
                          std::size_t count) {
    const auto expected = oracle(prices, volumes, count);
    const auto actual = calculate_vwap_int_avx2(prices, volumes, count);
    if (actual != expected) {
        std::fprintf(stderr, "VWAP mismatch for count=%zu: expected=%llu actual=%llu\n",
                     count, static_cast<unsigned long long>(expected),
                     static_cast<unsigned long long>(actual));
        std::abort();
    }
}

void test_zero_and_floor() {
    CHECK(calculate_vwap_int_avx2(nullptr, nullptr, 0) == 0);
    std::array<std::uint32_t, 25> prices{};
    std::array<std::uint32_t, 25> volumes{};
    prices.fill(kMax);
    for (std::size_t count = 0; count <= prices.size(); ++count)
        CHECK(calculate_vwap_int_avx2(prices.data(), volumes.data(), count) == 0);

    prices.fill(0);
    volumes.fill(kMax);
    CHECK(calculate_vwap_int_avx2(prices.data(), volumes.data(), prices.size()) == 0);

    const std::array<std::uint32_t, 3> small_prices{1, 2, kMax};
    const std::array<std::uint32_t, 3> small_volumes{1, 2, 0};
    CHECK(calculate_vwap_int_avx2(small_prices.data(), small_volumes.data(), 3) == 1);
    CHECK(calculate_vwap_int_avx2(&kMax, &kMax, 1) == kMax);
}

void test_overflow_counterexamples() {
    std::array<std::uint32_t, 40> prices{};
    std::array<std::uint32_t, 40> volumes{};

    // Two maximum products overflow a scalar 64-bit numerator.
    prices.fill(kMax);
    volumes.fill(kMax);
    CHECK(calculate_vwap_int_avx2(prices.data(), volumes.data(), 2) == kMax);

    // A single vector overflows a 64-bit horizontal reduction; later vectors
    // additionally overflow the old product and volume accumulators.
    for (const std::size_t count : {8U, 16U, 24U, 32U, 40U})
        CHECK(calculate_vwap_int_avx2(prices.data(), volumes.data(), count) == kMax);

    // Each of the eight volume lanes used to wrap independently to zero.
    prices.fill(23);
    for (std::size_t lane = 0; lane < 8; ++lane) {
        volumes.fill(0);
        volumes[lane] = std::uint32_t{1} << 31;
        volumes[lane + 8] = std::uint32_t{1} << 31;
        CHECK(calculate_vwap_int_avx2(prices.data(), volumes.data(), 16) == 23);
    }

    // Both sides of each even/odd product pair must remain unsigned.
    for (std::size_t lane = 0; lane < 8; ++lane) {
        prices.fill(0);
        volumes.fill(0);
        prices[lane] = kMax;
        volumes[lane] = kMax;
        CHECK(calculate_vwap_int_avx2(prices.data(), volumes.data(), 8) == kMax);
    }

    // Exercise every scalar remainder after one and two complete vectors.
    prices.fill(kMax);
    for (const std::size_t prefix : {8U, 16U}) {
        for (std::size_t tail = 1; tail < 8; ++tail) {
            volumes.fill(0);
            for (std::size_t i = 0; i < tail; ++i) volumes[prefix + i] = kMax;
            CHECK(calculate_vwap_int_avx2(prices.data(), volumes.data(),
                                          prefix + tail) == kMax);
        }
    }
}

std::uint32_t random_word(std::uint64_t& state) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return static_cast<std::uint32_t>((state * 0x2545f4914f6cdd1dULL) >> 32);
}

void test_lengths_alignment_and_oracle() {
    constexpr std::size_t kCount = 1032;
    alignas(32) std::array<std::uint32_t, kCount> prices{};
    alignas(32) std::array<std::uint32_t, kCount> volumes{};
    std::uint64_t state = 0x8479a32b4916f20dULL;

    for (unsigned trial = 0; trial < 12; ++trial) {
        for (std::size_t i = 0; i < kCount; ++i) {
            prices[i] = random_word(state);
            volumes[i] = random_word(state);
            if (i % 11 == 0) prices[i] = kMax;
            if (i % 13 == 0) volumes[i] = kMax;
            if (i % 17 == 0) volumes[i] = 0;
        }
        for (std::size_t price_offset = 0; price_offset < 8; ++price_offset) {
            for (std::size_t volume_offset = 0; volume_offset < 8; ++volume_offset) {
                for (std::size_t count = 0; count <= 33; ++count) {
                    check_against_oracle(prices.data() + price_offset,
                                         volumes.data() + volume_offset, count);
                }
                for (const std::size_t count : {63U, 64U, 65U, 255U, 256U, 257U, 1025U}) {
                    check_against_oracle(prices.data() + price_offset,
                                         volumes.data() + volume_offset, count);
                }
            }
        }
    }
}

} // namespace

int main() {
    test_zero_and_floor();
    test_overflow_counterexamples();
    test_lengths_alignment_and_oracle();
    std::puts("VWAP tests passed");
}
