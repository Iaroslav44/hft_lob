`timescale 1ns/1ps

module feed_line_arbitrator (
    input  wire clk,
    input  wire rst_n,
    
    // Feed A (e.g., primary 10G)
    input  wire [63:0] feed_a_tdata,
    input  wire feed_a_tvalid,
    input  wire feed_a_tlast,
    
    // Feed B (e.g., secondary 10G or microwave)
    input  wire [63:0] feed_b_tdata,
    input  wire feed_b_tvalid,
    input  wire feed_b_tlast,
    
    // Output (Muxed Feed)
    output reg  [63:0] out_tdata,
    output reg  out_tvalid,
    output reg  out_tlast
);

    // Inputs cannot be stalled. Give A priority when both packets start together,
    // and discard every beat of an unselected packet, including its later tail.
    // Reset must coincide with packet boundaries on both input streams.
    reg in_frame_a;
    reg in_frame_b;
    localparam [1:0] IDLE = 2'd0, SELECT_A = 2'd1, SELECT_B = 2'd2;
    reg [1:0] selected_feed;
    
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            in_frame_a <= 1'b0;
            in_frame_b <= 1'b0;
            selected_feed <= IDLE;
            out_tdata <= 64'h0;
            out_tvalid <= 1'b0;
            out_tlast <= 1'b0;
        end else begin
            out_tvalid <= 1'b0;
            out_tlast <= 1'b0;

            // Track both ingress positions even while the other feed owns output.
            if (feed_a_tvalid) in_frame_a <= ~feed_a_tlast;
            if (feed_b_tvalid) in_frame_b <= ~feed_b_tlast;

            case (selected_feed)
                SELECT_A: if (feed_a_tvalid) begin
                    out_tdata <= feed_a_tdata;
                    out_tvalid <= 1'b1;
                    out_tlast <= feed_a_tlast;
                    if (feed_a_tlast) selected_feed <= IDLE;
                end
                SELECT_B: if (feed_b_tvalid) begin
                    out_tdata <= feed_b_tdata;
                    out_tvalid <= 1'b1;
                    out_tlast <= feed_b_tlast;
                    if (feed_b_tlast) selected_feed <= IDLE;
                end
                default: begin
                    selected_feed <= IDLE;
                    if (feed_a_tvalid && !in_frame_a) begin
                        out_tdata <= feed_a_tdata;
                        out_tvalid <= 1'b1;
                        out_tlast <= feed_a_tlast;
                        if (!feed_a_tlast) selected_feed <= SELECT_A;
                    end else if (feed_b_tvalid && !in_frame_b) begin
                        out_tdata <= feed_b_tdata;
                        out_tvalid <= 1'b1;
                        out_tlast <= feed_b_tlast;
                        if (!feed_b_tlast) selected_feed <= SELECT_B;
                    end
                end
            endcase
        end
    end

endmodule
