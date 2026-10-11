`timescale 1ns/1ps

module tb_parsers;
    reg clk = 1'b0;
    always #5 clk = ~clk;
    reg rst_n = 1'b0;
    integer failures = 0;
    integer checks = 0;

    initial begin
        #100000;
        $fatal(1, "parser test watchdog expired");
    end

    reg [255:0] mold_data = 0;
    reg [31:0] mold_keep = 0;
    reg mold_valid = 0;
    reg mold_last = 0;
    wire [79:0] mold_session;
    wire [63:0] mold_sequence;
    wire [15:0] mold_count;
    wire mold_out_valid;

    nasdaq_itch_moldudp64 mold (
        .clk(clk), .rst_n(rst_n), .s_axis_tdata(mold_data),
        .s_axis_tkeep(mold_keep), .s_axis_tvalid(mold_valid),
        .s_axis_tlast(mold_last), .mold_session(mold_session),
        .mold_sequence(mold_sequence), .mold_count(mold_count),
        .msg_valid(mold_out_valid)
    );

    reg [63:0] sbe_data = 0;
    reg [7:0] sbe_keep = 0;
    reg sbe_valid = 0;
    reg sbe_last = 0;
    wire [15:0] template_id;
    wire [15:0] schema_id;
    wire [15:0] version;
    wire sbe_out_valid;

    cme_sbe_parser sbe (
        .clk(clk), .rst_n(rst_n), .s_axis_tdata(sbe_data),
`ifndef LEGACY_RTL
        .s_axis_tkeep(sbe_keep),
`endif
        .s_axis_tvalid(sbe_valid), .s_axis_tlast(sbe_last),
        .msg_template_id(template_id), .schema_id(schema_id),
        .version(version), .out_valid(sbe_out_valid)
    );

    reg [63:0] net_data = 0;
    reg [7:0] net_keep = 0;
    reg net_valid = 0;
    reg net_last = 0;
    wire [31:0] src_ip;
    wire [31:0] dst_ip;
    wire [15:0] udp_len;
    wire net_out_valid;

    hft_network_stack network (
        .clk(clk), .rst_n(rst_n), .s_axis_rx_tdata(net_data),
`ifndef LEGACY_RTL
        .s_axis_rx_tkeep(net_keep),
`endif
        .s_axis_rx_tvalid(net_valid), .s_axis_rx_tlast(net_last),
        .src_ip(src_ip), .dst_ip(dst_ip), .udp_len(udp_len),
        .rx_valid(net_out_valid)
    );

    reg [7:0] packet [0:2047];
    reg [255:0] mold_header;
    reg [79:0] saved_session;
    reg [63:0] saved_sequence;
    reg [15:0] saved_count;
    reg [47:0] saved_sbe;
    reg [15:0] expected_udp_len = 16'd20;
    integer i;

    task automatic check(input bit condition, input string message);
        begin
            checks = checks + 1;
            if (!condition) begin
                failures = failures + 1;
                $display("FAIL: %s", message);
            end
        end
    endtask

    task automatic reset_all;
        begin
            @(negedge clk);
            rst_n = 0;
            mold_valid = 0;
            sbe_valid = 0;
            net_valid = 0;
            @(posedge clk); #1;
            check({mold_out_valid, mold_session, mold_sequence, mold_count} === 161'b0,
                  "Mold reset clears metadata and valid");
            check({sbe_out_valid, template_id, schema_id, version} === 49'b0,
                  "SBE reset clears metadata and valid");
            check({net_out_valid, src_ip, dst_ip, udp_len} === 81'b0,
                  "network reset clears metadata and valid");
            @(negedge clk);
            rst_n = 1;
        end
    endtask

    task automatic mold_step(input [255:0] data, input [31:0] keep,
                             input bit valid, input bit last, input bit expected);
        begin
            @(negedge clk);
            mold_data = data; mold_keep = keep; mold_valid = valid; mold_last = last;
            @(posedge clk); #1;
            check(mold_out_valid === expected, "Mold valid pulse");
        end
    endtask

    task automatic sbe_step(input [63:0] data, input [7:0] keep,
                            input bit valid, input bit last, input bit expected);
        begin
            @(negedge clk);
            sbe_data = data; sbe_keep = keep; sbe_valid = valid; sbe_last = last;
            @(posedge clk); #1;
            check(sbe_out_valid === expected, "SBE valid pulse");
        end
    endtask

    task automatic make_packet;
        integer j;
        begin
            for (j = 0; j < 2048; j = j + 1) packet[j] = 0;
            packet[12] = 8'h08; packet[13] = 8'h00;
            packet[14] = 8'h45;
            packet[16] = 0; packet[17] = 40;
            packet[20] = 8'h40; packet[21] = 0;
            packet[22] = 64; packet[23] = 17;
            packet[26] = 192; packet[27] = 0; packet[28] = 2; packet[29] = 1;
            packet[30] = 198; packet[31] = 51; packet[32] = 100; packet[33] = 2;
            packet[38] = 0; packet[39] = 20;
        end
    endtask

    task automatic network_step(input [63:0] data, input [7:0] keep,
                                input bit valid, input bit last, input bit expected);
        begin
            @(negedge clk);
            net_data = data; net_keep = keep; net_valid = valid; net_last = last;
            @(posedge clk); #1;
            check(net_out_valid === expected, "network valid pulse");
            if (expected) begin
                check(src_ip === 32'hc0000201, "network source address byte order");
                check(dst_ip === 32'hc6336402, "network destination address across beats");
                check(udp_len === expected_udp_len, "network UDP length offset and byte order");
            end
        end
    endtask

    task automatic send_packet(input integer length, input integer bad_keep_beat,
                               input [7:0] bad_keep, input bit accepted,
                               input bit idle_after = 1);
        integer beat;
        integer lane;
        integer words;
        reg [63:0] data;
        reg [7:0] keep;
        reg [79:0] previous_metadata;
        begin
            previous_metadata = {src_ip, dst_ip, udp_len};
            words = (length + 7) / 8;
            for (beat = 0; beat < words; beat = beat + 1) begin
                data = 0;
                keep = 0;
                for (lane = 0; lane < 8; lane = lane + 1) begin
                    if (beat * 8 + lane < length) begin
                        data[lane * 8 +: 8] = packet[beat * 8 + lane];
                        // A rejected frame must not leak a different address into published metadata.
                        if (!accepted && beat * 8 + lane >= 26 && beat * 8 + lane < 34)
                            data[lane * 8 +: 8] = ~packet[beat * 8 + lane];
                        keep[lane] = 1;
                    end
                end
                if (beat == bad_keep_beat) keep = bad_keep;
                if (beat == 3)
                    network_step(64'hffffffffffffffff, 8'hff, 0, 1, 0);
                network_step(data, keep, 1, beat == words - 1, accepted && beat == 5);
            end
            if (idle_after) network_step(0, 0, 0, 1, 0);
            if (!accepted)
                check({src_ip, dst_ip, udp_len} === previous_metadata,
                      "network rejection preserves published metadata");
        end
    endtask

    initial begin
        reset_all();

        // Session ABCDEFGHIJ, sequence 0102030405060708, count 2, in wire order.
        mold_header = 0;
        for (i = 0; i < 10; i = i + 1) mold_header[i * 8 +: 8] = 8'h41 + i;
        for (i = 0; i < 8; i = i + 1) mold_header[(10 + i) * 8 +: 8] = i + 1;
        mold_header[19 * 8 +: 8] = 2;
        mold_step(mold_header, 32'h000fffff, 1, 1, 1);
        check(mold_session === 80'h4142434445464748494a, "Mold preserves all ten session bytes");
        check(mold_sequence === 64'h0102030405060708, "Mold sequence starts at byte ten");
        check(mold_count === 16'd2, "Mold count starts at byte eighteen");
        saved_session = mold_session; saved_sequence = mold_sequence; saved_count = mold_count;
        for (i = 0; i < 20; i = i + 1) begin
            mold_step(~mold_header, 32'h000fffff ^ (32'b1 << i), 1, 1, 0);
            check({mold_session, mold_sequence, mold_count} ===
                  {saved_session, saved_sequence, saved_count}, "Mold rejection preserves metadata");
        end
        mold_step(mold_header, 32'hffffffff, 1, 0, 1);
        mold_step(0, 0, 0, 1, 0);
        mold_step(~mold_header, 32'hffffffff, 1, 0, 0);
        mold_step(~mold_header, 32'hffffffff, 1, 1, 0);
        mold_step(mold_header, 0, 1, 0, 0);
        mold_step(mold_header, 32'hffffffff, 1, 1, 0);
        mold_header[159:144] = 0;
        mold_step(mold_header, 32'h000fffff, 1, 1, 1);
        check(mold_count === 0, "Mold heartbeat header is valid");
        mold_header[159:144] = 16'hffff;
        mold_step(mold_header, 32'h000fffff, 1, 1, 1);
        check(mold_count === 16'hffff, "Mold end-of-session header is valid");
        mold_step(0, 0, 0, 0, 0);

        // CME example after the packet header and MsgSize have been removed.
        sbe_step(64'h000900010032000b, 8'hff, 1, 1, 1);
        check({template_id, schema_id, version} === {16'd50, 16'd1, 16'd9},
              "CME raw SBE header uses little-endian fields");
        saved_sbe = {template_id, schema_id, version};
        for (i = 0; i < 8; i = i + 1) begin
            sbe_step(64'hffffffffffffffff, 8'hff ^ (8'b1 << i), 1, 1, 0);
            check({template_id, schema_id, version} === saved_sbe,
                  "SBE rejection preserves metadata");
        end
        sbe_step(64'h000900010032000b, 8'hff, 1, 0, 1);
        sbe_step(0, 0, 0, 1, 0);
        sbe_step(64'hffffffffffffffff, 8'hff, 1, 1, 0);
        sbe_step(0, 0, 1, 0, 0);
        sbe_step(64'h000900010032000b, 8'hff, 1, 1, 0);
        sbe_step(64'h000900010032000b, 8'hff, 1, 1, 1);
        sbe_step(0, 0, 0, 0, 0);

        make_packet();
        send_packet(54, -1, 0, 1);
        send_packet(42, -1, 0, 1, 0);
        send_packet(42, -1, 0, 1);
        send_packet(200, -1, 0, 1);
        network_step(0, 0, 1, 1, 0);
        for (i = 1; i < 42; i = i + 1) send_packet(i, -1, 0, 0);
        for (i = 0; i < 5; i = i + 1) send_packet(54, i, 8'hfe, 0);
        send_packet(54, 5, 8'hfd, 0);
        send_packet(54, 5, 8'hfe, 0);
        packet[12] = 8'h81; packet[13] = 0;
        send_packet(54, -1, 0, 0);
        make_packet(); packet[14] = 8'h46;
        send_packet(54, -1, 0, 0);
        make_packet(); packet[14] = 8'h65;
        send_packet(54, -1, 0, 0);
        make_packet(); packet[23] = 6;
        send_packet(54, -1, 0, 0);
        make_packet(); packet[20] = 8'h20;
        send_packet(54, -1, 0, 0);
        make_packet(); packet[20] = 8'h80;
        send_packet(54, -1, 0, 0);
        make_packet(); packet[21] = 1;
        send_packet(54, -1, 0, 0);
        make_packet(); packet[17] = 27;
        send_packet(54, -1, 0, 0);
        make_packet(); packet[17] = 0;
        send_packet(54, -1, 0, 0);
        make_packet(); packet[39] = 7;
        send_packet(54, -1, 0, 0);
        make_packet(); packet[39] = 21;
        send_packet(54, -1, 0, 0);
        make_packet();
        send_packet(54, -1, 0, 1);
        packet[16] = 8'hff; packet[17] = 8'hff;
        packet[38] = 8'hff; packet[39] = 8'heb;
        expected_udp_len = 16'hffeb;
        send_packet(42, -1, 0, 1);
        packet[39] = 8'hec;
        send_packet(42, -1, 0, 0);
        make_packet(); packet[17] = 28; packet[39] = 8;
        expected_udp_len = 16'd8;
        send_packet(42, -1, 0, 1);
        expected_udp_len = 16'd20;

        // Reset must discard incomplete frames independently in all three parsers.
        mold_step(mold_header, 32'hffffffff, 1, 0, 1);
        sbe_step(64'h000900010032000b, 8'hff, 1, 0, 1);
        network_step(0, 8'hff, 1, 0, 0);
        reset_all();
        mold_step(mold_header, 32'h000fffff, 1, 1, 1);
        mold_step(0, 0, 0, 0, 0);
        sbe_step(64'h000900010032000b, 8'hff, 1, 1, 1);
        sbe_step(0, 0, 0, 0, 0);
        make_packet();
        send_packet(54, -1, 0, 1);

        if (failures != 0) $fatal(1, "%0d of %0d parser checks failed", failures, checks);
        $display("PASS: %0d parser checks", checks);
        $display("RTL_TEST_PASS: tb_parsers");
        $finish;
    end
endmodule
