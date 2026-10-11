#pragma once

#include <ap_int.h>
#include <hls_stream.h>

// Upstream: Xilinx/Vitis_Libraries, commit
// 86459b4c4d57f14c6b979ecf078e9ab30deb5576, security/L1/include/xf_security/gcm.hpp.
// Each invocation processes a finite batch. Supply one false end_lengths token
// per message, then one true token to finish the batch. Each message consumes one
// key, IV and pair of bit lengths, plus ceil(length / 128) words on each data input.
// Byte zero occupies bits [7:0]; zero unused high bytes of each partial input word.
// Lengths must be multiples of eight; payload_bits must not exceed 2^39 - 256.
// The caller must never reuse an IV with the same key, including after reset.
// Advance independent streams concurrently: backpressure on any output can stall
// input acceptance. These length-framed streams have no TKEEP or TLAST signals.
// Outputs are ciphertext, its bit length and one 128-bit authentication tag per
// message; end_tags emits false per tag followed by true at batch completion.
// Unused high bytes in the final ciphertext word are unspecified; use its length.
// Upstream writes each tag before its false end_tags token. Do not wait for that
// token before allowing the corresponding tag transfer.
// This entry point encrypts only. It supplies no key storage, nonce allocation,
// transport framing, replay protection or authenticated plaintext release.
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
    hls::stream<bool>& end_tags);
