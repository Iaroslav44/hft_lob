#pragma once

#include "nexus_ethz_toe.hpp"

// IPv4 echo-request path after Ethernet admission and IPv4 header validation.
// The input has a 20-byte IPv4 header, at least eight ICMP bytes and MTU <= 1500.
// Complete packets remain buffered until the upstream ICMP checksum decision.
void nexus_icmp_echo(hls::stream<NexusToeBeat>& input,
                     hls::stream<NexusToeBeat>& output,
                     ap_uint<32>& rejected_frames);
