# Applied only to the private source copy prepared by prepare_tcp_ip_stack.tcl.
# Every replacement requires exact context and an explicit match count.
proc nexus_read_text {path} {
    set channel [open $path r]
    fconfigure $channel -encoding utf-8 -translation auto
    set text [read $channel]
    close $channel
    return $text
}

proc nexus_write_text {path text} {
    set channel [open $path w]
    fconfigure $channel -encoding utf-8 -translation lf
    puts -nonewline $channel $text
    close $channel
}

proc nexus_replace_exact {text before after count label} {
    if {$before eq ""} { error "Empty overlay context: $label" }
    set found 0
    set position 0
    while {1} {
        set position [string first $before $text $position]
        if {$position < 0} { break }
        incr found
        incr position [string length $before]
    }
    if {$found != $count} {
        error "Overlay '$label' requires $count exact matches; found $found"
    }
    return [string map [list $before $after] $text]
}

proc nexus_extract_exact {text first next label} {
    foreach anchor [list $first $next] {
        set offset [string first $anchor $text]
        if {$offset < 0 || [string first $anchor $text [expr {$offset + 1}]] >= 0} {
            error "Overlay '$label' requires unique function boundary anchors"
        }
    }
    set start [string first $first $text]
    set finish [string first $next $text]
    if {$finish <= $start} { error "Reversed function anchors: $label" }
    return [string range $text $start [expr {$finish - 1}]]
}

proc nexus_apply_timer_patch {text patch_path} {
    set patch [nexus_read_text $patch_path]
    set original ""
    set replacement ""
    set in_hunk 0
    foreach line [split $patch "\n"] {
        if {[string match "@@ *" $line]} {
            if {$in_hunk} { error "Expected one retransmission timer patch hunk" }
            set in_hunk 1
            continue
        }
        if {!$in_hunk || $line eq ""} { continue }
        set body [string range $line 1 end]
        switch -- [string index $line 0] {
            " " {
                append original $body "\n"
                append replacement $body "\n"
            }
            "-" { append original $body "\n" }
            "+" { append replacement $body "\n" }
            default { error "Unexpected retransmission timer patch line" }
        }
    }
    if {!$in_hunk} { error "Missing retransmission timer patch hunk" }
    return [nexus_replace_exact $text $original $replacement 1 "timer scan bound"]
}

proc nexus_apply_tcp_ip_overlays {hls_dir adapter_dir} {
    set timer [file join $hls_dir toe retransmit_timer retransmit_timer.cpp]
    nexus_write_text $timer [nexus_apply_timer_patch [nexus_read_text $timer] \
        [file join $adapter_dir ethz_retransmit_timer.patch]]

    # The pinned prefixed checksum functions exist only at 512 bits. Copy the
    # complete 64-bit implementation twice so RX and TX own separate static state.
    set path [file join $hls_dir ipv4 ipv4_utils.cpp]
    set text [nexus_read_text $path]
    set first "void compute_ipv4_checksum(\thls::stream<net_axis<64> >&\tdataIn,"
    set next "void compute_ipv4_checksum(\thls::stream<net_axis<128> >&\tdataIn,"
    set body [nexus_extract_exact $text $first $next "64-bit IPv4 checksum"]
    foreach name {mac_compute_ipv4_checksum ip_handler_compute_ipv4_checksum} {
        set clone [nexus_replace_exact $body "void compute_ipv4_checksum(" \
            "void ${name}(" 1 $name]
        append text "\n// Separate 64-bit checksum state for this pipeline direction.\n" $clone
    }
    nexus_write_text $path $text

    set path [file join $hls_dir ipv4 ipv4.hpp]
    set text [nexus_read_text $path]
    set declarations {
void mac_compute_ipv4_checksum(hls::stream<net_axis<64> >& dataIn,
                              hls::stream<net_axis<64> >& dataOut,
                              hls::stream<subSums<4> >& subSumFiFoOut,
                              const bool skipChecksum=false);
void ip_handler_compute_ipv4_checksum(hls::stream<net_axis<64> >& dataIn,
                                     hls::stream<net_axis<64> >& dataOut,
                                     hls::stream<subSums<4> >& subSumFiFoOut,
                                     const bool skipChecksum=false);

}
    set anchor "void mac_compute_ipv4_checksum(\thls::stream<net_axis<512> >&\tdataIn,"
    nexus_write_text $path [nexus_replace_exact $text $anchor \
        "$declarations$anchor" 1 "64-bit checksum declarations"]

    # A direct-mapped ARP slot is a hit only when its complete key matches.
    set path [file join $hls_dir arp_server_subnet arp_server_subnet.cpp]
    set text [nexus_read_text $path]
    set before "\t\tcurrEntry = arpTable\[query_ip(31,24)\];\n\t\tif (!currEntry.valid)"
    set after "\t\tcurrEntry = arpTable\[query_ip(31,24)\];\n\t\tbool hit = currEntry.valid && currEntry.ipAddress == query_ip;\n\t\tif (!hit)"
    set text [nexus_replace_exact $text $before $after 2 "ARP lookup key match"]
    set text [nexus_replace_exact $text \
        "arpTableReply(currEntry.macAddress, currEntry.valid)" \
        "arpTableReply(currEntry.macAddress, hit)" 2 "ARP lookup hit response"]
    set text [nexus_replace_exact $text \
        "\t#pragma HLS DEPENDENCE variable=arpTable inter false\n" \
        "\t// Preserve dependencies between updates and lookups of the same slot.\n" \
        1 "ARP table write/read dependence"]
    nexus_write_text $path $text

    set path [file join $hls_dir arp_server_subnet arp_server_subnet.hpp]
    set text [nexus_read_text $path]
    nexus_write_text $path [nexus_replace_exact $text \
        "\tarpTableEntry() {}" \
        "\tarpTableEntry() : ipAddress(0), macAddress(0), valid(false) {}" \
        1 "ARP table reset state"]

    # Packet conversions must define every advertised byte qualifier.
    set path [file join $hls_dir axi_utils.hpp]
    set text [nexus_read_text $path]
    set first "void convert_net_axis_to_axis(hls::stream<net_axis<WIDTH> >& input,"
    set next "void convert_axis_to_net_axis(hls::stream<ap_axiu<WIDTH, 0, 0, 0> >& input,"
    set body [nexus_extract_exact $text $first $next "plain AXI converter"]
    set fixed [nexus_replace_exact $body \
        "\t\toutputWord.keep = inputWord.keep;\n" \
        "\t\toutputWord.keep = inputWord.keep;\n\t\toutputWord.strb = inputWord.keep;\n" \
        1 "AXI output strobe"]
    nexus_write_text $path [nexus_replace_exact $text $body $fixed 1 "plain AXI converter"]
}
