`timescale 1ns/1ps

module cme_sbe_parser (
    input  wire clk,
    input  wire rst_n,
    
    // One deframed SBE message per frame, starting at blockLength in lane zero.
    // The upstream framer removes the CME packet header and each MsgSize prefix.
    input  wire [63:0] s_axis_tdata,
    input  wire [7:0] s_axis_tkeep,
    input  wire s_axis_tvalid,
    input  wire s_axis_tlast,
    
    output reg  [15:0] msg_template_id,
    output reg  [15:0] schema_id,
    output reg  [15:0] version,
    output reg  out_valid // Header metadata only; the message body is not checked.
);

    reg in_frame;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            in_frame <= 1'b0;
            out_valid <= 1'b0;
            msg_template_id <= 16'h0;
            schema_id <= 16'h0;
            version <= 16'h0;
        end else begin
            out_valid <= 1'b0;
            if (s_axis_tvalid) begin
                if (!in_frame && (&s_axis_tkeep)) begin
                    // CME fields are little-endian: blockLength, templateId, schemaId, version.
                    msg_template_id <= s_axis_tdata[31:16];
                    schema_id       <= s_axis_tdata[47:32];
                    version         <= s_axis_tdata[63:48];
                    out_valid       <= 1'b1;
                end
                // A rejected header still consumes the frame through its accepted last beat.
                in_frame <= !s_axis_tlast;
            end
        end
    end

endmodule
