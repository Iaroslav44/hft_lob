`timescale 1ns/1ps

// Integration boundary: one complete, integrity-checked UDP payload per frame.
// MAC/IP/UDP checks, clock crossing and lossless input buffering belong to the
// enclosing NIC. All streams here share clk and support ready/valid backpressure.
module nexus_mold_ingress #(
    parameter integer MAX_PACKET_BYTES = 2048
) (
    input  wire        clk,
    input  wire        rst_n,
    input  wire [63:0] s_axis_tdata,
    input  wire [7:0]  s_axis_tkeep,
    input  wire        s_axis_tvalid,
    input  wire        s_axis_tlast,
    input  wire        s_axis_tuser,
    output wire        s_axis_tready,

    input  wire        cfg_valid,
    output wire        cfg_ready,
    input  wire [79:0] cfg_session,
    input  wire [63:0] cfg_next_sequence,

    output wire [63:0] m_axis_tdata,
    output wire [7:0]  m_axis_tkeep,
    output wire        m_axis_tvalid,
    output wire        m_axis_tlast,
    input  wire        m_axis_tready,
    output wire [79:0] m_order_session,
    output wire [63:0] m_order_sequence,
    output wire        m_order_packet_last,

    output wire        recovery_needed,
    output wire [3:0]  fault_code,
    output wire [63:0] fault_sequence,
    output wire [63:0] expected_sequence,
    output wire        duplicate_pulse,
    output wire        accepted_pulse,
    output wire        decoder_reject,
    output wire        idle
);
    wire buffer_idle, decoder_idle, buffer_ready;
    wire packet_valid, packet_ready, packet_accept;
    wire [79:0] packet_session;
    wire [63:0] packet_sequence;
    wire [15:0] packet_count, packet_bytes;
    wire [3:0] packet_error;
    wire [63:0] message_data, message_sequence;
    wire [7:0] message_keep;
    wire message_valid, message_last, message_ready, message_packet_last;
    wire [79:0] message_session;
    wire [15:0] message_length;
    wire configure = cfg_valid && cfg_ready;

    assign idle = buffer_idle && decoder_idle;
    // A checkpoint takes precedence over the first beat of the next packet.
    assign s_axis_tready = buffer_ready && !configure;

    moldudp64_packet_buffer #(.MAX_PACKET_BYTES(MAX_PACKET_BYTES)) packet_buffer (
        .clk(clk), .rst_n(rst_n), .idle(buffer_idle),
        .s_axis_tdata(s_axis_tdata), .s_axis_tkeep(s_axis_tkeep),
        .s_axis_tvalid(s_axis_tvalid && !configure), .s_axis_tlast(s_axis_tlast),
        .s_axis_tuser(s_axis_tuser),
        .s_axis_tready(buffer_ready),
        .packet_valid(packet_valid), .packet_ready(packet_ready),
        .packet_accept(packet_accept), .packet_session(packet_session),
        .packet_sequence(packet_sequence), .packet_count(packet_count),
        .packet_bytes(packet_bytes), .packet_error(packet_error),
        .m_axis_tdata(message_data), .m_axis_tkeep(message_keep),
        .m_axis_tvalid(message_valid), .m_axis_tlast(message_last),
        .m_axis_tready(message_ready), .m_msg_session(message_session),
        .m_msg_sequence(message_sequence), .m_msg_length(message_length),
        .m_msg_packet_last(message_packet_last)
    );

    mold_session_gate session_gate (
        .clk(clk), .rst_n(rst_n), .quiescent(idle),
        .cfg_valid(cfg_valid), .cfg_ready(cfg_ready),
        .cfg_session(cfg_session), .cfg_next_sequence(cfg_next_sequence),
        .packet_valid(packet_valid), .packet_ready(packet_ready),
        .packet_accept(packet_accept), .packet_session(packet_session),
        .packet_sequence(packet_sequence), .packet_count(packet_count),
        .packet_error(packet_error), .recovery_needed(recovery_needed),
        .fault_code(fault_code), .fault_sequence(fault_sequence),
        .expected_sequence(expected_sequence), .duplicate_pulse(duplicate_pulse),
        .accepted_pulse(accepted_pulse)
    );

    itch_a_normalizer normalizer (
        .clk(clk), .rst_n(rst_n), .idle(decoder_idle),
        .s_axis_tdata(message_data), .s_axis_tkeep(message_keep),
        .s_axis_tvalid(message_valid), .s_axis_tlast(message_last),
        .s_axis_tready(message_ready), .s_msg_session(message_session),
        .s_msg_sequence(message_sequence), .s_msg_length(message_length),
        .s_msg_packet_last(message_packet_last),
        .m_axis_tdata(m_axis_tdata), .m_axis_tkeep(m_axis_tkeep),
        .m_axis_tvalid(m_axis_tvalid), .m_axis_tlast(m_axis_tlast),
        .m_axis_tready(m_axis_tready), .m_order_session(m_order_session),
        .m_order_sequence(m_order_sequence),
        .m_order_packet_last(m_order_packet_last), .reject_valid(decoder_reject)
    );
endmodule
