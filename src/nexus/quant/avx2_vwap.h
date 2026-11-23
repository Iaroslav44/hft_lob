#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace nexus::quant {

// Return floor(sum(price * volume) / sum(volume)), or zero for zero total volume.
// Both arrays contain count elements; null pointers are permitted when count is zero.
inline std::uint64_t calculate_vwap_int_avx2(const std::uint32_t* prices,
                                           const std::uint32_t* volumes,
                                           std::size_t count) noexcept {
    using Wide = __uint128_t;
    static_assert(std::numeric_limits<std::size_t>::digits <= 64,
                  "VWAP accumulation requires a count of at most 64 bits");

    // With 32-bit inputs and a 64-bit count, the numerator is strictly below
    // 2^128 and the denominator below 2^96. Widen before every potentially large sum.
    Wide total_value = 0;
    Wide total_volume = 0;
    std::size_t i = 0;

#if defined(__AVX2__)
    const __m256i low_word_mask = _mm256_set1_epi64x(0xffffffffLL);
    for (; count - i >= 8; i += 8) {
        const __m256i p = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(prices + i));
        const __m256i v = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(volumes + i));

        // AVX2 multiplies the even 32-bit lanes. Shift within each 64-bit lane
        // to obtain the odd products without changing their unsigned interpretation.
        const __m256i even_products = _mm256_mul_epu32(p, v);
        const __m256i odd_products = _mm256_mul_epu32(
            _mm256_srli_epi64(p, 32), _mm256_srli_epi64(v, 32));
        const __m256i paired_volumes = _mm256_add_epi64(
            _mm256_and_si256(v, low_word_mask), _mm256_srli_epi64(v, 32));

        alignas(32) std::uint64_t even[4];
        alignas(32) std::uint64_t odd[4];
        alignas(32) std::uint64_t volume_pairs[4];
        _mm256_store_si256(reinterpret_cast<__m256i*>(even), even_products);
        _mm256_store_si256(reinterpret_cast<__m256i*>(odd), odd_products);
        _mm256_store_si256(reinterpret_cast<__m256i*>(volume_pairs), paired_volumes);

        // A single product fits in 64 bits; two maximum products do not.
        // Reduce every vector before another iteration can overflow its lanes.
        std::uint64_t block_volume = 0;
        for (unsigned lane = 0; lane < 4; ++lane) {
            total_value += static_cast<Wide>(even[lane]) + odd[lane];
            block_volume += volume_pairs[lane];
        }
        total_volume += block_volume;
    }
#endif

    for (; i < count; ++i) {
        total_value += static_cast<Wide>(prices[i]) * volumes[i];
        total_volume += volumes[i];
    }

    return total_volume == 0 ? 0 :
        static_cast<std::uint64_t>(total_value / total_volume);
}

} // namespace nexus::quant
