`timescale 1ns/1ps

module hft_network_stack (
    input  wire clk,
    input  wire rst_n,
    
    // Ethernet II frames, first byte in lane zero; no VLAN tags or IPv4 options.
    // The first five beats require all lanes; beat six requires lanes zero and one.
    // Byte validity after the UDP header is outside this metadata extractor.
    input  wire [63:0] s_axis_rx_tdata,
    input  wire [7:0] s_axis_rx_tkeep,
    input  wire s_axis_rx_tvalid,
    input  wire s_axis_rx_tlast,
    
    // Metadata becomes valid after all 42 header bytes arrive. Payload length,
    // checksums and Ethernet FCS must be validated elsewhere before packet use.
    output reg  [31:0] src_ip,
    output reg  [31:0] dst_ip,
    output reg  [15:0] udp_len,
    output reg  rx_valid
);

    reg [2:0] beat_cnt;
    reg header_ok;
    reg [15:0] ip_total_len;
    reg [31:0] pending_src_ip;
    reg [31:0] pending_dst_ip;
    reg [15:0] pending_udp_len;

    wire [15:0] network_word0 = {s_axis_rx_tdata[7:0], s_axis_rx_tdata[15:8]};
    wire [15:0] network_word2 = {s_axis_rx_tdata[39:32], s_axis_rx_tdata[47:40]};
    wire [15:0] network_word3 = {s_axis_rx_tdata[55:48], s_axis_rx_tdata[63:56]};
    
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            beat_cnt <= 3'b0;
            header_ok <= 1'b0;
            ip_total_len <= 16'd0;
            pending_src_ip <= 32'd0;
            pending_dst_ip <= 32'd0;
            pending_udp_len <= 16'd0;
            rx_valid <= 1'b0;
            src_ip <= 32'h0;
            dst_ip <= 32'h0;
            udp_len <= 16'h0;
        end else begin
            rx_valid <= 1'b0;
            if (s_axis_rx_tvalid) begin
                case (beat_cnt)
                    3'd0: header_ok <= &s_axis_rx_tkeep;
                    3'd1: begin
                        header_ok <= header_ok && (&s_axis_rx_tkeep) &&
                                     network_word2 == 16'h0800 &&
                                     s_axis_rx_tdata[55:48] == 8'h45;
                    end
                    3'd2: begin
                        ip_total_len <= network_word0;
                        // Permit DF; reject the reserved flag, MF and nonzero fragment offsets.
                        header_ok <= header_ok && (&s_axis_rx_tkeep) &&
                                     (network_word2 & 16'hbfff) == 16'd0 &&
                                     s_axis_rx_tdata[63:56] == 8'd17 &&
                                     network_word0 >= 16'd28;
                    end
                    3'd3: begin
                        header_ok <= header_ok && (&s_axis_rx_tkeep);
                        pending_src_ip <= {s_axis_rx_tdata[23:16], s_axis_rx_tdata[31:24],
                                           s_axis_rx_tdata[39:32], s_axis_rx_tdata[47:40]};
                        pending_dst_ip[31:16] <= network_word3;
                    end
                    3'd4: begin
                        header_ok <= header_ok && (&s_axis_rx_tkeep) &&
                                     network_word3 >= 16'd8 &&
                                     network_word3 <= ip_total_len - 16'd20;
                        pending_dst_ip[15:0] <= network_word0;
                        pending_udp_len <= network_word3;
                    end
                    3'd5: begin
                        if (header_ok && (&s_axis_rx_tkeep[1:0])) begin
                            src_ip <= pending_src_ip;
                            dst_ip <= pending_dst_ip;
                            udp_len <= pending_udp_len;
                            rx_valid <= 1'b1;
                        end
                    end
                    default: begin end
                endcase
                
                // Saturate while draining payload; a long frame cannot restart the parser.
                if (s_axis_rx_tlast) beat_cnt <= 3'd0;
                else if (beat_cnt != 3'd6) beat_cnt <= beat_cnt + 3'd1;
            end
        end
    end

endmodule
