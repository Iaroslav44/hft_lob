#include "nexus_icmp_echo.hpp"

// Exact signature from the pinned icmp_server.cpp. Its public header still
// declares an obsolete axiWord top, so it is not used to declare this function.
void check_icmp_checksum(hls::stream<net_axis<64>>& input,
                         hls::stream<net_axis<64>>& rewritten,
                         hls::stream<bool>& valid,
                         hls::stream<ap_uint<16>>& reply_checksum);

namespace {

void unpack_echo(hls::stream<NexusToeBeat>& input,
                 hls::stream<net_axis<64>>& output) {
#pragma HLS INLINE off
#pragma HLS PIPELINE II=1
    if (!input.empty() && !output.full()) {
        const NexusToeBeat beat = input.read();
        net_axis<64> word;
        word.data = beat.data;
        word.keep = beat.keep;
        word.last = beat.last;
        output.write(word);
    }
}

void commit_echo(hls::stream<net_axis<64>>& input,
                 hls::stream<bool>& validity,
                 hls::stream<ap_uint<16>>& checksums,
                 hls::stream<NexusToeBeat>& output,
                 ap_uint<32>& rejected_frames) {
#pragma HLS INLINE off
#pragma HLS PIPELINE II=1
    enum class State { Decision, Checksum, Forward, Drop };
    static State state = State::Decision;
    static ap_uint<16> reply_checksum = 0;
    static ap_uint<8> word_index = 0;
    static ap_uint<32> rejected = 0;

    rejected_frames = rejected;
    switch (state) {
    case State::Decision:
        if (!validity.empty()) {
            if (validity.read()) {
                state = State::Checksum;
            } else {
                ++rejected;
                state = State::Drop;
            }
        }
        break;
    case State::Checksum:
        if (!checksums.empty()) {
            reply_checksum = checksums.read();
            word_index = 0;
            state = State::Forward;
        }
        break;
    case State::Forward:
        if (!input.empty() && !output.full()) {
            const net_axis<64> word = input.read();
            NexusToeBeat beat;
            beat.data = word.data;
            beat.keep = word.keep;
            beat.strb = word.keep;
            beat.last = word.last;
            // The upstream result is already in its wire-byte representation.
            // With IHL=5, ICMP checksum bytes 22..23 occupy these two lanes.
            if (word_index == 2) beat.data.range(63, 48) = reply_checksum;
            output.write(beat);
            if (word.last) {
                state = State::Decision;
            } else {
                ++word_index;
            }
        }
        break;
    case State::Drop:
        if (!input.empty() && input.read().last) state = State::Decision;
        break;
    }
}

} // namespace

void nexus_icmp_echo(hls::stream<NexusToeBeat>& input,
                     hls::stream<NexusToeBeat>& output,
                     ap_uint<32>& rejected_frames) {
#pragma HLS INLINE off
#pragma HLS DATAFLOW disable_start_propagation
    static hls::stream<net_axis<64>> request("echo_request");
    static hls::stream<net_axis<64>> rewritten("echo_rewritten");
    static hls::stream<bool> validity("echo_validity");
    static hls::stream<ap_uint<16>> checksums("echo_checksums");
#pragma HLS STREAM variable=request depth=2
    // The checksum decision follows LAST. The entire admitted 1500-byte IP
    // packet must fit here before its first word can be released or discarded.
#pragma HLS STREAM variable=rewritten depth=256
#pragma HLS STREAM variable=validity depth=4
#pragma HLS STREAM variable=checksums depth=4

    unpack_echo(input, request);
    check_icmp_checksum(request, rewritten, validity, checksums);
    commit_echo(rewritten, validity, checksums, output, rejected_frames);
}
