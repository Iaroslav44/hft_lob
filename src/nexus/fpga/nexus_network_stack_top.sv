`timescale 1ns/1ps

module nexus_network_stack_top (
    input  wire clk,
    input  wire rst_n,
    
    // Always-ready 512-bit stream; no protocol processing or byte-valid mask.
    input  wire [511:0] s_axis_net_tdata,
    input  wire s_axis_net_tvalid,
    input  wire s_axis_net_tlast,
    
    output wire [511:0] m_axis_net_tdata,
    output wire m_axis_net_tvalid,
    output wire m_axis_net_tlast
);

    // Legacy loopback fixture. Use nexus_mold_ingress for committed UDP messages.
    // Payload is irrelevant while valid is low.
    assign m_axis_net_tdata = s_axis_net_tdata;
    assign m_axis_net_tvalid = rst_n && s_axis_net_tvalid;
    assign m_axis_net_tlast = s_axis_net_tlast;

endmodule
