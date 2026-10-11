# External FPGA integration boundaries

These sources call pinned external implementations. The former XOR crypto,
registered-loopback TCP and empty RTO modules have been removed; these HLS entry
points intentionally use new names and complete interfaces. They do not provide
drop-in module-name compatibility, instantiate a board shell, or connect to the
host benchmark. The composed `nexus_tcp_ip_stack` described below now provides the
network endpoint, including its session table. No build, simulation, lint, synthesis or hardware run was performed
for this integration revision. The Tcl files are prepared recipes only.

## Integrated Ethernet/IPv4/TCP top

[`nexus_tcp_ip_stack.hpp`](nexus_tcp_ip_stack.hpp) declares the complete HLS top;
[`nexus_tcp_ip_stack.cpp`](nexus_tcp_ip_stack.cpp) connects these active processes:

1. `nexus_ethernet_guard` quarantines a complete MAC frame, validates supported
   headers and strips Ethernet padding before protocol admission.
2. ETH `ip_handler` removes Ethernet framing and dispatches ARP, IPv4/TCP and ICMP.
3. ETH `arp_server_subnet` responds to ARP and resolves egress next-hop addresses.
4. `nexus_icmp_echo` uses ETH's checksum/echo transformation with a bounded
   256-word packet FIFO and a decision gate that waits for the complete checksum.
5. `nexus_ethz_toe` runs the TCP engine. `nexus_session_table` supplies its internal
   lookup/update service, preserving session ownership without hash eviction.
6. Packet-locked round-robin arbiters merge TCP/ICMP before ETH `mac_ip_encode`,
   then merge encoded IP and ARP frames on Ethernet TX. Frames never interleave;
   a selected stream retains ownership through gaps until its accepted TLAST.

The profile is Ethernet II / IPv4 unicast, MTU 1500, 64-bit datapath, 64 TCP sessions
and a real 156.25 MHz clock. ARP and ICMP Echo are included. UDP, IPv6, VLANs,
IPv4 options and fragments are rejected. The separate `nexus_mold_ingress` is not
wired to this endpoint; it still needs a UDP feed provider. No DHCP, IP forwarding,
fragment reassembly or automatic board selection is implied.

RX uses `NexusEthernetBeat = ap_axiu<64,1,0,0>`: destination MAC byte zero is in
bits [7:0], KEEP/STRB identify valid bytes, LAST ends the frame, and USER marks any
MAC/FCS/CDC error. Non-final beats must have KEEP=STRB=ff. A final beat requires a
nonzero contiguous low-byte mask and STRB=KEEP. Any marked beat rejects the whole
frame. The MAC supplies frames without preamble/FCS. TX uses `NexusToeBeat` with
STRB=KEEP; the MAC supplies minimum-frame padding and FCS. Both ports can stall.

The guard checks actual frame extent (at most 1514 bytes), complete ARP fields,
local destination, IPv4 version/IHL/length/checksum, nonzero TTL and unfragmented
transport bounds. TCP options are checked for bounded framing before admission;
EOL, NOP and well-formed unknown options are supported. TCP checksum and connection
state are handled by the TOE. ICMP Echo checksum failures are dropped before TX.
An incomplete frame has no admission decision until LAST or reset.

The guard is single-buffered and pauses reception during validation/replay. This
is a bounded source implementation with **no measured latency, throughput or
timing-closure claim**. Upstream FIFO depths, HLS directives and combinational
session comparisons need synthesis and co-simulation on the chosen device.
An unstoppable MAC needs an enclosing whole-frame FIFO and overflow reporting.

Public application/memory streams are the same typed interfaces documented for
the standalone TOE below. The top internally owns all four session lookup/update
streams. The table stores 64 keys plus a validity bitmap, compares complete keys,
and never evicts a live binding. Updates precede lookups; blocked replies preserve
request ownership. Do not connect another allocator or manipulate live IDs.

`arp_lookup_request`/`arp_lookup_response` expose the actual ARP cache query service.
A miss returns `hit=false` and triggers an ARP request; query again after resolution
before relying on a hit. Drain each reply. The same behavior on the egress path
drops the unresolved IP transmission; TCP timers retry it. ICMP has no retransmit
queue. Cache index collisions return misses after the full stored IP is compared.
There is no periodic ARP aging in this provider; coordinate address/neighbor changes
with reset or the enclosing control plane.

Scalar MAC/IP/netmask/gateway values use address byte zero in bits [7:0]. For
example, IPv4 `10.1.2.3` is `0x0302010a`, `/24` is `0x00ffffff`, and MAC
`02:00:00:00:00:01` is `0x010000000002`. Configure a gateway on the same subnet as
the local address; the pinned encoder uses its subnet for on-link decisions.
Keep all values stable until coordinated reset/quiescence. Reset the complete top,
memory controllers and application ownership together. Outstanding buffers and
sessions do not survive reset.

`session_count`, `arp_request_count` and `arp_reply_count` use upstream 16-bit
status outputs. `rx_rejected_frames` counts guard rejections and
`icmp_rejected_frames` counts echo checksum rejections, both modulo 2^32. The
guard counter does not count TCP checksum drops, ARP misses or physical MAC loss.
Observe the exported status-valid signals; these counters are not DMA completions.

### Source preparation and IP export

[`prepare_tcp_ip_stack.tcl`](prepare_tcp_ip_stack.tcl) is the explicit build entry.
It checks the clean pinned commit, copies tracked provider HLS files into its
dedicated `build-hls/nexus_tcp_ip_stack/vendor` directory, and applies exact-context
overlays to that generated copy. Original copyright/license notices are retained.
The dependency cache is not patched. The preparation includes:

- Separate 64-bit RX/TX IPv4 checksum helpers; the pin otherwise supplies these
  prefixed entry points only at 512 bits. Each direction owns independent state.
- Full-key ARP cache checks, explicit empty-entry initialization and removal of an
  unsafe declaration that consecutive table accesses have no dependencies.
- Deterministic output STRB, the bounded retransmission scan, and TCP option
  parsing/payload-boundary fixes. The pinned cuckoo table is replaced by the
  internal bounded session table because failed insertion could evict a live key.

Set `NEXUS_HLS_PART` to the exact installed device part before an explicit future
export. The clock setting must match the physical network clock:

```bash
export NEXUS_HLS_CLOCK_NS=6.4
vitis_hls -f src/nexus/fpga/integration/prepare_tcp_ip_stack.tcl
```

Alternatively, the optional CMake tooling exposes the explicit `fpga_tcp_ip`
target, with `NEXUS_VITIS_HLS_EXECUTABLE` selecting the tool. Neither path runs
automatically. HLS export produces the actual interface metadata and RTL under
`build-hls/nexus_tcp_ip_stack/solution/impl/ip`. Subsequently, in an open enclosing
Vivado project:

```tcl
source fpga/tcp_ip_stack.tcl
```

This registers `nexus.local:hls:nexus_tcp_ip_stack:1.0`. Instantiate that IP and use
its generated `component.xml` for signal names and packed widths. The source
integration does not guess generated ports, create a board top or connect pins.
No preparation recipe, export, test or synthesis command above was run for this
revision; interoperability, reset behavior, memory service correctness and timing
remain unverified on hardware.

## Taxi packet CDC

`nexus_axis_async_fifo64.sv` instantiates the actual `taxi_axis_async_fifo` and
`taxi_sync_reset` modules from
[`fpganinja/taxi` at cc70b270](https://github.com/fpganinja/taxi/tree/cc70b270b910d369ab1ad7b3855e76399fd461f1).
The selected FIFO/synchronizers and their constraint scripts carry
CERN-OHL-S-2.0 notices; the selected `taxi_axis_if.sv` carries an MIT notice.
The upstream repository also offers commercial licensing. No upstream source or
notice is copied into or modified by this wrapper.

The flat interface has separate `s_clk`/`s_aresetn` and `m_clk`/`m_aresetn`
domains. Both streams carry 64-bit data, eight keep bits, valid/ready, last and
one user bit. TUSER=1 marks an erroneous frame. Every transfer requires
`valid && ready`; data and all sidebands must remain stable until accepted.
Frame boundaries must be complete and ordered, with the bad-frame indication
present on the accepted final beat: upstream's bad-frame decision checks that
beat. Keep masks are transported unchanged, not validated here.

The configuration enables frame buffering, dropping marked-bad frames and
dropping oversized frames. Ordinary FIFO fullness deasserts input ready; it
does **not** unconditionally accept and discard incoming traffic. An oversized
frame is consumed through its final beat and discarded in full. A source that
never supplies TLAST prevents resynchronization. This is suitable for a source
that can honour backpressure; it cannot receive an unstoppable MAC losslessly.
The enclosing receive path still owns physical packet loss reporting and recovery.

`DEPTH_BYTES` defaults to 4096 and must be a power of two of at least 16. Each
accepted beat consumes eight byte lanes regardless of its keep mask, so the
default capacity is 512 beats. A frame larger than that beat capacity cannot be
committed. `RAM_PIPELINE` defaults to one; the output FIFO and pause feature are
disabled. Neither a measured latency nor achieved throughput is claimed.

Each active-low asynchronous reset asserts a local active-high reset immediately
and releases it through four registers in its own clock domain. Taxi separately
synchronizes the opposite-side reset internally. The wrapper suppresses local
ready/valid handshakes during local reset; payload bits while invalid are
unspecified. Assert both resets during startup, run both clocks for reset release,
and resume the source at a new frame boundary after its reset.

A unilateral destination reset abandons pending output. The upstream FIFO drains
an interrupted input frame before accepting the next complete frame. A unilateral
source reset can end an already-started output frame with an additional
`TLAST=1, TUSER=1` termination beat. Its payload is not a valid packet suffix:
the consumer must quarantine the frame until its final integrity decision and
reject that frame. When feeding `nexus_mold_ingress`, connect **all** stream
signals, including `m_axis_tuser` to its `s_axis_tuser`; the packet buffer latches
accepted error marks before replay/admission. A reset cannot retract payload
already committed by an unrelated downstream consumer.

The `s_status_*` outputs belong to `s_clk`, and `m_status_*` to `m_clk`.
Depths are delayed RAM-pointer observations in eight-byte units. They exclude
data already advanced into the read pipeline, may be transient during reset,
and must not replace ready/valid flow control. Source-domain `overflow`,
`bad_frame` and `good_frame` are upstream event pulses: oversize/reset-aborted
input discard, final-beat error discard and frame commit, respectively. Read-side
event indications cross through toggles and can merge events at unequal clock
rates; count source-domain pulses locally when exact diagnostic counts are needed.
Reset loses pending data and diagnostic history.

`taxi_axis_async_fifo_sources.tcl` reads the interface, reset/signal synchronizers,
FIFO and wrapper from the exact clean pinned checkout. Source it in the enclosing
Vivado flow before synthesis. It does not choose the top, FPGA part, clocks or pins.
From the repository root, the explicit source-loading command is:

```tcl
source src/nexus/fpga/integration/taxi_axis_async_fifo_sources.tcl
```

After synthesis/linking, with both clocks defined by the enclosing clock XDC,
source `taxi_axis_async_fifo_constraints.tcl` before implementation. It verifies
that synthesized pointer registers and clock periods exist, then sources the
original FIFO, reset and signal constraint scripts. RTL elaboration alone does
not provide the register objects those scripts require. Apply the loader to each
implementation design; sourcing it once in a separate inspection session does
not configure future runs.

With that synthesized design open and its clock constraints already loaded:

```tcl
source src/nexus/fpga/integration/taxi_axis_async_fifo_constraints.tcl
```

For a Vivado project flow, the enclosing project can register the loader as an
implementation-only Tcl file in its constraint set. Vivado sources constraint-set
Tcl after XDC when opening a design; in a non-project flow, explicitly source it
after clock XDC in the linked synthesized design. This follows
[AMD UG894's constraint-script ordering](https://docs.amd.com/r/en-US/ug894-vivado-tcl-scripting/Using-Tcl-Scripts-in-a-Constraints-Set).
Keep the upstream datapath-delay and bus-skew constraints active. Broad
asynchronous clock groups or false paths can override the maximum-delay checks;
they are not substitutes for constraining Gray-pointer and bundled-pointer CDC.
CDC reports, timing-exception coverage, reset behavior and packet regressions
remain unexecuted integration checks.

## ETH TCP engine

This section documents the smaller standalone `nexus_ethz_toe` boundary. The
integrated top above supplies Ethernet/IP/ARP dispatch and the session table
internally; only the remaining application, memory and board services are external.

`nexus_ethz_toe.cpp` calls the actual `toe` entry point from
[`fpgasystems/fpga-network-stack` at 75c32bae](https://github.com/fpgasystems/fpga-network-stack/tree/75c32bae73f5c336b5c46c9de35ae0cb9cb7cc1c).
The upstream HLS headers retain their BSD-3-Clause notices. The wrapper uses those
types directly instead of reproducing their binary layouts.

The upstream legacy `hdl/common/tcp_stack.sv` is deliberately not part of this
integration. At this revision it declares a 24-bit open-status stream, whereas
`hls/toe/toe.hpp` defines `openStatus` with 72 semantic bits: session ID (16),
success (8), IP address (32), and port (16). It also depends on external DavOS
interface definitions and old generated port names. Export the new HLS top and
use its generated `component.xml` for the actual RTL interface and byte padding.

### Required enclosing system

- A single network clock, a reset shared with the session table and memory
  controllers, and a stable local IPv4 address. Changing the address requires
  closing sessions and draining their buffers. Reset abandons outstanding traffic;
  the enclosing application must recover connections and message sequencing.
- An IPv4/TCP packet path with the first IP-header byte in byte lane zero. Ethernet
  MAC/PCS, ARP, IP routing/demultiplexing, FCS/error handling and CDC are external.
  Every stream supports backpressure; never connect an always-ready-only source
  without buffering and an explicit overflow policy. Input TSTRB must equal TKEEP.
- Separate RX and TX buffer services. Each exposes read/write commands, read/write
  data and write-completion status. Commands are the full upstream `mmCmd`, not a
  host physical address or an invented DMA descriptor. Preserve its byte count,
  address, tag and control fields. There is no upstream read-status stream; shell
  read faults must be escalated instead of supplying fabricated successful data.
- A session lookup/update implementation preserving request/response ordering,
  keys, source selectors, hit/success status and session IDs. No constant-hit stub
  or private replacement table is supplied by this wrapper. Application requests
  and session-table responses must reference live session IDs in `[0, 64)`;
  upstream tables do not validate arbitrary control-plane indices.
- Application control for listen/open/close, notifications, receive reads, and
  transmit admission. Submit TX metadata, consume its status, and send the payload
  only after success. Preserve returned session IDs and all open-status fields.

Retransmission, probe/close timers, acknowledgements and congestion state remain in
the upstream engine. Its TX memory reads serve retransmission as well as ordinary
transmit; the memory service must retain unacknowledged data and honour the engine's
commands. The removed empty `tcp_rto_heap` has no role in this integration.

`ethz_toe_config.hpp` must be **force-included in every upstream translation unit**
and in the adapter. The prepared Tcl does this. The fixed configuration is:

| Setting | Value |
|---|---:|
| Datapath | 8 bytes / 64 bits |
| Maximum sessions | 64 |
| MSS | 1460 bytes |
| RX DDR bypass | Disabled |
| Window scaling | Enabled |
| Fast retransmit | Enabled |
| TCP_NODELAY | Enabled |

With the upstream 18-bit per-session buffer address, provision 16 MiB of address
space for each of the RX and TX buffer services at this session count. These are
logical service ranges; the board memory controller owns their physical mapping.
No host-memory coherence or PCIe DMA contract is implied.

The upstream `net_axis` to `ap_axiu` converter does not assign TSTRB. Four distinct
egress stages set TSTRB=TKEEP for network TX, RX/TX buffer writes and application
RX. Each has a two-word internal FIFO and preserves backpressure. They add pipeline
storage; no cycle latency or initiation interval has been established by synthesis.

`prepare_ethz_toe.tcl` contains the complete source manifest, checks the checkout
commit, force-includes the configuration, selects `nexus_ethz_toe`, and prepares
Vivado IP export through HLS. It requires explicit `NEXUS_HLS_PART` and
`NEXUS_HLS_CLOCK_NS=6.4`. The supplied hardware clock must also be 156.25 MHz:
upstream timer constants assume 6.4 ns per cycle. A different clock requires
regenerating all timer constants, not merely changing the synthesis constraint.
Arbitration, backpressure and achieved scheduling can extend actual timeout
intervals; the constants are not a hard real-time guarantee. No board part or
achieved clock is inferred. Its reset policy
resets all HLS state. The board must handle reset/initialisation before admitting
traffic. This recipe has not been executed.

The recipe applies `ethz_retransmit_timer.patch` to a generated source copy under
`build-hls/`, preserving the original copyright/license header and leaving the
dependency checkout untouched. It requires the exact pinned revision, a clean
checkout and one exact match of the patch context. Upstream advances the scan
cursor by five positions after a write hazard without wrapping that branch. For
example, cursor 63 plus a timer update for session 63 leaves cursor 68; the next
scan indexes outside a 64-entry table before its normal increment wraps. The
patch subtracts the capacity when needed. Given `0 <= cursor < 64`, the increment
produces at most 68, below twice the capacity, so one subtraction restores the
bound. The ordinary increment already wraps; ACK, probe and close scans likewise
wrap their unit increments. Their event-provided session IDs still require valid
upstream session ownership. This is a source-level fix, not an executed regression.

All uses of `MAX_SESSIONS` were inspected for the 64-session profile: the session
manager and table cursors can represent this bound, table extents use the same
constant, the 12-bit ACK counter holds `TIME_64us=157`, and even `TIME_120s` fits
the 32-bit timer fields. Changing the profile requires a new bounds review.

## AES-256-GCM encryption

`../../hls/security/nexus_aes256_gcm_encrypt.cpp` calls
`xf::security::aes256GcmEncrypt` from
[`Xilinx/Vitis_Libraries` at 86459b4c](https://github.com/Xilinx/Vitis_Libraries/tree/86459b4c4d57f14c6b979ecf078e9ab30deb5576/security).
The external implementation is Apache-2.0. Add its `security/L1/include` directory
to the HLS include path; the implementation is header-based. There is no software
fallback when the HLS toolchain or pinned headers are absent.

The top uses `ap_ctrl_hs` and independent AXI streams. Each invocation processes a
finite batch using the upstream protocol:

1. Supply one `false` token on `end_lengths` for each message and a final `true`
   token after the last message.
2. Per message supply one 256-bit key, one 96-bit IV, one AAD bit length and one
   plaintext bit length. Supply `ceil(length / 128)` words on each corresponding
   128-bit data stream; a zero length consumes no data words.
3. Drain ciphertext, its bit length, the 128-bit tags and `end_tags` concurrently.
   The latter emits one `false` token per tag followed by one final `true` token.
   Upstream writes the tag before its `false` token, so waiting for that token
   before accepting the tag can deadlock. A permanently blocked output can
   prevent completion.

Byte zero occupies bits [7:0]. Lengths are measured in **bits**, must be multiples
of eight, and plaintext length must not exceed `2^39 - 256` bits. Zero unused high
bytes of partial input words. The corresponding high bytes in the final ciphertext
word are unspecified and must be ignored according to `ciphertext_bits`.
These are trusted input preconditions; the wrapper
does not silently truncate, discard or repair malformed records. There is no
TKEEP/TLAST on these length-framed data streams.

The enclosing service owns key access, unique IV allocation across resets,
framing and replay policy. This adapter exports encryption only. The upstream
decrypt API produces plaintext and a calculated tag without accepting an expected
tag; exporting it directly would not provide authenticated plaintext release.
No decryption endpoint or claim of a certified cryptographic module is supplied.

`../../hls/security/prepare_aes256_gcm.tcl` checks the source revision and prepares
the encrypt top with the same explicit part/clock environment variables. The
kernel needs its own cryptographic vectors, malformed-record tests, scheduling
report and board timing review before deployment; none was run for this revision.
