# Explicitly invoked HLS synthesis/export recipe; this file is not run by CMake.
# Required environment: NEXUS_HLS_PART and NEXUS_HLS_CLOCK_NS for the target board.
set script_dir [file dirname [file normalize [info script]]]
set repo_dir [file normalize [file join $script_dir ../../../..]]
set upstream_dir [file join $repo_dir .deps fpga vitis_security]
set expected_commit 86459b4c4d57f14c6b979ecf078e9ab30deb5576

if {![info exists ::env(NEXUS_HLS_PART)] || $::env(NEXUS_HLS_PART) eq ""} {
    error "Set NEXUS_HLS_PART to the exact installed FPGA device part"
}
if {![info exists ::env(NEXUS_HLS_CLOCK_NS)] ||
    ![string is double -strict $::env(NEXUS_HLS_CLOCK_NS)] ||
    $::env(NEXUS_HLS_CLOCK_NS) <= 0} {
    error "Set NEXUS_HLS_CLOCK_NS to a positive target clock period"
}
set actual_commit [string trim [exec git -C $upstream_dir rev-parse HEAD]]
if {$actual_commit ne $expected_commit} {
    error "The Vitis Security checkout does not match the adapter revision"
}
if {[string trim [exec git -C $upstream_dir status --porcelain --untracked-files=all]] ne ""} {
    error "The pinned Vitis Security checkout contains local changes"
}

set include_dir [file join $upstream_dir security L1 include]
if {![file isfile [file join $include_dir xf_security gcm.hpp]]} {
    error "Missing pinned Vitis Security AES-GCM headers"
}
set cflags [format {-std=c++14 -I"%s"} $include_dir]

# Only the dedicated generated project directory is reset by this recipe.
open_project -reset [file join $repo_dir build-hls nexus_aes256_gcm_encrypt]
set_top nexus_aes256_gcm_encrypt
add_files [file join $script_dir nexus_aes256_gcm_encrypt.cpp] -cflags $cflags
open_solution -reset solution -flow_target vivado
set_part $::env(NEXUS_HLS_PART)
create_clock -period $::env(NEXUS_HLS_CLOCK_NS) -name default
config_rtl -reset all -reset_level low
csynth_design
export_design -format ip_catalog -vendor nexus.local -library hls -version 1.0
exit
