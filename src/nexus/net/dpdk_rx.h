#pragma once
#include <cstdint>

// Forward declarations for DPDK structs to avoid heavy #include <rte_ethdev.h> everywhere
struct rte_mbuf;

namespace nexus::net {

class DpdkRxPoller {
    std::uint16_t port_id_;
    std::uint16_t queue_id_;

public:
    DpdkRxPoller(std::uint16_t port_id, std::uint16_t queue_id)
        : port_id_(port_id), queue_id_(queue_id) {}

    // Actual DPDK burst receive logic (assuming DPDK is linked)
    // extern "C" uint16_t rte_eth_rx_burst(uint16_t port_id, uint16_t queue_id, struct rte_mbuf **rx_pkts, uint16_t nb_pkts);
    //
    // template<typename Handler>
    // inline void poll(Handler&& handler) {
    //     rte_mbuf* pkts[32];
    //     uint16_t nb_rx = rte_eth_rx_burst(port_id_, queue_id_, pkts, 32);
    //     for (uint16_t i = 0; i < nb_rx; i++) {
    //         handler(rte_pktmbuf_mtod(pkts[i], const char*), rte_pktmbuf_data_len(pkts[i]));
    //         rte_pktmbuf_free(pkts[i]);
    //     }
    // }
};

} // namespace nexus::net
