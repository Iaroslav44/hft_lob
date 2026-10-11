# Buffered MoldUDP64 and ITCH Add Order path

This component connects a 64-bit UDP-payload stream to a canonical 32-byte Add
Order record stream. It contains no MAC, IP/UDP checksum engine, DMA engine or
clock-domain crossing. It does not use the older always-ready header extractors.

The implementation consists of:

- `src/nexus/fpga/market_data/moldudp64_packet_buffer.sv`
- `src/nexus/fpga/market_data/itch_a_normalizer.sv`

These files have received source-level review only. No compilation, lint,
simulation, synthesis, timing analysis or hardware execution was performed for
this addition, as requested. Earlier verification results for other files do not
cover these modules.

## Stream and reset contract

Both modules use the same clock. A transfer occurs only on a rising edge with
`valid && ready`. The source holds data, keep, last, user and associated metadata stable
while valid is asserted and ready is low. Lane zero contains the earliest byte.
`last` while valid is low has no effect.

One input frame is exactly one UDP payload. All nonfinal beats have keep `ff`;
the final beat has a nonempty contiguous mask starting at bit zero. There are no
Ethernet, IPv4 or UDP headers, padding bytes or interleaved datagrams on this
interface. The packet input's one-bit `s_axis_tuser` marks a bad frame. A flag on
any accepted beat rejects the complete packet, including a late transport or
CDC-abort flag; it is sticky through the accepted final beat. With no upstream
error signalling, connect it to zero. The upstream boundary must establish
transport integrity and length before permitting the packet decision to accept
it, or flag failure before completing the frame. A metadata-only UDP header
extractor does not establish that integrity.

Asserting `rst_n=0` discards pending input, packet decisions and output records.
Input framing must restart at a fresh packet/message boundary after reset.
Reset release and external clock-domain crossings belong to the integrating
design. `ready`, `valid` and `idle` are low during reset.

## Packet quarantine and external decision

`moldudp64_packet_buffer` stores one packet in a bounded 64-bit memory. Its
`MAX_PACKET_BYTES` parameter defaults to 2048 and must be a multiple of eight
between 24 and 65528 inclusive. The storage has one synchronous read port and one
write port; memory contents are not reset or treated as valid without a completed
receive/validation transaction.

An oversized or malformed frame is consumed through its accepted final beat.
Its packet decision reports an error, and none of its message bytes are emitted.
The byte count saturates at `MAX_PACKET_BYTES + 1` on overflow. A source that
never supplies a final beat leaves the module busy; timeout/reset policy is
external.

After the final input beat, `packet_valid` exposes stable decision metadata:

| Signal | Meaning |
|---|---|
| `packet_session[79:0]` | All ten opaque session bytes, first wire byte at the MSB |
| `packet_sequence[63:0]` | First message sequence, decoded from big-endian wire bytes |
| `packet_count[15:0]` | Original message count, including heartbeat/end-session values |
| `packet_bytes[15:0]` | Received byte count, saturated as described above |
| `packet_error[3:0]` | Validation result below |

On `packet_valid && packet_ready`, replay starts only if `packet_accept=1` and
`packet_error=0`. A rejection releases the buffer without message output. Fields
from an erroneous/incomplete header must not be used for sequence/session state.
Decision metadata may change when the decision has been consumed; latch it at
the handshake if it is needed later.

The integrating controller can hold `packet_ready=0` until it has transport and
sequence decisions. Session ownership, A/B deduplication, missing-message
recovery, restart and administrative rearming are deliberately external. The
buffer never infers that one feed is a duplicate of another from arrival time.

| Error | Meaning |
|---:|---|
| 0 | Complete supported packet framing |
| 1 | Invalid keep mask or partial nonfinal beat |
| 2 | Packet exceeds bounded storage |
| 3 | Fewer than 20 MoldUDP64 header bytes |
| 4 | Final byte arrives halfway through a two-byte message length |
| 5 | A declared message body is truncated |
| 6 | Declared blocks are missing, or bytes remain after the declared blocks |
| 7 | `sequence + count` overflows 64 bits for a data packet |
| 8 | An ITCH A block has length other than 36, invalid B/S side or zero shares |
| 9 | Upstream asserted the bad-frame marker on an accepted input beat |

The first error detected is retained. Only the count's exact number of blocks is
accepted, and the last block must end exactly at the UDP payload boundary.
Zero-length message blocks are supported by MoldUDP64. The checked sequence sum
is the next expected sequence; this component deliberately disallows rollover
within one session.

Count zero is a heartbeat; count `ffff` is end of session. Both require exactly
the 20-byte header, expose a successful packet decision, and produce no message
frames regardless of `packet_accept`. Their interpretation belongs to the
external controller. Count `ffff` is never treated as 65535 message blocks.

Every A block is validated during reception, even if it is the final block after
several valid A blocks. Therefore a malformed later A prevents all earlier A
blocks in that packet from being published. Non-A bodies remain opaque: this is
not validation of every ITCH message type or instrument field.

## Message replay

The output removes Mold headers and length prefixes and repacks each message
into its own `m_axis_*` frame. All full message beats have keep `ff`; a final
partial beat uses contiguous low lanes. An empty message is represented by one
accepted beat with keep `00` and last asserted.

`m_msg_session`, `m_msg_sequence`, `m_msg_length` and `m_msg_packet_last` stay
constant throughout each message, including downstream stalls. Sequence
increments for every Mold block, including empty and unsupported messages.
`m_msg_packet_last` marks the source packet's final message, not the final Add
Order after filtering.

Replay serially assembles bytes from synchronous RAM into 64-bit output words.
Receive, decision and replay do not overlap. The buffer stops accepting new
packets until replay finishes. This is a correctness-oriented bounded component,
not a line-rate 10/25/100G implementation. A physical receiver that cannot stop
must provide packet buffering or an explicit whole-packet drop policy upstream.
Clock frequency, resource use, throughput and latency have not been measured.

## ITCH A normalization and host representation

Connect the buffer's message stream and `m_msg_*` metadata to the corresponding
`s_axis_*` and `s_msg_*` normalizer ports. The normalizer skips other types and
empty messages. For type A, it independently requires exactly five input beats,
keep `ff` on the first four, keep `0f` and last on the fifth, a B/S side and
positive shares. No record becomes valid before the complete message arrives.
Malformed A input is drained and raises a one-cycle `reject_valid` diagnostic.
An input with no valid type byte also raises that diagnostic unless it is the
explicit empty-message representation. Unsupported types do not raise it.

Each accepted A becomes four output beats, all with keep `ff` and last only on
the fourth. Output words and metadata remain stable under backpressure.

| Output bytes | Field | Encoding |
|---|---|---|
| 0..7 | `order_id` | Little-endian uint64 |
| 8..15 | `ts_ns` | Little-endian uint64; upper 16 bits zero |
| 16..19 | `price` | Little-endian uint32; unchanged ITCH price units |
| 20..23 | `qty` | Little-endian uint32 |
| 24..25 | `locate` | Little-endian uint16 |
| 26 | `is_buy` | Byte value 1 for B, 0 for S |
| 27..31 | Padding | Zero |

This matches `MdAddOrder` field positions on the project's current little-endian
host ABI. The record is an explicit serialization, not a portable C++ object
representation: a big-endian host must decode it, and DMA publication/object
lifetime still require a host integration contract. Tracking number and stock
symbol are not included, matching the existing host normalizer. ID zero and
price zero remain valid; locate-based routing is external.

`m_order_session`, `m_order_sequence` and `m_order_packet_last` retain source
metadata on all four record beats. If a packet ends with a skipped non-A message,
no order record has `m_order_packet_last=1`; it must not be used as an unconditional
packet-completion notification. Reset and accepted packet replay cannot retract
records already consumed downstream.

The buffer's `idle` means no received partial packet, pending decision or replay.
The normalizer's `idle` means no partial message or pending record. A controller
must require both and coordinate upstream quiescence before changing sequence or
session configuration.

Protocol references: [Nasdaq MoldUDP64](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/moldudp64.pdf)
and [TotalView-ITCH 5.0, Add Order](https://nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf).
