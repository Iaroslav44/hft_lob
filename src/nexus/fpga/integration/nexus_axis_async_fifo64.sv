`resetall
`timescale 1ns / 1ps
`default_nettype none

// Packet CDC using the pinned Taxi implementation. The source must honour
// backpressure and mark a bad frame on its accepted final beat (TUSER=1).
// A source-domain reset can terminate an output frame with TUSER=1 and TLAST;
// the destination must reject that entire frame before committing its payload.
module nexus_axis_async_fifo64 #(
    // Capacity in byte lanes, not valid bytes: each stored beat consumes eight.
    parameter int unsigned DEPTH_BYTES = 4096,
    parameter int unsigned RAM_PIPELINE = 1
) (
    input  wire logic s_clk,
    input  wire logic s_aresetn,
    input  wire logic [63:0] s_axis_tdata,
    input  wire logic [7:0] s_axis_tkeep,
    input  wire logic s_axis_tvalid,
    output wire logic s_axis_tready,
    input  wire logic s_axis_tlast,
    input  wire logic s_axis_tuser,

    input  wire logic m_clk,
    input  wire logic m_aresetn,
    output wire logic [63:0] m_axis_tdata,
    output wire logic [7:0] m_axis_tkeep,
    output wire logic m_axis_tvalid,
    input  wire logic m_axis_tready,
    output wire logic m_axis_tlast,
    output wire logic m_axis_tuser,

    // Depths are delayed domain-local observations of the upstream RAM pointers.
    output wire logic [$clog2(DEPTH_BYTES):0] s_status_depth,
    output wire logic [$clog2(DEPTH_BYTES):0] s_status_depth_commit,
    output wire logic s_status_overflow,
    output wire logic s_status_bad_frame,
    output wire logic s_status_good_frame,
    output wire logic [$clog2(DEPTH_BYTES):0] m_status_depth,
    output wire logic [$clog2(DEPTH_BYTES):0] m_status_depth_commit,
    output wire logic m_status_overflow,
    output wire logic m_status_bad_frame,
    output wire logic m_status_good_frame
);

    // At least two beats are required by the upstream Gray-pointer comparison.
    initial begin
        if (DEPTH_BYTES < 16 || (DEPTH_BYTES & (DEPTH_BYTES - 1)) != 0)
            $fatal(1, "DEPTH_BYTES must be a power of two of at least 16");
    end

    wire logic s_reset;
    wire logic m_reset;

    // Assert asynchronously and release separately in each local clock domain.
    // Taxi also synchronizes each reset into the opposite domain internally.
    (* KEEP_HIERARCHY = "yes" *)
    taxi_sync_reset #(.N(4)) source_reset_sync (
        .clk(s_clk),
        .rst(!s_aresetn),
        .out(s_reset)
    );

    (* KEEP_HIERARCHY = "yes" *)
    taxi_sync_reset #(.N(4)) destination_reset_sync (
        .clk(m_clk),
        .rst(!m_aresetn),
        .out(m_reset)
    );

    taxi_axis_if #(
        .DATA_W(64), .KEEP_W(8), .KEEP_EN(1'b1), .STRB_EN(1'b0),
        .LAST_EN(1'b1), .ID_EN(1'b0), .DEST_EN(1'b0),
        .USER_EN(1'b1), .USER_W(1)
    ) fifo_input ();

    taxi_axis_if #(
        .DATA_W(64), .KEEP_W(8), .KEEP_EN(1'b1), .STRB_EN(1'b0),
        .LAST_EN(1'b1), .ID_EN(1'b0), .DEST_EN(1'b0),
        .USER_EN(1'b1), .USER_W(1)
    ) fifo_output ();

    assign fifo_input.tdata = s_axis_tdata;
    assign fifo_input.tkeep = s_axis_tkeep;
    assign fifo_input.tstrb = s_axis_tkeep;
    assign fifo_input.tid = '0;
    assign fifo_input.tdest = '0;
    assign fifo_input.tlast = s_axis_tlast;
    assign fifo_input.tuser = s_axis_tuser;
    assign fifo_input.tvalid = s_axis_tvalid && !s_reset;
    assign s_axis_tready = fifo_input.tready && !s_reset;

    assign m_axis_tdata = fifo_output.tdata;
    assign m_axis_tkeep = fifo_output.tkeep;
    assign m_axis_tlast = fifo_output.tlast;
    assign m_axis_tuser = fifo_output.tuser;
    assign m_axis_tvalid = fifo_output.tvalid && !m_reset;
    assign fifo_output.tready = m_axis_tready && !m_reset;

    // Ordinary congestion applies backpressure. An oversized frame is consumed
    // through TLAST and discarded, allowing a later complete frame to proceed.
    (* KEEP_HIERARCHY = "yes" *)
    taxi_axis_async_fifo #(
        .DEPTH(DEPTH_BYTES),
        .RAM_PIPELINE(RAM_PIPELINE),
        .OUTPUT_FIFO_EN(1'b0),
        .FRAME_FIFO(1'b1),
        .USER_BAD_FRAME_VALUE(1'b1),
        .USER_BAD_FRAME_MASK(1'b1),
        .DROP_OVERSIZE_FRAME(1'b1),
        .DROP_BAD_FRAME(1'b1),
        .DROP_WHEN_FULL(1'b0),
        .MARK_WHEN_FULL(1'b0),
        .PAUSE_EN(1'b0)
    ) packet_fifo (
        .s_clk(s_clk),
        .s_rst(s_reset),
        .s_axis(fifo_input),
        .m_clk(m_clk),
        .m_rst(m_reset),
        .m_axis(fifo_output),
        .s_pause_req(1'b0),
        .s_pause_ack(),
        .m_pause_req(1'b0),
        .m_pause_ack(),
        .s_status_depth(s_status_depth),
        .s_status_depth_commit(s_status_depth_commit),
        .s_status_overflow(s_status_overflow),
        .s_status_bad_frame(s_status_bad_frame),
        .s_status_good_frame(s_status_good_frame),
        .m_status_depth(m_status_depth),
        .m_status_depth_commit(m_status_depth_commit),
        .m_status_overflow(m_status_overflow),
        .m_status_bad_frame(m_status_bad_frame),
        .m_status_good_frame(m_status_good_frame)
    );

endmodule

`resetall
