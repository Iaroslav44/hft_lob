`timescale 1ns/1ps

module tb_streams;
    reg clk = 1'b0;
    always #5 clk = ~clk;
    reg rst_n = 1'b0;

    reg [63:0] a_data = 64'h0;
    reg a_valid = 1'b0;
    reg a_last = 1'b0;
    reg [63:0] b_data = 64'h0;
    reg b_valid = 1'b0;
    reg b_last = 1'b0;
    wire [63:0] out_data;
    wire out_valid;
    wire out_last;

    reg [511:0] net_data = 512'h0;
    reg net_valid = 1'b0;
    reg net_last = 1'b0;
    wire [511:0] net_out_data;
    wire net_out_valid;
    wire net_out_last;
    integer step_number = 0;

    feed_line_arbitrator arbiter (
        .clk(clk), .rst_n(rst_n),
        .feed_a_tdata(a_data), .feed_a_tvalid(a_valid), .feed_a_tlast(a_last),
        .feed_b_tdata(b_data), .feed_b_tvalid(b_valid), .feed_b_tlast(b_last),
        .out_tdata(out_data), .out_tvalid(out_valid), .out_tlast(out_last)
    );

    nexus_network_stack_top loopback (
        .clk(clk), .rst_n(rst_n),
        .s_axis_net_tdata(net_data), .s_axis_net_tvalid(net_valid),
        .s_axis_net_tlast(net_last), .m_axis_net_tdata(net_out_data),
        .m_axis_net_tvalid(net_out_valid), .m_axis_net_tlast(net_out_last)
    );

    task automatic reset_arbiter;
        begin
            @(negedge clk);
            rst_n = 1'b0;
            a_valid = 1'b0;
            b_valid = 1'b0;
            a_last = 1'b0;
            b_last = 1'b0;
            @(posedge clk);
            #1;
            if (out_valid !== 1'b0 || out_data !== 64'h0 || out_last !== 1'b0)
                $fatal(1, "arbiter reset must clear all outputs");
            @(negedge clk);
            rst_n = 1'b1;
        end
    endtask

    task automatic beat(
        input bit av, input [63:0] ad, input bit al,
        input bit bv, input [63:0] bd, input bit bl,
        input bit expected_valid, input [63:0] expected_data, input bit expected_last
    );
        begin
            @(negedge clk);
            a_valid = av;
            a_data = ad;
            a_last = al;
            b_valid = bv;
            b_data = bd;
            b_last = bl;
            @(posedge clk);
            #1;
            step_number = step_number + 1;
            if (out_valid !== expected_valid)
                $fatal(1, "step %0d: output valid=%b, expected=%b",
                       step_number, out_valid, expected_valid);
            if (expected_valid &&
                (out_data !== expected_data || out_last !== expected_last))
                $fatal(1, "step %0d: output data=%h last=%b, expected=%h/%b",
                       step_number, out_data, out_last, expected_data, expected_last);
        end
    endtask

    initial begin
        if (!$test$plusargs("ARBITER_ONLY")) begin
            @(negedge clk);
            rst_n = 1'b0;
            net_data = {8{64'h0123456789abcdef}};
            net_valid = 1'b1;
            net_last = 1'b1;
            #1;
            if (net_out_valid !== 1'b0)
                $fatal(1, "network loopback asserted valid during reset");
            @(negedge clk);
            rst_n = 1'b1;
            #1;
            if (net_out_valid !== 1'b1 || net_out_data !== net_data || net_out_last !== 1'b1)
                $fatal(1, "network loopback altered a valid beat");
            net_valid = 1'b0;
            #1;
            if (net_out_valid !== 1'b0)
                $fatal(1, "network loopback ignored an idle input");
            net_last = 1'b0;
        end

        if (!$test$plusargs("TOP_ONLY")) begin
            reset_arbiter();
            // The delayed copy of a losing packet must not emerge as a fragment.
            beat(1, 64'ha0, 0, 0, 64'h0,  0, 1, 64'ha0, 0);
            beat(1, 64'ha1, 1, 1, 64'hb0, 0, 1, 64'ha1, 1);
            beat(0, 64'h0,  0, 1, 64'hb1, 1, 0, 64'h0,  0);
            beat(0, 64'h0,  0, 1, 64'hb2, 1, 1, 64'hb2, 1);

            // Repeat with B owning output; A's priority cannot cut into B's frame.
            beat(0, 64'h0,  0, 1, 64'hb3, 0, 1, 64'hb3, 0);
            beat(1, 64'ha2, 0, 1, 64'hb4, 1, 1, 64'hb4, 1);
            beat(1, 64'ha3, 1, 0, 64'h0,  0, 0, 64'h0,  0);
            beat(1, 64'ha4, 1, 0, 64'h0,  0, 1, 64'ha4, 1);

            // A wins simultaneous starts and retains ownership through bubbles.
            beat(1, 64'ha5, 0, 1, 64'hb5, 0, 1, 64'ha5, 0);
            beat(0, 64'h0,  0, 1, 64'hb6, 0, 0, 64'h0,  0);
            beat(1, 64'ha6, 0, 1, 64'hb7, 1, 1, 64'ha6, 0);
            beat(0, 64'h0,  0, 1, 64'hb8, 1, 0, 64'h0,  0);
            beat(1, 64'ha7, 1, 1, 64'hb9, 0, 1, 64'ha7, 1);
            beat(0, 64'h0,  0, 1, 64'hba, 1, 0, 64'h0,  0);
            beat(0, 64'h0,  0, 1, 64'hbb, 1, 1, 64'hbb, 1);

            // TLAST has no effect without TVALID, including inside a selected frame.
            beat(0, 64'h0,  1, 0, 64'h0,  1, 0, 64'h0,  0);
            beat(0, 64'h0,  0, 1, 64'hbc, 0, 1, 64'hbc, 0);
            beat(1, 64'ha8, 1, 0, 64'h0,  1, 0, 64'h0,  0);
            beat(0, 64'h0,  0, 1, 64'hbd, 1, 1, 64'hbd, 1);

            // Reset abandons ownership; upstream restarts on packet boundaries.
            beat(1, 64'ha9, 0, 0, 64'h0,  0, 1, 64'ha9, 0);
            reset_arbiter();
            beat(0, 64'h0,  0, 1, 64'hbe, 1, 1, 64'hbe, 1);
            beat(1, 64'haa, 1, 0, 64'h0,  0, 1, 64'haa, 1);
            beat(0, 64'h0,  0, 0, 64'h0,  0, 0, 64'h0,  0);
        end

        $display("RTL_TEST_PASS: tb_streams");
        $finish;
    end

    initial begin
        #10000;
        $fatal(1, "stream test watchdog expired");
    end
endmodule
