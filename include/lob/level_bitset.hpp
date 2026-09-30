#pragma once
// Two-level bitset over price levels. Finding the next non-empty level from
// any position costs at most two word scans per level plus a summary scan,
// i.e. O(N/4096) words in the worst case and O(1) in practice, versus O(log N)
// pointer-chasing for a std::map ladder.
#include <bit>
#include <cstdint>
#include <vector>

namespace lob {

class LevelBitset {
public:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    explicit LevelBitset(std::size_t n)
        : n_(n), words_((n + 63) / 64, 0), summary_((words_.size() + 63) / 64, 0) {}

    void set(std::size_t i) noexcept {
        words_[i >> 6] |= bit(i);
        summary_[i >> 12] |= bit(i >> 6);
    }
    void clear(std::size_t i) noexcept {
        std::size_t w = i >> 6;
        words_[w] &= ~bit(i);
        if (!words_[w]) summary_[w >> 6] &= ~bit(w);
    }
    [[nodiscard]] bool test(std::size_t i) const noexcept { return words_[i >> 6] & bit(i); }

    // Smallest set index >= i, or npos.
    [[nodiscard]] std::size_t find_next(std::size_t i) const noexcept {
        if (i >= n_) return npos;
        std::size_t w = i >> 6;
        if (std::uint64_t m = words_[w] & (~0ULL << (i & 63))) return (w << 6) | lo(m);
        std::size_t nw = next_word(w + 1);
        return nw == npos ? npos : (nw << 6) | lo(words_[nw]);
    }

    // Largest set index <= i, or npos.
    [[nodiscard]] std::size_t find_prev(std::size_t i) const noexcept {
        if (i >= n_) i = n_ - 1;
        std::size_t w = i >> 6;
        std::uint64_t mask = (i & 63) == 63 ? ~0ULL : ((1ULL << ((i & 63) + 1)) - 1);
        if (std::uint64_t m = words_[w] & mask) return (w << 6) | hi(m);
        if (w == 0) return npos;
        std::size_t pw = prev_word(w - 1);
        return pw == npos ? npos : (pw << 6) | hi(words_[pw]);
    }

private:
    static constexpr std::uint64_t bit(std::size_t i) noexcept { return 1ULL << (i & 63); }
    // Index of lowest / highest set bit (m != 0).
    static constexpr std::size_t lo(std::uint64_t m) noexcept { return static_cast<std::size_t>(std::countr_zero(m)); }
    static constexpr std::size_t hi(std::uint64_t m) noexcept { return 63u - static_cast<std::size_t>(std::countl_zero(m)); }

    std::size_t next_word(std::size_t w) const noexcept {
        if (w >= words_.size()) return npos;
        std::size_t s = w >> 6;
        if (std::uint64_t m = summary_[s] & (~0ULL << (w & 63))) return (s << 6) | lo(m);
        for (++s; s < summary_.size(); ++s)
            if (summary_[s]) return (s << 6) | lo(summary_[s]);
        return npos;
    }
    std::size_t prev_word(std::size_t w) const noexcept {
        std::size_t s = w >> 6;
        std::uint64_t mask = (w & 63) == 63 ? ~0ULL : ((1ULL << ((w & 63) + 1)) - 1);
        if (std::uint64_t m = summary_[s] & mask) return (s << 6) | hi(m);
        while (s-- > 0)
            if (summary_[s]) return (s << 6) | hi(summary_[s]);
        return npos;
    }

    std::size_t n_;
    std::vector<std::uint64_t> words_;
    std::vector<std::uint64_t> summary_;
};

}  // namespace lob
