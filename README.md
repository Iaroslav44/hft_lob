# HFT Market Data Pipeline

A C++20 market data hot path: decode NASDAQ ITCH 5.0 Add Order messages, transfer them between threads through a bounded SPSC ring, and apply them to a single-instrument order book. On Windows, build and run the project inside WSL Linux.

## Repository layout

| File | Responsibility |
|---|---|
| `src/nexus/core/platform.h` | Spin hints, endian-safe loads and stores, aligned allocation and page prefaulting |
| `src/nexus/core/spsc_queue.h` | Bounded SPSC queue with in-place production, explicit batch publication and bulk consumption |
| `src/nexus/core/disruptor.h` | SPSC sequence ring with blocking reservation and modular availability checks |
| `src/nexus/md/itch_parser.h` | ITCH type A wire layout and 32-byte normalised messages |
| `src/nexus/md/order_book.h` | Order pool, per-price FIFO queues, removal and partial execution by slot |
| `src/nexus/hw/cxl_shared_memory.h` | Checked atomic host-memory writes and CPU cache write-back; device protocol supplied by the caller |
| `src/main.cpp` | Two-thread synthetic throughput benchmark with complete result validation |
| `tests/test_main.cpp` | Boundary, concurrency and regression checks |
| `tests/stress_test.cpp` | Concurrent slot reuse, checked thread startup and model-based FIFO churn |
| `tests/test_disruptor.cpp`, `tests/test_cxl.cpp` | Sequence rollover, publication, reclamation and shared-memory boundary regressions |
| `tests/check_cli.cmake` | Exact exit-status and diagnostic checks for rejected CLI arguments |
| `benchmarks/compare.py` | Sequential, shuffled throughput comparisons with raw results and binary hashes |

Source comments and diagnostics are written in English. Comments explain ownership, invariants and microarchitectural trade-offs where the code alone is insufficient.

## Build and run

Requires CMake 3.20 or later, GCC or Clang with C++20 support, and POSIX threads.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/hft_pipeline
./build/hft_pipeline 2 4
./build/hft_pipeline 2 4 32
```

The first two arguments select the producer and consumer CPUs. Both default to -1, which disables pinning. Select available CPUs explicitly for reproducible measurements; the process CPU mask is listed under `Cpus_allowed_list` in `/proc/self/status`. Invalid CPU arguments, CPUs outside the process mask and affinity failures produce a non-zero exit status.

The optional third argument is the maximum publication batch size. It defaults to 1, preserving immediate single-message publication. Larger values use the explicit batch API with an already available receive burst; no timeout or accumulation wait is introduced. The accepted range is 1 through the queue capacity. The consumer cannot observe a batch until all its slots have been written.

`HFT_QUEUE_CAPACITY` configures the benchmark ring at build time. The default is 65536 slots, or 2 MiB of message storage, instead of the previous 1048576 slots / 32 MiB. It must be a power of two and at least two; for example, configure a separate build with `-DHFT_QUEUE_CAPACITY=16384`. Queue capacity and publication batch size are independent settings.

`HFT_NATIVE=ON` enables `-march=native`; the binary requires the instruction-set features of the build host. Use `-DHFT_NATIVE=OFF` for a portable build. LTO is enabled when the compiler and linker support it; disable it with `-DHFT_LTO=OFF`.

## Sanitizers and validation

Every test group has separate `_asan` (ASan + UBSan), `_ubsan` and `_tsan`
executables in the same build. Their runtimes are never linked together. The
throughput benchmark remains uninstrumented. For a Debug sanitizer run:

```bash
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DHFT_LTO=OFF
cmake --build build-san -j
ctest --test-dir build-san --output-on-failure
```

The former `HFT_SANITIZE` and `HFT_TSAN` configuration options are no longer used.
Test checks remain active in Release builds. CTest timeouts detect stalled
concurrent tests.

When `setarch` is available, CTest runs ASan and TSan executables with process-local
ASLR disabled. This handles address-space collisions in older runtimes under
WSL, including a reproduced Clang 14 ASan startup fault. It does not change
system-wide settings or suppress failures. A sanitizer runtime failure before
the test starts is not a successful test run.

Current validation on 11 October 2026: GCC Release/native/LTO and Clang
Release/portable each passed 28/28 CTest cases; GCC Debug ASan+UBSan passed all
four instrumented test groups. Proof obligations, counterexamples, precise scope
and commands are recorded in [CONCURRENCY_AUDIT.md](CONCURRENCY_AUDIT.md).

Historical validation before the module reorganisation, recorded on 10 October
2026, Ubuntu WSL2, x86-64:

| Configuration | CTest result |
|---|---|
| GCC 11.4, Release, native, LTO | 13/13 |
| Clang 14, Release, HFT_NATIVE=OFF, LTO | 13/13 |
| GCC 11.4, Debug, ASan + UBSan | 13/13, no diagnostics |
| GCC 11.4, Debug, TSan via setarch | 13/13, no diagnostics |

Fault injection into the second `pthread_create` call with `EAGAIN` and CPU affinity with `EINVAL` also confirmed clean termination without a hang. Execution on ARM64 has not been validated.

## Hot-path contracts

- Queue storage, the order pool and price levels are allocated before processing starts. Queue and book operations allocate no memory, make no system calls and acquire no locks.
- `alloc_prefaulted` checks size overflow, aligns allocations to 2 MiB, requests transparent huge pages and writes to every page. A compiler barrier preserves prefaulting under LTO. THP remains an operating-system hint; memory is not locked with `mlock`. Avoiding later page faults requires separate OS configuration and validation on the target host.
- Producer and consumer state are separated by at least 128 bytes. Normalised messages and order nodes have 32-byte size and alignment.
- The queue has exactly one producer and one consumer. Thread-owned counters use relaxed operations; publication and slot reclamation use release/acquire ordering. Capacity is a power of two and every slot is usable.
- `push`, `pop`, `try_emplace` and `try_produce_batch` do not wait for the other thread. The caller chooses whether to retry. `consume_all` takes a fresh snapshot of the published tail and processes that snapshot with one acquire/release pair.
- `try_produce_batch(max_count, writer)` produces up to the requested number of messages, bounded by capacity and available space. It calls `writer(slot, offset)` with consecutive offsets starting at zero, then publishes the completed batch with one release store. It returns the number produced. A zero request or a full queue returns zero without calling the writer.
- Queue callbacks must be `noexcept`. They must not retain slot references or re-enter the same end of the queue. User callbacks do not acquire a bounded execution-time guarantee merely by running inside a queue operation.
- Explicit batches amortise publication traffic but delay visibility until the batch is complete. Batch size is a throughput/latency policy decision; single-message publication remains available.

## ITCH decoding

The host ITCH decoder described below is separate from the FPGA header extractors.

The message layout follows section 1.3.1 of the [Nasdaq TotalView-ITCH 5.0 specification](https://nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHSpecification.pdf): 36 bytes, big-endian fields, a 48-bit timestamp, and prices in units of 0.0001.

Accept external input through the checked parser:

```cpp
hft::MdAddOrder message{};
if (hft::parse_itch_add_order(data, available_bytes, message)) {
    // Pass the validated message to the next stage.
}
```

The parser validates the pointer, available length, message type A, side B/S and positive quantity. On failure, the output is unchanged. A larger buffer is accepted, but only the first message is read. The caller owns transport framing.

`decode_add_order` is the unchecked path for an already validated message with at least 36 accessible bytes. `ItchAddOrder` defines field offsets; input bytes are never accessed through a pointer to that packed structure. Loads use `memcpy` and support unaligned input. Timestamp decoding uses one 64-bit load at offset 5 and discards the low 16 bits; that load remains entirely within the 36-byte message. The standalone `load_be48` helper still reads exactly six bytes.

## Order-book scope

`OrderBook(base_price, capacity)` stores one instrument. Its price window is [base_price, base_price + 100000), capped by the representable `uint32_t` range. The default base is zero and default capacity is 999999 orders. The benchmark uses base 500000 and prices 500000 through 500255. Widening the price to unsigned 64 bits before subtracting the base checks both ends of the window with one comparison, including bases near UINT32_MAX.

`add_order` returns a pool slot. Zero indicates an invalid price, zero quantity or exhausted capacity; rejection leaves the book unchanged. An exchange order ID may be zero and is distinct from the pool slot.

`head(price, side)` and `tail(price, side)` expose each level's FIFO endpoints. `remove_order(slot)` removes an order; `execute_order(slot, qty)` reduces its quantity and releases the slot on full execution. Zero and excessive execution quantities are rejected. Released slots are reused, so callers must discard stale indices.

Instrument routing, order-ID uniqueness and the exchange-ID-to-slot map belong to the caller. The host pipeline does not include network transport, sequence-gap recovery, decoders for other ITCH message types, best-price discovery or a matching engine. Optional FPGA integration components do not change the host benchmark's input source. The book supplies order storage and direct price-level access, owned by one thread.

## FPGA integration and verification

The reusable `nexus_mold_ingress` component accepts validated UDP payloads,
quarantines and validates complete MoldUDP64 packets, enforces session/sequence
admission, and emits canonical 32-byte ITCH Add Order records. It supports
backpressure and requires an external MAC/UDP/DMA shell. A single packet buffer
and serialized replay bound storage but do not provide a line-rate guarantee.

The `nexus_tcp_ip_stack` HLS top connects Ethernet frame admission, ARP, IPv4,
ICMP Echo, the ETH TCP engine and an internal 64-session table. It exposes real
TCP application and RX/TX memory interfaces; MAC/PHY and memory controllers remain
board services. Its fixed profile is 64 bits at 156.25 MHz, IPv4 MTU 1500, without
VLANs, IP options, fragmentation, IPv6 or UDP. The receive guard buffers a complete
frame before protocol admission, so no line-rate or latency guarantee is claimed.
The separate Mold ingress still requires a UDP provider.

Pinned ETH HLS TCP and Vitis Security AES-256-GCM sources replace the former
TCP/RTO/XOR demonstrations. Taxi supplies the optional stream CDC integration;
LitePCIe and alternative NIC/accelerator platforms have explicit source profiles.
The original `nexus_network_stack_top` is retained only as a loopback fixture.
See [FPGA_INTEGRATION.md](FPGA_INTEGRATION.md) for interfaces, dependency licenses,
source retrieval, reset/recovery contracts and enclosing-system requirements.

This integration revision was prepared without builds, runs, simulations, lint,
synthesis or formal execution, as requested. The results below are historical;
they do not validate the newly added components. The earlier audit is preserved
in [FPGA_AUDIT.md](FPGA_AUDIT.md).

Enable the portable verification suite explicitly:

```bash
cmake -S . -B build-fpga -DCMAKE_BUILD_TYPE=Release -DHFT_FPGA_TESTS=ON
cmake --build build-fpga -j4
ctest --test-dir build-fpga --output-on-failure
python3 tests/rtl/run_tests.py --build-dir build-fpga-synthesis --synth
```

Simulation requires Python 3, Icarus Verilog and vvp; optional generic synthesis
requires Yosys. The runner supports installed tools and the local `.tools/`
directory, logs source hashes, and fails if required tools or simulation success
markers are missing. On 11 October 2026, the integrated GCC Release/LTO suite
passed 30/30 tests; both RTL benches and generic synthesis of all eight modules
passed in the pre-integration revision. This does not establish Vivado timing
closure or board-level correctness. New optional cocotb regressions and formal
prove/cover harnesses are documented in
[docs/FPGA_VERIFICATION.md](docs/FPGA_VERIFICATION.md); they have not been run.

`HFT_FPGA_TOOLING=ON` exposes explicit `fpga_sources`, `fpga_tcp_ip`, `fpga_cocotb`
and `fpga_formal` targets. These targets do not run during the default build and do not
install drivers or enable device access. The integration recipes also work from
an enclosing Vivado/FuseSoC project without using the host benchmark's CMake flow.

## Benchmark interpretation

The synthetic workload uses one instrument and 256 price levels: 128 bid levels below 128 ask levels, with quantities varying by level. The complete 36-byte wire tape is encoded and prefaulted before timing. The producer reads that tape and decodes into queue storage; the consumer appends each order to its price-level FIFO. One warm-up precedes 21 measured runs of 999999 messages.

Materialising the input is deliberate: serialising and immediately decoding known values in one loop allowed the optimiser to remove the wire conversion entirely. The old benchmark therefore did not measure the decoder. Comparisons below use the same materialised-input harness for both the original and optimised implementations.

Timing starts after both threads are ready and ends when the consumer processes the final message. Memory preparation and result validation are outside the interval. Every run checks the order count, rejection count, all order fields and both directions of the FIFO links, making the book writes observable.

Output reports minimum, median and maximum throughput. It does not measure per-message latency, real network ingestion or general market behaviour. The tape is synthetic, remains resident between runs, and uses a regular price sequence. WSL results depend on the host workload, scheduler and CPU selection. Report latency percentiles only from a dedicated measurement on the target hardware.

## Hot-path measurements

Measured on 10 October 2026 in x86-64 Ubuntu WSL2 with GCC 11.4, `-O3 -march=native -flto -fno-plt`, producer CPU 2 and consumer CPU 4. Each entry is the median of seven process medians; each process performs one warm-up and 21 validated measured runs. Cases were interleaved in seeded random order, with no simultaneous benchmark jobs.

| Implementation and ring | Publication | Median Mmsg/s |
|---|---|---:|
| Original decoder/book, 1048576 slots / 32 MiB | Immediate | 71.292 |
| Original decoder/book, 65536 slots / 2 MiB | Immediate | 171.046 |
| Final implementation, 65536 slots / 2 MiB | Immediate, default | 187.543 |
| Final implementation, 65536 slots / 2 MiB | Batches up to 8 | 216.832 |
| Final implementation, 65536 slots / 2 MiB | Batches up to 32 | 220.237 |

The immediate-publish configuration improved by approximately 2.63x in this workload; explicit batches of up to 32 reached approximately 3.09x the original-ring baseline. The dominant measured change was the ring working set. A 16384-slot variant reached 189.801 Mmsg/s with immediate publication, close to the selected 65536-slot configuration; the default keeps four times the burst capacity. These results do not imply universal optimal capacity or improved tail latency.

The decoder's timestamp path now needs one load and shift instead of two loads, extension, shift and OR. On the inspected GCC build, the isolated decoder fell from 17 to 12 instructions. The book range check lost one conditional branch. These local changes have a much smaller effect than ring sizing; the data does not establish a precise standalone speedup for either change. An aggregate-output decoder was tested and rejected because it did not reliably outperform the simpler scalar implementation.

Batch publication amortises tail loads/stores and the associated cross-core cache-line traffic. On x86-64 these acquire/release operations compile to ordinary loads/stores, not locked instructions. Hardware performance counters were unavailable in this environment, so no cache-miss or stall counts are claimed. WSL variability was substantial, especially with immediate publication: inspect every sample in `benchmarks/throughput-results.json` rather than treating the median as a latency guarantee. `benchmarks/decoder-results.json` retains the isolated decoder experiment.

For future comparisons, build equivalent workloads into separate executables, then run:

```bash
python3 benchmarks/compare.py \
  --case immediate ./build/hft_pipeline 1 \
  --case burst32 ./build/hft_pipeline 32 \
  --producer-cpu 2 --consumer-cpu 4 --repeats 7 --seed 20261011 \
  --output /tmp/hft-comparison.json
```

Both executables must support the current three-argument benchmark CLI. The runner records binary hashes, available CMake build metadata, execution order and raw process output. It stops on a failed benchmark and retains the partial report. Do not compare the old encode/decode-cancelled workload with the materialised-input workload.
