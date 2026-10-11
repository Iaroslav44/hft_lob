#pragma once
#include <cstdint>
#include <cstddef>

namespace nexus::hw {

// CPU cannot emit raw TLPs directly. It interacts with the PCIe Root Complex
// via Memory-Mapped I/O (MMIO) to ring doorbells and signal the FPGA DMA subsystem.
class PcieDoorbell {
    volatile uint32_t* bar_ptr_;

public:
    explicit PcieDoorbell(void* mapped_bar)
        : bar_ptr_(static_cast<volatile uint32_t*>(mapped_bar)) {}

    // Ring doorbell register to notify FPGA that new descriptors/packets are ready
    void ring(uint32_t queue_id, uint32_t tail_index) noexcept {
        if (!bar_ptr_) return;
        bar_ptr_[queue_id] = tail_index;
    }
};

} // namespace nexus::hw
