module session_gate_formal(input wire clk);
    (* anyseq *) reg rst_n;
    (* anyseq *) reg quiescent;
    (* anyseq *) reg cfg_valid;
    (* anyseq *) reg [79:0] cfg_session;
    (* anyseq *) reg [63:0] cfg_next_sequence;
    (* anyseq *) reg packet_valid;
    (* anyseq *) reg [79:0] packet_session;
    (* anyseq *) reg [63:0] packet_sequence;
    (* anyseq *) reg [15:0] packet_count;
    (* anyseq *) reg [3:0] packet_error;

    wire cfg_ready;
    wire packet_ready;
    wire packet_accept;
    wire recovery_needed;
    wire [3:0] fault_code;
    wire [63:0] fault_sequence;
    wire [63:0] expected_sequence;
    wire duplicate_pulse;
    wire accepted_pulse;

    mold_session_gate dut (
        .clk(clk), .rst_n(rst_n), .quiescent(quiescent),
        .cfg_valid(cfg_valid), .cfg_ready(cfg_ready),
        .cfg_session(cfg_session), .cfg_next_sequence(cfg_next_sequence),
        .packet_valid(packet_valid), .packet_ready(packet_ready),
        .packet_accept(packet_accept), .packet_session(packet_session),
        .packet_sequence(packet_sequence), .packet_count(packet_count),
        .packet_error(packet_error), .recovery_needed(recovery_needed),
        .fault_code(fault_code), .fault_sequence(fault_sequence),
        .expected_sequence(expected_sequence), .duplicate_pulse(duplicate_pulse),
        .accepted_pulse(accepted_pulse)
    );

    reg past_valid = 1'b0;
    reg [79:0] checkpoint_session;
    reg seen_checkpoint;
    reg seen_accept;
    reg seen_fault;

    wire install = cfg_valid && cfg_ready;
    wire heartbeat = packet_count == 0;
    wire end_session = packet_count == 16'hffff;
    wire [64:0] range_end = {1'b0, packet_sequence} + {49'd0, packet_count};
    wire stale = (heartbeat || end_session) ?
                 packet_sequence < expected_sequence :
                 !range_end[64] && range_end <= {1'b0, expected_sequence};

    // The public transaction contract gives malformed input priority over sequencing.
    reg [3:0] rejection;
    always @* begin
        rejection = 0;
        if (packet_error != 0)
            rejection = 2;
        else if (packet_session != checkpoint_session)
            rejection = 3;
        else if (!heartbeat && !end_session && range_end[64])
            rejection = 6;
        else if (!stale) begin
            if (packet_sequence > expected_sequence)
                rejection = 4;
            else if (packet_sequence < expected_sequence)
                rejection = 5;
            else if (end_session)
                rejection = 7;
        end
    end

    wire eligible = rst_n && packet_valid && !recovery_needed;
    wire admit = eligible && rejection == 0 && !stale;
    wire discard_duplicate = eligible && rejection == 0 && stale;

    always @(posedge clk) begin
        past_valid <= 1'b1;
        if (!past_valid)
            assume(!rst_n);

        // No traffic, progress or output correctness is assumed after initial reset.
        if (!rst_n) begin
            checkpoint_session <= 0;
            seen_checkpoint <= 0;
            seen_accept <= 0;
            seen_fault <= 0;
        end else begin
            if (install) begin
                checkpoint_session <= cfg_session;
                seen_checkpoint <= 1;
            end
            if (accepted_pulse)
                seen_accept <= 1;
            if (recovery_needed && fault_code != 1)
                seen_fault <= 1;
        end

        if (past_valid) begin
            assert(cfg_ready == (rst_n && quiescent && !packet_valid));
            assert(packet_ready == rst_n);
            assert(packet_accept == admit);
            assert(!(duplicate_pulse && accepted_pulse));

            if (!$past(rst_n)) begin
                assert(recovery_needed);
                assert(expected_sequence == 0);
                assert(fault_code == 1 && fault_sequence == 0);
                assert(!accepted_pulse && !duplicate_pulse);
            end else begin
                assert(accepted_pulse == $past(admit && !heartbeat));
                assert(duplicate_pulse == $past(discard_duplicate));

                if ($past(install)) begin
                    assert(!recovery_needed);
                    assert(expected_sequence == $past(cfg_next_sequence));
                    assert(fault_code == 0 && fault_sequence == 0);
                end else if ($past(recovery_needed)) begin
                    // Packets and refused checkpoint requests cannot clear the first fault.
                    assert(recovery_needed);
                    assert(expected_sequence == $past(expected_sequence));
                    assert(fault_code == $past(fault_code));
                    assert(fault_sequence == $past(fault_sequence));
                end else if ($past(packet_valid && rejection != 0)) begin
                    assert(recovery_needed);
                    assert(fault_code == $past(rejection));
                    assert(fault_sequence == $past(packet_sequence));
                    assert(expected_sequence == $past(expected_sequence));
                end else begin
                    assert(!recovery_needed);
                    assert(fault_code == 0 && fault_sequence == 0);
                    if ($past(admit))
                        assert({1'b0, expected_sequence} == $past(range_end));
                    else
                        assert(expected_sequence == $past(expected_sequence));
                end

                // Only a checkpoint or a committed data transaction can advance state.
                if (!$past(install) && !accepted_pulse)
                    assert(expected_sequence == $past(expected_sequence));
                if (duplicate_pulse) begin
                    assert(!recovery_needed);
                    assert(expected_sequence == $past(expected_sequence));
                    assert(!accepted_pulse);
                end
            end

            cover(seen_checkpoint && accepted_pulse);
            cover(seen_accept && duplicate_pulse);
            cover(seen_checkpoint && admit && heartbeat);
            cover(seen_checkpoint && recovery_needed && fault_code == 4);
            cover(seen_checkpoint && recovery_needed && fault_code == 5);
            cover(seen_checkpoint && recovery_needed && fault_code == 6);
            cover(seen_checkpoint && recovery_needed && fault_code == 7);
            cover(seen_fault && recovery_needed && packet_valid && !install);
            cover(seen_fault && install);
            cover(seen_checkpoint && cfg_valid && !quiescent && !cfg_ready);
        end
    end
endmodule
