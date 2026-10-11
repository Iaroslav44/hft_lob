# Apply only to the build directory's copy of the pinned ETH network sources.
# The dependency checkout remains unchanged. This script runs no HLS tools.
proc nexus_tcp_options_replace_once {text needle replacement label} {
    set position [string first $needle $text]
    if {$position < 0 || [string last $needle $text] != $position} {
        error "TCP options overlay anchor changed: $label"
    }
    return [string replace $text $position \
        [expr {$position + [string length $needle] - 1}] $replacement]
}

proc nexus_apply_tcp_options_overlay {hls_dir adapter_dir} {
    set source_file [file join $hls_dir toe rx_engine rx_engine.cpp]
    set stream [open $source_file r]
    set text [read $stream]
    close $stream

    set begin_marker "void parse_optional_header_fields("
    set end_marker "\nvoid merge_header_meta("
    set begin [string first $begin_marker $text]
    set end [string first $end_marker $text]
    if {$begin < 0 || $end <= $begin ||
        [string last $begin_marker $text] != $begin ||
        [string last $end_marker $text] != $end} {
        error "TCP options parser boundaries differ from the pinned source"
    }

    set parser {
void parse_optional_header_fields(hls::stream<ap_uint<4> >& dataOffsetIn,
                                  hls::stream<ap_uint<320> >& optionalHeaderFieldsIn,
                                  hls::stream<ap_uint<4> >& windowScaleOut)
{
#pragma HLS PIPELINE II=1
#pragma HLS INLINE off
    enum fsmStateType {IDLE, PARSE};
    static fsmStateType state = IDLE;
    static ap_uint<6> remaining = 0;
    static ap_uint<320> fields = 0;
#pragma HLS RESET variable=state
#pragma HLS RESET variable=remaining

    switch (state) {
    case IDLE:
        if (!dataOffsetIn.empty() && !optionalHeaderFieldsIn.empty()) {
            const ap_uint<4> words = dataOffsetIn.read();
            fields = optionalHeaderFieldsIn.read();
            // The caller supplies option words, excluding the fixed TCP header.
            if (words == 0 || words > 10) {
                windowScaleOut.write(0);
            } else {
                remaining = ap_uint<6>(words) * 4;
                state = PARSE;
            }
        }
        break;
    case PARSE: {
        const ap_uint<8> kind = fields.range(7, 0);
        if (remaining == 0 || kind == 0) {
            windowScaleOut.write(0);
            state = IDLE;
            break;
        }
        ap_uint<8> length = 1;
        if (kind != 1) {
            // Every non-padding option needs a length byte and must advance.
            if (remaining < 2) {
                windowScaleOut.write(0);
                state = IDLE;
                break;
            }
            length = fields.range(15, 8);
            const bool bad_length = length < 2 || length > remaining ||
                (kind == 2 && length != 4) ||
                (kind == 3 && length != 3) ||
                (kind == 4 && length != 2) ||
                (kind == 5 && (length < 10 || ((length - 2) & 7) != 0)) ||
                (kind == 8 && length != 10);
            if (bad_length) {
                // Guarded ingress rejects these packets before metadata reaches
                // the TOE. This fallback still completes exactly one request.
                windowScaleOut.write(0);
                state = IDLE;
                break;
            }
        }
        if (kind == 3) {
            const ap_uint<8> scale = fields.range(23, 16);
            windowScaleOut.write(scale > 14 ? ap_uint<4>(14) : ap_uint<4>(scale));
            state = IDLE;
        } else if (length == remaining) {
            windowScaleOut.write(0);
            state = IDLE;
        } else {
            remaining -= length;
            fields >>= length * 8;
        }
        break;
    }
    }
}
}
    set text [string replace $text $begin [expr {$end - 1}] [string trimleft $parser "\n"]]

    # The current word follows the previous word's complete option beat. Once
    # the remaining word count reaches zero, this current word is all payload.
    set needle "\t\t\t\tdataOffset -= WIDTH/32;\n"
    set replacement "\t\t\t\tdataOffset -= WIDTH/32;\n\t\t\t\tif (dataOffset == 0) dataOut.write(currWord);\n"
    set text [nexus_tcp_options_replace_once $text $needle $replacement "aligned payload retention"]

    # Test the extent before indexing KEEP; an option-only final beat can leave
    # a residual full-word count, for which KEEP[WIDTH/8] is outside the vector.
    set needle "if (currWord.keep\[dataOffset*4\] != 0 && dataOffset != 0)"
    set replacement "if (dataOffset != 0 && dataOffset*4 < WIDTH/8 && currWord.keep\[dataOffset*4\] != 0)"
    set text [nexus_tcp_options_replace_once $text $needle $replacement "final keep index"]

    set stream [open $source_file w]
    puts -nonewline $stream $text
    close $stream
}
