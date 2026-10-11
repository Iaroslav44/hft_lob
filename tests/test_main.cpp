#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>

#include "nexus/md/itch_parser.h"
#include "nexus/md/order_book.h"
#include "nexus/core/spsc_queue.h"

namespace {

using namespace hft;

// Keep checks active in Release builds, regardless of NDEBUG.
#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "%s:%d: check failed: %s\n",                    \
                         __FILE__, __LINE__, #condition);                        \
            std::exit(EXIT_FAILURE);                                            \
        }                                                                       \
    } while (false)

template <class T>
void check_endian_value(T value) {
    std::array<char, sizeof(T) + 2> bytes;
    bytes.fill('x');
    store_be<T>(bytes.data() + 1, value);
    CHECK(bytes.front() == 'x');
    CHECK(bytes.back() == 'x');
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        const auto expected = static_cast<unsigned char>(value >> (8 * (sizeof(T) - 1 - i)));
        CHECK(static_cast<unsigned char>(bytes[i + 1]) == expected);
    }
    CHECK(load_be<T>(bytes.data() + 1) == value);
}

void test_endian_and_allocation() {
    check_endian_value<std::uint8_t>(0xa5);
    check_endian_value<std::uint16_t>(0x1234);
    check_endian_value<std::uint32_t>(0x89abcdefU);
    check_endian_value<std::uint64_t>(0x0123456789abcdefULL);

    std::array<char, 8> bytes;
    bytes.fill('x');
    store_be48(bytes.data() + 1, 0x0000123456789abcULL);
    constexpr std::array<unsigned char, 6> expected{0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc};
    for (std::size_t i = 0; i < expected.size(); ++i)
        CHECK(static_cast<unsigned char>(bytes[i + 1]) == expected[i]);
    CHECK(bytes.front() == 'x');
    CHECK(bytes.back() == 'x');
    CHECK(load_be48(bytes.data() + 1) == 0x0000123456789abcULL);

    constexpr auto size = kHugePage + 17;
    auto* const allocation = static_cast<unsigned char*>(alloc_prefaulted(size));
    CHECK(reinterpret_cast<std::uintptr_t>(allocation) % kHugePage == 0);
    for (std::size_t i = 0; i < size; ++i) CHECK(allocation[i] == 0);
    allocation[0] = 0xa5;
    allocation[size - 1] = 0x5a;
    CHECK(allocation[0] == 0xa5 && allocation[size - 1] == 0x5a);
    free_prefaulted(allocation);
    free_prefaulted(nullptr);

    for (const auto invalid_size : {std::size_t{0}, std::numeric_limits<std::size_t>::max()}) {
        bool rejected = false;
        try {
            free_prefaulted(alloc_prefaulted(invalid_size));
        } catch (const std::bad_alloc&) {
            rejected = true;
        }
        CHECK(rejected);
    }
}

void test_queue_full_empty_wrap() {
    SPSCQueue<int, 4> queue;
    int value = -1;
    CHECK(!queue.pop(value));
    CHECK(value == -1);
    for (int round = 0; round < 64; ++round) {
        for (int i = 0; i < 4; ++i) CHECK(queue.push(round * 4 + i));
        CHECK(!queue.push(-1));
        for (int i = 0; i < 4; ++i) {
            CHECK(queue.pop(value));
            CHECK(value == round * 4 + i);
        }
        CHECK(!queue.pop(value));
        CHECK(value == round * 4 + 3);
    }
}

void test_queue_in_place_and_bulk() {
    SPSCQueue<MdAddOrder, 4> queue;
    std::size_t calls = 0;
    for (std::uint64_t id = 1; id <= 4; ++id) {
        CHECK(queue.try_emplace([&](MdAddOrder& slot) noexcept {
            ++calls;
            CHECK(reinterpret_cast<std::uintptr_t>(&slot) % alignof(MdAddOrder) == 0);
            slot = MdAddOrder{id, id + 10, 123, 17, 2, 1};
        }));
    }
    CHECK(!queue.try_emplace([&](MdAddOrder&) noexcept { ++calls; }));
    CHECK(calls == 4);
    std::uint64_t next = 1;
    CHECK(queue.consume_all([&](const MdAddOrder& value) noexcept {
        CHECK(value.order_id == next);
        CHECK(value.ts_ns == next + 10);
        CHECK(value.price == 123 && value.qty == 17 && value.locate == 2 && value.is_buy == 1);
        ++next;
    }) == 4);
    CHECK(next == 5);
    CHECK(queue.consume_all([](const MdAddOrder&) noexcept { CHECK(false); }) == 0);
    CHECK(queue.try_emplace([](MdAddOrder& slot) noexcept { slot.order_id = 5; }));
    MdAddOrder value{};
    CHECK(queue.pop(value));
    CHECK(value.order_id == 5);
}

void test_queue_bulk_after_pop() {
    SPSCQueue<int, 4> queue;
    CHECK(queue.push(1));
    CHECK(queue.push(2));
    int value = 0;
    CHECK(queue.pop(value));
    CHECK(value == 1);
    CHECK(queue.push(3));
    int next = 2;
    CHECK(queue.consume_all([&](const int& item) noexcept { CHECK(item == next++); }) == 2);
    CHECK(next == 4);

    // The next batch crosses the physical ring boundary.
    for (int i = 4; i <= 7; ++i) CHECK(queue.push(i));
    CHECK(queue.consume_all([&](const int& item) noexcept { CHECK(item == next++); }) == 4);
    CHECK(next == 8);
    CHECK(!queue.pop(value));
}

void test_queue_batch_boundaries() {
    SPSCQueue<int, 4> queue;
    CHECK(queue.try_produce_batch(0, [](int&, std::size_t) noexcept { CHECK(false); }) == 0);

    std::size_t calls = 0;
    CHECK(queue.try_produce_batch(std::numeric_limits<std::size_t>::max(),
                                 [&](int& slot, std::size_t offset) noexcept {
        CHECK(offset == calls++);
        slot = static_cast<int>(offset) + 1;
        // No part of the batch is visible before the final publication.
        int value = -1;
        CHECK(!queue.pop(value));
        CHECK(value == -1);
    }) == 4);
    CHECK(calls == 4);
    CHECK(queue.try_produce_batch(2, [](int&, std::size_t) noexcept { CHECK(false); }) == 0);

    int value = 0;
    CHECK(queue.pop(value) && value == 1);
    CHECK(queue.pop(value) && value == 2);
    calls = 0;
    CHECK(queue.try_produce_batch(4, [&](int& slot, std::size_t offset) noexcept {
        CHECK(offset == calls++);
        slot = static_cast<int>(offset) + 5;
    }) == 2);
    CHECK(calls == 2);
    int next = 3;
    CHECK(queue.consume_all([&](const int& item) noexcept { CHECK(item == next++); }) == 4);
    CHECK(next == 7);

    // Start at slot two, wrap the batch, then mix both single-message APIs.
    CHECK(queue.try_produce_batch(3, [](int& slot, std::size_t offset) noexcept {
        slot = static_cast<int>(offset) + 7;
    }) == 3);
    CHECK(queue.push(10));
    CHECK(queue.pop(value) && value == 7);
    CHECK(queue.try_emplace([](int& slot) noexcept { slot = 11; }));
    next = 8;
    CHECK(queue.consume_all([&](const int& item) noexcept { CHECK(item == next++); }) == 4);
    CHECK(next == 12);
    CHECK(!queue.pop(value));
}

struct alignas(32) QueuePayload {
    std::uint64_t sequence;
    std::uint64_t inverse;
    std::uint64_t mixed;
    std::uint64_t repeated;
};

template <std::size_t Capacity>
void test_queue_concurrent(std::uint64_t count) {
    SPSCQueue<QueuePayload, Capacity> queue;
    constexpr std::uint64_t multiplier = 0x9e3779b97f4a7c15ULL;
    std::thread producer([&] {
        for (std::uint64_t i = 1; i <= count; ++i) {
            if ((i & 1) == 0) {
                const QueuePayload value{i, ~i, i * multiplier, i};
                while (!queue.push(value)) cpu_relax();
            } else {
                while (!queue.try_emplace([&](QueuePayload& slot) noexcept {
                    slot.sequence = i;
                    slot.inverse = ~i;
                    slot.mixed = i * multiplier;
                    slot.repeated = i;
                })) cpu_relax();
            }
        }
    });

    std::uint64_t next = 1;
    auto verify = [&](const QueuePayload& value) noexcept {
        CHECK(value.sequence == next);
        CHECK(value.inverse == ~next);
        CHECK(value.mixed == next * multiplier);
        CHECK(value.repeated == next);
        ++next;
    };
    bool single = true;
    while (next <= count) {
        if (single) {
            QueuePayload value{};
            if (queue.pop(value)) verify(value);
            else cpu_relax();
        } else if (queue.consume_all(verify) == 0) {
            cpu_relax();
        }
        single = !single;
    }
    producer.join();
    CHECK(next == count + 1);
    QueuePayload value{};
    CHECK(!queue.pop(value));
}

template <std::size_t Capacity>
void test_queue_batch_concurrent(std::uint64_t count) {
    SPSCQueue<QueuePayload, Capacity> queue;
    constexpr std::uint64_t multiplier = 0x9e3779b97f4a7c15ULL;
    std::thread producer([&] {
        std::uint64_t next = 1;
        while (next <= count) {
            if ((next & 7) == 0) {
                const QueuePayload value{next, ~next, next * multiplier, next};
                if (queue.push(value)) ++next;
                else cpu_relax();
            } else {
                const auto requested = static_cast<std::size_t>(
                    std::min<std::uint64_t>(17, count - next + 1));
                const auto produced = queue.try_produce_batch(
                    requested, [&](QueuePayload& slot, std::size_t offset) noexcept {
                        const auto sequence = next + offset;
                        slot = QueuePayload{sequence, ~sequence, sequence * multiplier, sequence};
                    });
                next += produced;
                if (produced == 0) cpu_relax();
            }
        }
    });

    std::uint64_t next = 1;
    auto verify = [&](const QueuePayload& value) noexcept {
        CHECK(value.sequence == next && value.repeated == next);
        CHECK(value.inverse == ~next && value.mixed == next * multiplier);
        ++next;
    };
    bool single = true;
    while (next <= count) {
        if (single) {
            QueuePayload value{};
            if (queue.pop(value)) verify(value);
            else cpu_relax();
        } else if (queue.consume_all(verify) == 0) {
            cpu_relax();
        }
        single = !single;
    }
    producer.join();
    CHECK(next == count + 1);
    QueuePayload value{};
    CHECK(!queue.pop(value));
}

constexpr std::array<unsigned char, 36> kWireFixture{
    'A', 0x12, 0x34, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc,
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 'S',
    0x01, 0x02, 0x03, 0x04, 'M', 'S', 'F', 'T', ' ', ' ', ' ', ' ',
    0x12, 0x34, 0x56, 0x78
};

void check_fixture_message(const MdAddOrder& value) {
    CHECK(value.locate == 0x1234);
    CHECK(value.ts_ns == 0x0000123456789abcULL);
    CHECK(value.order_id == 0x0123456789abcdefULL);
    CHECK(value.is_buy == 0);
    CHECK(value.qty == 0x01020304U);
    CHECK(value.price == 0x12345678U);
}

void test_itch_fixture_and_roundtrip() {
    // Offset the input by one byte to exercise unaligned wire loads.
    std::array<char, sizeof(ItchAddOrder) + 2> storage;
    storage.fill('x');
    char* const wire = storage.data() + 1;
    std::memcpy(wire, kWireFixture.data(), kWireFixture.size());
    MdAddOrder value{};
    CHECK(parse_itch_add_order(wire, sizeof(ItchAddOrder), value));
    check_fixture_message(value);
    decode_add_order(wire, value);
    check_fixture_message(value);

    constexpr char stock[8] = {'M', 'S', 'F', 'T', ' ', ' ', ' ', ' '};
    encode_add_order(wire, 0x1234, 0x0000123456789abcULL, 0x0123456789abcdefULL,
                     false, 0x01020304U, stock, 0x12345678U);
    CHECK(std::memcmp(wire, kWireFixture.data(), kWireFixture.size()) == 0);
    CHECK(storage.front() == 'x' && storage.back() == 'x');
    CHECK(parse_itch_add_order(wire, sizeof(ItchAddOrder) + 1, value));
    check_fixture_message(value);

    encode_add_order(wire, 0xffff, 0x0000ffffffffffffULL,
                     std::numeric_limits<std::uint64_t>::max(), true,
                     std::numeric_limits<std::uint32_t>::max(), stock,
                     std::numeric_limits<std::uint32_t>::max());
    CHECK(parse_itch_add_order(wire, sizeof(ItchAddOrder), value));
    CHECK(value.locate == 0xffff && value.ts_ns == 0x0000ffffffffffffULL);
    CHECK(value.order_id == std::numeric_limits<std::uint64_t>::max());
    CHECK(value.qty == std::numeric_limits<std::uint32_t>::max());
    CHECK(value.price == std::numeric_limits<std::uint32_t>::max());
    CHECK(value.is_buy == 1);
}

void check_rejected_message(const char* data, std::size_t size) {
    MdAddOrder value{11, 22, 33, 44, 55, 1};
    CHECK(!parse_itch_add_order(data, size, value));
    CHECK(value.order_id == 11 && value.ts_ns == 22);
    CHECK(value.price == 33 && value.qty == 44 && value.locate == 55 && value.is_buy == 1);
}

void test_itch_invalid_input() {
    check_rejected_message(nullptr, 0);
    check_rejected_message(nullptr, sizeof(ItchAddOrder));
    for (std::size_t size = 0; size < sizeof(ItchAddOrder); ++size) {
        auto truncated = std::make_unique<char[]>(size);
        std::memcpy(truncated.get(), kWireFixture.data(), size);
        check_rejected_message(truncated.get(), size);
    }

    std::array<char, sizeof(ItchAddOrder)> wire;
    std::memcpy(wire.data(), kWireFixture.data(), kWireFixture.size());
    for (unsigned int byte = 0; byte <= 255; ++byte) {
        if (byte == static_cast<unsigned int>('A')) continue;
        wire[0] = static_cast<char>(byte);
        check_rejected_message(wire.data(), wire.size());
    }
    wire[0] = 'A';
    for (unsigned int byte = 0; byte <= 255; ++byte) {
        if (byte == static_cast<unsigned int>('B') || byte == static_cast<unsigned int>('S')) continue;
        wire[offsetof(ItchAddOrder, buy_sell_indicator)] = static_cast<char>(byte);
        check_rejected_message(wire.data(), wire.size());
    }
    wire[offsetof(ItchAddOrder, buy_sell_indicator)] = 'S';
    store_be<std::uint64_t>(wire.data() + offsetof(ItchAddOrder, order_reference_number), 0);
    MdAddOrder value{};
    CHECK(parse_itch_add_order(wire.data(), wire.size(), value));
    CHECK(value.order_id == 0);
    store_be<std::uint32_t>(wire.data() + offsetof(ItchAddOrder, shares), 0);
    check_rejected_message(wire.data(), wire.size());
}

template <std::size_t N>
void check_level(const OrderBook& book, std::uint32_t price, bool is_buy,
                 const std::array<std::uint32_t, N>& indices) {
    if constexpr (N == 0) {
        CHECK(book.head(price, is_buy) == 0 && book.tail(price, is_buy) == 0);
    } else {
        CHECK(book.head(price, is_buy) == indices.front());
        CHECK(book.tail(price, is_buy) == indices.back());
        for (std::size_t i = 0; i < indices.size(); ++i) {
            const Order* const order = book.get_order(indices[i]);
            CHECK(order != nullptr);
            CHECK(order->price == price && order->is_buy == is_buy);
            CHECK(order->prev_order_idx == (i == 0 ? 0 : indices[i - 1]));
            CHECK(order->next_order_idx == (i + 1 == indices.size() ? 0 : indices[i + 1]));
        }
    }
}

void test_order_book_fifo_and_recycling() {
    auto book = std::make_unique<OrderBook>(1000, 7);
    CHECK(book->size() == 0 && book->capacity() == 7);
    const auto first = book->add_order(1, 1010, 100, true);
    const auto middle = book->add_order(2, 1010, 200, true);
    const auto last = book->add_order(3, 1010, 300, true);
    const auto ask = book->add_order(4, 1010, 400, false);
    const auto other = book->add_order(5, 1011, 500, true);
    CHECK(first != 0 && middle != 0 && last != 0 && ask != 0 && other != 0);
    CHECK(book->size() == 5);
    check_level(*book, 1010, true, std::array{first, middle, last});
    check_level(*book, 1010, false, std::array{ask});
    check_level(*book, 1011, true, std::array{other});
    CHECK(book->get_order(first)->id == 1 && book->get_order(first)->qty == 100);

    CHECK(book->execute_order(middle, 50));
    CHECK(book->get_order(middle)->qty == 150);
    CHECK(book->size() == 5);
    check_level(*book, 1010, true, std::array{first, middle, last});

    CHECK(book->remove_order(middle));
    CHECK(book->get_order(middle) == nullptr);
    CHECK(!book->remove_order(middle));
    CHECK(!book->execute_order(middle, 1));
    check_level(*book, 1010, true, std::array{first, last});
    CHECK(book->size() == 4);
    CHECK(book->remove_order(first));
    check_level(*book, 1010, true, std::array{last});
    CHECK(book->remove_order(last));
    check_level(*book, 1010, true, std::array<std::uint32_t, 0>{});
    check_level(*book, 1010, false, std::array{ask});

    CHECK(!book->execute_order(ask, 0));
    CHECK(!book->execute_order(ask, 401));
    CHECK(book->get_order(ask)->qty == 400);
    CHECK(book->execute_order(ask, 123));
    CHECK(book->get_order(ask)->qty == 277);
    CHECK(book->size() == 2);
    CHECK(book->execute_order(ask, 277));
    CHECK(book->get_order(ask) == nullptr);
    check_level(*book, 1010, false, std::array<std::uint32_t, 0>{});

    std::array<std::uint32_t, 6> recycled{};
    for (std::size_t i = 0; i < recycled.size(); ++i) {
        recycled[i] = book->add_order(100 + i, 1010, 1, false);
        CHECK(recycled[i] != 0);
    }
    CHECK(book->size() == book->capacity());
    CHECK(book->add_order(200, 1010, 1, false) == 0);
    check_level(*book, 1010, false, recycled);
    check_level(*book, 1011, true, std::array{other});
    CHECK(book->remove_order(recycled.back()));
    const auto replacement = book->add_order(201, 1010, 2, false);
    CHECK(replacement == recycled.back());
    recycled.back() = replacement;
    check_level(*book, 1010, false, recycled);
    CHECK(book->get_order(replacement)->id == 201 && book->get_order(replacement)->qty == 2);
}

void test_order_book_boundaries() {
    auto book = std::make_unique<OrderBook>(500000, 2);
    constexpr auto outside = std::uint32_t{500000} + static_cast<std::uint32_t>(MAX_PRICE_LEVELS);
    CHECK(book->add_order(1, 499999, 1, true) == 0);
    CHECK(book->add_order(2, outside, 1, true) == 0);
    CHECK(book->add_order(3, 500000, 0, true) == 0);
    CHECK(book->size() == 0);
    CHECK(book->get_order(0) == nullptr);
    CHECK(book->get_order(1) == nullptr);
    CHECK(book->get_order(std::numeric_limits<std::uint32_t>::max()) == nullptr);
    CHECK(!book->remove_order(0) && !book->remove_order(3));
    CHECK(!book->execute_order(0, 1) && !book->execute_order(3, 1));
    CHECK(book->head(499999, true) == 0 && book->tail(outside, false) == 0);

    const auto lowest = book->add_order(0, 500000, 1, true);
    const auto highest = book->add_order(5, outside - 1, 1, false);
    CHECK(lowest != 0 && highest != 0 && lowest != highest);
    CHECK(book->get_order(lowest)->id == 0);
    check_level(*book, 500000, true, std::array{lowest});
    check_level(*book, outside - 1, false, std::array{highest});
    CHECK(book->add_order(6, 500001, 1, false) == 0);
    CHECK(book->size() == 2);

    auto empty = std::make_unique<OrderBook>(0, 0);
    CHECK(empty->capacity() == 0 && empty->size() == 0);
    CHECK(empty->add_order(1, 0, 1, true) == 0);
    CHECK(empty->get_order(1) == nullptr);

    constexpr auto max_price = std::numeric_limits<std::uint32_t>::max();
    constexpr auto base = max_price - static_cast<std::uint32_t>(MAX_PRICE_LEVELS) + 1;
    auto upper = std::make_unique<OrderBook>(base, 1);
    const auto upper_index = upper->add_order(1, max_price, 1, false);
    CHECK(upper_index != 0);
    check_level(*upper, max_price, false, std::array{upper_index});
    CHECK(upper->head(0, false) == 0 && upper->tail(0, false) == 0);

    auto clipped = std::make_unique<OrderBook>(max_price, 1);
    CHECK(clipped->add_order(1, 0, 1, true) == 0);
    CHECK(clipped->add_order(1, max_price, 1, true) != 0);
    CHECK(clipped->head(0, true) == 0 && clipped->tail(0, true) == 0);

    bool rejected = false;
    try {
        auto invalid = std::make_unique<OrderBook>(0, static_cast<std::uint32_t>(MAX_ORDERS));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
}

void test_order_book_pool_exhaustion() {
    auto book = std::make_unique<OrderBook>();
    CHECK(book->capacity() == MAX_ORDERS - 1);
    std::uint32_t first = 0;
    std::uint32_t last = 0;
    for (std::uint32_t i = 1; i <= book->capacity(); ++i) {
        const auto index = book->add_order(i, 100, 1, true);
        CHECK(index != 0);
        if (first == 0) first = index;
        last = index;
    }
    CHECK(book->size() == book->capacity());
    CHECK(book->head(100, true) == first && book->tail(100, true) == last);
    CHECK(book->add_order(MAX_ORDERS, 100, 1, true) == 0);
    CHECK(book->get_order(last)->id == book->capacity());
    CHECK(book->remove_order(first));
    CHECK(book->add_order(MAX_ORDERS + 1, 100, 2, true) == first);
    CHECK(book->tail(100, true) == first);
    CHECK(book->get_order(first)->prev_order_idx == last);
    CHECK(book->get_order(last)->next_order_idx == first);
    CHECK(book->size() == book->capacity());
}

}

int main() {
    test_endian_and_allocation();
    test_queue_full_empty_wrap();
    test_queue_in_place_and_bulk();
    test_queue_bulk_after_pop();
    test_queue_batch_boundaries();
    test_queue_concurrent<2>(100'000);
    test_queue_concurrent<1024>(500'000);
    test_queue_batch_concurrent<2>(60'003);
    test_queue_batch_concurrent<64>(200'003);
    test_itch_fixture_and_roundtrip();
    test_itch_invalid_input();
    test_order_book_fifo_and_recycling();
    test_order_book_boundaries();
    test_order_book_pool_exhaustion();
    std::puts("All checks passed");
    return EXIT_SUCCESS;
}
