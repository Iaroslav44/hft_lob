#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include "nexus/core/platform.h"
#include "nexus/core/spsc_queue.h"
#include "nexus/core/disruptor.h"
#include "nexus/md/order_book.h"
#include "nexus/hw/cxl_shared_memory.h"

#define ASSERT_TRUE(cond)                                                          \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "[STRESS_FAIL] %s:%d: Assertion failed: %s\n",    \
                         __FILE__, __LINE__, #cond);                               \
            std::abort();                                                          \
        }                                                                          \
    } while (false)

namespace {

using namespace hft;

int pin_thread_to_cpu(int cpu) noexcept {
#if defined(__linux__)
    if (cpu < 0) return 0;
    if (cpu >= CPU_SETSIZE) return EINVAL;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#else
    (void)cpu;
    return 0;
#endif
}

struct CpuPair {
    int producer{-1};
    int consumer{-1};
};

void wait_for_peer(const CpuPair& cpus) noexcept {
    if (cpus.producer >= 0 && cpus.producer == cpus.consumer)
        std::this_thread::yield();
    else
        cpu_relax();
}

CpuPair select_allowed_cpus() {
    CpuPair cpus;
#if defined(__linux__)
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
        throw std::system_error(errno, std::generic_category(), "sched_getaffinity");
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &allowed)) continue;
        if (cpus.producer < 0) cpus.producer = cpu;
        else {
            cpus.consumer = cpu;
            break;
        }
    }
    if (cpus.producer < 0) throw std::runtime_error("The process CPU mask is empty");
    if (cpus.consumer < 0) cpus.consumer = cpus.producer;
#endif
    return cpus;
}

template <class Producer, class Consumer>
void run_pair(const CpuPair& cpus, Producer&& produce, Consumer&& consume) {
    enum class State : unsigned char { waiting, run, abort };
    std::atomic<State> state{State::waiting};
    std::atomic<unsigned> ready{0};
    std::array<int, 2> pin_errors{};
    auto await_start = [&](std::size_t index, int cpu) noexcept {
        pin_errors[index] = pin_thread_to_cpu(cpu);
        ready.fetch_add(1, std::memory_order_release);
        State current;
        while ((current = state.load(std::memory_order_acquire)) == State::waiting)
            cpu_relax();
        return current == State::run;
    };

    std::jthread producer;
    std::jthread consumer;
    try {
        producer = std::jthread([&] { if (await_start(0, cpus.producer)) produce(); });
        consumer = std::jthread([&] { if (await_start(1, cpus.consumer)) consume(); });
    } catch (...) {
        // A worker already waiting at the gate must leave if the second launch fails.
        state.store(State::abort, std::memory_order_release);
        throw;
    }

    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    if (pin_errors[0] != 0 || pin_errors[1] != 0) {
        state.store(State::abort, std::memory_order_release);
        throw std::system_error(pin_errors[0] != 0 ? pin_errors[0] : pin_errors[1],
                                std::generic_category(), "pthread_setaffinity_np");
    }
    state.store(State::run, std::memory_order_release);
    producer.join();
    consumer.join();
}

// ============================================================================
// STRESS TEST 1: SPSC slot reuse under concurrent production and consumption
// ============================================================================
// CPU indices come from the process mask; no SMT topology is assumed.
struct alignas(32) MessagePayload {
    std::uint64_t sequence;
    std::uint64_t generation;
    std::uint64_t checksum;
    std::uint64_t padding;
};

void test_spsc_queue_contention(const CpuPair& cpus) {
    std::printf("[Stress 1] SPSC slot reuse (capacity 16)...\n");
    constexpr std::size_t kCapacity = 16;
    constexpr std::uint64_t kTotalMessages = 500'000;

    auto queue = std::make_unique<SPSCQueue<MessagePayload, kCapacity>>();
    std::uint64_t consumer_received = 0;

    run_pair(cpus, [&] {
        for (std::uint64_t i = 1; i <= kTotalMessages; ++i) {
            MessagePayload msg{i, i / kCapacity, i ^ 0x5a5a5a5a5a5a5a5aULL, 0};
            while (!queue->push(msg)) {
                wait_for_peer(cpus);
            }
        }
    }, [&] {
        std::uint64_t expected_seq = 1;
        while (expected_seq <= kTotalMessages) {
            MessagePayload msg{};
            if (queue->pop(msg)) {
                ASSERT_TRUE(msg.sequence == expected_seq);
                ASSERT_TRUE(msg.generation == expected_seq / kCapacity);
                ASSERT_TRUE(msg.checksum == (expected_seq ^ 0x5a5a5a5a5a5a5a5aULL));
                ++expected_seq;
            } else {
                wait_for_peer(cpus);
            }
        }
        consumer_received = expected_seq - 1;
    });

    ASSERT_TRUE(consumer_received == kTotalMessages);
    std::printf("[Stress 1] PASSED: %lu messages verified without tearing or loss.\n", kTotalMessages);
}

// ============================================================================
// STRESS TEST 2: SPSC batch publication and repeated physical slot reuse
// ============================================================================
// Rapid circular slot turnover exercising try_produce_batch and consume_all.
void test_spsc_queue_batch_churn(const CpuPair& cpus) {
    std::printf("[Stress 2] SPSC batch publication and slot reuse (capacity 32)...\n");
    constexpr std::size_t kCapacity = 32;
    constexpr std::uint64_t kTotalMessages = 1'000'000;

    auto queue = std::make_unique<SPSCQueue<MessagePayload, kCapacity>>();
    run_pair(cpus, [&] {
        std::uint64_t sent = 0;
        while (sent < kTotalMessages) {
            std::size_t batch_size = (sent % 7) + 1;
            batch_size = std::min<std::uint64_t>(batch_size, kTotalMessages - sent);

            std::size_t produced = queue->try_produce_batch(batch_size, [&](MessagePayload& slot, std::size_t offset) noexcept {
                std::uint64_t seq = sent + offset + 1;
                slot.sequence = seq;
                slot.generation = seq / kCapacity;
                slot.checksum = seq ^ ~0ULL;
                slot.padding = 0xAA;
            });
            sent += produced;
            if (produced == 0) {
                wait_for_peer(cpus);
            }
        }
    }, [&] {
        std::uint64_t consumed = 0;
        while (consumed < kTotalMessages) {
            std::size_t got = queue->consume_all([&](const MessagePayload& slot) noexcept {
                ++consumed;
                ASSERT_TRUE(slot.sequence == consumed);
                ASSERT_TRUE(slot.generation == consumed / kCapacity);
                ASSERT_TRUE(slot.checksum == (consumed ^ ~0ULL));
                ASSERT_TRUE(slot.padding == 0xAA);
            });
            if (got == 0) {
                wait_for_peer(cpus);
            }
        }
    });

    std::printf("[Stress 2] PASSED: %lu batch messages verified through %lu buffer cycles.\n",
                kTotalMessages, kTotalMessages / kCapacity);
}

// ============================================================================
// STRESS TEST 3: Disruptor single-producer saturation and slot reclamation
// ============================================================================
void test_disruptor_saturation(const CpuPair& cpus) {
    std::printf("[Stress 3] Disruptor single-producer saturation...\n");
    constexpr std::size_t kCapacity = 64;
    constexpr std::int64_t kIterations = 200'000;

    auto disruptor = std::make_unique<nexus::core::DisruptorRingBuffer<std::uint64_t, kCapacity>>();
    run_pair(cpus, [&] {
        for (std::int64_t i = 0; i < kIterations; ++i) {
            std::int64_t seq = disruptor->next();
            disruptor->get(seq) = static_cast<std::uint64_t>(seq ^ 0x123456789ABCDEF0ULL);
            disruptor->publish(seq);
        }
    }, [&] {
        for (std::int64_t expected_seq = 0; expected_seq < kIterations; ++expected_seq) {
            while (!disruptor->is_published(expected_seq)) {
                wait_for_peer(cpus);
            }
            std::uint64_t val = disruptor->get(expected_seq);
            ASSERT_TRUE(val == static_cast<std::uint64_t>(expected_seq ^ 0x123456789ABCDEF0ULL));
            disruptor->set_consumer_sequence(expected_seq);
        }
    });

    std::printf("[Stress 3] PASSED: %ld Disruptor events processed under tight cursor pressure.\n", kIterations);
}

// ============================================================================
// STRESS TEST 4: Order-book slot reclamation and FIFO maintenance
// ============================================================================
void test_orderbook_pool_reclamation_churn() {
    std::printf("[Stress 4] OrderBook Pool Index Reclamation & Level FIFO Churn...\n");
    constexpr std::uint32_t kBasePrice = 100'000;
    constexpr std::uint32_t kCapacity = 2048;
    auto book = std::make_unique<OrderBook>(kBasePrice, kCapacity);

    struct ExpectedOrder {
        std::uint64_t id;
        std::uint32_t price;
        std::uint32_t qty;
        bool is_buy;
    };
    std::array<ExpectedOrder, kCapacity + 1> expected{};
    std::array<std::vector<std::uint32_t>, 32> levels;
    auto level_index = [](std::uint32_t price, bool is_buy) {
        return 2 * (price - kBasePrice) + static_cast<std::uint32_t>(is_buy);
    };
    auto verify_book = [&] {
        std::size_t total = 0;
        for (std::size_t level = 0; level < levels.size(); ++level) {
            const auto price = kBasePrice + static_cast<std::uint32_t>(level / 2);
            const bool is_buy = (level & 1) != 0;
            const auto& indices = levels[level];
            ASSERT_TRUE(book->head(price, is_buy) == (indices.empty() ? 0 : indices.front()));
            ASSERT_TRUE(book->tail(price, is_buy) == (indices.empty() ? 0 : indices.back()));
            for (std::size_t i = 0; i < indices.size(); ++i) {
                const auto index = indices[i];
                const Order* const order = book->get_order(index);
                ASSERT_TRUE(order != nullptr);
                ASSERT_TRUE(order->id == expected[index].id);
                ASSERT_TRUE(order->price == price && order->is_buy == is_buy);
                ASSERT_TRUE(order->qty == expected[index].qty);
                ASSERT_TRUE(order->prev_order_idx == (i == 0 ? 0 : indices[i - 1]));
                ASSERT_TRUE(order->next_order_idx == (i + 1 == indices.size() ? 0 : indices[i + 1]));
            }
            total += indices.size();
        }
        ASSERT_TRUE(book->size() == total);
        for (std::uint32_t index = 1; index <= kCapacity; ++index)
            ASSERT_TRUE((book->get_order(index) != nullptr) == (expected[index].qty != 0));
    };

    // Keep an independent FIFO model while repeatedly reclaiming half the pool.
    std::vector<std::uint32_t> active_indices;
    active_indices.reserve(kCapacity);

    for (std::uint32_t i = 1; i <= 1024; ++i) {
        const auto price = kBasePrice + (i % 16);
        const bool is_buy = (i & 16) != 0;
        std::uint32_t idx = book->add_order(i, price, 100, is_buy);
        ASSERT_TRUE(idx != 0);
        ASSERT_TRUE(idx <= kCapacity && expected[idx].qty == 0);
        expected[idx] = ExpectedOrder{i, price, 100, is_buy};
        levels[level_index(price, is_buy)].push_back(idx);
        active_indices.push_back(idx);
    }
    ASSERT_TRUE(book->size() == 1024);
    verify_book();

    // Deterministic churn keeps failures reproducible.
    for (std::uint32_t cycle = 1; cycle <= 100'000; ++cycle) {
        std::size_t target_slot = cycle % active_indices.size();
        std::uint32_t old_idx = active_indices[target_slot];

        const Order* ord = book->get_order(old_idx);
        ASSERT_TRUE(ord != nullptr);
        ASSERT_TRUE(ord->qty == 100);

        auto& old_level = levels[level_index(expected[old_idx].price, expected[old_idx].is_buy)];
        const auto position = std::find(old_level.begin(), old_level.end(), old_idx);
        ASSERT_TRUE(position != old_level.end());
        old_level.erase(position);
        expected[old_idx].qty = 0;
        ASSERT_TRUE(book->remove_order(old_idx));
        ASSERT_TRUE(book->get_order(old_idx) == nullptr);
        ASSERT_TRUE(!book->remove_order(old_idx));
        ASSERT_TRUE(!book->execute_order(old_idx, 1));

        // A released index is no longer an order handle, even if it is reused here.
        std::uint64_t new_id = 10'000'000ULL + cycle;
        std::uint32_t new_price = kBasePrice + (cycle % 16);
        const bool is_buy = (cycle & 16) != 0;
        std::uint32_t new_idx = book->add_order(new_id, new_price, 100, is_buy);
        ASSERT_TRUE(new_idx != 0);
        ASSERT_TRUE(new_idx <= kCapacity && expected[new_idx].qty == 0);
        expected[new_idx] = ExpectedOrder{new_id, new_price, 100, is_buy};
        levels[level_index(new_price, is_buy)].push_back(new_idx);

        active_indices[target_slot] = new_idx;
        if ((cycle & 511) == 0) verify_book();
    }

    ASSERT_TRUE(book->size() == 1024);
    verify_book();
    std::printf("[Stress 4] PASSED: 100,000 recycled free-list operations verified.\n");
}

// ============================================================================
// STRESS TEST 5: CPU-visible shared-memory writes through the CXL bridge
// ============================================================================
void test_cxl_memory_bridge() {
    std::printf("[Stress 5] CPU-visible CXL bridge buffer smoke (no device attached)...\n");
    if (!nexus::hw::CxlMemoryBridge::is_supported()) {
        std::printf("[Stress 5] SKIPPED: this CPU does not support CLWB.\n");
        return;
    }
    alignas(64) std::uint64_t memory_buffer[1024]{};
    nexus::hw::CxlMemoryBridge bridge(memory_buffer, sizeof(memory_buffer));
    ASSERT_TRUE(bridge.valid());

    for (std::size_t i = 0; i < 64; ++i) {
        std::size_t offset = i * sizeof(std::uint64_t);
        ASSERT_TRUE(bridge.write_data(offset, 0xCAFEBABEDEADBEEFULL + i));
        ASSERT_TRUE(memory_buffer[i] == (0xCAFEBABEDEADBEEFULL + i));
    }
    std::printf("[Stress 5] PASSED: local buffer writes verified; device visibility is not tested.\n");
}

} // namespace

int main() {
    // Preserve the last started case in CTest output if a later operation stalls.
    (void)std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        const auto cpus = select_allowed_cpus();
        std::printf("HFT concurrency stress: producer CPU=%d, consumer CPU=%d\n",
                    cpus.producer, cpus.consumer);
        const auto start_time = std::chrono::steady_clock::now();

        test_spsc_queue_contention(cpus);
        test_spsc_queue_batch_churn(cpus);
        test_disruptor_saturation(cpus);
        test_orderbook_pool_reclamation_churn();
        test_cxl_memory_bridge();

        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_time).count();
        std::printf("All stress checks passed in %.3f seconds\n", elapsed);
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "stress_test: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
