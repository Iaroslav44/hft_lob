#pragma once

#include "nexus_ethernet_guard.hpp"

// Canonical upstream ARP type. mac_ip_encode.hpp independently redeclares the
// same type and must not be included alongside this header in one translation unit.
#include <arp_server_subnet/arp_server_subnet.hpp>

// Board-independent Ethernet II / IPv4 / TCP endpoint with ARP and ICMP echo.
// The 64-bit streams start with the destination MAC in byte lane zero. RX USER
// marks any MAC/CDC frame error; preamble and FCS are handled by the external MAC.
// Both Ethernet directions require ready/valid backpressure. TX padding and FCS
// generation remain MAC duties. VLAN, IPv6, fragmented/optioned IPv4 and UDP are
// rejected at ingress; this top does not provide a UDP market-data service.
//
// All ports use one clock/reset domain, supplied by the enclosing design. Use
// the prepared reset-all HLS recipe and the actual 156.25 MHz timer clock. Reset
// also invalidates sessions, ARP entries, queued frames and outstanding memory
// operations; coordinate it with the application and RX/TX buffer controllers.
// MAC, IP, mask and gateway use the upstream lane representation: the first
// address byte occupies bits [7:0]. Keep them stable until reset/quiescence.
// The gateway must belong to the configured local subnet.
//
// Application and memory streams retain the full nexus_ethz_toe contract. TX
// buffer data must survive until acknowledged; memory status means completion.
// Send application TX data only after its successful metadata status. The
// internal session table is exclusively owned by the TOE session controller.
// ARP misses trigger a request and discard that IP transmission; the TCP engine
// owns retransmission. The host lookup port can resolve a peer before opening a
// connection. Its request/reply stream must be serviced in order.
void nexus_tcp_ip_stack(
    hls::stream<NexusEthernetBeat>& ethernet_rx,
    hls::stream<NexusToeBeat>& ethernet_tx,
    hls::stream<mmStatus>& rx_buffer_write_status,
    hls::stream<mmStatus>& tx_buffer_write_status,
    hls::stream<NexusToeBeat>& rx_buffer_read_data,
    hls::stream<NexusToeBeat>& tx_buffer_read_data,
    hls::stream<mmCmd>& rx_buffer_write_command,
    hls::stream<mmCmd>& rx_buffer_read_command,
    hls::stream<mmCmd>& tx_buffer_write_command,
    hls::stream<mmCmd>& tx_buffer_read_command,
    hls::stream<NexusToeBeat>& rx_buffer_write_data,
    hls::stream<NexusToeBeat>& tx_buffer_write_data,
    hls::stream<ap_uint<16>>& listen_port_request,
    hls::stream<appReadRequest>& rx_data_request,
    hls::stream<ipTuple>& open_connection_request,
    hls::stream<ap_uint<16>>& close_connection_request,
    hls::stream<appTxMeta>& tx_data_metadata,
    hls::stream<NexusToeBeat>& tx_data,
    hls::stream<bool>& listen_port_response,
    hls::stream<appNotification>& notification,
    hls::stream<ap_uint<16>>& rx_data_metadata,
    hls::stream<NexusToeBeat>& rx_data,
    hls::stream<openStatus>& open_connection_response,
    hls::stream<appTxRsp>& tx_data_status,
    hls::stream<ap_uint<32>>& arp_lookup_request,
    hls::stream<arpTableReply>& arp_lookup_response,
    ap_uint<48> local_mac_address,
    ap_uint<32> local_ip_address,
    ap_uint<32> subnet_mask,
    ap_uint<32> default_gateway,
    ap_uint<16>& session_count,
    ap_uint<16>& arp_request_count,
    ap_uint<16>& arp_reply_count,
    ap_uint<32>& rx_rejected_frames,
    ap_uint<32>& icmp_rejected_frames);
