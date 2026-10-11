`timescale 1ns/1ps

module nasdaq_itch_moldudp64 (
    input  wire clk,
    input  wire rst_n,
    
    // One UDP payload per frame; byte zero occupies the least significant lane.
    // The complete 20-byte MoldUDP64 header must be present in the first beat.
    input  wire [255:0] s_axis_tdata,
    input  wire [31:0]  s_axis_tkeep,
    input  wire s_axis_tvalid,
    input  wire s_axis_tlast,
    
    // Header metadata only; message blocks and packet integrity are not checked.
    output reg  [79:0] mold_session,
    output reg  [63:0] mold_sequence,
    output reg  [15:0] mold_count,
    output reg  msg_valid
);

    reg in_frame;
    
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            in_frame <= 1'b0;
            msg_valid <= 1'b0;
            mold_session <= 80'b0;
            mold_sequence <= 64'b0;
            mold_count <= 16'b0;
        end else begin
            msg_valid <= 1'b0;
            if (s_axis_tvalid) begin
                if (!in_frame && (&s_axis_tkeep[19:0])) begin
                    // Preserve the opaque session in wire order, first byte at the MSB.
                    mold_session <= {s_axis_tdata[7:0], s_axis_tdata[15:8], s_axis_tdata[23:16], s_axis_tdata[31:24],
                                     s_axis_tdata[39:32], s_axis_tdata[47:40], s_axis_tdata[55:48], s_axis_tdata[63:56],
                                     s_axis_tdata[71:64], s_axis_tdata[79:72]};
                    
                    // Numeric fields use network byte order at offsets 10 and 18.
                    mold_sequence <= {s_axis_tdata[87:80], s_axis_tdata[95:88], s_axis_tdata[103:96], s_axis_tdata[111:104],
                                      s_axis_tdata[119:112], s_axis_tdata[127:120], s_axis_tdata[135:128], s_axis_tdata[143:136]};
                    
                    mold_count <= {s_axis_tdata[151:144], s_axis_tdata[159:152]};
                    
                    msg_valid <= 1'b1;
                end
                // Drain the frame even when its first beat has an incomplete header.
                in_frame <= !s_axis_tlast;
            end
        end
    end

endmodule
