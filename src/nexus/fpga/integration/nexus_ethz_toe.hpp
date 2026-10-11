#pragma once

#include "ethz_toe_config.hpp"

#include <toe/toe.hpp>
#include <toe/session_lookup_controller/session_lookup_controller.hpp>

// Upstream: fpgasystems/fpga-network-stack, commit
// 75c32bae73f5c336b5c46c9de35ae0cb9cb7cc1c. The include path is its hls directory.
// This is an IPv4/TCP engine boundary, not an Ethernet MAC or a board shell.
// All streams use independent ready/valid handshakes in the same clock domain.
// Reset the engine, session table and buffer controllers as one ownership domain.
// Keep local_ip_address stable until every session has closed and buffers drained.
// Packet byte zero occupies bits [7:0]; valid bytes are selected by KEEP.
// Input STRB must equal KEEP. Output STRB is normalised to KEEP by the adapter.
using NexusToeBeat = ap_axiu<DATA_WIDTH, 0, 0, 0>;

// Memory commands retain the upstream 72-bit DataMover layout, including tags.
// TX storage must retain unacknowledged bytes for the upstream retransmit engine.
// A write status acknowledges a completed memory operation, not command acceptance.
// The upstream API has no memory-read-status input: handle read faults at the shell.
// Session lookup/update responses must preserve the upstream key/source semantics.
// Application and session-table IDs must refer to live sessions below MAX_SESSIONS;
// upstream table accesses do not validate arbitrary control-plane session IDs.
// Supply TX payload only after a successful tx_data_status response to its metadata.
// The full openStatus type is retained: sessionID, success, IP address and port.
void nexus_ethz_toe(
    hls::stream<NexusToeBeat>& network_rx,
    hls::stream<mmStatus>& rx_buffer_write_status,
    hls::stream<mmStatus>& tx_buffer_write_status,
    hls::stream<NexusToeBeat>& rx_buffer_read_data,
    hls::stream<NexusToeBeat>& tx_buffer_read_data,
    hls::stream<NexusToeBeat>& network_tx,
    hls::stream<mmCmd>& rx_buffer_write_command,
    hls::stream<mmCmd>& rx_buffer_read_command,
    hls::stream<mmCmd>& tx_buffer_write_command,
    hls::stream<mmCmd>& tx_buffer_read_command,
    hls::stream<NexusToeBeat>& rx_buffer_write_data,
    hls::stream<NexusToeBeat>& tx_buffer_write_data,
    hls::stream<rtlSessionLookupReply>& session_lookup_response,
    hls::stream<rtlSessionUpdateReply>& session_update_response,
    hls::stream<rtlSessionLookupRequest>& session_lookup_request,
    hls::stream<rtlSessionUpdateRequest>& session_update_request,
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
    ap_uint<32> local_ip_address,
    ap_uint<16>& session_count);
