`timescale 1ns/1ps

module verification_top (
    input wire clk,
    input wire rst_n,
    input wire [63:0] a_axis_tdata,
    input wire a_axis_tvalid,
    input wire a_axis_tlast,
    input wire [63:0] b_axis_tdata,
    input wire b_axis_tvalid,
    input wire b_axis_tlast,
    output wire [63:0] out_axis_tdata,
    output wire out_axis_tvalid,
    output wire out_axis_tlast,
    input wire [255:0] mold_axis_tdata,
    input wire [31:0] mold_axis_tkeep,
    input wire mold_axis_tvalid,
    input wire mold_axis_tlast,
    output wire [79:0] mold_session,
    output wire [63:0] mold_sequence,
    output wire [15:0] mold_count,
    output wire mold_valid,
    input wire [63:0] sbe_axis_tdata,
    input wire [7:0] sbe_axis_tkeep,
    input wire sbe_axis_tvalid,
    input wire sbe_axis_tlast,
    output wire [15:0] sbe_template,
    output wire [15:0] sbe_schema,
    output wire [15:0] sbe_version,
    output wire sbe_valid,
    input wire [63:0] net_axis_tdata,
    input wire [7:0] net_axis_tkeep,
    input wire net_axis_tvalid,
    input wire net_axis_tlast,
    output wire [31:0] net_src,
    output wire [31:0] net_dst,
    output wire [15:0] net_length,
    output wire net_valid,
    input wire [511:0] loop_axis_tdata,
    input wire loop_axis_tvalid,
    input wire loop_axis_tlast,
    output wire [511:0] loop_out_tdata,
    output wire loop_out_tvalid,
    output wire loop_out_tlast
);
    feed_line_arbitrator arbiter (
        .clk(clk), .rst_n(rst_n),
        .feed_a_tdata(a_axis_tdata), .feed_a_tvalid(a_axis_tvalid),
        .feed_a_tlast(a_axis_tlast), .feed_b_tdata(b_axis_tdata),
        .feed_b_tvalid(b_axis_tvalid), .feed_b_tlast(b_axis_tlast),
        .out_tdata(out_axis_tdata), .out_tvalid(out_axis_tvalid),
        .out_tlast(out_axis_tlast)
    );
    nasdaq_itch_moldudp64 mold (
        .clk(clk), .rst_n(rst_n),
        .s_axis_tdata(mold_axis_tdata), .s_axis_tkeep(mold_axis_tkeep),
        .s_axis_tvalid(mold_axis_tvalid), .s_axis_tlast(mold_axis_tlast),
        .mold_session(mold_session), .mold_sequence(mold_sequence),
        .mold_count(mold_count), .msg_valid(mold_valid)
    );
    cme_sbe_parser sbe (
        .clk(clk), .rst_n(rst_n),
        .s_axis_tdata(sbe_axis_tdata), .s_axis_tkeep(sbe_axis_tkeep),
        .s_axis_tvalid(sbe_axis_tvalid), .s_axis_tlast(sbe_axis_tlast),
        .msg_template_id(sbe_template), .schema_id(sbe_schema),
        .version(sbe_version), .out_valid(sbe_valid)
    );
    hft_network_stack network (
        .clk(clk), .rst_n(rst_n),
        .s_axis_rx_tdata(net_axis_tdata), .s_axis_rx_tkeep(net_axis_tkeep),
        .s_axis_rx_tvalid(net_axis_tvalid), .s_axis_rx_tlast(net_axis_tlast),
        .src_ip(net_src), .dst_ip(net_dst), .udp_len(net_length), .rx_valid(net_valid)
    );
    nexus_network_stack_top loopback (
        .clk(clk), .rst_n(rst_n),
        .s_axis_net_tdata(loop_axis_tdata), .s_axis_net_tvalid(loop_axis_tvalid),
        .s_axis_net_tlast(loop_axis_tlast), .m_axis_net_tdata(loop_out_tdata),
        .m_axis_net_tvalid(loop_out_tvalid), .m_axis_net_tlast(loop_out_tlast)
    );
endmodule
