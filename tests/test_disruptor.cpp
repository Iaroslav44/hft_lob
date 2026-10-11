#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>

#include "nexus/core/disruptor.h"

namespace {

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "%s:%d: check failed: %s\n",                    \
                         __FILE__, __LINE__, #condition);                        \
            std::abort();                                                       \
        }                                                                       \
    } while (false)

struct alignas(32) Payload {
    std::uint64_t sequence;
    std::uint64_t inverse;
    std::uint64_t checksum;
    std::uint64_t repeated;
};

constexpr std::uint64_t kMultiplier = 0x9e3779b97f4a7c15ULL;

std::int64_t sequence_bits(std::uint64_t sequence) noexcept {
    return std::bit_cast<std::int64_t>(sequence);
}

Payload payload_for(std::uint64_t sequence) noexcept {
    return {sequence, ~sequence, sequence * kMultiplier, sequence};
}

void verify_payload(const Payload& payload, std::uint64_t sequence) noexcept {
    CHECK(payload.sequence == sequence);
    CHECK(payload.inverse == ~sequence);
    CHECK(payload.checksum == sequence * kMultiplier);
    CHECK(payload.repeated == sequence);
}

template <std::size_t Capacity>
void test_rollover(std::uint64_t seed) {
    nexus::core::DisruptorRingBuffer<Payload, Capacity> ring;
    CHECK(ring.get_cursor() == -1);
    CHECK(!ring.is_published(0));

    // Both endpoints are quiescent. Equal existing sequence setters represent
    // an empty, reachable state near rollover without a production test hook.
    ring.publish(sequence_bits(seed));
    ring.set_consumer_sequence(sequence_bits(seed));
    std::uint64_t produced = seed;
    std::uint64_t consumed = seed;

    for (std::size_t round = 0; round < 8; ++round) {
        CHECK(!ring.is_published(sequence_bits(consumed + 1)));
        for (std::size_t i = 0; i < Capacity; ++i) {
            const auto claimed = ring.next();
            ++produced;
            CHECK(claimed == sequence_bits(produced));
            ring.get(claimed) = payload_for(produced);
            if (i == 0) CHECK(!ring.is_published(sequence_bits(consumed + 1)));
            ring.publish(claimed);
        }
        for (std::size_t i = 0; i < Capacity; ++i) {
            ++consumed;
            const auto sequence = sequence_bits(consumed);
            CHECK(ring.is_published(sequence));
            const auto& const_ring = ring;
            verify_payload(const_ring.get(sequence), consumed);
            ring.set_consumer_sequence(sequence);
        }
        CHECK(produced == consumed);
    }
    CHECK(!ring.is_published(sequence_bits(consumed + 1)));
}

template <std::size_t Capacity>
void test_concurrent_rollover(std::uint64_t seed) {
    nexus::core::DisruptorRingBuffer<Payload, Capacity> ring;
    ring.publish(sequence_bits(seed));
    ring.set_consumer_sequence(sequence_bits(seed));
    constexpr std::uint64_t kCount = 50'000;

    std::jthread producer([&] {
        for (std::uint64_t i = 1; i <= kCount; ++i) {
            const std::uint64_t expected = seed + i;
            const auto sequence = ring.next();
            CHECK(sequence == sequence_bits(expected));
            ring.get(sequence) = payload_for(expected);
            ring.publish(sequence);
        }
    });

    for (std::uint64_t i = 1; i <= kCount; ++i) {
        const std::uint64_t expected = seed + i;
        const auto sequence = sequence_bits(expected);
        while (!ring.is_published(sequence)) hft::cpu_relax();
        const Payload& payload = ring.get(sequence);
        verify_payload(payload, expected);
        if ((i & 31) == 0) {
            // Keep ownership across a scheduling opportunity; reclamation must
            // precede any producer overwrite, including for a one-slot ring.
            std::this_thread::yield();
            verify_payload(payload, expected);
        }
        ring.set_consumer_sequence(sequence);
    }
    producer.join();
    CHECK(!ring.is_published(sequence_bits(seed + kCount + 1)));
}

}  // namespace

int main() {
    constexpr auto kSignedBoundary =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - 3;
    constexpr auto kUnsignedBoundary = std::numeric_limits<std::uint64_t>::max() - 3;

    test_rollover<1>(kSignedBoundary);
    test_rollover<4>(kSignedBoundary);
    test_rollover<1>(kUnsignedBoundary);
    test_rollover<4>(kUnsignedBoundary);
    test_concurrent_rollover<1>(kSignedBoundary);
    test_concurrent_rollover<4>(kUnsignedBoundary);
    std::puts("Disruptor tests passed");
}
