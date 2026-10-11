# Register an explicitly exported HLS IP in the enclosing Vivado project.
# This loader does not run HLS, assign pins, or invent generated RTL port names.
set nexus_script_dir [file dirname [file normalize [info script]]]
set nexus_repo_dir [file dirname $nexus_script_dir]
set nexus_ip_dir [file join $nexus_repo_dir build-hls nexus_tcp_ip_stack solution impl ip]

if {![file isfile [file join $nexus_ip_dir component.xml]]} {
    error "Export nexus_tcp_ip_stack with prepare_tcp_ip_stack.tcl before loading its IP"
}
set nexus_project [current_project -quiet]
if {$nexus_project eq ""} {
    error "Open the enclosing Vivado project before registering nexus_tcp_ip_stack"
}
set nexus_ip_paths [get_property ip_repo_paths $nexus_project]
if {[lsearch -exact $nexus_ip_paths $nexus_ip_dir] < 0} {
    lappend nexus_ip_paths $nexus_ip_dir
    set_property ip_repo_paths $nexus_ip_paths $nexus_project
}
update_ip_catalog
if {[llength [get_ipdefs -all nexus.local:hls:nexus_tcp_ip_stack:1.0]] == 0} {
    error "The exported catalog does not contain nexus.local:hls:nexus_tcp_ip_stack:1.0"
}

# Instantiate this IP from the catalog and connect the interfaces documented in
# src/nexus/fpga/integration/nexus_tcp_ip_stack.hpp using its component.xml.
unset nexus_ip_paths
unset nexus_project
unset nexus_ip_dir
unset nexus_repo_dir
unset nexus_script_dir
