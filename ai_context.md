# SYSTEM_HANDOVER_SPEC: HFT_PIPELINE_V2
SCHEMA_VERSION: 2.4.0
TARGET_AGENT: FORMAL_VERIFICATION_LLM
MODE: STRICT_MACHINE_READABLE
COMPRESSION: MAX_TOKEN_DENSITY
PROSE_LEVEL: ZERO

## 1. HARDWARE_SYNCHRONIZATION_COORDINATES
| COMPONENT | FILE_PATH | LINE | PRIMITIVE | ORDERING | RATIONALE |
|---|---|---|---|---|---|
| SPSCQueue | src/nexus/core/spsc_queue.h | 32 | static_assert | N/A | Lock-free guarantee check (atomic<size_t>) |
| SPSCQueue | src/nexus/core/spsc_queue.h | 56 | atomic::load | relaxed | Producer reads local tail counter |
| SPSCQueue | src/nexus/core/spsc_queue.h | 60 | atomic::load | acquire | Producer samples consumer head on ring saturation |
| SPSCQueue | src/nexus/core/spsc_queue.h | 65 | atomic::store | release | Producer publishes single item commit |
| SPSCQueue | src/nexus/core/spsc_queue.h | 81 | atomic::load | relaxed | Producer batch head evaluation |
| SPSCQueue | src/nexus/core/spsc_queue.h | 84 | atomic::load | acquire | Producer batch sync with consumer head |
| SPSCQueue | src/nexus/core/spsc_queue.h | 92 | atomic::store | release | Producer batch multi-slot publication barrier |
| SPSCQueue | src/nexus/core/spsc_queue.h | 97 | atomic::load | relaxed | Consumer reads local head counter |
| SPSCQueue | src/nexus/core/spsc_queue.h | 100 | atomic::load | acquire | Consumer synchronizes with producer tail on starvation |
| SPSCQueue | src/nexus/core/spsc_queue.h | 105 | atomic::store | release | Consumer publishes slot reclamation to producer |
| SPSCQueue | src/nexus/core/spsc_queue.h | 115 | atomic::load | relaxed | Consumer batch load local head |
| SPSCQueue | src/nexus/core/spsc_queue.h | 116 | atomic::load | acquire | Consumer batch sync with producer tail |
| SPSCQueue | src/nexus/core/spsc_queue.h | 122 | atomic::store | release | Consumer batch reclamation barrier |
| SPSCQueue | src/nexus/core/spsc_queue.h | 128 | alignas(128) | N/A | False sharing elimination (Producer line: tail_, head_cache_) |
| SPSCQueue | src/nexus/core/spsc_queue.h | 133 | alignas(128) | N/A | False sharing elimination (Consumer line: head_, tail_cache_) |
| Platform | src/nexus/core/platform.h | 39 | _mm_pause | intrinsic | SMT spin-loop relax, hyperthread yield, pipeline de-pipeline |
| Platform | src/nexus/core/platform.h | 41 | yield | asm volatile | ARM64 hardware pipeline yield barrier |
| Platform | src/nexus/core/platform.h | 43 | atomic_signal_fence | acq_rel | Fallback compiler reordering barrier |
| Platform | src/nexus/core/platform.h | 114 | asm volatile memory | asm barrier | Compiler barrier ensuring prefault writes survive LTO |
| Disruptor | src/nexus/core/disruptor.h | 14 | alignas(64) | N/A | Producer sequence line isolation |
| Disruptor | src/nexus/core/disruptor.h | 15 | alignas(64) | N/A | Consumer sequence line isolation |
| Disruptor | src/nexus/core/disruptor.h | 21 | atomic::load | relaxed | Producer cursor query |
| Disruptor | src/nexus/core/disruptor.h | 24 | atomic::load | acquire | Producer throttle spin-check against consumer sequence |
| Disruptor | src/nexus/core/disruptor.h | 31 | atomic::store | release | Producer cursor update publication |
| Disruptor | src/nexus/core/disruptor.h | 43 | atomic::load | acquire | Consumer cursor query synchronization |
| Disruptor | src/nexus/core/disruptor.h | 47 | atomic::store | release | Consumer sequence advancement publication |
| CXL Bridge | src/nexus/hw/cxl_shared_memory.h | 21 | _mm_clwb | intrinsic | Write-back dirty cache line to memory controller without invalidation |
| CXL Bridge | src/nexus/hw/cxl_shared_memory.h | 22 | _mm_sfence | intrinsic | Store barrier serializing all clwb/stores prior to next memory transaction |
| CXL Bridge | src/nexus/hw/cxl_shared_memory.h | 28 | atomic::store | release | Atomic write to CXL shared region |
| StartGate | src/main.cpp | 66 | alignas(128) | N/A | NUMA/HT False sharing isolation |
| StartGate | src/main.cpp | 71 | atomic::fetch_add | release | Barrier worker rendezvous registration |
| StartGate | src/main.cpp | 73 | atomic::load | acquire | Spin-loop synchronization barrier |
| StartGate | src/main.cpp | 217 | atomic::store | release | Abort state propagation |
| StartGate | src/main.cpp | 222 | atomic::load | acquire | Master thread worker barrier wait |
| StartGate | src/main.cpp | 230 | atomic::store | release | Master thread GO signal broadcast |
| OrderBook | src/nexus/md/order_book.h | 82 | _mm_prefetch | intrinsic | L1d prefetch of prev_order_idx node before pointer unlinking |
| OrderBook | src/nexus/md/order_book.h | 84 | _mm_prefetch | intrinsic | L1d prefetch of next_order_idx node before pointer unlinking |
| TscClock | src/nexus/core/tsc_clock.h | 17 | __rdtscp | intrinsic | Serializing instruction boundary + TSC read |

## 2. MEMORY_LAYOUT_SPECIFICATIONS
```
STRUCT: hft::Order (Total: 32 bytes, Alignment: 32 bytes)
[0x00 - 0x07] : uint64_t id                  (8 bytes)
[0x08 - 0x0B] : uint32_t price               (4 bytes)
[0x0C - 0x0F] : uint32_t qty                 (4 bytes)
[0x10 - 0x13] : uint32_t prev_order_idx      (4 bytes)
[0x14 - 0x17] : uint32_t next_order_idx      (4 bytes)
[0x18 - 0x18] : bool     is_buy              (1 byte)
[0x19 - 0x1F] : uint8_t  _pad[7]             (7 bytes explicit alignment padding)
CACHE_DENSITY: Exactly 2 Order structs fit in 1 standard 64-byte L1d cache line.
Zero cache-line split across all indexed reads.

STRUCT: hft::MdAddOrder (Total: 32 bytes, Alignment: 32 bytes)
[0x00 - 0x07] : uint64_t order_id            (8 bytes)
[0x08 - 0x0F] : uint64_t ts_ns               (8 bytes)
[0x10 - 0x13] : uint32_t price               (4 bytes)
[0x14 - 0x17] : uint32_t qty                 (4 bytes)
[0x18 - 0x19] : uint16_t locate              (2 bytes)
[0x1A - 0x1A] : uint8_t  is_buy              (1 byte)
[0x1B - 0x1F] : uint8_t  _pad[5]             (5 bytes explicit alignment padding)
CACHE_DENSITY: Exactly 2 messages per 64-byte cache line.

STRUCT: hft::PriceLevel (Total: 8 bytes, Alignment: 4 bytes)
[0x00 - 0x03] : uint32_t head                (4 bytes)
[0x04 - 0x07] : uint32_t tail                (4 bytes)
ARRAY_MAPPING: levels_ allocated contiguously for 2 * MAX_PRICE_LEVELS (Bid followed by Ask).

STRUCT: hft::SPSCQueue Memory Layout
PRODUCER_CACHE_LINE (Offset 0x00, alignas(128)):
  - tail_           : std::atomic<size_t> (8 bytes)
  - head_cache_     : size_t              (8 bytes)
  - buffer_prod_    : T*                  (8 bytes)
  - _pad0           : 104 bytes reserved to prevent adjacent prefetch intrusion
CONSUMER_CACHE_LINE (Offset 0x80, alignas(128)):
  - head_           : std::atomic<size_t> (8 bytes)
  - tail_cache_     : size_t              (8 bytes)
  - buffer_cons_    : T*                  (8 bytes)
  - _pad1           : 104 bytes reserved
SLOT_BUFFER:
  - T buffer_[Capacity] pre-faulted via alloc_prefaulted() aligned to 2 MiB boundary (kHugePage).
```

## 3. FORMAL_VERIFICATION_ATTACK_SURFACE (SUSPICION_LIST)
```yaml
ATTACK_VECTORS:
  VEC_01_RING_MODULO_WRAPAROUND:
    target: "hft::SPSCQueue::try_emplace / try_produce_batch"
    vector: "Unsigned 64-bit integer rollover of tail_ and head_"
    invariant: "(t - head_cache_ <= Capacity) strictly maintained across 0xFFFFFFFFFFFFFFFF -> 0x0"
    formal_proof_need: "Prove modular distance arithmetic invariants under 2^64 unsigned boundary overflow."

  VEC_02_BURST_BATCH_STORE_HOISTING:
    target: "hft::SPSCQueue::try_produce_batch"
    vector: "Loop write buffer[(t + i) & kMask] followed by tail_.store(release)"
    vulnerability_hypothesis: "Can speculative execution or non-TSO architectures observe slot writes out of order before tail_ visibility?"
    mitigation_audit: "x86-64 TSO prevents store-store reordering; verify on non-x86 architectures if memory_order_release suffices for slot arrays."

  VEC_03_DISRUPTOR_MULTI_PRODUCER_COLLISION:
    target: "nexus::core::DisruptorRingBuffer::next()"
    vector: "Non-atomic fetch-and-add: next_seq = cursor_.load(relaxed) + 1"
    vulnerability: "FATAL DATA RACE if invoked by >1 concurrent producer thread."
    classification: "DESIGN_CONSTRAINT_VIOLATION"
    proof_requirement: "Enforce Single-Producer restriction via static analysis or convert to CAS loop (compare_exchange_weak)."

  VEC_04_ORDERBOOK_FREE_LIST_RECYCLING_LIFETIME:
    target: "hft::OrderBook::release_order() and add_order()"
    vector: "LIFO free_head_ pointer chaining: order.next_order_idx = free_head_; free_head_ = idx;"
    vulnerability_hypothesis: "Dangling index retention in feed adapter layer if external cancel caller attempts double-delete on reused slot."
    mitigation_audit: "is_live(idx) check guards qty != 0; confirm generation tags if distributed state is enabled."

  VEC_05_CXL_CLWB_ORDERING_HAZARD:
    target: "nexus::hw::CxlMemoryBridge::flush_to_device()"
    vector: "_mm_clwb() followed by _mm_sfence()"
    vulnerability_hypothesis: "CXL device read occurs between atomic store and sfence draining store buffers."
    proof_requirement: "Verify memory controller write acknowledgment protocol guarantees under PCI-Express 5.0 / CXL 2.0 HDM interface."

  VEC_06_PREFAULT_PAGE_EVICTION_LATENCY_SPIKE:
    target: "hft::alloc_prefaulted"
    vector: "madvise(MADV_HUGEPAGE) without mlock(MCL_CURRENT | MCL_FUTURE)"
    vulnerability: "Kernel page reclaim or kcompactd migration introduces microsecond latency tail on hot path."
    remediation_recommendation: "Inject mlockall(MCL_CURRENT | MCL_FUTURE) into initialization sequence."
```
