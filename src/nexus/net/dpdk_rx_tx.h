#pragma once
#include "dpdk_rx.h"

namespace nexus::net {

class DpdkRxTx : public DpdkRxPoller {
public:
    DpdkRxTx(std::uint16_t port_id, std::uint16_t queue_id) : DpdkRxPoller(port_id, queue_id) {}
    
    // extern "C" uint16_t rte_eth_tx_burst(uint16_t port_id, uint16_t queue_id, struct rte_mbuf **tx_pkts, uint16_t nb_pkts);
};

} // namespace nexus::net
