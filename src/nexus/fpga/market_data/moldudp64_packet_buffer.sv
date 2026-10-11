`timescale 1ns/1ps

module moldudp64_packet_buffer #(
    parameter integer MAX_PACKET_BYTES = 2048
) (
    input  wire         clk,
    input  wire         rst_n,
    input  wire [63:0]  s_axis_tdata,
    input  wire [7:0]   s_axis_tkeep,
    input  wire         s_axis_tvalid,
    input  wire         s_axis_tlast,
    input  wire         s_axis_tuser,
    output wire         s_axis_tready,

    output wire         packet_valid,
    input  wire         packet_ready,
    input  wire         packet_accept,
    output wire [79:0]  packet_session,
    output wire [63:0]  packet_sequence,
    output wire [15:0]  packet_count,
    output wire [15:0]  packet_bytes,
    output wire [3:0]   packet_error,

    output reg  [63:0]  m_axis_tdata,
    output reg  [7:0]   m_axis_tkeep,
    output wire         m_axis_tvalid,
    output reg          m_axis_tlast,
    input  wire         m_axis_tready,
    output wire [79:0]  m_msg_session,
    output reg  [63:0]  m_msg_sequence,
    output reg  [15:0]  m_msg_length,
    output wire         m_msg_packet_last,
    output wire         idle
);

    // One already validated UDP payload per input frame, lane zero first.
    // Receive and validate before publishing anything; only one packet is buffered.
    localparam integer WORDS = MAX_PACKET_BYTES / 8;
    localparam integer ADDR_W = (WORDS > 1) ? $clog2(WORDS) : 1;
    localparam [3:0] ERR_NONE = 4'd0, ERR_KEEP = 4'd1, ERR_CAPACITY = 4'd2,
                     ERR_HEADER = 4'd3, ERR_LENGTH = 4'd4, ERR_BODY = 4'd5,
                     ERR_COUNT = 4'd6, ERR_SEQUENCE = 4'd7, ERR_ITCH_A = 4'd8,
                     ERR_UPSTREAM = 4'd9;
    localparam [2:0] RECEIVE = 3'd0, DECIDE = 3'd1, FETCH = 3'd2,
                     UNPACK = 3'd3, SEND = 3'd4;
    localparam [2:0] HEADER = 3'd0, LENGTH_HI = 3'd1, LENGTH_LO = 3'd2,
                     BODY = 3'd3, DONE = 3'd4;

    generate
        if (MAX_PACKET_BYTES < 24 || MAX_PACKET_BYTES > 65528 ||
            MAX_PACKET_BYTES % 8 != 0) begin : invalid_capacity
            initial $fatal(1, "MAX_PACKET_BYTES must be a multiple of 8 in [24,65528]");
        end
    endgenerate

    // One synchronous read port and one write port; RAM contents do not need reset.
    reg [63:0] memory [0:WORDS-1];
    reg [63:0] read_word;
    reg [ADDR_W:0] write_word;
    reg [16:0] received_bytes;
    reg receiving_packet;
    reg [2:0] state;

    reg [2:0] phase_q, phase_d;
    reg [4:0] header_pos_q, header_pos_d;
    reg [79:0] session_q, session_d;
    reg [63:0] sequence_q, sequence_d;
    reg [15:0] count_q, count_d;
    reg [15:0] blocks_q, blocks_d;
    reg [15:0] length_q, length_d;
    reg [15:0] remaining_q, remaining_d;
    reg [15:0] body_pos_q, body_pos_d;
    reg [7:0] type_q, type_d;
    reg [31:0] quantity_q, quantity_d;
    reg [3:0] error_q, error_d;
    reg [3:0] lane_count;
    reg [64:0] sequence_end;
    reg [7:0] byte_value;
    integer lane;

    reg [15:0] read_pos;
    reg [2:0] replay_phase;
    reg [7:0] length_hi;
    reg [15:0] replay_remaining;
    reg [15:0] replay_blocks;
    reg [2:0] output_lane;
    wire [7:0] replay_byte = read_word[read_pos[2:0] * 8 +: 8];
    reg [16:0] next_byte_count;

    assign s_axis_tready = rst_n && state == RECEIVE;
    assign packet_valid = rst_n && state == DECIDE;
    assign packet_session = session_q;
    assign packet_sequence = sequence_q;
    assign packet_count = count_q;
    assign packet_bytes = received_bytes[15:0];
    assign packet_error = error_q;
    assign m_axis_tvalid = rst_n && state == SEND;
    assign m_msg_session = session_q;
    assign m_msg_packet_last = replay_blocks == 16'd1;
    assign idle = rst_n && state == RECEIVE && !receiving_packet;

    // The byte-state transition is unrolled across the eight lanes of a beat.
    // An error is sticky through TLAST; a rejected frame is drained without output.
    always @* begin
        phase_d = phase_q;
        header_pos_d = header_pos_q;
        session_d = session_q;
        sequence_d = sequence_q;
        count_d = count_q;
        blocks_d = blocks_q;
        length_d = length_q;
        remaining_d = remaining_q;
        body_pos_d = body_pos_q;
        type_d = type_q;
        quantity_d = quantity_q;
        error_d = error_q;
        lane_count = 4'd0;
        sequence_end = 65'd0;
        byte_value = 8'd0;

        for (lane = 0; lane < 8; lane = lane + 1)
            lane_count = lane_count + {3'd0, s_axis_tkeep[lane]};
        next_byte_count = received_bytes + {13'd0, lane_count};

        // A transport or CDC abort marker quarantines the complete packet.
        if (error_d == ERR_NONE && s_axis_tuser)
            error_d = ERR_UPSTREAM;
        if (error_d == ERR_NONE &&
            ((!s_axis_tlast && s_axis_tkeep != 8'hff) ||
             (s_axis_tlast && (s_axis_tkeep == 8'd0 ||
                              (s_axis_tkeep & (s_axis_tkeep + 8'd1)) != 8'd0))))
            error_d = ERR_KEEP;
        if (error_d == ERR_NONE && next_byte_count > MAX_PACKET_BYTES)
            error_d = ERR_CAPACITY;

        for (lane = 0; lane < 8; lane = lane + 1) begin
            byte_value = s_axis_tdata[lane * 8 +: 8];
            if (s_axis_tkeep[lane] && error_d == ERR_NONE) begin
                case (phase_d)
                    HEADER: begin
                        if (header_pos_d < 5'd10)
                            session_d = {session_d[71:0], byte_value};
                        else if (header_pos_d < 5'd18)
                            sequence_d = {sequence_d[55:0], byte_value};
                        else
                            count_d = {count_d[7:0], byte_value};
                        if (header_pos_d == 5'd19) begin
                            if (count_d == 16'd0 || count_d == 16'hffff) begin
                                phase_d = DONE;
                            end else begin
                                sequence_end = {1'b0, sequence_d} + {49'd0, count_d};
                                if (sequence_end[64]) error_d = ERR_SEQUENCE;
                                blocks_d = count_d;
                                phase_d = LENGTH_HI;
                            end
                        end else begin
                            header_pos_d = header_pos_d + 5'd1;
                        end
                    end
                    LENGTH_HI: begin
                        length_d = {byte_value, 8'd0};
                        phase_d = LENGTH_LO;
                    end
                    LENGTH_LO: begin
                        length_d = {length_d[15:8], byte_value};
                        remaining_d = length_d;
                        body_pos_d = 16'd0;
                        type_d = 8'd0;
                        quantity_d = 32'd0;
                        if (length_d == 16'd0) begin
                            blocks_d = blocks_d - 16'd1;
                            phase_d = blocks_d == 16'd0 ? DONE : LENGTH_HI;
                        end else begin
                            phase_d = BODY;
                        end
                    end
                    BODY: begin
                        if (body_pos_d == 16'd0) begin
                            type_d = byte_value;
                            if (byte_value == 8'h41 && length_d != 16'd36)
                                error_d = ERR_ITCH_A;
                        end
                        if (type_d == 8'h41) begin
                            if (body_pos_d == 16'd19 && byte_value != 8'h42 && byte_value != 8'h53)
                                error_d = ERR_ITCH_A;
                            if (body_pos_d >= 16'd20 && body_pos_d <= 16'd23) begin
                                quantity_d = {quantity_d[23:0], byte_value};
                                if (body_pos_d == 16'd23 && quantity_d == 32'd0)
                                    error_d = ERR_ITCH_A;
                            end
                        end
                        remaining_d = remaining_d - 16'd1;
                        body_pos_d = body_pos_d + 16'd1;
                        if (remaining_d == 16'd0) begin
                            blocks_d = blocks_d - 16'd1;
                            phase_d = blocks_d == 16'd0 ? DONE : LENGTH_HI;
                        end
                    end
                    default: error_d = ERR_COUNT;
                endcase
            end
        end

        if (s_axis_tlast && error_d == ERR_NONE) begin
            case (phase_d)
                HEADER: error_d = ERR_HEADER;
                LENGTH_HI: error_d = ERR_COUNT;
                LENGTH_LO: error_d = ERR_LENGTH;
                BODY: error_d = ERR_BODY;
                default: begin end
            endcase
        end
    end

    always @(posedge clk) begin
        if (rst_n && state == RECEIVE && s_axis_tvalid && write_word < WORDS)
            memory[write_word[ADDR_W-1:0]] <= s_axis_tdata;
        if (rst_n && state == FETCH)
            read_word <= memory[read_pos >> 3];
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= RECEIVE;
            receiving_packet <= 1'b0;
            write_word <= 0;
            received_bytes <= 0;
            phase_q <= HEADER;
            header_pos_q <= 0;
            session_q <= 0;
            sequence_q <= 0;
            count_q <= 0;
            blocks_q <= 0;
            length_q <= 0;
            remaining_q <= 0;
            body_pos_q <= 0;
            type_q <= 0;
            quantity_q <= 0;
            error_q <= ERR_NONE;
            read_pos <= 0;
            replay_phase <= LENGTH_HI;
            length_hi <= 0;
            replay_remaining <= 0;
            replay_blocks <= 0;
            output_lane <= 0;
            m_axis_tdata <= 0;
            m_axis_tkeep <= 0;
            m_axis_tlast <= 0;
            m_msg_sequence <= 0;
            m_msg_length <= 0;
        end else begin
            case (state)
                RECEIVE: if (s_axis_tvalid) begin
                    receiving_packet <= 1'b1;
                    if (write_word < WORDS) write_word <= write_word + 1'b1;
                    received_bytes <= next_byte_count > MAX_PACKET_BYTES ?
                                      MAX_PACKET_BYTES + 1 : next_byte_count;
                    phase_q <= phase_d;
                    header_pos_q <= header_pos_d;
                    session_q <= session_d;
                    sequence_q <= sequence_d;
                    count_q <= count_d;
                    blocks_q <= blocks_d;
                    length_q <= length_d;
                    remaining_q <= remaining_d;
                    body_pos_q <= body_pos_d;
                    type_q <= type_d;
                    quantity_q <= quantity_d;
                    error_q <= error_d;
                    if (s_axis_tlast) state <= DECIDE;
                end
                DECIDE: if (packet_ready) begin
                    if (packet_accept && error_q == ERR_NONE && count_q != 0 && count_q != 16'hffff) begin
                        read_pos <= 16'd20;
                        replay_phase <= LENGTH_HI;
                        replay_blocks <= count_q;
                        m_msg_sequence <= sequence_q;
                        m_axis_tdata <= 0;
                        m_axis_tkeep <= 0;
                        m_axis_tlast <= 0;
                        output_lane <= 0;
                        state <= FETCH;
                    end else begin
                        state <= RECEIVE;
                        receiving_packet <= 1'b0;
                        write_word <= 0;
                        received_bytes <= 0;
                        phase_q <= HEADER;
                        header_pos_q <= 0;
                        session_q <= 0;
                        sequence_q <= 0;
                        count_q <= 0;
                        error_q <= ERR_NONE;
                    end
                end
                FETCH: state <= UNPACK;
                UNPACK: begin
                    read_pos <= read_pos + 16'd1;
                    state <= read_pos[2:0] == 3'd7 ? FETCH : UNPACK;
                    case (replay_phase)
                        LENGTH_HI: begin
                            length_hi <= replay_byte;
                            replay_phase <= LENGTH_LO;
                        end
                        LENGTH_LO: begin
                            m_msg_length <= {length_hi, replay_byte};
                            replay_remaining <= {length_hi, replay_byte};
                            replay_phase <= BODY;
                            if ({length_hi, replay_byte} == 16'd0) begin
                                m_axis_tlast <= 1'b1;
                                state <= SEND;
                            end
                        end
                        default: begin
                            m_axis_tdata[output_lane * 8 +: 8] <= replay_byte;
                            m_axis_tkeep[output_lane] <= 1'b1;
                            replay_remaining <= replay_remaining - 16'd1;
                            output_lane <= output_lane + 3'd1;
                            if (output_lane == 3'd7 || replay_remaining == 16'd1) begin
                                m_axis_tlast <= replay_remaining == 16'd1;
                                state <= SEND;
                            end
                        end
                    endcase
                end
                SEND: if (m_axis_tready) begin
                    m_axis_tdata <= 0;
                    m_axis_tkeep <= 0;
                    output_lane <= 0;
                    state <= FETCH;
                    if (m_axis_tlast) begin
                        m_axis_tlast <= 0;
                        if (replay_blocks == 16'd1) begin
                            state <= RECEIVE;
                            receiving_packet <= 1'b0;
                            write_word <= 0;
                            received_bytes <= 0;
                            phase_q <= HEADER;
                            header_pos_q <= 0;
                            session_q <= 0;
                            sequence_q <= 0;
                            count_q <= 0;
                            error_q <= ERR_NONE;
                        end else begin
                            replay_blocks <= replay_blocks - 16'd1;
                            m_msg_sequence <= m_msg_sequence + 64'd1;
                            replay_phase <= LENGTH_HI;
                        end
                    end
                end
                default: state <= RECEIVE;
            endcase
        end
    end
endmodule
