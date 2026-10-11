#pragma once
#include <cstdint>

// Forward declarations for Solarflare EFVI
struct ef_vi;
struct ef_driver_data;

namespace nexus::net {

class EfviRxPoller {
    ef_vi* vi_;

public:
    explicit EfviRxPoller(ef_vi* vi) : vi_(vi) {}

    // Actual EFVI polling logic
    // template<typename Handler>
    // inline void poll(Handler&& handler) {
    //     ef_event evs[32];
    //     int n_ev = ef_eventq_poll(vi_, evs, 32);
    //     for (int i = 0; i < n_ev; ++i) {
    //         if (EF_EVENT_TYPE(evs[i]) == EF_EVENT_TYPE_RX) {
    //             uint32_t id = EF_EVENT_RX_RQ_ID(evs[i]);
    //             const char* data = get_buffer(id);
    //             uint32_t len = EF_EVENT_RX_BYTES(evs[i]);
    //             handler(data, len);
    //             ef_vi_receive_post(vi_, id, id);
    //         }
    //     }
    // }
};

} // namespace nexus::net
