#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include "nexus/md/itch_parser.h"
#include "nexus/md/order_book.h"
#include "nexus/core/spsc_queue.h"

namespace {

using Clock = std::chrono::steady_clock;
#ifndef HFT_QUEUE_CAPACITY
#define HFT_QUEUE_CAPACITY 65536
#endif
constexpr std::size_t kQueueCapacity = HFT_QUEUE_CAPACITY;
constexpr std::uint32_t kMessagesPerRun = hft::MAX_ORDERS - 1;
constexpr std::size_t kRuns = 21;
constexpr std::uint32_t kPrice = 500000;
constexpr std::uint32_t kQuantity = 100;
constexpr std::uint32_t kActiveLevels = 256;
using Queue = hft::SPSCQueue<hft::MdAddOrder, kQueueCapacity>;

class WireInput {
public:
    WireInput()
        : data_(static_cast<char*>(hft::alloc_prefaulted(
              std::size_t{kMessagesPerRun} * sizeof(hft::ItchAddOrder)))) {
        static constexpr char kStock[8] = {'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};
        for (std::uint32_t i = 0; i < kMessagesPerRun; ++i) {
            const std::uint32_t level = i % kActiveLevels;
            hft::encode_add_order(data_ + std::size_t{i} * sizeof(hft::ItchAddOrder),
                                 1, std::uint64_t{i + 1} * 10, i + 1,
                                 level < kActiveLevels / 2, kQuantity + level,
                                 kStock, kPrice + level);
        }
    }

    ~WireInput() { hft::free_prefaulted(data_); }
    WireInput(const WireInput&) = delete;
    WireInput& operator=(const WireInput&) = delete;

    const char* operator[](std::size_t index) const noexcept {
        return data_ + index * sizeof(hft::ItchAddOrder);
    }

private:
    char* const data_;
};

enum class StartState : unsigned char { waiting, run, abort };

struct alignas(hft::kFalseSharingRange) StartGate {
    std::atomic<StartState> state{StartState::waiting};
    std::atomic<unsigned> ready{0};

    bool await_start() noexcept {
        ready.fetch_add(1, std::memory_order_release);
        StartState current;
        while ((current = state.load(std::memory_order_acquire)) == StartState::waiting)
            hft::cpu_relax();
        return current == StartState::run;
    }
};

int parse_cpu(const char* text) {
    int cpu = -1;
    const char* const end = text + std::strlen(text);
    const auto result = std::from_chars(text, end, cpu);
    if (result.ec != std::errc{} || result.ptr != end || cpu < -1)
        throw std::invalid_argument("CPU must be -1 or a nonnegative integer");
    return cpu;
}

std::size_t parse_batch_size(const char* text) {
    std::size_t size = 0;
    const char* const end = text + std::strlen(text);
    const auto result = std::from_chars(text, end, size);
    if (result.ec != std::errc{} || result.ptr != end || size == 0 || size > kQueueCapacity)
        throw std::invalid_argument("Batch size must be between 1 and queue capacity");
    return size;
}

void validate_cpu(int cpu) {
    if (cpu == -1) return;
#if defined(__linux__)
    if (cpu >= CPU_SETSIZE) throw std::invalid_argument("CPU exceeds CPU_SETSIZE");
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
        throw std::system_error(errno, std::generic_category(), "sched_getaffinity");
    if (!CPU_ISSET(cpu, &allowed))
        throw std::invalid_argument("CPU is outside the process affinity mask");
#else
    throw std::invalid_argument("CPU affinity is supported only on Linux; use -1");
#endif
}

int pin_to_cpu(int cpu) noexcept {
#if defined(__linux__)
    if (cpu == -1) return 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)cpu;
    return 0;
#endif
}

void producer(Queue& queue, const WireInput& input, StartGate& gate, int cpu,
              int& pin_error, std::size_t batch_size) noexcept {
    pin_error = pin_to_cpu(cpu);
    if (!gate.await_start()) return;

    // Keep immediate publication as a separate loop with no per-message mode branch.
    if (batch_size == 1) {
        for (std::size_t i = 0; i < kMessagesPerRun; ++i) {
            while (!queue.try_emplace([&](hft::MdAddOrder& slot) noexcept {
                hft::decode_add_order(input[i], slot);
            })) hft::cpu_relax();
        }
        return;
    }

    // The tape models an already received burst. Never wait to accumulate a batch.
    for (std::size_t sent = 0; sent < kMessagesPerRun;) {
        const std::size_t requested = std::min(batch_size, kMessagesPerRun - sent);
        const std::size_t produced = queue.try_produce_batch(
            requested, [&](hft::MdAddOrder& slot, std::size_t offset) noexcept {
                hft::decode_add_order(input[sent + offset], slot);
            });
        sent += produced;
        if (produced == 0) hft::cpu_relax();
    }
}

void verify_book(const hft::OrderBook& book, std::uint64_t rejected) {
    if (rejected != 0 || book.size() != kMessagesPerRun)
        throw std::runtime_error("Order book rejected or lost orders");

    for (std::uint32_t level = 0; level < kActiveLevels; ++level) {
        const bool is_buy = level < kActiveLevels / 2;
        const std::uint32_t first = level + 1;
        const std::uint32_t last = first + ((kMessagesPerRun - first) / kActiveLevels) * kActiveLevels;
        if (book.head(kPrice + level, is_buy) != first ||
            book.tail(kPrice + level, is_buy) != last ||
            book.head(kPrice + level, !is_buy) != 0 || book.tail(kPrice + level, !is_buy) != 0)
            throw std::runtime_error("Price level endpoints are inconsistent");
    }

    // Validate actual stores and FIFO links after timing; keep the work observable.
    for (std::uint32_t i = 1; i <= kMessagesPerRun; ++i) {
        const hft::Order* order = book.get_order(i);
        const std::uint32_t level = (i - 1) % kActiveLevels;
        const std::uint32_t prev = i > kActiveLevels ? i - kActiveLevels : 0;
        const std::uint32_t next = i + kActiveLevels <= kMessagesPerRun ? i + kActiveLevels : 0;
        if (order == nullptr || order->id != i || order->price != kPrice + level ||
            order->qty != kQuantity + level || order->is_buy != (level < kActiveLevels / 2) ||
            order->prev_order_idx != prev || order->next_order_idx != next)
            throw std::runtime_error("Order book contents or FIFO links are inconsistent");
    }
}

double run_once(Queue& queue, const WireInput& input, int prod_cpu, int cons_cpu,
                std::size_t batch_size) {
    // Allocate and prefault the pool and price levels before the timed region.
    hft::OrderBook book{kPrice};
    StartGate gate;
    std::array<int, 2> pin_errors{};
    std::uint64_t rejected = 0;
    Clock::time_point end;
    std::jthread prod;
    std::jthread cons;

    try {
        prod = std::jthread([&] { producer(queue, input, gate, prod_cpu, pin_errors[0], batch_size); });
        cons = std::jthread([&] {
            pin_errors[1] = pin_to_cpu(cons_cpu);
            if (!gate.await_start()) return;

            std::uint64_t done = 0;
            while (done < kMessagesPerRun) {
                const std::size_t got = queue.consume_all([&](const hft::MdAddOrder& m) noexcept {
                    if (book.add_order(m.order_id, m.price, m.qty, m.is_buy != 0) == 0)
                        ++rejected;
                });
                done += got;
                if (got == 0) hft::cpu_relax();
            }
            end = Clock::now();
        });
    } catch (...) {
        // Release the first worker if creating the second worker fails.
        gate.state.store(StartState::abort, std::memory_order_release);
        throw;
    }

    // Wait for both workers explicitly; a fixed sleep does not establish readiness.
    while (gate.ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    if (pin_errors[0] != 0 || pin_errors[1] != 0) {
        gate.state.store(StartState::abort, std::memory_order_release);
        throw std::system_error(pin_errors[0] != 0 ? pin_errors[0] : pin_errors[1],
                                std::generic_category(), "pthread_setaffinity_np");
    }

    const auto start = Clock::now();
    gate.state.store(StartState::run, std::memory_order_release);
    prod.join();
    cons.join();

    verify_book(book, rejected);
    const double seconds = std::chrono::duration<double>(end - start).count();
    if (seconds <= 0) throw std::runtime_error("Clock interval is not positive");
    return static_cast<double>(kMessagesPerRun) / seconds;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
            std::puts("Usage: hft_pipeline [producer_cpu] [consumer_cpu] [batch_size]\n"
                      "CPU -1 disables pinning; batch_size defaults to 1 (immediate publication)");
            return 0;
        }
        if (argc > 4) throw std::invalid_argument("Expected at most three arguments");
        const int prod_cpu = argc > 1 ? parse_cpu(argv[1]) : -1;
        const int cons_cpu = argc > 2 ? parse_cpu(argv[2]) : -1;
        const std::size_t batch_size = argc > 3 ? parse_batch_size(argv[3]) : 1;
        validate_cpu(prod_cpu);
        validate_cpu(cons_cpu);

        // Materialise the wire input before timing so encode/decode cannot cancel out.
        WireInput input;
        Queue queue;
        std::printf("[HFT Pipeline] %zu runs x %u msgs, producer cpu=%d, consumer cpu=%d, "
                    "queue=%zu, batch=%zu, levels=%u\n",
                    kRuns, kMessagesPerRun, prod_cpu, cons_cpu, kQueueCapacity, batch_size, kActiveLevels);

        // Warm up code and runtime state; the warm-up result is validated as well.
        (void)run_once(queue, input, prod_cpu, cons_cpu, batch_size);
        std::array<double, kRuns> rates{};
        for (double& rate : rates) rate = run_once(queue, input, prod_cpu, cons_cpu, batch_size);
        std::sort(rates.begin(), rates.end());
        std::printf("[HFT Pipeline] throughput msgs/sec  min=%.3fM  median=%.3fM  max=%.3fM\n",
                    rates.front() / 1e6, rates[rates.size() / 2] / 1e6, rates.back() / 1e6);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "hft_pipeline: %s\n", error.what());
        return 1;
    }
}
