# Explicit HLS synthesis/export recipe. CMake never invokes this file automatically.
# Required environment: NEXUS_HLS_PART and NEXUS_HLS_CLOCK_NS=6.4.
set script_dir [file dirname [file normalize [info script]]]
set repo_dir [file normalize [file join $script_dir ../../../..]]
set upstream_dir [file join $repo_dir .deps fpga eth_network_stack]
set expected_commit 75c32bae73f5c336b5c46c9de35ae0cb9cb7cc1c

if {![info exists ::env(NEXUS_HLS_PART)] || $::env(NEXUS_HLS_PART) eq ""} {
    error "Set NEXUS_HLS_PART to the exact installed FPGA device part"
}
if {![info exists ::env(NEXUS_HLS_CLOCK_NS)] ||
    ![string is double -strict $::env(NEXUS_HLS_CLOCK_NS)] ||
    $::env(NEXUS_HLS_CLOCK_NS) != 6.4} {
    error "The pinned TOE timer constants require NEXUS_HLS_CLOCK_NS=6.4"
}
if {[string trim [exec git -C $upstream_dir rev-parse HEAD]] ne $expected_commit} {
    error "The ETH network stack checkout does not match the integration revision"
}
if {[string trim [exec git -C $upstream_dir status --porcelain --untracked-files=all]] ne ""} {
    error "The pinned ETH network stack checkout contains local changes"
}

# The manifest retains the complete TOE and adds only the required Ethernet/IP
# functions. UDP, RoCE, the obsolete ICMP top, and IPv4 TX generation are not called.
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
    ipv4/ipv4_utils.cpp
    ip_handler/ip_handler.cpp
    arp_server_subnet/arp_server_subnet.cpp
    mac_ip_encode/mac_ip_encode.cpp
    icmp_server/icmp_server.cpp
}
set local_sources {
    nexus_ethz_toe.cpp
    nexus_session_table.cpp
    nexus_icmp_echo.cpp
    nexus_ethernet_guard.cpp
    nexus_tcp_ip_stack.cpp
}
foreach relative_path $upstream_sources {
    if {![file isfile [file join $upstream_dir hls $relative_path]]} {
        error "Missing pinned HLS source: $relative_path"
    }
}
foreach relative_path $local_sources {
    if {![file isfile [file join $script_dir $relative_path]]} {
        error "Missing integration HLS source: $relative_path"
    }
}
foreach relative_path {
    ethz_toe_config.hpp ethz_retransmit_timer.patch ethz_tcp_ip_overlays.tcl
    ethz_tcp_options_overlay.tcl
    nexus_ethz_toe.hpp nexus_ethernet_guard.hpp nexus_tcp_ip_stack.hpp
    nexus_session_table.hpp
    nexus_icmp_echo.hpp
} {
    if {![file isfile [file join $script_dir $relative_path]]} {
        error "Missing integration support file: $relative_path"
    }
}

# Reset only this named generated HLS project. Copy tracked files rather than the
# whole checkout so ignored build products cannot shadow a pinned source header.
set project_dir [file join $repo_dir build-hls nexus_tcp_ip_stack]
open_project -reset $project_dir
set generated_dir [file join $project_dir vendor]
set hls_dir [file join $generated_dir hls]
set tracked [split [string trim [exec git -C $upstream_dir ls-files -- hls]] "\n"]
if {[llength $tracked] == 0} { error "The pinned checkout contains no tracked HLS files" }
foreach relative_path $tracked {
    if {![string match "hls/*" $relative_path] ||
        [lsearch -exact [file split $relative_path] ".."] >= 0} {
        error "Unexpected tracked HLS path: $relative_path"
    }
    set source_path [file join $upstream_dir $relative_path]
    set target_path [file join $generated_dir $relative_path]
    if {![file isfile $source_path] || [file type $source_path] eq "link"} {
        error "Expected a regular pinned HLS file: $relative_path"
    }
    file mkdir [file dirname $target_path]
    file copy -force $source_path $target_path
}
source [file join $script_dir ethz_tcp_ip_overlays.tcl]
nexus_apply_tcp_ip_overlays $hls_dir $script_dir
source [file join $script_dir ethz_tcp_options_overlay.tcl]
nexus_apply_tcp_options_overlay $hls_dir $script_dir

set config_file [file join $script_dir ethz_toe_config.hpp]
set cflags [format {-std=c++14 -I"%s" -I"%s" -include "%s"} \
    $hls_dir $script_dir $config_file]
set_top nexus_tcp_ip_stack
foreach relative_path $local_sources {
    add_files [file join $script_dir $relative_path] -cflags $cflags
}
foreach relative_path $upstream_sources {
    add_files [file join $hls_dir $relative_path] -cflags $cflags
}

open_solution -reset solution -flow_target vivado
set_part $::env(NEXUS_HLS_PART)
create_clock -period $::env(NEXUS_HLS_CLOCK_NS) -name default
config_rtl -reset all -reset_level low
csynth_design
export_design -format ip_catalog -vendor nexus.local -library hls -version 1.0
exit
