#include "nexus_session_table.hpp"

namespace {

constexpr unsigned session_capacity = 64;
static_assert(TCP_STACK_MAX_SESSIONS == session_capacity,
              "The internal session table requires the 64-session TOE profile");

ap_uint<64> pack_key(const threeTupleInternal& tuple) {
#pragma HLS INLINE
    ap_uint<64> key;
    key.range(31, 0) = tuple.theirIp;
    key.range(47, 32) = tuple.myPort;
    key.range(63, 48) = tuple.theirPort;
    return key;
}

} // namespace

void nexus_session_table(
    hls::stream<rtlSessionLookupRequest>& lookup_request,
    hls::stream<rtlSessionUpdateRequest>& update_request,
    hls::stream<rtlSessionLookupReply>& lookup_response,
    hls::stream<rtlSessionUpdateReply>& update_response) {
#pragma HLS INLINE off
#pragma HLS PIPELINE II=1

    static ap_uint<64> keys[session_capacity];
    static ap_uint<64> occupied = 0;
#pragma HLS ARRAY_PARTITION variable=keys complete dim=1
#pragma HLS RESET variable=keys off
#pragma HLS RESET variable=occupied

    const bool is_update = !update_request.empty();
    threeTupleInternal tuple;
    lookupSource source;
    lookupOp operation = INSERT;
    ap_uint<16> requested_id = 0;

    // A queued deletion must not be bypassed by a lookup of its released tuple.
    // Do not consume a request while its response stream is full.
    if (is_update) {
        if (update_response.full()) return;
        const rtlSessionUpdateRequest request = update_request.read();
        tuple = request.key;
        source = request.source;
        operation = request.op;
        requested_id = request.value;
    } else {
        if (lookup_request.empty() || lookup_response.full()) return;
        const rtlSessionLookupRequest request = lookup_request.read();
        tuple = request.key;
        source = request.source;
    }

    const ap_uint<64> key = pack_key(tuple);
    ap_uint<64> matches = 0;
    for (unsigned index = 0; index < session_capacity; ++index) {
#pragma HLS UNROLL
        matches[index] = occupied[index] && keys[index] == key;
    }

    if (is_update) {
        rtlSessionUpdateReply reply;
        reply.op = operation;
        reply.key = tuple;
        reply.sessionID = requested_id;
        reply.source = source;
        reply.success = false;

        if (requested_id < session_capacity) {
            const ap_uint<6> slot = requested_id.range(5, 0);
            const bool same_binding = matches[slot];
            if (operation == INSERT) {
                // Neither another ID for this tuple nor another tuple at this
                // ID can be overwritten. The TOE allocator avoids both cases.
                if (same_binding || (!occupied[slot] && matches == 0)) {
                    keys[slot] = key;
                    occupied[slot] = true;
                    reply.success = true;
                }
            } else if (operation == DELETE && same_binding) {
                occupied[slot] = false;
                reply.success = true;
            }
        }
        update_response.write(reply);
    } else {
        ap_uint<6> matched_id = 0;
        // INSERT preserves key uniqueness, so a bitwise reduction identifies
        // the only matching slot without giving duplicate keys a priority rule.
        for (unsigned index = 0; index < session_capacity; ++index) {
#pragma HLS UNROLL
            if (matches[index]) matched_id |= ap_uint<6>(index);
        }
        rtlSessionLookupReply reply;
        reply.key = tuple;
        reply.source = source;
        reply.hit = matches != 0;
        reply.sessionID = matched_id;
        lookup_response.write(reply);
    }
}
