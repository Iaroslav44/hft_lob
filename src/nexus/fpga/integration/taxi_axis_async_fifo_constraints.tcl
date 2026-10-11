# Source after synthesis/linking and after the enclosing design's clock XDC.
# This script uses synthesized register names; RTL elaboration is insufficient.
# Apply it before implementation, including every newly loaded implementation run.
namespace eval ::nexus_taxi_constraints {
    set script_dir [file dirname [file normalize [info script]]]
    set repo_dir [file normalize [file join $script_dir ../../../..]]
    set upstream_dir [file join $repo_dir .deps fpga taxi]
    set expected_commit cc70b270b910d369ab1ad7b3855e76399fd461f1

    if {[string trim [exec git -C $upstream_dir rev-parse HEAD]] ne $expected_commit} {
        error "The Taxi checkout does not match the CDC constraint revision"
    }
    if {[string trim [exec git -C $upstream_dir status --porcelain --untracked-files=all]] ne ""} {
        error "The pinned Taxi checkout contains local changes"
    }

    set constraint_files [list \
        [file join $upstream_dir src axis syn vivado taxi_axis_async_fifo.tcl] \
        [file join $upstream_dir src sync syn vivado taxi_sync_reset.tcl] \
        [file join $upstream_dir src sync syn vivado taxi_sync_signal.tcl]]
    foreach constraint_file $constraint_files {
        if {![file isfile $constraint_file]} {
            error "Missing CDC constraints: $constraint_file"
        }
    }

    set fifos [get_cells -quiet -hier -filter \
        {(ORIG_REF_NAME == taxi_axis_async_fifo || REF_NAME == taxi_axis_async_fifo)}]
    if {![llength $fifos]} {
        error "No synthesized Taxi asynchronous FIFO found; retain its hierarchy"
    }

    # The upstream script otherwise substitutes a 1 ns period when clocks are
    # absent. Reject that case and missing pointer registers before applying it.
    foreach fifo $fifos {
        foreach register_name {wr_ptr_reg_reg rd_ptr_reg_reg} {
            set pointer_regs [get_cells -quiet [format {%s/%s[*]} $fifo $register_name]]
            if {![llength $pointer_regs]} {
                error "Missing synthesized pointer registers in $fifo"
            }
            set pointer_clocks [get_clocks -quiet -of_objects $pointer_regs]
            if {![llength $pointer_clocks]} {
                error "Define both FIFO clocks before loading CDC constraints: $fifo"
            }
            foreach pointer_clock $pointer_clocks {
                set period [get_property PERIOD $pointer_clock]
                if {![string is double -strict $period] || $period <= 0} {
                    error "Invalid clock period for $pointer_clock"
                }
            }
        }
    }

    # Keep upstream Gray-pointer, bundled commit-pointer, status and asynchronous
    # reset constraints together. Do not replace them with blanket false paths.
    foreach constraint_file $constraint_files {
        source $constraint_file
    }
}
namespace delete ::nexus_taxi_constraints
