#include "nexus_ethz_toe.hpp"

// The pinned upstream toe.cpp defines this entry point but toe.hpp declares only
// toe_core. Use the identical function type to keep the complete typed boundary.
decltype(nexus_ethz_toe) toe;

namespace {

// Upstream net_axis conversion leaves STRB unspecified. Preserve packet ownership
// and backpressure while making every byte qualifier deterministic at our boundary.
template <unsigned Port>
void normalise_strobes(hls::stream<NexusToeBeat>& input,
                      hls::stream<NexusToeBeat>& output) {
#pragma HLS INLINE off
#pragma HLS PIPELINE II=1
    if (!input.empty()) {
        NexusToeBeat beat = input.read();
        beat.strb = beat.keep;
        output.write(beat);
    }
}

} // namespace

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
    ap_uint<16>& session_count) {
#pragma HLS INTERFACE ap_ctrl_none port=return
#pragma HLS DATAFLOW disable_start_propagation

#pragma HLS INTERFACE axis register both port=network_rx
#pragma HLS INTERFACE axis register both port=network_tx
#pragma HLS INTERFACE axis register both port=rx_buffer_read_data
#pragma HLS INTERFACE axis register both port=tx_buffer_read_data
#pragma HLS INTERFACE axis register both port=rx_buffer_write_data
#pragma HLS INTERFACE axis register both port=tx_buffer_write_data
#pragma HLS INTERFACE axis register both port=tx_data
#pragma HLS INTERFACE axis register both port=rx_data

#pragma HLS INTERFACE axis register both port=rx_buffer_write_status
#pragma HLS INTERFACE axis register both port=tx_buffer_write_status
#pragma HLS INTERFACE axis register both port=rx_buffer_write_command
#pragma HLS INTERFACE axis register both port=rx_buffer_read_command
#pragma HLS INTERFACE axis register both port=tx_buffer_write_command
#pragma HLS INTERFACE axis register both port=tx_buffer_read_command
#pragma HLS aggregate variable=rx_buffer_write_status compact=bit
#pragma HLS aggregate variable=tx_buffer_write_status compact=bit
#pragma HLS aggregate variable=rx_buffer_write_command compact=bit
#pragma HLS aggregate variable=rx_buffer_read_command compact=bit
#pragma HLS aggregate variable=tx_buffer_write_command compact=bit
#pragma HLS aggregate variable=tx_buffer_read_command compact=bit

#pragma HLS INTERFACE axis register both port=session_lookup_response
#pragma HLS INTERFACE axis register both port=session_update_response
#pragma HLS INTERFACE axis register both port=session_lookup_request
#pragma HLS INTERFACE axis register both port=session_update_request
#pragma HLS aggregate variable=session_lookup_response compact=bit
#pragma HLS aggregate variable=session_update_response compact=bit
#pragma HLS aggregate variable=session_lookup_request compact=bit
#pragma HLS aggregate variable=session_update_request compact=bit

#pragma HLS INTERFACE axis register both port=listen_port_request
#pragma HLS INTERFACE axis register both port=listen_port_response
#pragma HLS INTERFACE axis register both port=rx_data_request
#pragma HLS INTERFACE axis register both port=open_connection_request
#pragma HLS INTERFACE axis register both port=close_connection_request
#pragma HLS INTERFACE axis register both port=tx_data_metadata
#pragma HLS INTERFACE axis register both port=notification
#pragma HLS INTERFACE axis register both port=rx_data_metadata
#pragma HLS INTERFACE axis register both port=open_connection_response
#pragma HLS INTERFACE axis register both port=tx_data_status
#pragma HLS aggregate variable=rx_data_request compact=bit
#pragma HLS aggregate variable=open_connection_request compact=bit
#pragma HLS aggregate variable=tx_data_metadata compact=bit
#pragma HLS aggregate variable=notification compact=bit
#pragma HLS aggregate variable=open_connection_response compact=bit
#pragma HLS aggregate variable=tx_data_status compact=bit
#pragma HLS INTERFACE ap_none register port=local_ip_address
#pragma HLS INTERFACE ap_vld port=session_count

    static hls::stream<NexusToeBeat> network_tx_raw("network_tx_raw");
    static hls::stream<NexusToeBeat> rx_buffer_write_raw("rx_buffer_write_raw");
    static hls::stream<NexusToeBeat> tx_buffer_write_raw("tx_buffer_write_raw");
    static hls::stream<NexusToeBeat> rx_data_raw("rx_data_raw");
#pragma HLS STREAM variable=network_tx_raw depth=2
#pragma HLS STREAM variable=rx_buffer_write_raw depth=2
#pragma HLS STREAM variable=tx_buffer_write_raw depth=2
#pragma HLS STREAM variable=rx_data_raw depth=2

    toe(network_rx, rx_buffer_write_status, tx_buffer_write_status,
        rx_buffer_read_data, tx_buffer_read_data, network_tx_raw,
        rx_buffer_write_command, rx_buffer_read_command,
        tx_buffer_write_command, tx_buffer_read_command,
        rx_buffer_write_raw, tx_buffer_write_raw,
        session_lookup_response, session_update_response,
        session_lookup_request, session_update_request,
        listen_port_request, rx_data_request, open_connection_request,
        close_connection_request, tx_data_metadata, tx_data,
        listen_port_response, notification, rx_data_metadata, rx_data_raw,
        open_connection_response, tx_data_status, local_ip_address, session_count);

    normalise_strobes<0>(network_tx_raw, network_tx);
    normalise_strobes<1>(rx_buffer_write_raw, rx_buffer_write_data);
    normalise_strobes<2>(tx_buffer_write_raw, tx_buffer_write_data);
    normalise_strobes<3>(rx_data_raw, rx_data);
}
