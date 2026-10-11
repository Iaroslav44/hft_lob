`timescale 1ns/1ps

// One configured MoldUDP64 session, shared by all already-serialized feed lines.
// Sequence numbers do not wrap within a session. Recovery is a control-plane
// operation: pause ingress, drain committed output, then install a new checkpoint.
module mold_session_gate (
    input  wire        clk,
    input  wire        rst_n,
    input  wire        quiescent,
    input  wire        cfg_valid,
    output wire        cfg_ready,
    input  wire [79:0] cfg_session,
    input  wire [63:0] cfg_next_sequence,

    input  wire        packet_valid,
    output wire        packet_ready,
    output wire        packet_accept,
    input  wire [79:0] packet_session,
    input  wire [63:0] packet_sequence,
    input  wire [15:0] packet_count,
    input  wire [3:0]  packet_error,

    output wire        recovery_needed,
    output reg  [3:0]  fault_code,
    output reg  [63:0] fault_sequence,
    output reg  [63:0] expected_sequence,
    output reg         duplicate_pulse,
    output reg         accepted_pulse
);
    localparam [3:0] FAULT_UNCONFIGURED = 4'd1;
    localparam [3:0] FAULT_MALFORMED    = 4'd2;
    localparam [3:0] FAULT_SESSION      = 4'd3;
    localparam [3:0] FAULT_GAP          = 4'd4;
    localparam [3:0] FAULT_OVERLAP      = 4'd5;
    localparam [3:0] FAULT_SEQUENCE_END = 4'd6;
    localparam [3:0] FAULT_SESSION_END  = 4'd7;

    reg armed;
    reg [79:0] session;
    wire heartbeat = packet_count == 16'd0;
    wire session_end = packet_count == 16'hffff;
    wire [64:0] packet_end = {1'b0, packet_sequence} + {49'd0, packet_count};
    wire stale_data = !heartbeat && !session_end && !packet_end[64] &&
                      packet_end[63:0] <= expected_sequence;
    wire stale_control = (heartbeat || session_end) &&
                         packet_sequence < expected_sequence;
    wire duplicate = stale_data || stale_control;

    assign cfg_ready = rst_n && quiescent && !packet_valid;
    assign packet_ready = rst_n;
    assign recovery_needed = !armed;
    assign packet_accept = rst_n && packet_valid && armed &&
                           packet_error == 4'd0 && packet_session == session &&
                           packet_sequence == expected_sequence && !session_end &&
                           !packet_end[64];

    always @(posedge clk) begin
        if (!rst_n) begin
            armed <= 1'b0;
            session <= 80'd0;
            expected_sequence <= 64'd0;
            fault_code <= FAULT_UNCONFIGURED;
            fault_sequence <= 64'd0;
            duplicate_pulse <= 1'b0;
            accepted_pulse <= 1'b0;
        end else begin
            duplicate_pulse <= 1'b0;
            accepted_pulse <= 1'b0;
            if (cfg_valid && cfg_ready) begin
                armed <= 1'b1;
                session <= cfg_session;
                expected_sequence <= cfg_next_sequence;
                fault_code <= 4'd0;
                fault_sequence <= 64'd0;
            end else if (packet_valid && packet_ready && armed) begin
                // A fault is sticky. No later packet can silently heal a gap.
                if (packet_error != 4'd0) begin
                    armed <= 1'b0;
                    fault_code <= FAULT_MALFORMED;
                    fault_sequence <= packet_sequence;
                end else if (packet_session != session) begin
                    armed <= 1'b0;
                    fault_code <= FAULT_SESSION;
                    fault_sequence <= packet_sequence;
                end else if (!heartbeat && !session_end && packet_end[64]) begin
                    armed <= 1'b0;
                    fault_code <= FAULT_SEQUENCE_END;
                    fault_sequence <= packet_sequence;
                end else if (duplicate) begin
                    duplicate_pulse <= 1'b1;
                end else if (packet_sequence > expected_sequence) begin
                    armed <= 1'b0;
                    fault_code <= FAULT_GAP;
                    fault_sequence <= packet_sequence;
                end else if (packet_sequence < expected_sequence) begin
                    // A partially overlapping packet requires replay from an
                    // exact checkpoint; skipping only its prefix is not supported.
                    armed <= 1'b0;
                    fault_code <= FAULT_OVERLAP;
                    fault_sequence <= packet_sequence;
                end else if (session_end) begin
                    armed <= 1'b0;
                    fault_code <= FAULT_SESSION_END;
                    fault_sequence <= packet_sequence;
                end else begin
                    expected_sequence <= packet_end[63:0];
                    accepted_pulse <= !heartbeat;
                end
            end
        end
    end
endmodule
