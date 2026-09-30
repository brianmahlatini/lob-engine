#pragma once
// Open-addressing hash map (linear probing, backward-shift deletion) from
// OrderId to pool index. No tombstones, so probe lengths stay short under
// heavy add/cancel churn, which is exactly the order-book workload.
#include <bit>
#include <cstdint>
#include <optional>
#include <vector>
#include "types.hpp"

namespace lob {

class FlatIdMap {
public:
    explicit FlatIdMap(std::size_t expected = 1024) {
        std::size_t cap = std::bit_ceil(expected * 2 < 16 ? 16 : expected * 2);
        slots_.assign(cap, Slot{});
        mask_ = cap - 1;
    }

    // Returns false if the key already exists.
    bool insert(OrderId key, Index value) {
        if ((size_ + 1) * 4 > slots_.size() * 3) grow();   // max load 0.75
        for (std::size_t i = hash(key) & mask_;; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (!s.used) { s = {key, value, true}; ++size_; return true; }
            if (s.key == key) return false;
        }
    }

    [[nodiscard]] std::optional<Index> find(OrderId key) const noexcept {
        for (std::size_t i = hash(key) & mask_;; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (!s.used) return std::nullopt;
            if (s.key == key) return s.value;
        }
    }

    bool erase(OrderId key) noexcept {
        std::size_t i = hash(key) & mask_;
        for (;; i = (i + 1) & mask_) {
            if (!slots_[i].used) return false;
            if (slots_[i].key == key) break;
        }
        // Backward-shift: pull later entries of the cluster into the hole if
        // their home slot is at or before it (cyclically).
        std::size_t hole = i;
        for (std::size_t j = (i + 1) & mask_; slots_[j].used; j = (j + 1) & mask_) {
            std::size_t home = hash(slots_[j].key) & mask_;
            if (((j - home) & mask_) >= ((j - hole) & mask_)) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].used = false;
        --size_;
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    struct Slot { OrderId key = 0; Index value = 0; bool used = false; };

    static std::size_t hash(OrderId k) noexcept {   // splitmix64 finaliser
        k ^= k >> 30; k *= 0xbf58476d1ce4e5b9ULL;
        k ^= k >> 27; k *= 0x94d049bb133111ebULL;
        return static_cast<std::size_t>(k ^ (k >> 31));
    }

    void grow() {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(old.size() * 2, Slot{});
        mask_ = slots_.size() - 1;
        size_ = 0;
        for (const Slot& s : old) if (s.used) insert(s.key, s.value);
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
};

}  // namespace lob
