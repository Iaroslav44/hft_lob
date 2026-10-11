# Concurrency audit — 11 October 2026

## Scope and result

The reviewed paths are the ITCH decoder, SPSC queue, Disruptor ring, single-owner
order book, thread start gate, prefaulting and CXL bridge. The initial 20 CTest
cases all passed, including the existing sanitizer executables. They did not
exercise sequence-counter overflow or invalid CXL addresses.

Two production headers changed: `src/nexus/core/disruptor.h` and
`src/nexus/hw/cxl_shared_memory.h`. SPSC, parser, book and pipeline code are
unchanged. The accompanying changes extend tests, their launch configuration and
documentation. No locks, sequentially consistent operations or hot-path
allocations were introduced.

This is a source-level invariant and happens-before analysis supported by targeted
regressions. It is not an exhaustive model-checker proof or hardware certification.

## Confirmed defects and remediation

| Defect | Evidence before correction | Correction |
|---|---|---|
| Disruptor signed overflow | UBSan reported `9223372036854775807 + 1` in `next()` and exited 1 | Perform increment and distance subtraction in `uint64_t`; preserve signed API and atomic field types with `bit_cast` |
| Signed availability comparison across rollover | A cursor at `INT64_MAX` compares greater than the next, unpublished `INT64_MIN` sequence | `is_published(next_unconsumed)` uses bounded modular distance; stress consumer now uses it |
| CXL atomic object lifetime | An array of live `uint64_t` was cast to `atomic<uint64_t>*`; no atomic object exists there | Use a lock-free `atomic_ref<uint64_t>` on the existing object |
| CXL unchecked offset/alignment | ASan reported an 8-byte stack-buffer-overflow at `offset == size`; UBSan rejected offset 1 | Check declared extent and required atomic alignment before access, without overflowing `offset + 8` |
| CXL ISA assumption | GCC rejected `_mm_clwb` with `-mno-clwb`; unsupported CPUs would execute an unavailable instruction | Check CPUID at setup and use guarded inline CLWB, supporting both assembler dialects |
| Stress startup and affinity handling | Second thread creation could leave a joinable/waiting first thread; pinning errors were ignored | Abortable start gate, RAII threads, checked CPUs from the allowed mask; injected second-create failure now exits 1 |

CXL construction produces an inactive bridge for null/misaligned/short regions or
unsupported CPUs. `valid()` exposes this state. `write_data` and `flush_to_device`
now return `[[nodiscard]] bool`; failure touches no memory. The caller must still
provide an accurately sized, accessible region containing live `uint64_t` objects.
The bridge neither constructs these objects nor establishes a mapping.

## SPSC proof obligations

Let `P` and `C` be logical, unbounded produced/consumed counts, with capacity `K`
and unsigned counter modulus `M`. The stored counters are their residues modulo
`M`. Capacity and storage constraints ensure `0 < K < M`.

The producer's cached head `H` satisfies `H <= C <= P` and `0 <= P - H <= K`.
Each single write requires space. A batch chooses `n <= K - (P - H)`. Thus
publication preserves the bound. An acquire refresh cannot move the observed
head backwards in atomic modification order; the consumer cannot advance beyond
previously published data. The bounded difference is represented exactly by
unsigned subtraction, including at `SIZE_MAX -> 0`.

The consumer uses an acquired tail snapshot `T` and reads only `[C, T)`.
`consume_all` refreshes that snapshot even when interleaved with `pop`. Cached
snapshots remain useful because acquiring a publication orders all writes before
that publication, not just the first slot consumed afterwards.

For each reused physical slot the ordering chain is:

```
producer writes payload
  -> release tail -> acquire tail
  -> consumer finishes all payload accesses
  -> release head -> acquire head
  -> producer overwrites the reclaimed slot
```

Here arrows through matching atomics establish synchronization; the other arrows
are sequencing within the owning thread. This follows the C++
[release/acquire rule](https://eel.is/c++draft/atomics.order#2), so it does not
depend on x86 TSO. A batch release covers every completed writer callback.
ARM `YIELD` is only a spin hint; it supplies none of these ordering edges.

A paused endpoint prevents its peer from advancing more than `K` outstanding
slots. The peer therefore cannot complete a whole counter lap behind a stale
snapshot. Repeated counter bit patterns cannot conceal a full-modulus ABA under
this ownership contract. False full/empty results from stale observations affect
progress, not slot safety. Callbacks must not retain references or re-enter the
same endpoint, and destruction must follow both endpoints' completion.

## Disruptor proof obligations

Exactly one producer and one consumer own this ring. There are no CAS loops in
the audited queues. A load followed by publication is not an MPSC reservation;
adding producers would require a different algorithm and per-slot publication
protocol. The current caller graph has one producer, so no MPSC conversion was
made.

Let `P` be the last published sequence and `C` the last reclaimed sequence on a
logical unbounded sequence line. Initially both are -1. The invariant is
`0 <= P - C <= K`, with `K < 2^63`. There is at most one outstanding producer
claim. Its candidate is `N = P + 1`. Let `C_observed` be the producer's acquired
reclamation position. Coherence prevents this observation from preceding the one
that permitted its previous claim, so `1 <= N - C_observed <= K + 1`. Publishing
requires `N - C_observed <= K`. Unsigned 64-bit subtraction computes this bounded
distance exactly through both signed and unsigned rollover. A stale observation
can delay reservation but cannot permit premature reuse.

The consumer asks only about `S = C + 1`. Its coherent acquired cursor observation
`Q` is between `C` and `C + K`: it cannot precede the publication already used to
consume `C`. If `Q >= S`, then `0 <= Q - S < K` and reading the slot is safe.
If `Q == C`, the modular difference is outside that range. This is the predicate
in `is_published`. It can return false for an already published slot when `Q` is
stale. It is not an arbitrary history lookup for reclaimed sequences.

Release publication/acquire availability orders payload initialization before
reading. Release reclamation/acquire reservation orders completed reads before
overwrite. The ring has lock-free atomic fields, but `next()` waits on a full
ring and does not guarantee completion if the consumer stops.

The regression seeds both existing public cursor setters to an equal, quiescent
state near rollover. It tests real production operations, capacities 1 and 4,
withheld publication, both boundaries, and concurrent reclamation while the
consumer deliberately retains a slot across a scheduling opportunity. No test
hooks or reduced-width copies of the production algorithm were introduced.

## Book, startup and layout

The book partitions issued slots into live nodes (`qty > 0`) and free nodes
(`qty == 0`). Release unlinks a node from both FIFO neighbours and endpoints
before putting it on the free list. Allocation takes a free node or advances the
bounded pool index, initializes its fields, then links it into one level.
Partial execution preserves identity and position; complete execution releases it.

The sole pipeline consumer owns all book mutations. Verification follows its
join. A released index is invalid as an order handle; after reuse it denotes a
new order. No asynchronous adapter retains these indices in the running pipeline.
Generation tags would change the contract without repairing a demonstrated race.
The strengthened churn test checks every level against an independent FIFO model,
including IDs, quantities, links and liveness of all issued slots.

Worker pinning results precede release increments of the start gate's ready
counter. The master's acquire observation of both increments covers both workers
through the atomic RMW release sequence. Run/abort is a separate release/acquire
handshake. On thread-start failure, abort is published before RAII joins execute.

Existing alignments and fields remain unchanged. SPSC endpoint state is separated
by at least 128 bytes; orders and normalized messages remain 32-byte objects.
Disruptor retains its existing cache-line alignment: the checked `uint64_t, 4`
instantiation remains size 192/alignment 64. CXL bridge remains size 16/alignment 8
on the tested x86-64 ABI. Shared producer/consumer access to a publication counter
is required coherence traffic, not removable false sharing. Adjacent-line
prefetch behaviour was not measured and does not justify changing these layouts.

## CXL ordering and hardware limits

`atomic_ref` requires live, suitably aligned objects and exclusive use of atomic
references for that word while any reference exists. Concurrent host accesses
must use atomic operations. These are explicit mapping/ownership preconditions,
not properties that a pointer cast can establish.
See the C++ [lifetime rules](https://eel.is/c++draft/atomics.ref.generic.general)
and [alignment requirement](https://eel.is/c++draft/atomics.ref.ops).

The emitted write is `MOV -> CLWB -> SFENCE` on tested x86-64 compilers. CPUID is
outside the write path; there is no out-of-line flush call. The compiler memory
clobber preserves ordering around CLWB. The instruction ordering is described in
the [Intel SDM, CLWB section 3-172](https://cdrdv2-public.intel.com/774492/325383-sdm-vol-2abcd.pdf).

This sequence does not acknowledge device processing. A CPU acquire observing
the release store can succeed before the subsequent CLWB/SFENCE completes.
The caller must supply a device-specific ownership and notification protocol;
the bridge supports coherent write-back memory, not MMIO doorbells. The tests
use ordinary host RAM and make no CXL fabric, persistence or PCIe completion
claim. On non-x86 targets the bridge is explicitly inactive.

Prefaulting and THP advice do not lock resident pages. Automatic `mlockall` was
not introduced: it changes process-wide resource and thread-allocation behaviour,
and no page-reclamation latency was measured here. Residency policy requires
deployment-specific limits and validation. Sub-microsecond tail latency has not
been established by the throughput benchmark or sanitizer tests.

## Dynamic evidence and reproducible commands

Environment: Ubuntu WSL2 x86-64, Linux 6.18.40.1, GCC 11.4 and Clang 14.

| Configuration | Result |
|---|---|
| Original working tree, GCC RelWithDebInfo | 20/20, before boundary regressions |
| Corrected GCC RelWithDebInfo, LTO off | 28/28 |
| Corrected GCC Release, native, LTO | 28/28 |
| Corrected Clang Release, native tuning off, LTO off | 28/28 |
| Corrected GCC Debug, ASan+UBSan targets | 4/4 |

The 28-case suites include separate ASan+UBSan, UBSan and TSan executables for the
unit, stress, Disruptor and CXL tests. CLWB was available and the host-memory
cases executed on this machine. Checks remain active in Release builds.

Clang's first run failed in two ASan executables before `main`. GDB reproduced
the stack `internal_mmap -> ReservedAddressRange::Init -> Allocator::Init ->
AsanInitInternal -> _dl_init`. Extending the existing process-local TSan
`setarch -R` wrapper to ASan resolved this runtime startup failure. CTest retains
nonzero exit statuses and does not retry or suppress sanitizer diagnostics.
No system-wide ASLR setting was changed.

The standalone stress test passed with the normal two-CPU mask and with one
allowed CPU (11.801 seconds, unchanged iteration counts). A temporary
`pthread_create` interposer returning `EAGAIN` on the second call caused a clean
exit 1 within five seconds, without a stranded worker.

The before/after GCC RelWithDebInfo pipeline `.text` sections were identical:
SHA-256 `7ebc2a7b2b397f624ae293b18c63d584bef6bb5cc7c3a0421eecd01154f2722b`.
This establishes that this audit did not alter that build's pipeline instructions;
it is not a latency measurement of the separate Disruptor or CXL operations.

```bash
cmake -S . -B build-audit-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-audit-release -j4
ctest --test-dir build-audit-release --output-on-failure

cmake -S . -B build-audit-clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Release -DHFT_NATIVE=OFF -DHFT_LTO=OFF
cmake --build build-audit-clang -j4
ctest --test-dir build-audit-clang --output-on-failure

cmake -S . -B build-audit-debug -DCMAKE_BUILD_TYPE=Debug -DHFT_LTO=OFF
cmake --build build-audit-debug --target hft_tests_asan stress_test_asan \
  hft_disruptor_tests_asan hft_cxl_tests_asan -j4
ctest --test-dir build-audit-debug -R _asan --output-on-failure
```

## Remaining scope

Execution on ARM64 and a real CXL device was not available. No system tuning
script was executed. The supplied handover's claims about book prefetches,
explicit padding, ARM YIELD as a memory barrier and SFENCE ordering every future
memory transaction do not describe the inspected code or the required contracts.

Current CMake does not build the FPGA/HLS, network adapters, quant helpers,
`FlatOrderMap`, `XdmaBridge`, NUMA/MSR/PCIe utilities or TSC helper into the tested
pipeline. These are not covered by the passing CTest results. In particular,
the standalone flat map's sentinel/tombstone probing and the copyable XDMA file
descriptor owner require separate review before integration. They are outside
the synchronization paths changed here.
