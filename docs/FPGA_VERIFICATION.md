# Optional RTL verification

These suites extend the existing Icarus regressions. They require explicitly
selected optional tools; the normal C++ build does not need them. The sources and
launch commands were prepared by static inspection only. No dependency
installation, build, Python compilation, simulation, lint or formal execution was
performed for this addition. There are no new passing-test or proof claims.

The contracts in [FPGA_AUDIT.md](../FPGA_AUDIT.md) and
[FPGA_MARKET_DATA.md](FPGA_MARKET_DATA.md) define the tested boundaries. The older
standalone inputs have no backpressure. The new Mold ingress uses ready/valid and
expects one complete integrity-checked UDP payload per frame. Neither suite
establishes Ethernet/IP/UDP checksum correctness or MAC FCS validation.

## Seeded cocotb regressions

Direct dependencies are pinned in [requirements.txt](../tests/rtl/requirements.txt):
cocotb 2.1.0 and cocotbext-axi 0.1.28. The launch API was reviewed against the
cocotb 2.1.0 documentation; the pair has not been exercised in this workspace.
Use Linux Python 3.10+ with working Icarus 11+, or a compatible Verilator
installation (cocotb 2.1 requires Verilator 5.036+). The simulator is a separate
dependency.

Prepared commands, **not executed**:

~~~bash
python3 -m venv .tools/rtl-venv
.tools/rtl-venv/bin/python -m pip install -r tests/rtl/requirements.txt
.tools/rtl-venv/bin/python tests/rtl/cocotb/run.py \
  --sim icarus --seed 20261011 --seed 1592594996 \
  --build-dir build-rtl-cocotb
~~~

The default runs both suites. Select only one with --suite standalone or --suite
ingress. For the existing locally extracted Icarus package, prepare its binary
path and relocated compiler component directory before using the same launcher:

~~~bash
export PATH="$PWD/.tools/iverilog/usr/bin:$PATH"
export IVERILOG_BASE="$PWD/.tools/iverilog/usr/lib/x86_64-linux-gnu/ivl"
~~~

The standalone suite contains four tests:

- An arbiter cycle scoreboard assigns each ingress frame one disposition:
  admitted or discarded. It checks exact output selection, priority, valid gaps,
  delayed losing tails and reset recovery against an independent model.
- AXI stream sources and a passive monitor check that every emitted frame is an
  entire submitted frame, without corruption, duplication, concatenation or a
  trailing fragment. No ready signal is invented. Frames contain whole 64-bit
  words because this interface has no byte qualifiers. The exact cycle
  scoreboard supplies selection checks; the frame test allows complete drops.
- Byte-oriented parser scoreboards accumulate accepted wire bytes. Seeded inputs
  include valid and malformed headers, truncation, missing byte lanes, arbitrary
  invalid beats, long payloads and resets. Metadata must remain unchanged on
  rejection. Coverage counters require both accepted and rejected headers for
  each parser. The existing directed SV bench retains exhaustive checks for
  individual missing header-byte positions.
- The loopback test checks combinational payload/last preservation and valid
  masking during asserted reset.

The ingress suite drives the real nexus_mold_ingress top with independent
MoldUDP64 and ITCH byte encoders. It checks the exact four-word little-endian
record, all 80 session bits, per-message sequence, packet-last metadata, output
stability under backpressure, input valid gaps, whole-packet duplicate drops,
sticky gap recovery, rearm refusal while input or output is busy, packet-wide
rejection of a malformed later A, truncation, invalid keep, upstream bad-frame
markers on early and final beats, sequence overflow,
partial overlaps, bounded-store overflow, a skipped final block,
heartbeat/end-session handling, and reset of held output. The integrated overflow
error is fault code 2: packet validation rejects it before the gate's standalone
sequence-end case. Decoder rejects are forbidden for packets accepted by this
validated path.

Each test has a simulation-time deadline. The ingress driver also bounds every
handshake/drain loop, allowing generous latency for serial replay from RAM.
The launcher stores build/simulation logs and per-seed JUnit XML under the
selected build directory. It requires all named tests without skips and rejects
stale or incomplete results. An external CI job should additionally impose a
wall-clock timeout on the whole launcher, including simulator compilation.
Direct Python dependencies are pinned; record resolved transitive packages and
simulator versions with any published result.

## Formal safety and reachability

Six configurations cover the existing arbiter, Ethernet metadata, MoldUDP64
header extractor, deframed SBE extractor, combinational loopback and standalone
Mold session gate. Each has separate prove and cover tasks. These harnesses do
not cover the packet buffer, normalizer or complete integrated Mold ingress.

Install a coherent SymbiYosys/Yosys/SMTBMC/Z3 toolchain on Linux first. The
existing YoWASP synthesis command alone does not provide this formal stack.
The runner never installs or downloads dependencies.

Prepared commands, **not executed**:

~~~bash
python3 tests/rtl/formal/run.py --build-dir build-rtl-formal
python3 tests/rtl/formal/run.py --case arbiter --mode prove \
  --build-dir build-rtl-formal-arbiter
python3 tests/rtl/formal/run.py --case network --mode cover \
  --build-dir build-rtl-formal-network
python3 tests/rtl/formal/run.py --case session_gate \
  --build-dir build-rtl-formal-session-gate
~~~

SBY may name an alternative executable. Yosys, yosys-smtbmc and Z3 must be on PATH.
The default wall-clock timeout is 120 seconds per task; use --timeout to adjust
it. A timeout kills the task's process group and is a failure. Logs, solver
status and a JSON result list are kept under the requested build root.

The sequential harnesses assume reset at the first sampled edge only. There is
no assumption that packets are valid, that either input makes progress, or that
an output matches the model. Valid gaps and last while invalid are unrestricted.
After reset, the first accepted beat is interpreted as a new frame, matching the
DUT interface contract. This does not establish correct external reset framing.

The arbiter reference tracks per-frame dispositions rather than reading the
DUT's owner register. Parser references assemble wire bytes rather than copying
the DUT's field-register pipeline. Equality of published metadata on every
checked cycle also checks rejection preservation. Covers require accepted input,
malformed-input recovery, valid gaps, long-frame draining and reset recovery.
They expose overconstrained or unreachable scenarios; run every cover task
alongside the safety proofs.

The session-gate harness checks checkpoint installation, data-only sequence
advancement, duplicate suppression without advancement, fault classification,
and preservation of the first fault until a permitted checkpoint or reset.
Its covers require admission, a later duplicate, heartbeat, gap/overlap/overflow,
end-session, blocked traffic during recovery, and explicit rearm. The quiescent
input is an unconstrained administrative indication; proving that the enclosing
pipeline asserts it only after draining remains outside this standalone harness.

The configured proof depth is 32. In prove mode this bounds search and induction
effort; success must include a completed proof. A bounded run, induction failure,
unknown result or timeout is not a proof. Cover establishes reachability only.
The runner requires a successful SBY status and a zero process exit code.

These are sampled single-clock safety models. They do not prove asynchronous
reset release, CDC/RDC correctness, metastability safety, fairness, eventual
packet completion, timing closure, vendor-IP behaviour or board operation.
A permanent selected-feed stall and A-priority starvation remain permitted by
the current arbiter contract. Assertions may need strengthening for induction;
tool compatibility and proof convergence remain unvalidated until execution.

## References

- [cocotb runner API](https://docs.cocotb.org/en/v2.1.0/library_reference.html#python-test-runner)
- [cocotb simulator support](https://docs.cocotb.org/en/v2.1.0/simulator_support.html)
- [cocotbext-axi](https://github.com/alexforencich/cocotbext-axi)
- [SymbiYosys proof and cover examples](https://yosyshq.readthedocs.io/projects/sby/en/latest/quickstart.html)
