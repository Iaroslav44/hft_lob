# Source from the enclosing Vivado flow before synthesis. This only reads RTL;
# clocks, board constraints, synthesis and implementation remain caller-owned.
namespace eval ::nexus_taxi_sources {
    set script_dir [file dirname [file normalize [info script]]]
    set repo_dir [file normalize [file join $script_dir ../../../..]]
    set upstream_dir [file join $repo_dir .deps fpga taxi]
    set expected_commit cc70b270b910d369ab1ad7b3855e76399fd461f1

    if {[string trim [exec git -C $upstream_dir rev-parse HEAD]] ne $expected_commit} {
        error "The Taxi checkout does not match the CDC adapter revision"
    }
    if {[string trim [exec git -C $upstream_dir status --porcelain --untracked-files=all]] ne ""} {
        error "The pinned Taxi checkout contains local changes"
    }

    # Canonical paths avoid the upstream package's self-referencing library links.
    set sources [list \
        [file join $upstream_dir src axis rtl taxi_axis_if.sv] \
        [file join $upstream_dir src sync rtl taxi_sync_reset.sv] \
        [file join $upstream_dir src sync rtl taxi_sync_signal.sv] \
        [file join $upstream_dir src axis rtl taxi_axis_async_fifo.sv] \
        [file join $script_dir nexus_axis_async_fifo64.sv]]
    foreach source_file $sources {
        if {![file isfile $source_file]} {
            error "Missing CDC source: $source_file"
        }
    }
    foreach source_file $sources {
        read_verilog -sv $source_file
    }
}
namespace delete ::nexus_taxi_sources

# After loading the synthesized design and defining both clocks, source
# taxi_axis_async_fifo_constraints.tcl. Reading RTL does not apply CDC constraints.
