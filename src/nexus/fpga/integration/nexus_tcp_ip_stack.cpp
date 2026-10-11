#include "nexus_tcp_ip_stack.hpp"

#include "nexus_icmp_echo.hpp"
#include "nexus_session_table.hpp"

// Exact non-template entry points from the pinned upstream source. Its headers
// expose core templates or conflicting ARP declarations instead of these tops.
void ip_handler(hls::stream<NexusToeBeat>& input,
                 hls::stream<NexusToeBeat>& arp,
                 hls::stream<NexusToeBeat>& icmpv6,
                 hls::stream<NexusToeBeat>& ipv6udp,
                 hls::stream<NexusToeBeat>& icmp,
                 hls::stream<NexusToeBeat>& udp,
                 hls::stream<NexusToeBeat>& tcp,
                 hls::stream<NexusToeBeat>& roce,
                 ap_uint<32> local_ip);

void arp_server_subnet(hls::stream<NexusToeBeat>& input,
                        hls::stream<ap_uint<32>>& mac_query,
                        hls::stream<ap_uint<32>>& host_query,
                        hls::stream<NexusToeBeat>& output,
                        hls::stream<arpTableReply>& mac_reply,
                        hls::stream<arpTableReply>& host_reply,
                        ap_uint<48> local_mac,
                        ap_uint<32> local_ip,
                        ap_uint<16>& request_count,
                        ap_uint<16>& reply_count);

void mac_ip_encode(hls::stream<NexusToeBeat>& input,
                    hls::stream<arpTableReply>& arp_reply,
                    hls::stream<NexusToeBeat>& output,
                    hls::stream<ap_uint<32>>& arp_query,
                    ap_uint<48> local_mac,
                    ap_uint<32> subnet_mask,
                    ap_uint<32> gateway);

namespace {

template <unsigned Instance>
void discard_unsupported(hls::stream<NexusToeBeat>& input) {
#pragma HLS INLINE off
#pragma HLS PIPELINE II=1
    // Admission rejects these protocols. Still consume every unused upstream
    // branch so no unconnected output can retain a packet and block the router.
    if (!input.empty()) (void)input.read();
}

template <unsigned Instance>
void merge_packets(hls::stream<NexusToeBeat>& first,
                    hls::stream<NexusToeBeat>& second,
                    hls::stream<NexusToeBeat>& output) {
#pragma HLS INLINE off
#pragma HLS PIPELINE II=1
    static bool in_frame = false;
    static bool selected_second = false;
    static bool prefer_second = false;

    if (output.full()) return;
    if (!in_frame) {
        const bool first_available = !first.empty();
        const bool second_available = !second.empty();
        if (!first_available && !second_available) return;
        selected_second = second_available && (!first_available || prefer_second);
    }

    NexusToeBeat beat;
    if (selected_second) {
        if (second.empty()) return;
        beat = second.read();
    } else {
        if (first.empty()) return;
        beat = first.read();
    }
    beat.strb = beat.keep;
    output.write(beat);
    in_frame = !beat.last;
    if (beat.last) prefer_second = !selected_second;
}

} // namespace

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
    ap_uint<32>& icmp_rejected_frames) {
#pragma HLS INTERFACE ap_ctrl_none port=return
#pragma HLS DATAFLOW disable_start_propagation
#pragma HLS INTERFACE axis register both port=ethernet_rx
#pragma HLS INTERFACE axis register both port=ethernet_tx
#pragma HLS INTERFACE axis register both port=rx_buffer_write_status
#pragma HLS INTERFACE axis register both port=tx_buffer_write_status
#pragma HLS INTERFACE axis register both port=rx_buffer_read_data
#pragma HLS INTERFACE axis register both port=tx_buffer_read_data
#pragma HLS INTERFACE axis register both port=rx_buffer_write_command
#pragma HLS INTERFACE axis register both port=rx_buffer_read_command
#pragma HLS INTERFACE axis register both port=tx_buffer_write_command
#pragma HLS INTERFACE axis register both port=tx_buffer_read_command
#pragma HLS INTERFACE axis register both port=rx_buffer_write_data
#pragma HLS INTERFACE axis register both port=tx_buffer_write_data
#pragma HLS INTERFACE axis register both port=listen_port_request
#pragma HLS INTERFACE axis register both port=rx_data_request
#pragma HLS INTERFACE axis register both port=open_connection_request
#pragma HLS INTERFACE axis register both port=close_connection_request
#pragma HLS INTERFACE axis register both port=tx_data_metadata
#pragma HLS INTERFACE axis register both port=tx_data
#pragma HLS INTERFACE axis register both port=listen_port_response
#pragma HLS INTERFACE axis register both port=notification
#pragma HLS INTERFACE axis register both port=rx_data_metadata
#pragma HLS INTERFACE axis register both port=rx_data
#pragma HLS INTERFACE axis register both port=open_connection_response
#pragma HLS INTERFACE axis register both port=tx_data_status
#pragma HLS INTERFACE axis register both port=arp_lookup_request
#pragma HLS INTERFACE axis register both port=arp_lookup_response
#pragma HLS AGGREGATE variable=rx_buffer_write_status compact=bit
#pragma HLS AGGREGATE variable=tx_buffer_write_status compact=bit
#pragma HLS AGGREGATE variable=rx_buffer_write_command compact=bit
#pragma HLS AGGREGATE variable=rx_buffer_read_command compact=bit
#pragma HLS AGGREGATE variable=tx_buffer_write_command compact=bit
#pragma HLS AGGREGATE variable=tx_buffer_read_command compact=bit
#pragma HLS AGGREGATE variable=rx_data_request compact=bit
#pragma HLS AGGREGATE variable=open_connection_request compact=bit
#pragma HLS AGGREGATE variable=tx_data_metadata compact=bit
#pragma HLS AGGREGATE variable=notification compact=bit
#pragma HLS AGGREGATE variable=open_connection_response compact=bit
#pragma HLS AGGREGATE variable=tx_data_status compact=bit
#pragma HLS AGGREGATE variable=arp_lookup_response compact=bit
#pragma HLS INTERFACE ap_none register port=local_mac_address
#pragma HLS INTERFACE ap_none register port=local_ip_address
#pragma HLS INTERFACE ap_none register port=subnet_mask
#pragma HLS INTERFACE ap_none register port=default_gateway
#pragma HLS INTERFACE ap_vld port=session_count
#pragma HLS INTERFACE ap_vld port=arp_request_count
#pragma HLS INTERFACE ap_vld port=arp_reply_count
#pragma HLS INTERFACE ap_vld port=rx_rejected_frames
#pragma HLS INTERFACE ap_vld port=icmp_rejected_frames

    static hls::stream<NexusToeBeat> admitted("stack_admitted");
    static hls::stream<NexusToeBeat> arp_rx("stack_arp_rx");
    static hls::stream<NexusToeBeat> icmp_rx("stack_icmp_rx");
    static hls::stream<NexusToeBeat> tcp_rx("stack_tcp_rx");
    static hls::stream<NexusToeBeat> unused_icmpv6("stack_unused_icmpv6");
    static hls::stream<NexusToeBeat> unused_ipv6udp("stack_unused_ipv6udp");
    static hls::stream<NexusToeBeat> unused_udp("stack_unused_udp");
    static hls::stream<NexusToeBeat> unused_roce("stack_unused_roce");
    static hls::stream<NexusToeBeat> tcp_tx("stack_tcp_tx");
    static hls::stream<NexusToeBeat> icmp_tx("stack_icmp_tx");
    static hls::stream<NexusToeBeat> ip_tx("stack_ip_tx");
    static hls::stream<NexusToeBeat> encoded_tx("stack_encoded_tx");
    static hls::stream<NexusToeBeat> arp_tx("stack_arp_tx");
#pragma HLS STREAM variable=admitted depth=4
#pragma HLS STREAM variable=arp_rx depth=4
#pragma HLS STREAM variable=icmp_rx depth=4
#pragma HLS STREAM variable=tcp_rx depth=4
#pragma HLS STREAM variable=unused_icmpv6 depth=2
#pragma HLS STREAM variable=unused_ipv6udp depth=2
#pragma HLS STREAM variable=unused_udp depth=2
#pragma HLS STREAM variable=unused_roce depth=2
#pragma HLS STREAM variable=tcp_tx depth=4
#pragma HLS STREAM variable=icmp_tx depth=4
#pragma HLS STREAM variable=ip_tx depth=4
#pragma HLS STREAM variable=encoded_tx depth=4
#pragma HLS STREAM variable=arp_tx depth=4

    static hls::stream<ap_uint<32>> arp_query("stack_arp_query");
    static hls::stream<arpTableReply> arp_reply("stack_arp_reply");
    static hls::stream<rtlSessionLookupRequest> lookup_request("stack_lookup_request");
    static hls::stream<rtlSessionUpdateRequest> update_request("stack_update_request");
    static hls::stream<rtlSessionLookupReply> lookup_response("stack_lookup_response");
    static hls::stream<rtlSessionUpdateReply> update_response("stack_update_response");
#pragma HLS STREAM variable=arp_query depth=4
#pragma HLS STREAM variable=arp_reply depth=4
#pragma HLS STREAM variable=lookup_request depth=4
#pragma HLS STREAM variable=update_request depth=4
#pragma HLS STREAM variable=lookup_response depth=4
#pragma HLS STREAM variable=update_response depth=4

    nexus_ethernet_guard(ethernet_rx, admitted, local_mac_address,
                         local_ip_address, rx_rejected_frames);
    ip_handler(admitted, arp_rx, unused_icmpv6, unused_ipv6udp, icmp_rx,
                unused_udp, tcp_rx, unused_roce, local_ip_address);
    discard_unsupported<0>(unused_icmpv6);
    discard_unsupported<1>(unused_ipv6udp);
    discard_unsupported<2>(unused_udp);
    discard_unsupported<3>(unused_roce);

    arp_server_subnet(arp_rx, arp_query, arp_lookup_request, arp_tx,
                       arp_reply, arp_lookup_response, local_mac_address,
                       local_ip_address, arp_request_count, arp_reply_count);
    nexus_icmp_echo(icmp_rx, icmp_tx, icmp_rejected_frames);
    nexus_session_table(lookup_request, update_request, lookup_response, update_response);

    nexus_ethz_toe(tcp_rx, rx_buffer_write_status, tx_buffer_write_status,
                   rx_buffer_read_data, tx_buffer_read_data, tcp_tx,
                   rx_buffer_write_command, rx_buffer_read_command,
                   tx_buffer_write_command, tx_buffer_read_command,
                   rx_buffer_write_data, tx_buffer_write_data,
                   lookup_response, update_response, lookup_request, update_request,
                   listen_port_request, rx_data_request, open_connection_request,
                   close_connection_request, tx_data_metadata, tx_data,
                   listen_port_response, notification, rx_data_metadata, rx_data,
                   open_connection_response, tx_data_status, local_ip_address, session_count);

    merge_packets<0>(tcp_tx, icmp_tx, ip_tx);
    mac_ip_encode(ip_tx, arp_reply, encoded_tx, arp_query, local_mac_address,
                   subnet_mask, default_gateway);
    merge_packets<1>(encoded_tx, arp_tx, ethernet_tx);
}
