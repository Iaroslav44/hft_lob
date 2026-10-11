#pragma once

#include "ethz_toe_config.hpp"
#include <toe/session_lookup_controller/session_lookup_controller.hpp>

// Internal service for the pinned TOE controller, in the same clock/reset domain.
// The controller owns IDs 0..63 and serializes lookup/create transactions. Its
// update FIFO queues DELETE before returning an ID to the allocation free list.
// Reset this service, the controller, and their connecting FIFOs together.
// The pinned controller ignores insertion failure; its exclusive allocator and
// ordered updates must remain the only owners of this table's session bindings.
//
// INSERT binds an unused ID to a previously absent tuple; repeating the same
// binding succeeds. Conflicting bindings or IDs outside 0..63 fail unchanged.
// DELETE requires both tuple and ID to match; missing bindings fail unchanged.
// Lookup misses return sessionID zero. Every response echoes key and source;
// update responses additionally echo operation and requested ID.
//
// Pending updates precede lookups, including when their response is stalled.
// Per-stream FIFO order is preserved. The two request streams have no total
// arrival order; independent callers must use replies as completion barriers.
// Arbitrary continuous update traffic may defer lookups and is outside this
// internal-controller contract. Downstream must drain both response streams.
//
// Storage is 64 x 64-bit keys plus a 64-bit validity mask, with at most 64
// parallel key comparisons. The pipeline directive is a synthesis request;
// frequency, achieved initiation interval and resource use are unmeasured.
void nexus_session_table(
    hls::stream<rtlSessionLookupRequest>& lookup_request,
    hls::stream<rtlSessionUpdateRequest>& update_request,
    hls::stream<rtlSessionLookupReply>& lookup_response,
    hls::stream<rtlSessionUpdateReply>& update_response);
