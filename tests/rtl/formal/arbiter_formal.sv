module arbiter_formal (input wire clk);
    (* anyseq *) reg rst_n;
    (* anyseq *) reg [63:0] a_data, b_data;
    (* anyseq *) reg a_valid, a_last, b_valid, b_last;
    wire [63:0] out_data;
    wire out_valid, out_last;

    feed_line_arbitrator dut (
        .clk(clk), .rst_n(rst_n),
        .feed_a_tdata(a_data), .feed_a_tvalid(a_valid), .feed_a_tlast(a_last),
        .feed_b_tdata(b_data), .feed_b_tvalid(b_valid), .feed_b_tlast(b_last),
        .out_tdata(out_data), .out_tvalid(out_valid), .out_tlast(out_last)
    );

    // A per-frame disposition model does not inspect the DUT's ownership state.
    localparam [1:0] BOUNDARY = 0, ADMITTED = 1, DISCARDED = 2;
    reg [1:0] a_frame, b_frame;
    wire take_a = a_valid && (a_frame == ADMITTED ||
                  (a_frame == BOUNDARY && b_frame != ADMITTED));
    wire take_b = b_valid && (b_frame == ADMITTED ||
                  (b_frame == BOUNDARY && a_frame != ADMITTED && !take_a));
    reg expected_valid, expected_last;
    reg [63:0] expected_data;
    reg past_valid = 0;
    reg saw_a = 0, saw_b = 0, saw_gap = 0, saw_delayed_tail = 0;
    reg saw_midframe_reset = 0;

    always @(posedge clk) begin
        past_valid <= 1;
        // Reset establishes frame boundaries; later traffic and resets stay unconstrained.
        if (!past_valid) assume(!rst_n);
        if (past_valid && rst_n) begin
            assert(out_valid == expected_valid);
            assert(out_last == expected_last);
            if (expected_valid) assert(out_data == expected_data);
        end
        if (past_valid && !$past(rst_n)) begin
            assert(!out_valid);
            assert(!out_last);
            assert(out_data == 0);
        end

        if (!rst_n) begin
            if (past_valid && (a_frame != BOUNDARY || b_frame != BOUNDARY))
                saw_midframe_reset <= 1;
            a_frame <= BOUNDARY;
            b_frame <= BOUNDARY;
            expected_valid <= 0;
            expected_last <= 0;
            expected_data <= 0;
        end else begin
            assert(!(a_frame == ADMITTED && b_frame == ADMITTED));
            expected_valid <= take_a || take_b;
            expected_last <= (take_a && a_last) || (take_b && b_last);
            if (take_a) expected_data <= a_data;
            else if (take_b) expected_data <= b_data;
            if (a_valid) a_frame <= a_last ? BOUNDARY : (take_a ? ADMITTED : DISCARDED);
            if (b_valid) b_frame <= b_last ? BOUNDARY : (take_b ? ADMITTED : DISCARDED);

            if (take_a) saw_a <= 1;
            if (take_b) saw_b <= 1;
            if ((a_frame == ADMITTED && !a_valid) || (b_frame == ADMITTED && !b_valid))
                saw_gap <= 1;
            if (b_frame == DISCARDED && b_valid && b_last && a_frame == BOUNDARY)
                saw_delayed_tail <= 1;
            cover(take_a && a_valid && b_valid && a_frame == BOUNDARY && b_frame == BOUNDARY);
            cover(saw_a && saw_b && saw_gap);
            cover(saw_delayed_tail && take_b);
            cover(saw_midframe_reset && (take_a || take_b));
        end
    end
endmodule
