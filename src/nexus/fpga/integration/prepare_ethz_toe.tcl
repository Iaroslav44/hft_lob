# Explicitly invoked HLS synthesis/export recipe; this file is not run by CMake.
# Required environment: NEXUS_HLS_PART and NEXUS_HLS_CLOCK_NS for the target board.
set script_dir [file dirname [file normalize [info script]]]
set repo_dir [file normalize [file join $script_dir ../../../..]]
set upstream_dir [file join $repo_dir .deps fpga eth_network_stack]
set expected_commit 75c32bae73f5c336b5c46c9de35ae0cb9cb7cc1c

if {![info exists ::env(NEXUS_HLS_PART)] || $::env(NEXUS_HLS_PART) eq ""} {
    error "Set NEXUS_HLS_PART to the exact installed FPGA device part"
}
if {![info exists ::env(NEXUS_HLS_CLOCK_NS)] ||
    ![string is double -strict $::env(NEXUS_HLS_CLOCK_NS)] ||
    $::env(NEXUS_HLS_CLOCK_NS) <= 0} {
    error "Set NEXUS_HLS_CLOCK_NS to a positive target clock period"
}
if {$::env(NEXUS_HLS_CLOCK_NS) != 6.4} {
    error "The pinned TOE timer constants require a 6.4 ns clock period"
}
set actual_commit [string trim [exec git -C $upstream_dir rev-parse HEAD]]
if {$actual_commit ne $expected_commit} {
    error "The ETH network stack checkout does not match the adapter revision"
}
if {[string trim [exec git -C $upstream_dir status --porcelain --untracked-files=all]] ne ""} {
    error "The pinned ETH network stack checkout contains local changes"
}

set hls_dir [file join $upstream_dir hls]
set config_file [file join $script_dir ethz_toe_config.hpp]
set cflags [format {-std=c++14 -I"%s" -include "%s"} $hls_dir $config_file]

# This list follows the pinned hls/toe/CMakeLists.txt, without its testbench.
# Use the same forced configuration for every translation unit, including helpers.
set upstream_sources {
    axi_utils.cpp
    toe/ack_delay/ack_delay.cpp
    toe/close_timer/close_timer.cpp
    toe/event_engine/event_engine.cpp
    toe/port_table/port_table.cpp
    toe/probe_timer/probe_timer.cpp
    toe/retransmit_timer/retransmit_timer.cpp
    toe/rx_app_if/rx_app_if.cpp
    toe/rx_app_stream_if/rx_app_stream_if.cpp
    toe/rx_engine/rx_engine.cpp
    toe/rx_sar_table/rx_sar_table.cpp
    toe/session_lookup_controller/session_lookup_controller.cpp
    toe/state_table/state_table.cpp
    toe/tx_app_if/tx_app_if.cpp
    toe/tx_app_stream_if/tx_app_stream_if.cpp
    toe/tx_engine/tx_engine.cpp
    toe/tx_sar_table/tx_sar_table.cpp
    toe/tx_app_interface/tx_app_interface.cpp
    toe/toe.cpp
}
foreach relative_path $upstream_sources {
    if {![file isfile [file join $hls_dir $relative_path]]} {
        error "Missing pinned HLS source: $relative_path"
    }
}

# Only the dedicated generated project directory is reset by this recipe.
open_project -reset [file join $repo_dir build-hls nexus_ethz_toe]
set_top nexus_ethz_toe
add_files [file join $script_dir nexus_ethz_toe.cpp] -cflags $cflags

# Apply the tracked single-hunk patch to a generated copy only. Exact context
# matching fails closed if either the upstream source or patch format changes.
set timer_path toe/retransmit_timer/retransmit_timer.cpp
set input_file [open [file join $hls_dir $timer_path] r]
set timer_source [read $input_file]
close $input_file
set patch_file [open [file join $script_dir ethz_retransmit_timer.patch] r]
set patch_text [read $patch_file]
close $patch_file
set original_context ""
set replacement_context ""
set in_hunk 0
foreach line [split $patch_text "\n"] {
    if {[string match "@@ *" $line]} {
        if {$in_hunk} { error "Expected exactly one timer patch hunk" }
        set in_hunk 1
        continue
    }
    if {!$in_hunk || $line eq ""} { continue }
    set prefix [string index $line 0]
    set content [string range $line 1 end]
    switch -- $prefix {
        " " {
            append original_context $content "\n"
            append replacement_context $content "\n"
        }
        "-" { append original_context $content "\n" }
        "+" { append replacement_context $content "\n" }
        default { error "Unexpected timer patch line" }
    }
}
if {!$in_hunk || $original_context eq ""} {
    error "Missing timer patch context"
}
set patch_offset [string first $original_context $timer_source]
if {$patch_offset < 0 ||
    [string first $original_context $timer_source [expr {$patch_offset + 1}]] >= 0} {
    error "The timer patch requires one exact upstream context match"
}
set patch_end [expr {$patch_offset + [string length $original_context] - 1}]
set patched_source [string replace $timer_source $patch_offset $patch_end $replacement_context]
set generated_dir [file join $repo_dir build-hls nexus_ethz_toe patched_sources]
file mkdir $generated_dir
set generated_timer [file join $generated_dir retransmit_timer.cpp]
set output_file [open $generated_timer w]
puts -nonewline $output_file $patched_source
close $output_file
set timer_cflags [format {%s -I"%s"} $cflags [file join $hls_dir toe retransmit_timer]]

foreach relative_path $upstream_sources {
    if {$relative_path eq $timer_path} {
        add_files $generated_timer -cflags $timer_cflags
    } else {
        add_files [file join $hls_dir $relative_path] -cflags $cflags
    }
}
open_solution -reset solution -flow_target vivado
set_part $::env(NEXUS_HLS_PART)
create_clock -period $::env(NEXUS_HLS_CLOCK_NS) -name default
config_rtl -reset all -reset_level low
csynth_design
export_design -format ip_catalog -vendor nexus.local -library hls -version 1.0
exit
