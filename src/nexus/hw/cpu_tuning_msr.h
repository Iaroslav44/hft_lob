#pragma once
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace nexus::hw {

// Direct MSR tuning via /dev/cpu/CPUNUM/msr to lock Turbo Boost or set energy performance bias
inline bool write_cpu_msr(int cpu, std::uint32_t msr, std::uint64_t val) {
#if defined(__linux__)
    char path[64];
    snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return false;
    ssize_t res = pwrite(fd, &val, sizeof(val), msr);
    close(fd);
    return res == sizeof(val);
#else
    (void)cpu; (void)msr; (void)val;
    return false;
#endif
}

} // namespace nexus::hw
