#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>

#include "nexus/core/platform.h"
#include "nexus/hw/cxl_shared_memory.h"

namespace {

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "%s:%d: check failed: %s\n",                    \
                         __FILE__, __LINE__, #condition);                        \
            std::abort();                                                       \
        }                                                                       \
    } while (false)

using Bridge = nexus::hw::CxlMemoryBridge;
using AtomicWord = std::atomic_ref<std::uint64_t>;

void check_inactive(const Bridge& bridge) {
    CHECK(!bridge.valid());
    CHECK(!bridge.write_data(0, 42));
    CHECK(!bridge.flush_to_device(0));
}

void test_region_bounds() {
    alignas(hft::kCacheLine) std::uint64_t words[8]{};
    check_inactive(Bridge(nullptr, sizeof(words)));
    check_inactive(Bridge(words, 0));
    check_inactive(Bridge(words, sizeof(std::uint64_t) - 1));
    check_inactive(Bridge(reinterpret_cast<char*>(words) + 1, sizeof(words) - 1));

    Bridge bridge(words, sizeof(words));
    if (!Bridge::is_supported()) {
        check_inactive(bridge);
        return;
    }
    CHECK(bridge.valid());
    CHECK(bridge.write_data(0, 11));
    CHECK(bridge.write_data(sizeof(words) - sizeof(words[0]), 22));
    CHECK(words[0] == 11);
    CHECK(words[7] == 22);

    CHECK(!bridge.write_data(1, 99));
    CHECK(!bridge.write_data(sizeof(words) - 1, 99));
    CHECK(!bridge.write_data(sizeof(words), 99));
    CHECK(!bridge.write_data(std::numeric_limits<std::size_t>::max(), 99));
    CHECK(!bridge.flush_to_device(sizeof(words)));
    CHECK(!bridge.flush_to_device(std::numeric_limits<std::size_t>::max()));
    CHECK(bridge.flush_to_device(sizeof(words) - 1));
    for (std::size_t i = 0; i < 8; ++i)
        CHECK(words[i] == (i == 0 ? 11 : i == 7 ? 22 : 0));

    // A short final word must fail even at an otherwise aligned offset.
    Bridge partial(words, sizeof(words) - 1);
    CHECK(!partial.write_data(sizeof(words) - sizeof(words[0]), 99));
    CHECK(words[7] == 22);
}

void test_cpu_publication() {
    if (!Bridge::is_supported()) {
        std::puts("CXL CPU publication test skipped: CLWB is unavailable");
        return;
    }
    alignas(hft::kCacheLine) std::uint64_t words[2]{};
    Bridge bridge(words, sizeof(words));
    CHECK(bridge.valid());
    std::atomic<std::uint64_t> consumed{0};
    constexpr std::uint64_t kCount = 10'000;

    std::jthread producer([&] {
        for (std::uint64_t i = 1; i <= kCount; ++i) {
            while (consumed.load(std::memory_order_acquire) != i - 1)
                hft::cpu_relax();
            words[1] = ~i;
            CHECK(bridge.write_data(0, i));
        }
    });

    // This checks the C++ release/acquire contract on ordinary host RAM only.
    // Seeing the release store does not establish completion of its later CLWB.
    AtomicWord published(words[0]);
    for (std::uint64_t i = 1; i <= kCount; ++i) {
        while (published.load(std::memory_order_acquire) != i) hft::cpu_relax();
        CHECK(words[1] == ~i);
        consumed.store(i, std::memory_order_release);
    }
    producer.join();
}

}  // namespace

int main() {
    test_region_bounds();
    test_cpu_publication();
    std::puts("CXL host-memory tests passed");
}
