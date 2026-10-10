#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define HFT_X86 1
#endif

#if defined(__linux__)
#include <sys/mman.h>
#endif

#define HFT_ALWAYS_INLINE [[gnu::always_inline]] inline

namespace hft {

#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t kCacheLine = std::hardware_constructive_interference_size;
inline constexpr std::size_t kDestructive = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t kCacheLine = 64;
inline constexpr std::size_t kDestructive = 64;
#endif

// Separate thread-owned state by at least 128 bytes for adjacent-line prefetchers.
inline constexpr std::size_t kFalseSharingRange = kDestructive < 128 ? 128 : kDestructive;

HFT_ALWAYS_INLINE void cpu_relax() noexcept {
#if defined(HFT_X86)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_acq_rel);
#endif
}

// Wire loads require a uniformly little-endian or big-endian host.
static_assert(std::endian::native == std::endian::little ||
              std::endian::native == std::endian::big);

template <class T>
concept WireInteger = std::is_integral_v<T> && std::is_unsigned_v<T> &&
                      !std::is_same_v<T, bool> && std::is_same_v<T, std::remove_cv_t<T>> &&
                      (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);

template <WireInteger T>
HFT_ALWAYS_INLINE T bswap(T v) noexcept {
    if constexpr (sizeof(T) == 1) return v;
    else if constexpr (sizeof(T) == 2) return __builtin_bswap16(v);
    else if constexpr (sizeof(T) == 4) return __builtin_bswap32(v);
    else return __builtin_bswap64(v);
}

// memcpy permits unaligned access without violating aliasing rules.
template <WireInteger T>
HFT_ALWAYS_INLINE T load_be(const char* p) noexcept {
    T v;
    std::memcpy(&v, p, sizeof(T));
    if constexpr (std::endian::native == std::endian::little) v = bswap(v);
    return v;
}

template <WireInteger T>
HFT_ALWAYS_INLINE void store_be(char* p, T v) noexcept {
    if constexpr (std::endian::native == std::endian::little) v = bswap(v);
    std::memcpy(p, &v, sizeof(T));
}

// A 48-bit ITCH timestamp requires two unaligned loads.
HFT_ALWAYS_INLINE std::uint64_t load_be48(const char* p) noexcept {
    return (std::uint64_t{load_be<std::uint16_t>(p)} << 32) | load_be<std::uint32_t>(p + 2);
}

// Store the least significant 48 bits.
HFT_ALWAYS_INLINE void store_be48(char* p, std::uint64_t v) noexcept {
    store_be<std::uint16_t>(p, static_cast<std::uint16_t>(v >> 32));
    store_be<std::uint32_t>(p + 2, static_cast<std::uint32_t>(v));
}

inline constexpr std::size_t kHugePage = std::size_t{2} << 20;

// Startup allocation: 2 MiB alignment, a THP hint, and writes to every page.
// THP remains advisory; these pages are not locked into physical memory.
[[nodiscard]] inline void* alloc_prefaulted(std::size_t bytes) {
    if (bytes == 0 || bytes > std::numeric_limits<std::size_t>::max() - (kHugePage - 1)) {
        throw std::bad_alloc();
    }
    bytes = (bytes + kHugePage - 1) & ~(kHugePage - 1);
    void* p = std::aligned_alloc(kHugePage, bytes);
    if (p == nullptr) throw std::bad_alloc();
#if defined(__linux__) && defined(MADV_HUGEPAGE)
    (void)::madvise(p, bytes, MADV_HUGEPAGE);
#endif
    std::memset(p, 0, bytes);
    // Keep the prefaulting writes observable to the compiler, including under LTO.
    asm volatile("" : : "r"(p) : "memory");
    return p;
}

inline void free_prefaulted(void* p) noexcept { std::free(p); }

}  // namespace hft
