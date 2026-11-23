#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "nexus/core/flat_hash_map.h"

namespace {

using nexus::core::FlatOrderMap;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "FlatOrderMap check failed at line %d: %s\n", line, expression);
        std::exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

// The hash only selects adversarial inputs; the reference model below supplies
// expected values independently of bucket placement.
std::size_t test_bucket(std::uint64_t key, std::size_t mask) {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ULL;
    key ^= key >> 33;
    return static_cast<std::size_t>(key) & mask;
}

std::vector<std::uint64_t> colliding_keys(std::size_t mask, std::size_t bucket,
                                        std::size_t count) {
    std::vector<std::uint64_t> result;
    for (std::uint64_t key = 1; key <= 10000 && result.size() < count; ++key) {
        if (test_bucket(key, mask) == bucket) result.push_back(key);
    }
    CHECK(result.size() == count);
    return result;
}

void test_full_key_domain() {
    FlatOrderMap map(4);
    constexpr std::array<std::uint64_t, 5> keys{
        0, std::numeric_limits<std::uint64_t>::max(), 1,
        std::uint64_t{1} << 63, (std::uint64_t{1} << 63) - 1};
    for (std::size_t i = 0; i < keys.size(); ++i)
        CHECK(map.try_insert(keys[i], static_cast<std::uint32_t>(i + 10)));
    for (std::size_t i = 0; i < keys.size(); ++i)
        CHECK(map.get(keys[i]) == i + 10);
    for (const auto key : keys) {
        map.erase(key);
        map.erase(key);
        CHECK(map.get(key) == 0);
    }
    for (std::size_t i = 0; i < keys.size(); ++i)
        CHECK(map.try_insert(keys[i], static_cast<std::uint32_t>(i + 20)));
    CHECK(!map.try_insert(42, 1));

    FlatOrderMap zero_value(1);
    CHECK(zero_value.try_insert(0, 0));
    CHECK(!zero_value.try_insert(1, 1));
    CHECK(zero_value.try_insert(0, 7));
    CHECK(zero_value.get(0) == 7);
    zero_value.erase(0);
    CHECK(zero_value.try_insert(1, 1));
}

void test_capacity_and_updates() {
    FlatOrderMap map(2);
    map.insert(11, 1);
    map.insert(22, 2);
    CHECK(!map.try_insert(33, 3));
    CHECK(map.get(11) == 1 && map.get(22) == 2 && map.get(33) == 0);
    map.insert(11, 101);
    CHECK(map.try_insert(22, 202));
    CHECK(map.get(11) == 101 && map.get(22) == 202);
    bool full_threw = false;
    try {
        map.insert(33, 3);
    } catch (const std::runtime_error&) {
        full_threw = true;
    }
    CHECK(full_threw);
    CHECK(map.get(11) == 101 && map.get(22) == 202 && map.get(33) == 0);
    map.erase(11);
    CHECK(map.try_insert(33, 3));
    CHECK(map.get(22) == 202 && map.get(33) == 3);
}

void test_collision_update_after_erase() {
    FlatOrderMap map(4);
    const auto keys = colliding_keys(7, 7, 6);
    map.insert(keys[0], 10);
    map.insert(keys[1], 20);
    map.insert(keys[2], 30);
    map.erase(keys[0]);
    map.insert(keys[1], 99);
    CHECK(map.get(keys[1]) == 99 && map.get(keys[2]) == 30);
    map.erase(keys[1]);
    CHECK(map.get(keys[1]) == 0);
    CHECK(map.get(keys[2]) == 30);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i != 2 && i != 1) CHECK(map.try_insert(keys[i], 50));
    }
    CHECK(!map.try_insert(keys[1], 60));
    CHECK(map.get(keys[2]) == 30);
}

void test_wrapped_cluster_deletion() {
    const auto home_six = colliding_keys(7, 6, 2);
    const auto home_seven = colliding_keys(7, 7, 2);
    const auto home_zero = colliding_keys(7, 0, 1);
    const std::array<std::uint64_t, 5> keys{
        home_six[0], home_seven[0], home_six[1], home_zero[0], home_seven[1]};

    // Exercise wrapped holes, including a neighbour that stays at its home
    // bucket while a later displaced key moves back.
    for (std::size_t removed = 0; removed < keys.size(); ++removed) {
        FlatOrderMap map(4);
        for (std::size_t i = 0; i < keys.size(); ++i)
            map.insert(keys[i], static_cast<std::uint32_t>(i + 1));
        map.erase(keys[removed]);
        for (std::size_t i = 0; i < keys.size(); ++i)
            CHECK(map.get(keys[i]) == (i == removed ? 0 : i + 1));
        CHECK(map.try_insert(keys[removed], 99));
        CHECK(map.get(keys[removed]) == 99);
        for (const auto key : keys) map.erase(key);
        for (const auto key : keys) CHECK(map.get(key) == 0);
    }
}

void test_churn_and_absent_queries() {
    FlatOrderMap map(1);
    const auto even = colliding_keys(1, 0, 1)[0];
    const auto odd = colliding_keys(1, 1, 1)[0];
    for (std::uint32_t round = 1; round <= 10000; ++round) {
        for (const auto key : {even, odd}) {
            CHECK(map.try_insert(key, round));
            CHECK(map.get(key) == round);
            map.erase(key);
            map.erase(key);
        }
        CHECK(map.get(std::numeric_limits<std::uint64_t>::max()) == 0);
        map.erase(std::numeric_limits<std::uint64_t>::max());
        CHECK(map.get(0) == 0);
    }
    CHECK(map.try_insert(std::numeric_limits<std::uint64_t>::max(), 7));
    CHECK(map.get(std::numeric_limits<std::uint64_t>::max()) == 7);
}

void test_constructor_boundaries() {
    FlatOrderMap empty(0);
    CHECK(empty.get(0) == 0);
    CHECK(empty.get(std::numeric_limits<std::uint64_t>::max()) == 0);
    empty.erase(0);
    empty.erase(std::numeric_limits<std::uint64_t>::max());
    CHECK(!empty.try_insert(0, 1));
    bool full_threw = false;
    try {
        empty.insert(1, 1);
    } catch (const std::runtime_error&) {
        full_threw = true;
    }
    CHECK(full_threw);

    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    for (const auto capacity : {maximum, maximum / 2 + 1, maximum / 4 + 1}) {
        bool length_threw = false;
        try {
            const FlatOrderMap excessive(capacity);
            (void)excessive;
        } catch (const std::length_error&) {
            length_threw = true;
        }
        CHECK(length_threw);
    }
}

std::uint64_t next_random(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

void test_reference_model() {
    constexpr std::size_t key_count = 192;
    constexpr std::size_t limit = 89;  // 70% of 128 buckets.
    std::array<std::uint64_t, key_count> keys{};
    keys[1] = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t i = 2; i < keys.size(); ++i)
        keys[i] = (static_cast<std::uint64_t>(i) << 32) | i;

    FlatOrderMap map(64);
    std::unordered_map<std::uint64_t, std::uint32_t> expected;
    expected.reserve(key_count);
    std::uint64_t random_state = 0x68e31da4b5297c05ULL;
    for (std::size_t step = 0; step < 30000; ++step) {
        const auto random = next_random(random_state);
        const auto key = keys[random % key_count];
        const auto operation = (random >> 12) & 3;
        if (operation < 2) {
            const auto value = step % 17 == 0 ? 0 : static_cast<std::uint32_t>(random >> 32);
            const bool admitted = expected.find(key) != expected.end() || expected.size() < limit;
            CHECK(map.try_insert(key, value) == admitted);
            if (admitted) expected[key] = value;
        } else if (operation == 2) {
            map.erase(key);
            expected.erase(key);
        } else {
            const auto found = expected.find(key);
            CHECK(map.get(key) == (found == expected.end() ? 0 : found->second));
        }

        if (step % 257 == 0) {
            for (const auto candidate : keys) {
                const auto found = expected.find(candidate);
                CHECK(map.get(candidate) == (found == expected.end() ? 0 : found->second));
            }
        }
    }

    for (const auto key : keys) map.erase(key);
    for (const auto key : keys) CHECK(map.get(key) == 0);
    for (std::size_t i = 0; i < limit; ++i)
        CHECK(map.try_insert(keys[i], static_cast<std::uint32_t>(i + 1)));
    CHECK(!map.try_insert(keys[limit], 1));
    for (std::size_t i = 0; i < limit; ++i)
        CHECK(map.get(keys[i]) == i + 1);
}

}  // namespace

int main() {
    test_full_key_domain();
    test_capacity_and_updates();
    test_collision_update_after_erase();
    test_wrapped_cluster_deletion();
    test_churn_and_absent_queries();
    test_constructor_boundaries();
    test_reference_model();
    std::puts("FlatOrderMap regression checks passed");
    return EXIT_SUCCESS;
}
