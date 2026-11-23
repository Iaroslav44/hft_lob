#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace nexus::core {

// Fixed-storage, single-owner ID-to-slot map. All uint64_t keys are valid.
// A zero result from get() means absent or a stored zero value.
class FlatOrderMap {
    struct Entry {
        std::uint64_t key{};
        std::uint32_t value{};
        bool occupied{};
    };
    static_assert(sizeof(Entry) == 16);

    std::vector<Entry> entries_;
    std::size_t mask_;
    std::size_t size_ = 0;
    std::size_t max_size_;

public:
    // Keep at least twice the requested capacity in power-of-two storage.
    // Admission is limited to 70% of the buckets; zero capacity admits no keys.
    explicit FlatOrderMap(std::size_t capacity)
        : entries_(bucket_count(capacity)),
          mask_(entries_.size() - 1),
          max_size_((entries_.size() / 10) * 7 + (entries_.size() % 10) * 7 / 10) {}

    // No allocation or exception on this path. Rejection leaves the map unchanged;
    // an existing key can always be updated, including at the admission limit.
    [[nodiscard]] bool try_insert(std::uint64_t key, std::uint32_t value) noexcept {
        std::size_t idx = hash(key) & mask_;
        for (std::size_t probes = 0; probes < entries_.size(); ++probes) {
            Entry& entry = entries_[idx];
            if (!entry.occupied) {
                if (size_ >= max_size_) return false;
                entry = Entry{key, value, true};
                ++size_;
                return true;
            }
            if (entry.key == key) {
                entry.value = value;
                return true;
            }
            idx = (idx + 1) & mask_;
        }
        return false;
    }

    // Compatibility entry point; use try_insert() when capacity failure is expected.
    void insert(std::uint64_t key, std::uint32_t value) {
        if (!try_insert(key, value)) throw std::runtime_error("Map full");
    }

    [[nodiscard]] std::uint32_t get(std::uint64_t key) const noexcept {
        std::size_t idx = hash(key) & mask_;
        for (std::size_t probes = 0; probes < entries_.size(); ++probes) {
            const Entry& entry = entries_[idx];
            if (!entry.occupied) return 0;
            if (entry.key == key) return entry.value;
            idx = (idx + 1) & mask_;
        }
        return 0;
    }

    void erase(std::uint64_t key) noexcept {
        std::size_t idx = hash(key) & mask_;
        for (std::size_t probes = 0; probes < entries_.size(); ++probes) {
            if (!entries_[idx].occupied) return;
            if (entries_[idx].key == key) {
                close_hole(idx);
                --size_;
                return;
            }
            idx = (idx + 1) & mask_;
        }
    }

private:
    [[nodiscard]] static std::size_t bucket_count(std::size_t capacity) {
        const std::size_t limit = std::vector<Entry>{}.max_size();
        if (capacity > limit / 2)
            throw std::length_error("Map capacity exceeds the storage limit");
        const std::size_t required = capacity * 2;
        std::size_t count = 1;
        while (count < required) {
            if (count > limit / 2)
                throw std::length_error("Map capacity exceeds the storage limit");
            count *= 2;
        }
        return count;
    }

    void close_hole(std::size_t hole) noexcept {
        entries_[hole].occupied = false;
        std::size_t idx = (hole + 1) & mask_;
        for (std::size_t probes = 0; probes < entries_.size(); ++probes) {
            Entry& entry = entries_[idx];
            if (!entry.occupied) return;
            const std::size_t home = hash(entry.key) & mask_;
            // Move only when the hole lies on this key's modular probe path.
            // Closing holes avoids tombstone accumulation under sustained churn.
            if (((hole - home) & mask_) < ((idx - home) & mask_)) {
                entries_[hole] = entry;
                entry.occupied = false;
                hole = idx;
            }
            idx = (idx + 1) & mask_;
        }
    }

    [[nodiscard]] static std::size_t hash(std::uint64_t key) noexcept {
        key ^= key >> 33;
        key *= 0xff51afd7ed558ccdULL;
        key ^= key >> 33;
        key *= 0xc4ceb9fe1a85ec53ULL;
        key ^= key >> 33;
        return static_cast<std::size_t>(key);
    }
};

}  // namespace nexus::core
