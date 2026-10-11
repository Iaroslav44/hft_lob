#pragma once
#include <cstdint>
#include <vector>
#include <stdexcept>

namespace nexus::core {

class FlatOrderMap {
    struct Entry {
        std::uint64_t key;
        std::uint32_t value;
    };
    std::vector<Entry> entries_;
    std::size_t mask_;
    std::size_t size_ = 0;
    std::size_t max_size_;

public:
    explicit FlatOrderMap(std::size_t capacity) {
        std::size_t pow2 = 1;
        while (pow2 < capacity * 2) pow2 *= 2;
        entries_.resize(pow2, {0, 0});
        mask_ = pow2 - 1;
        max_size_ = static_cast<std::size_t>(pow2 * 0.7);
    }

    void insert(std::uint64_t key, std::uint32_t value) {
        if (size_ >= max_size_) throw std::runtime_error("Map full");
        std::size_t idx = hash(key) & mask_;
        while (true) {
            if (entries_[idx].key == 0 || entries_[idx].key == static_cast<std::uint64_t>(-1)) {
                entries_[idx] = {key, value};
                ++size_;
                return;
            }
            if (entries_[idx].key == key) {
                entries_[idx].value = value;
                return;
            }
            idx = (idx + 1) & mask_;
        }
    }

    std::uint32_t get(std::uint64_t key) const {
        std::size_t idx = hash(key) & mask_;
        while (true) {
            if (entries_[idx].key == 0) return 0;
            if (entries_[idx].key == key) return entries_[idx].value;
            idx = (idx + 1) & mask_;
        }
    }

    void erase(std::uint64_t key) {
        std::size_t idx = hash(key) & mask_;
        while (true) {
            if (entries_[idx].key == 0) return;
            if (entries_[idx].key == key) {
                entries_[idx].key = static_cast<std::uint64_t>(-1); // Tombstone
                --size_;
                return;
            }
            idx = (idx + 1) & mask_;
        }
    }

private:
    std::size_t hash(std::uint64_t k) const {
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdULL;
        k ^= k >> 33;
        k *= 0xc4ceb9fe1a85ec53ULL;
        k ^= k >> 33;
        return k;
    }
};
} // namespace nexus::core
