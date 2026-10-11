#pragma once
#include <cstdint>

// Forward declarations for libbpf/xsk
struct xsk_ring_cons;
struct xsk_ring_prod;
struct xsk_umem;

namespace nexus::net {

class AfXdpPoller {
    xsk_ring_cons* rx_ring_;
    xsk_ring_prod* fq_ring_;
    xsk_umem* umem_;

public:
    AfXdpPoller(xsk_ring_cons* rx, xsk_ring_prod* fq, xsk_umem* umem) 
        : rx_ring_(rx), fq_ring_(fq), umem_(umem) {}

    // Proper AF_XDP logic pulling from rx_ring_, NOT completion ring
    // template<typename Handler>
    // inline void poll(Handler&& handler) {
    //     uint32_t idx_rx;
    //     uint32_t rcvd = xsk_ring_cons__peek(rx_ring_, 32, &idx_rx);
    //     if (!rcvd) return;
    //
    //     for (uint32_t i = 0; i < rcvd; i++) {
    //         const struct xdp_desc *desc = xsk_ring_cons__rx_desc(rx_ring_, idx_rx++);
    //         const char* data = static_cast<const char*>(xsk_umem__get_data(umem_->buffer, desc->addr));
    //         handler(data, desc->len);
    //     }
    //     xsk_ring_cons__release(rx_ring_, rcvd);
    // }
};

} // namespace nexus::net
