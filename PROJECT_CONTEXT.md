# Project context

## Purpose and working environment

A C++20 ITCH Add Order → SPSC → single-instrument order-book pipeline. Build, test and run inside Linux under WSL. Source targets are x86-64 and ARM64; this is not a complete exchange gateway or matching engine.

Write all source comments and diagnostics in clear English. Prefer concise explanations of invariants, ownership and performance trade-offs over restating the code. README.md documents build commands, public contracts and benchmark limits.

## Invariants

1. The hot path performs no allocation, system calls or locking. Prepare the ring, pool and price levels before processing starts.
2. Prefaulting and THP advice do not pin pages. The code does not guarantee that later page faults are impossible.
3. Exactly one producer and one consumer own the queue. Publish data and reclaim slots with release/acquire ordering; thread-owned indices use relaxed operations.
4. Separate thread state by at least 128 bytes. Normalised messages and order nodes have 32-byte size and alignment.
5. Access wire fields through memcpy and endian conversion. Never cast the input buffer to a packed wire-structure pointer.
6. Invalid input and pool exhaustion return an observable failure without changing state.
7. Queue callbacks do not throw, retain slot references or recursively operate on the same queue end.
8. A single thread owns each order book. The caller validates the instrument, enforces exchange order-ID uniqueness and maintains the ID-to-slot map.
9. Explicit batches publish once after the writer finishes. Preserve single-message publication for callers whose latency requirements preclude batching.

## Components

- `platform.h`: cache geometry, x86 PAUSE / ARM64 YIELD, endian primitives and overflow-checked prefaulted allocation. A GNU compiler barrier after memset preserves page initialisation.
- `spsc_queue.h`: unsigned monotonic counters with defined wraparound, masked slot addressing, cached remote indices, in-place production and explicit batch publication. `consume_all` always acquires a fresh tail snapshot, including after `pop`.
- `itch_parser.h`: a 36-byte wire layout with offset checks, a 32-byte `MdAddOrder`, checked `parse_itch_add_order(p, bytes, out)` and trusted `decode_add_order(p, out)`. The decoder reads eight bytes at timestamp offset 5 and shifts away the final two bytes; the generic 48-bit helper retains its six-byte extent.
- `order_book.h`: one allocation for the pool and 200000 `PriceLevel {head, tail}` records, a bounded price window, separate bid/ask FIFOs, a free list, and O(1) removal and execution by slot.
- `main.cpp`: a prefaulted materialised wire tape, CPU validation, thread-start failure handling, an explicit start gate, warm-up and 21 measurements. The workload covers 256 prices and both sides. Full book validation runs outside the timed interval. The optional third argument selects the publication batch size; default 1 preserves immediate publication. HFT_QUEUE_CAPACITY defaults to 65536 slots.
- `tests/test_main.cpp`: checks remain active in Release. CTest also validates the pipeline run and CLI rejection diagnostics.
- `CMakeLists.txt`: C++20, strict warnings, optional native tuning and LTO, and separate ASan+UBSan / TSan configurations covering every executable.

## API contracts

```cpp
hft::SPSCQueue<hft::MdAddOrder, 1024> queue;
hft::MdAddOrder message{};
const bool parsed = hft::parse_itch_add_order(data, bytes, message);
// A false result leaves message unchanged.

hft::OrderBook book{500000, 1024};
const std::uint32_t slot = book.add_order(42, 500001, 100, true);
// Slot zero means rejection; exchange order ID zero is valid.
// Discard the slot after removal or full execution.
```

The maximum book capacity is `MAX_ORDERS - 1`; slot zero is reserved. Zero capacity is allowed. A valid price is at least base_price, with an offset below MAX_PRICE_LEVELS. Widen to unsigned 64 bits before subtracting: an out-of-range low price then underflows to a value outside the window. A 32-bit subtraction is incorrect for bases near UINT32_MAX.

Zero quantity marks a released slot. Liveness checks reject slot zero and indices never issued by the allocator before accessing pool storage. Reuse released slots before advancing the bump pointer to bound the working set under add/remove churn.

Queue elements must be trivially default-constructible, copyable, copy-assignable and destructible. Alignment and allocation-size overflow are checked at compile time. Queue capacity is a power of two and at least two.

`try_produce_batch(max_count, writer)` returns the number produced, which may be smaller than requested. The noexcept writer is invoked as an lvalue with `(T& slot, std::size_t offset)`, where offsets run from zero to the returned count minus one. No callback runs for an empty request or a full queue. Filled slots become visible together when the operation publishes its final tail; callers must account for that visibility delay.

## Validation policy

At minimum, run GCC Release, CTest and Debug ASan+UBSan. Synchronisation changes also require TSan where the environment supports its runtime. A runtime failure before main is not a passing test. Check portability with Clang and `HFT_NATIVE=OFF`.

Keep throughput comparisons on the same workload, flags, affinity and host load. The benchmark exercises synthetic additions across 256 prices using a materialised wire tape. Do not restore an encode/decode pair with known inputs inside the timed loop: the compiler can eliminate the decoding. p50/p99/p99.9 latency requires a separate measurement on target hardware; throughput improvements do not establish latency improvements.
