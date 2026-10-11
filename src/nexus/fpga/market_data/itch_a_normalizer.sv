`timescale 1ns/1ps

module itch_a_normalizer (
    input  wire         clk,
    input  wire         rst_n,
    input  wire [63:0]  s_axis_tdata,
    input  wire [7:0]   s_axis_tkeep,
    input  wire         s_axis_tvalid,
    input  wire         s_axis_tlast,
    output wire         s_axis_tready,
    input  wire [79:0]  s_msg_session,
    input  wire [63:0]  s_msg_sequence,
    input  wire [15:0]  s_msg_length,
    input  wire         s_msg_packet_last,

    output reg  [63:0]  m_axis_tdata,
    output wire [7:0]   m_axis_tkeep,
    output wire         m_axis_tvalid,
    output wire         m_axis_tlast,
    input  wire         m_axis_tready,
    output reg  [79:0]  m_order_session,
    output reg  [63:0]  m_order_sequence,
    output reg          m_order_packet_last,
    output reg          reject_valid,
    output wire         idle
);

    // Input is one committed Mold message per frame, repacked into low byte lanes.
    // Metadata stays constant throughout a message. Other ITCH types are skipped.
    localparam [1:0] FIRST = 2'd0, CAPTURE = 2'd1, DRAIN = 2'd2, EMIT = 2'd3;
    reg [1:0] state;
    reg [2:0] input_beat;
    reg [1:0] output_beat;
    reg drain_error;
    reg [63:0] order_id;
    reg [47:0] timestamp_ns;
    reg [31:0] price;
    reg [31:0] quantity;
    reg [15:0] locate;
    reg is_buy;

    wire [31:0] input_quantity = {s_axis_tdata[39:32], s_axis_tdata[47:40],
                                  s_axis_tdata[55:48], s_axis_tdata[63:56]};
    wire [7:0] input_side = s_axis_tdata[31:24];
    assign s_axis_tready = rst_n && state != EMIT;
    assign m_axis_tvalid = rst_n && state == EMIT;
    assign m_axis_tkeep = 8'hff;
    assign m_axis_tlast = output_beat == 2'd3;
    assign idle = rst_n && state == FIRST;

    // Canonical little-endian serialization of the host's 32-byte MdAddOrder.
    // Byte 26 is the side flag; bytes 27 through 31 are deterministic zero padding.
    always @* begin
        case (output_beat)
            2'd0: m_axis_tdata = order_id;
            2'd1: m_axis_tdata = {16'd0, timestamp_ns};
            2'd2: m_axis_tdata = {quantity, price};
            default: m_axis_tdata = {40'd0, 7'd0, is_buy, locate};
        endcase
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= FIRST;
            input_beat <= 0;
            output_beat <= 0;
            drain_error <= 1'b0;
            order_id <= 0;
            timestamp_ns <= 0;
            price <= 0;
            quantity <= 0;
            locate <= 0;
            is_buy <= 1'b0;
            m_order_session <= 0;
            m_order_sequence <= 0;
            m_order_packet_last <= 1'b0;
            reject_valid <= 1'b0;
        end else begin
            reject_valid <= 1'b0;
            case (state)
                FIRST: if (s_axis_tvalid) begin
                    if (s_msg_length == 16'd0 && s_axis_tkeep == 8'd0 && s_axis_tlast) begin
                        // Mold permits empty message blocks; they contain no ITCH message.
                    end else if (!s_axis_tkeep[0]) begin
                        drain_error <= 1'b1;
                        if (s_axis_tlast) reject_valid <= 1'b1;
                        else state <= DRAIN;
                    end else if (s_axis_tdata[7:0] != 8'h41) begin
                        drain_error <= 1'b0;
                        if (!s_axis_tlast) state <= DRAIN;
                    end else if (s_msg_length != 16'd36 || s_axis_tkeep != 8'hff || s_axis_tlast) begin
                        drain_error <= 1'b1;
                        if (s_axis_tlast) reject_valid <= 1'b1;
                        else state <= DRAIN;
                    end else begin
                        m_order_session <= s_msg_session;
                        m_order_sequence <= s_msg_sequence;
                        m_order_packet_last <= s_msg_packet_last;
                        locate <= {s_axis_tdata[15:8], s_axis_tdata[23:16]};
                        timestamp_ns[47:24] <= {s_axis_tdata[47:40], s_axis_tdata[55:48], s_axis_tdata[63:56]};
                        input_beat <= 3'd1;
                        state <= CAPTURE;
                    end
                end
                CAPTURE: if (s_axis_tvalid) begin
                    if (input_beat == 3'd4) begin
                        if (s_axis_tlast && s_axis_tkeep == 8'h0f) begin
                            price <= {s_axis_tdata[7:0], s_axis_tdata[15:8],
                                      s_axis_tdata[23:16], s_axis_tdata[31:24]};
                            output_beat <= 2'd0;
                            state <= EMIT;
                        end else begin
                            drain_error <= 1'b1;
                            if (s_axis_tlast) begin
                                reject_valid <= 1'b1;
                                state <= FIRST;
                            end else state <= DRAIN;
                        end
                    end else if (s_axis_tlast || s_axis_tkeep != 8'hff) begin
                        drain_error <= 1'b1;
                        if (s_axis_tlast) begin
                            reject_valid <= 1'b1;
                            state <= FIRST;
                        end else state <= DRAIN;
                    end else begin
                        input_beat <= input_beat + 3'd1;
                        case (input_beat)
                            3'd1: begin
                                timestamp_ns[23:0] <= {s_axis_tdata[7:0], s_axis_tdata[15:8], s_axis_tdata[23:16]};
                                order_id[63:24] <= {s_axis_tdata[31:24], s_axis_tdata[39:32], s_axis_tdata[47:40],
                                                   s_axis_tdata[55:48], s_axis_tdata[63:56]};
                            end
                            3'd2: begin
                                order_id[23:0] <= {s_axis_tdata[7:0], s_axis_tdata[15:8], s_axis_tdata[23:16]};
                                is_buy <= input_side == 8'h42;
                                quantity <= input_quantity;
                                if ((input_side != 8'h42 && input_side != 8'h53) || input_quantity == 32'd0) begin
                                    drain_error <= 1'b1;
                                    state <= DRAIN;
                                end
                            end
                            default: begin end
                        endcase
                    end
                end
                DRAIN: if (s_axis_tvalid && s_axis_tlast) begin
                    reject_valid <= drain_error;
                    state <= FIRST;
                end
                EMIT: if (m_axis_tready) begin
                    if (output_beat == 2'd3) begin
                        output_beat <= 2'd0;
                        state <= FIRST;
                    end else output_beat <= output_beat + 2'd1;
                end
                default: state <= FIRST;
            endcase
        end
    end
endmodule
