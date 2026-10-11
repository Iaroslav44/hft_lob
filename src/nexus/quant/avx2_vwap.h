#pragma once
#include <cstdint>
#include <cstddef>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace nexus::quant {

// Calculate VWAP using purely integer math to prevent precision loss.
inline std::uint64_t calculate_vwap_int_avx2(const std::uint32_t* prices, const std::uint32_t* volumes, std::size_t count) noexcept {
    std::uint64_t total_value = 0;
    std::uint64_t total_volume = 0;
    std::size_t i = 0;

#if defined(__AVX2__)
    __m256i sum_val_lo = _mm256_setzero_si256();
    __m256i sum_val_hi = _mm256_setzero_si256();
    __m256i sum_vol = _mm256_setzero_si256();

    for (; i + 8 <= count; i += 8) {
        __m256i p = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(prices + i));
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(volumes + i));
        
        sum_vol = _mm256_add_epi32(sum_vol, v);

        // multiply 32-bit x 32-bit to 64-bit
        __m256i mul_lo = _mm256_mul_epu32(p, v);
        __m256i p_shuf = _mm256_srli_epi64(p, 32);
        __m256i v_shuf = _mm256_srli_epi64(v, 32);
        __m256i mul_hi = _mm256_mul_epu32(p_shuf, v_shuf);

        sum_val_lo = _mm256_add_epi64(sum_val_lo, mul_lo);
        sum_val_hi = _mm256_add_epi64(sum_val_hi, mul_hi);
    }

    alignas(32) std::uint64_t val_arr[4];
    alignas(32) std::uint64_t val_hi_arr[4];
    alignas(32) std::uint32_t vol_arr[8];
    _mm256_store_si256(reinterpret_cast<__m256i*>(val_arr), sum_val_lo);
    _mm256_store_si256(reinterpret_cast<__m256i*>(val_hi_arr), sum_val_hi);
    _mm256_store_si256(reinterpret_cast<__m256i*>(vol_arr), sum_vol);

    for (int j = 0; j < 4; ++j) total_value += val_arr[j] + val_hi_arr[j];
    for (int j = 0; j < 8; ++j) total_volume += vol_arr[j];
#endif

    for (; i < count; ++i) {
        total_value += static_cast<std::uint64_t>(prices[i]) * volumes[i];
        total_volume += volumes[i];
    }

    return total_volume > 0 ? total_value / total_volume : 0;
}

} // namespace nexus::quant
