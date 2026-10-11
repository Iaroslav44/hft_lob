module metadata_formal #(
    parameter integer KIND = 0  // 0: Ethernet metadata, 1: MoldUDP64, 2: deframed SBE.
) (input wire clk);
    localparam integer WIDTH = KIND == 1 ? 256 : 64;
    (* anyseq *) reg rst_n;
    (* anyseq *) reg [WIDTH-1:0] data;
    (* anyseq *) reg [WIDTH/8-1:0] keep;
    (* anyseq *) reg valid, last;
    wire actual_valid;
    wire [159:0] actual_metadata;
    wire [159:0] decoded_metadata;
    wire supported_header;
    reg [2:0] accepted_beats;
    wire header_position = KIND == 0 ? accepted_beats == 5 : accepted_beats == 0;

    generate
        if (KIND == 0) begin : network_case
            wire [31:0] src, dst;
            wire [15:0] length;
            reg [319:0] prefix_bytes;
            reg [39:0] prefix_keep;
            wire [383:0] header = {data, prefix_bytes};
            wire [47:0] byte_present = {keep, prefix_keep};
            wire [15:0] ip_length = {header[135:128], header[143:136]};
            wire [15:0] fragment = {header[167:160], header[175:168]};
            wire [15:0] udp_length = {header[311:304], header[319:312]};

            // This reference stores wire bytes, rather than copying the DUT's field pipeline.
            always @(posedge clk) begin
                if (!rst_n) begin
                    prefix_bytes <= 0;
                    prefix_keep <= 0;
                end else if (valid && accepted_beats < 5) begin
                    prefix_bytes[accepted_beats*64 +: 64] <= data;
                    prefix_keep[accepted_beats*8 +: 8] <= keep;
                end
            end
            assign supported_header = (&byte_present[41:0]) &&
                header[111:96] == 16'h0008 && header[119:112] == 8'h45 &&
                header[191:184] == 8'd17 && (fragment & 16'hbfff) == 0 &&
                ip_length >= 28 && udp_length >= 8 &&
                {1'b0, udp_length} + 17'd20 <= {1'b0, ip_length};
            assign decoded_metadata = {80'd0,
                header[215:208], header[223:216], header[231:224], header[239:232],
                header[247:240], header[255:248], header[263:256], header[271:264],
                udp_length};
            assign actual_metadata = {80'd0, src, dst, length};
            hft_network_stack dut (
                .clk(clk), .rst_n(rst_n), .s_axis_rx_tdata(data), .s_axis_rx_tkeep(keep),
                .s_axis_rx_tvalid(valid), .s_axis_rx_tlast(last),
                .src_ip(src), .dst_ip(dst), .udp_len(length), .rx_valid(actual_valid)
            );
        end else if (KIND == 1) begin : mold_case
            wire [79:0] session;
            wire [63:0] sequence_number;
            wire [15:0] count;
            wire [159:0] wire_header;
            genvar lane;
            for (lane = 0; lane < 20; lane = lane + 1) begin : byte_order
                assign wire_header[159-lane*8 -: 8] = data[lane*8 +: 8];
            end
            assign supported_header = &keep[19:0];
            assign decoded_metadata = wire_header;
            assign actual_metadata = {session, sequence_number, count};
            nasdaq_itch_moldudp64 dut (
                .clk(clk), .rst_n(rst_n), .s_axis_tdata(data), .s_axis_tkeep(keep),
                .s_axis_tvalid(valid), .s_axis_tlast(last),
                .mold_session(session), .mold_sequence(sequence_number),
                .mold_count(count), .msg_valid(actual_valid)
            );
        end else begin : sbe_case
            wire [15:0] template_id, schema_id, version;
            assign supported_header = &keep;
            assign decoded_metadata = {112'd0, data[31:16], data[47:32], data[63:48]};
            assign actual_metadata = {112'd0, template_id, schema_id, version};
            cme_sbe_parser dut (
                .clk(clk), .rst_n(rst_n), .s_axis_tdata(data), .s_axis_tkeep(keep),
                .s_axis_tvalid(valid), .s_axis_tlast(last),
                .msg_template_id(template_id), .schema_id(schema_id),
                .version(version), .out_valid(actual_valid)
            );
        end
    endgenerate

    reg expected_valid;
    reg [159:0] expected_metadata;
    reg past_valid = 0;
    reg saw_bad_header = 0, saw_gap = 0, saw_payload = 0, saw_midframe_reset = 0;
    wire emit_header = valid && header_position && supported_header;

    always @(posedge clk) begin
        past_valid <= 1;
        if (!past_valid) assume(!rst_n);
        if (past_valid && rst_n) begin
            assert(actual_valid == expected_valid);
            // Equality on every cycle also checks preservation on malformed input.
            assert(actual_metadata == expected_metadata);
        end
        if (past_valid && !$past(rst_n)) begin
            assert(!actual_valid);
            assert(actual_metadata == 0);
        end
        if (!rst_n) begin
            if (past_valid && accepted_beats != 0) saw_midframe_reset <= 1;
            accepted_beats <= 0;
            expected_valid <= 0;
            expected_metadata <= 0;
        end else begin
            expected_valid <= emit_header;
            if (emit_header) expected_metadata <= decoded_metadata;
            if (valid) begin
                if (last) accepted_beats <= 0;
                else if (accepted_beats < 6) accepted_beats <= accepted_beats + 1'b1;
            end
            if (valid && header_position && !supported_header) saw_bad_header <= 1;
            if (!valid && accepted_beats != 0) saw_gap <= 1;
            if (valid && accepted_beats == 6) saw_payload <= 1;
            cover(emit_header);
            cover(saw_bad_header && emit_header);
            cover(saw_gap && emit_header);
            cover(saw_payload && emit_header);
            cover(saw_midframe_reset && emit_header);
        end
    end
endmodule
