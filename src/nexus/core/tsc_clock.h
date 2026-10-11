#pragma once
#include <cstdint>
#include <time.h>
#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace nexus::hw {

class TscClock {
    double tsc_to_ns_ = 1.0;
public:
    TscClock() { calibrate(); }

    [[nodiscard]] inline std::uint64_t rdtsc() const noexcept {
#if defined(__x86_64__)
        unsigned int aux;
        return __rdtscp(&aux);
#else
        return 0;
#endif
    }

    [[nodiscard]] inline std::uint64_t to_ns(std::uint64_t tsc) const noexcept {
        return static_cast<std::uint64_t>(tsc * tsc_to_ns_);
    }

private:
    void calibrate() {
#if defined(__x86_64__)
        struct timespec ts1, ts2;
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts1);
        std::uint64_t t1 = rdtsc();
        struct timespec req = {0, 10000000}; // 10ms
        nanosleep(&req, nullptr);
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts2);
        std::uint64_t t2 = rdtsc();

        std::uint64_t ns1 = ts1.tv_sec * 1000000000ULL + ts1.tv_nsec;
        std::uint64_t ns2 = ts2.tv_sec * 1000000000ULL + ts2.tv_nsec;
        
        if (t2 > t1 && ns2 > ns1) {
            tsc_to_ns_ = static_cast<double>(ns2 - ns1) / static_cast<double>(t2 - t1);
        }
#endif
    }
};
} // namespace nexus::hw
