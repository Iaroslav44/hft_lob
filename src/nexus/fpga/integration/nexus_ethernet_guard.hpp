#pragma once

#include "nexus_ethz_toe.hpp"

using NexusEthernetBeat = ap_axiu<64, 1, 0, 0>;

// Frame bytes exclude the preamble and FCS. USER marks any MAC/CDC frame error.
// Admission waits for LAST, validates the supported header profile, and removes
// Ethernet padding before any byte reaches a stateful protocol engine.
// The enclosing MAC must honour backpressure or provide whole-frame buffering.
void nexus_ethernet_guard(hls::stream<NexusEthernetBeat>& ethernet_rx,
                          hls::stream<NexusToeBeat>& validated_rx,
                          ap_uint<48> local_mac_address,
                          ap_uint<32> local_ip_address,
                          ap_uint<32>& rx_rejected_frames);
