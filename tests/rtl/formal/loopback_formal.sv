module loopback_formal;
    (* anyseq *) reg rst_n;
    (* anyseq *) reg [511:0] data;
    (* anyseq *) reg valid, last;
    wire [511:0] output_data;
    wire output_valid, output_last;

    nexus_network_stack_top dut (
        .clk(1'b0), .rst_n(rst_n), .s_axis_net_tdata(data),
        .s_axis_net_tvalid(valid), .s_axis_net_tlast(last),
        .m_axis_net_tdata(output_data), .m_axis_net_tvalid(output_valid),
        .m_axis_net_tlast(output_last)
    );
    always @* begin
        assert(output_data == data);
        assert(output_last == last);
        assert(output_valid == (rst_n && valid));
        cover(rst_n && valid);
        cover(!rst_n && valid);
        cover(rst_n && !valid);
    end
endmodule
