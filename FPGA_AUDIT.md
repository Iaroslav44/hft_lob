# FPGA revision — 11 October 2026

> Historical audit snapshot, before the integration revision. The XOR AES module,
> TCP loopback and empty RTO module described below have since been removed.
> New reusable RTL, external HLS adapters and prepared verification infrastructure
> are documented in [FPGA_INTEGRATION.md](FPGA_INTEGRATION.md). They were added
> without executing builds or tests at the user's request. Results in this audit
> must not be attributed to those new components.

## Readiness assessment

The FPGA directory contains eight standalone modules, not an integrated trading
pipeline. No module instantiates another. The top level is a combinational
loopback; there is no board shell, MAC/PCS integration, DMA descriptor format,
clock/reset integration, pin assignment or timing-constraint set. Passing RTL
simulation or generic synthesis does not make this a deployable bitstream.

The reference hardware for subsequent integration is **AMD Alveo UL3524**, with
the **XCVU2P-FSVJ2104-3-E** device and Vivado. This is an engineering reference
selected for the requested ultra-low-latency trading use case, not a claim about
which board is universally best or most popular among London firms. AMD lists
the board and device in its [UL3524 data sheet](https://docs.amd.com/r/en-US/ds1009-ul3524/Product-Details).
The vendor's transceiver-latency figures are not complete tick-to-trade results;
their scope and measurement conditions are on the [product page](https://www.amd.com/en/products/accelerators/alveo/ul3524/a-ul3524-p16g-pq-g.html).

No Vivado/Vitis synthesis, placement, routing, static timing analysis or hardware
execution was available in this workspace. No FPGA was programmed.

## Production blockers that remain

| Priority | Component | Actual behaviour and missing capability |
|---|---|---|
| P0 if used for security | `aes_gcm_offload.sv` | XORs plaintext with the key; its tag depends only on key/IV. A zero plaintext exposes the key and distinct plaintexts produce the same tag. It is not AES-GCM and must not protect traffic or secrets. |
| P1 | `nexus_network_stack_top.sv` | Returns its input unchanged. No parser, arbitration, strategy, risk check or order-generation path is connected. |
| P1 | `tcp_toe_wrapper.sv` | A one-cycle stream register. No TCP sessions, sequence/ACK processing, receive windows, checksums, retransmission or congestion handling. |
| P1 | `tcp_rto_heap.sv` | Ignores inserts and expiry values. Generic synthesis reduces it to constant outputs with zero cells. No timer implementation or timebase/cancellation contract exists. |
| P1 | End-to-end framing | No Ethernet-to-UDP payload extraction, stream-width adapters, Mold message-block splitter, CME packet/MsgSize deframer, sequence-gap recovery or order decoder is integrated in RTL. |
| P1 | Board boundary | No recovered-clock CDC, reset-release strategy, MAC error/FCS propagation, PCIe register map or device/host ownership protocol is specified. |

Implementing AES-GCM, a TCP engine or a timer subsystem is new functionality, not
a local repair to the present demonstration logic. These blocks remain explicitly
identified as incomplete rather than being represented as validated IP. AES-GCM
integration requires a reviewed implementation and conformance tests against the
[NIST GCM validation material](https://csrc.nist.gov/projects/cryptographic-algorithm-validation-program/cavp-testing-block-cipher-modes).

`hls4ml_predictor.cpp` computes a fixed linear reduction of sixteen floats and
its negation. It contains no calibrated parameters or validated trading signal.
Unrolling a floating-point accumulation does not establish an initiation interval,
clock frequency or latency; those require actual HLS scheduling and timing reports.

## Reproduced defects and changes

| Component | Before | Correction |
|---|---|---|
| MoldUDP64 | Session truncated to 64 bits; sequence/count read two bytes early; `tkeep` ignored; metadata not reset | Preserve all 80 session bits; read sequence at byte 10 and count at byte 18; require the first 20 header bytes; reset all outputs |
| Ethernet/IPv4/UDP metadata | Incorrect offsets; 3-bit counter wrapped during a long packet and emitted extra `rx_valid` pulses | Parse the documented Ethernet II layout; validate supported header fields and required byte lanes; publish after 42 header bytes; drain without counter wrap |
| CME SBE header | Missing byte qualifiers allowed an incomplete first beat to be decoded | Require all eight header bytes and document the deframed SBE-message input contract |
| Feed arbitration | An unselected packet could lose its header and later have its tail forwarded as a new packet | Track both ingress packet positions independently and select only at packet starts; retain ownership through valid gaps |
| Loopback reset | Forwarded `valid=1` while reset was asserted | Mask only output valid during reset; retain the direct 512-bit data path |
| HLS host compilation | Strict GCC rejected unknown HLS pragmas | Restrict HLS directives to synthesis; retain identical floating-point arithmetic and accumulation order |
| XDMA descriptor ownership | Copying the bridge duplicated ownership; destruction of the copy invalidated the original descriptors | Delete copy construction/assignment; correct the claim that the class allocates coherent DMA buffers |

The original Mold fixture with session `ABCDEFGHIJ`, sequence
`0102030405060708` and count 2 decoded sequence `494a010203040506` and count
`0708`. These offsets follow the [Nasdaq MoldUDP64 specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/moldudp64.pdf).
The original parser regression produced 141 failures in its initial 565 checks.
The expanded corrected regression has 617 checks.

The arbitration counterexample consists of three cycles: `(A0, idle)`,
`(A1/last, B0)`, `(idle, B1/last)`. The original emits `A0, A1/last, B1/last`,
losing the start of B's packet. The corrected output discards the whole B packet.

Destroying a copied original XDMA bridge caused a subsequent read through the
original to fail with `EBADF`, reproduced using `/dev/null` without hardware.
The new ownership contract is checked at compile time and by a host descriptor
smoke test. Driver errors, short I/O, device offsets and DMA synchronization remain
the caller's responsibility. `PcieDoorbell` likewise has no BAR-size, queue-count
or descriptor-publication protocol; these must be supplied by board integration.

## Interface and timing contracts

All current RTL modules use a single supplied clock. The stream consumers are
always ready and the sinks must accept every valid output beat: there is no
backpressure path. A valid beat is accepted on a rising clock edge; `last` while
`valid=0` has no effect. This is a restricted stream interface, not a lossless
multi-input AXI-Stream fabric. Handshake and byte-qualifier meanings come from the
[Arm AXI-Stream specification](https://documentation-service.arm.com/static/64819f1516f0f201aa6b963c).

Reset must align both input feeds to fresh packet boundaries. Independent physical
receive clocks require CDC before the arbiter. A selected packet owns the output
until its accepted last beat; a permanent stall has no failover timeout. A has
priority on simultaneous starts and can starve B. Losing packets are dropped in
full. There is no sequence-based deduplication, gap repair or lossless buffering.

| Module | Input framing | Registered result |
|---|---|---|
| `nasdaq_itch_moldudp64` | One UDP payload per frame, lane zero first; all 20 header bytes in the first 256-bit beat | Header metadata on the first accepted beat, once per frame; count 0 and `ffff` retained as protocol values |
| `cme_sbe_parser` | One raw SBE message per frame starting at `blockLength`; upstream removes the 12-byte CME packet header and each two-byte MsgSize prefix | Header metadata on the first full 64-bit beat, once per frame |
| `hft_network_stack` | Ethernet II, untagged IPv4 with IHL=5, unfragmented UDP; byte zero in lane zero | Metadata on the sixth accepted 64-bit beat; gaps increase elapsed cycles |
| `feed_line_arbitrator` | Whole packets on two already synchronized streams | One register stage; packet ownership preserved through input gaps |

**Interface changes:** `mold_session` is now `[79:0]`; CME gains
`s_axis_tkeep[7:0]`; the network metadata parser gains
`s_axis_rx_tkeep[7:0]`. No existing repository instantiations required migration.
External instantiations must connect the new byte qualifiers and session width.

The network parser checks EtherType, IPv4 version/IHL, protocol, reserved and
fragmentation bits, minimum IP/UDP lengths and UDP length against IP payload
length. It rejects VLANs, options and fragments. These fields follow
[IPv4](https://www.rfc-editor.org/rfc/rfc791) and
[UDP](https://www.rfc-editor.org/rfc/rfc768). Header validity does **not** establish
received payload length, IP/UDP checksum correctness or Ethernet FCS validity.
Published metadata remains unchanged when a header is rejected. The CME input
boundary follows the [CME message-header description](https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457325932/MDP%2B3.0%2B-%2BSBE%2BMessage%2BHeader);
a full CME UDP packet cannot be connected directly to this header extractor.

The 64-, 256- and 512-bit modules are not directly interchangeable. Sustainable
bandwidth depends on width, accepted beats per cycle and achieved clock rate.
The source's old "100G" label did not establish any of these properties.

## Verification and reproduction

Recorded results on Ubuntu WSL2 x86-64:

| Check | Result |
|---|---|
| Icarus elaboration | 8/8 RTL modules |
| Functional simulation | 617 parser checks and the stream/reset regression passed |
| Yosys generic synthesis and structural check | 8/8 RTL modules |
| GCC Release/native/LTO with `HFT_FPGA_TESTS=ON` | 30/30 CTest cases, including the existing sanitizer groups |
| Standalone host reference, strict GCC 11.4 and Clang 14 | Both passed |

Portable verification uses Icarus Verilog/vvp 11.0 and optional Yosys 0.69 generic
synthesis. The tool runner logs commands, tool versions, source hashes, failures
and timeouts; simulation requires an explicit success marker as well as exit 0.
Generated files stay under ignored `build-*` directories. Local extracted tools
under `.tools/` are also ignored; the runner never installs dependencies itself.

```bash
# Icarus Verilog and vvp must be on PATH, provided by overrides, or locally extracted.
python3 tests/rtl/run_tests.py --build-dir build-fpga-tests

# Optional generic synthesis; this is not Vivado timing or board validation.
python3 tests/rtl/run_tests.py --build-dir build-fpga-tests-synth --synth

cmake -S . -B build-fpga -DCMAKE_BUILD_TYPE=Release -DHFT_FPGA_TESTS=ON
cmake --build build-fpga -j4
ctest --test-dir build-fpga --output-on-failure
```

`IVERILOG`, `VVP` and `YOSYS` can specify executable paths; `IVERILOG_BASE` can
specify relocated compiler components. `HFT_FPGA_TESTS` defaults to OFF so ordinary
C++ builds do not require an HDL toolchain. Enabling it registers RTL verification
and the strict C++ HLS/XDMA host reference test. Missing tools cause failure,
not a silently passing RTL test.

The final generic-synthesis run is recorded in
`build-fpga-audit/final/rtl-results.json`; the integrated CTest run is in
`build-audit-release/Testing/Temporary/LastTest.log`. Before-change parser and
stream failures and the XOR/empty-timer capability probes remain in
`build-fpga-audit/`. A missing-tool override and a fake simulator returning exit 0
without its success marker both correctly failed the runner.

The parser bench covers byte order, all missing header-byte positions, truncated
headers, reset, valid gaps, invalid last markers, back-to-back frames, invalid
header draining and long payloads. The stream bench covers arbitration in both
directions, simultaneous starts, ownership through gaps, discarded tails, reset
and loopback behaviour. Host reference tests exercise the unchanged HLS arithmetic
and unique descriptor ownership; they do not exercise PCIe, DMA or a board.

All eight original modules already passed elaboration and generic synthesis,
including the XOR and empty timer. Functional results must therefore be read
together with the blockers above. No formal equivalence, exhaustive property
proof, CDC/RDC analysis, timing closure or nanosecond latency result is claimed.
