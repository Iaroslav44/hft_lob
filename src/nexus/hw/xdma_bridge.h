#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

namespace nexus::hw {

// Owns the XDMA character-device descriptors. Buffer allocation, DMA mapping and
// coherence are supplied by the caller and driver, not established by this class.
class XdmaBridge {
    int c2h_fd_ = -1;
    int h2c_fd_ = -1;

public:
    XdmaBridge(const char* c2h_dev, const char* h2c_dev) {
        c2h_fd_ = open(c2h_dev, O_RDONLY);
        h2c_fd_ = open(h2c_dev, O_WRONLY);
    }

    XdmaBridge(const XdmaBridge&) = delete;
    XdmaBridge& operator=(const XdmaBridge&) = delete;

    ~XdmaBridge() {
        if (c2h_fd_ >= 0) close(c2h_fd_);
        if (h2c_fd_ >= 0) close(h2c_fd_);
    }

    // Returns the driver's byte count; the caller must handle errors and short I/O.
    ssize_t read_fpga(uint64_t fpga_addr, void* dest, size_t size) {
        if (c2h_fd_ < 0 || dest == nullptr) return -1;
        return pread(c2h_fd_, dest, size, fpga_addr);
    }

    // The address is a driver-defined device offset, not a host physical address.
    ssize_t write_fpga(uint64_t fpga_addr, const void* src, size_t size) {
        if (h2c_fd_ < 0 || src == nullptr) return -1;
        return pwrite(h2c_fd_, src, size, fpga_addr);
    }
};

} // namespace nexus::hw
