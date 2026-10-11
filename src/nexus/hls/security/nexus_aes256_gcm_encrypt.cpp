#include "nexus_aes256_gcm_encrypt.hpp"

#include <xf_security/gcm.hpp>

void nexus_aes256_gcm_encrypt(
    hls::stream<ap_uint<128>>& plaintext,
    hls::stream<ap_uint<256>>& keys,
    hls::stream<ap_uint<96>>& ivs,
    hls::stream<ap_uint<128>>& aad,
    hls::stream<ap_uint<64>>& aad_bits,
    hls::stream<ap_uint<64>>& plaintext_bits,
    hls::stream<bool>& end_lengths,
    hls::stream<ap_uint<128>>& ciphertext,
    hls::stream<ap_uint<64>>& ciphertext_bits,
    hls::stream<ap_uint<128>>& tags,
    hls::stream<bool>& end_tags) {
#pragma HLS INTERFACE ap_ctrl_hs port=return
#pragma HLS INTERFACE axis register both port=plaintext
#pragma HLS INTERFACE axis register both port=keys
#pragma HLS INTERFACE axis register both port=ivs
#pragma HLS INTERFACE axis register both port=aad
#pragma HLS INTERFACE axis register both port=aad_bits
#pragma HLS INTERFACE axis register both port=plaintext_bits
#pragma HLS INTERFACE axis register both port=end_lengths
#pragma HLS INTERFACE axis register both port=ciphertext
#pragma HLS INTERFACE axis register both port=ciphertext_bits
#pragma HLS INTERFACE axis register both port=tags
#pragma HLS INTERFACE axis register both port=end_tags

    xf::security::aes256GcmEncrypt(
        plaintext, keys, ivs, aad, aad_bits, plaintext_bits, end_lengths,
        ciphertext, ciphertext_bits, tags, end_tags);
}
