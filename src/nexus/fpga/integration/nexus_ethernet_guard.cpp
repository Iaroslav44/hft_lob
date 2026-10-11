#include "nexus_ethernet_guard.hpp"

namespace {

constexpr unsigned kMaxFrameBytes = 1514;
constexpr unsigned kFrameWords = (kMaxFrameBytes + 7) / 8;
// Ethernet + IPv4 + the maximum TCP header occupies 94 bytes.
using Header = ap_uint<768>;

template <unsigned Offset>
ap_uint<8> byte_at(const Header& header) {
#pragma HLS INLINE
    return header.range(Offset * 8 + 7, Offset * 8);
}

template <unsigned Offset>
ap_uint<16> be16_at(const Header& header) {
#pragma HLS INLINE
    ap_uint<16> value = 0;
    value.range(15, 8) = byte_at<Offset>(header);
    value.range(7, 0) = byte_at<Offset + 1>(header);
    return value;
}

bool ipv4_checksum_valid(const Header& header) {
#pragma HLS INLINE
    ap_uint<32> sum = 0;
    for (unsigned i = 0; i < 10; ++i) {
#pragma HLS UNROLL
        ap_uint<16> word = 0;
        word.range(15, 8) = header.range((14 + 2 * i) * 8 + 7, (14 + 2 * i) * 8);
        word.range(7, 0) = header.range((15 + 2 * i) * 8 + 7, (15 + 2 * i) * 8);
        sum += word;
    }
    sum = (sum & 0xffff) + (sum >> 16);
    sum = (sum & 0xffff) + (sum >> 16);
    return sum == 0xffff;
}

// Return zero for a rejected frame, otherwise its extent without link padding.
ap_uint<11> admitted_length(const Header& header, ap_uint<11> received,
                            ap_uint<48> local_mac, ap_uint<32> local_ip) {
#pragma HLS INLINE
    const ap_uint<48> destination = header.range(47, 0);
    const ap_uint<48> source = header.range(95, 48);
    const bool local_destination = destination == local_mac;
    const bool broadcast_destination = destination == ap_uint<48>(0xffffffffffffULL);
    if (received < 42 || source == 0 || source[0] ||
        (!local_destination && !broadcast_destination)) {
        return 0;
    }

    const ap_uint<16> ether_type = be16_at<12>(header);
    if (ether_type == 0x0806) {
        const ap_uint<16> operation = be16_at<20>(header);
        const ap_uint<48> sender_mac = header.range(223, 176);
        const ap_uint<48> target_mac = header.range(303, 256);
        const ap_uint<32> target_ip = header.range(335, 304);
        if (be16_at<14>(header) != 1 || be16_at<16>(header) != 0x0800 ||
            byte_at<18>(header) != 6 || byte_at<19>(header) != 4 ||
            (operation != 1 && operation != 2) || sender_mac != source ||
            target_ip != local_ip || (operation == 2 && target_mac != local_mac)) {
            return 0;
        }
        return 42;
    }

    // IPv4 options, fragments, VLANs and IPv6 are outside this endpoint profile.
    // In particular, no truncated header may enter the upstream metadata FIFOs.
    if (ether_type != 0x0800 || !local_destination || byte_at<14>(header) != 0x45 ||
        (be16_at<20>(header) & 0xbfff) != 0 || byte_at<22>(header) == 0 ||
        ap_uint<32>(header.range(271, 240)) != local_ip || !ipv4_checksum_valid(header)) {
        return 0;
    }
    const ap_uint<16> ip_length = be16_at<16>(header);
    if (ip_length > 1500 || ip_length + 14 > received) {
        return 0;
    }
    const ap_uint<8> protocol = byte_at<23>(header);
    if (protocol == 6) {
        const ap_uint<8> tcp_header_bytes = (byte_at<46>(header) >> 4) * 4;
        if (ip_length < 40 || tcp_header_bytes < 20 ||
            ap_uint<16>(tcp_header_bytes) + 20 > ip_length) {
            return 0;
        }
    } else if (protocol == 1) {
        // The pinned ICMP engine implements echo requests with a 20-byte IP header.
        if (ip_length < 28 || byte_at<34>(header) != 8 || byte_at<35>(header) != 0) {
            return 0;
        }
    } else {
        return 0;
    }
    return ip_length + 14;
}

} // namespace

void nexus_ethernet_guard(hls::stream<NexusEthernetBeat>& ethernet_rx,
                          hls::stream<NexusToeBeat>& validated_rx,
                          ap_uint<48> local_mac_address,
                          ap_uint<32> local_ip_address,
                          ap_uint<32>& rx_rejected_frames) {
#pragma HLS INLINE off
#pragma HLS PIPELINE II=1
    enum class State { Receive, Validate, Options, Replay };
    static State state = State::Receive;
    static ap_uint<64> frame[kFrameWords];
#pragma HLS BIND_STORAGE variable=frame type=ram_1p impl=bram
#pragma HLS RESET variable=frame off
    static Header header = 0;
    static ap_uint<8> words = 0;
    static ap_uint<11> bytes = 0;
    static ap_uint<8> replay_word = 0;
    static ap_uint<11> replay_bytes = 0;
    static ap_uint<7> option_cursor = 0;
    static ap_uint<7> option_end = 0;
    static bool bad_frame = false;
    static ap_uint<32> rejected = 0;

    rx_rejected_frames = rejected;
    switch (state) {
    case State::Receive:
        if (!ethernet_rx.empty()) {
            const NexusEthernetBeat beat = ethernet_rx.read();
            ap_uint<4> valid_bytes = 0;
            ap_uint<64> data = beat.data;
            for (unsigned lane = 0; lane < 8; ++lane) {
#pragma HLS UNROLL
                valid_bytes += beat.keep[lane] ? 1 : 0;
                if (!beat.keep[lane]) {
                    data.range(lane * 8 + 7, lane * 8) = 0;
                }
            }
            const ap_uint<8> next_keep = beat.keep + 1;
            const bool contiguous = beat.keep != 0 && (beat.keep & next_keep) == 0;
            const bool invalid = bad_frame || beat.user != 0 || beat.strb != beat.keep ||
                                 !contiguous || (!beat.last && beat.keep != 0xff) ||
                                 words >= kFrameWords || bytes + valid_bytes > kMaxFrameBytes;
            if (!invalid) {
                frame[words] = data;
                if (words < 12) {
                    header.range(words * 64 + 63, words * 64) = data;
                }
                ++words;
                bytes += valid_bytes;
            }
            bad_frame = invalid;
            if (beat.last) {
                state = State::Validate;
            }
        }
        break;
    case State::Validate:
        replay_bytes = bad_frame ? ap_uint<11>(0) :
            admitted_length(header, bytes, local_mac_address, local_ip_address);
        if (replay_bytes == 0) {
            ++rejected;
            words = 0;
            bytes = 0;
            header = 0;
            bad_frame = false;
            state = State::Receive;
        } else {
            replay_word = 0;
            if (be16_at<12>(header) == 0x0800 && byte_at<23>(header) == 6 &&
                (byte_at<46>(header) >> 4) > 5) {
                option_cursor = 54;
                option_end = 34 + (byte_at<46>(header) >> 4) * 4;
                state = State::Options;
            } else {
                state = State::Replay;
            }
        }
        break;
    case State::Options: {
        // The cursor starts within the verified TCP header and every accepted
        // option advances it. At most 40 bytes are inspected before replay.
        const ap_uint<8> kind = header.range(option_cursor * 8 + 7, option_cursor * 8);
        ap_uint<8> length = 1;
        bool valid = true;
        if (kind != 0 && kind != 1) {
            if (option_end - option_cursor < 2) {
                valid = false;
            } else {
                length = header.range((option_cursor + 1) * 8 + 7, (option_cursor + 1) * 8);
                valid = length >= 2 && length <= option_end - option_cursor;
                if ((kind == 2 && length != 4) || (kind == 3 && length != 3) ||
                    (kind == 4 && length != 2) ||
                    (kind == 5 && (length < 10 || ((length - 2) & 7) != 0)) ||
                    (kind == 8 && length != 10)) {
                    valid = false;
                }
            }
        }
        if (!valid) {
            ++rejected;
            words = 0;
            bytes = 0;
            header = 0;
            bad_frame = false;
            state = State::Receive;
        } else if (kind == 0 || option_cursor + length == option_end) {
            state = State::Replay;
        } else {
            option_cursor += length;
        }
        break;
    }
    case State::Replay:
        if (!validated_rx.full()) {
            NexusToeBeat beat;
            beat.data = frame[replay_word];
            beat.keep = 0;
            for (unsigned lane = 0; lane < 8; ++lane) {
#pragma HLS UNROLL
                const bool valid = replay_bytes > lane;
                beat.keep[lane] = valid;
                if (!valid) {
                    beat.data.range(lane * 8 + 7, lane * 8) = 0;
                }
            }
            beat.strb = beat.keep;
            beat.last = replay_bytes <= 8;
            validated_rx.write(beat);
            if (beat.last) {
                words = 0;
                bytes = 0;
                header = 0;
                bad_frame = false;
                state = State::Receive;
            } else {
                replay_bytes -= 8;
                ++replay_word;
            }
        }
        break;
    }
}
