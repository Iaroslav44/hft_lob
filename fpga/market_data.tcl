# Source this file from the enclosing Vivado project after creating its clock,
# reset, MAC/UDP and DMA interfaces. No board pins or achieved timing are inferred.
set nexus_fpga_root [file dirname [file normalize [info script]]]
set nexus_repo_root [file dirname $nexus_fpga_root]
set nexus_rtl_root [file join $nexus_repo_root src nexus fpga]

foreach nexus_source {
    market_data/moldudp64_packet_buffer.sv
    market_data/itch_a_normalizer.sv
    mold_session_gate.sv
    nexus_mold_ingress.sv
} {
    read_verilog -sv [file join $nexus_rtl_root $nexus_source]
}

# The parent project instantiates nexus_mold_ingress inside its board shell.
# Do not override the parent's top, part, clocks, pin constraints or reset policy.
unset nexus_source
unset nexus_rtl_root
unset nexus_repo_root
unset nexus_fpga_root
