#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#if defined(__x86_64__)
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace nexus::hw {

// The region must contain live uint64_t objects in coherent, write-back memory.
// It must outlive the bridge and all accesses. Concurrent CPU accesses to a word
// must use atomic_ref. While any atomic_ref to a word exists, all CPU accesses
// to that word must use atomic_ref. This interface does not support MMIO.
// CLWB/SFENCE orders CPU write-back, not device consumption or acknowledgement.
// A device-specific ownership/notification protocol remains the caller's duty.
class CxlMemoryBridge {
    using AtomicWord = std::atomic_ref<std::uint64_t>;
    static_assert(AtomicWord::is_always_lock_free);

    void* addr_;
    std::size_t size_;

    static void write_back(void* address) noexcept {
#if defined(__x86_64__)
        // The constructor checks CPUID. Inline assembly also permits a baseline
        // x86-64 build without enabling CLWB for unrelated code or adding a call.
        asm volatile("clwb {(%0)|[%0]}" : : "r"(address) : "memory");
        _mm_sfence();
#else
        (void)address;
#endif
    }

public:
    // Invalid regions and unsupported CPUs produce an inactive bridge; writes
    // then fail without touching memory. Capability detection runs at setup only.
    CxlMemoryBridge(void* addr, std::size_t size) noexcept : addr_(addr), size_(size) {
        if (addr == nullptr || size < sizeof(std::uint64_t) ||
            reinterpret_cast<std::uintptr_t>(addr) % AtomicWord::required_alignment != 0 ||
            !is_supported()) {
            addr_ = nullptr;
            size_ = 0;
        }
    }

    [[nodiscard]] static bool is_supported() noexcept {
#if defined(__x86_64__)
        unsigned int eax, ebx, ecx, edx;
        return __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) != 0 &&
               (ebx & bit_CLWB) != 0;
#else
        return false;
#endif
    }

    [[nodiscard]] bool valid() const noexcept { return addr_ != nullptr; }

    // Write back the cache line containing this byte; the mapping remains owned
    // by the caller. Success does not acknowledge a device-side read.
    [[nodiscard]] bool flush_to_device(std::size_t offset) const noexcept {
        if (!valid() || offset >= size_) return false;
        write_back(static_cast<char*>(addr_) + offset);
        return true;
    }

    [[nodiscard]] bool write_data(std::size_t offset, std::uint64_t data) const noexcept {
        // Subtraction after the bound check avoids overflow even for SIZE_MAX.
        if (!valid() || offset > size_ || size_ - offset < sizeof(std::uint64_t) ||
            offset % AtomicWord::required_alignment != 0) return false;
        auto* word = reinterpret_cast<std::uint64_t*>(static_cast<char*>(addr_) + offset);
        AtomicWord(*word).store(data, std::memory_order_release);
        write_back(word);
        return true;
    }
};
} // namespace nexus::hw
