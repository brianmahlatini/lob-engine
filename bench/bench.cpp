// Replays a pre-generated synthetic order flow (so RNG cost is not measured)
// and reports throughput plus per-operation latency percentiles.
#include <lob/order_book.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

using namespace lob;
using Clock = std::chrono::steady_clock;

struct Op { enum Kind : std::uint8_t { Add, Cancel } kind; OrderRequest req; };

struct Counter {
    std::uint64_t trades = 0, volume = 0;
    void on_trade(const Trade& t) noexcept { ++trades; volume += static_cast<std::uint64_t>(t.qty); }
};

static std::vector<Op> generate(std::size_t n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<Op> ops;
    ops.reserve(n);
    std::vector<OrderId> live;
    OrderId next = 1;
    const Price mid = 50'000;
    for (std::size_t i = 0; i < n; ++i) {
        unsigned r = static_cast<unsigned>(rng() % 100);
        if (r < 35 && !live.empty()) {                      // cancel
            std::size_t k = rng() % live.size();
            ops.push_back({Op::Cancel, {live[k], Side::Buy, 0, 0}});
            live[k] = live.back();
            live.pop_back();
        } else {
            Side s = rng() & 1 ? Side::Buy : Side::Sell;
            bool aggressive = r >= 90;                       // ~10% cross the spread
            Price off = static_cast<Price>(rng() % 50);
            Price p = s == Side::Buy ? (aggressive ? mid + off : mid - 1 - off)
                                     : (aggressive ? mid - off : mid + 1 + off);
            Qty q = 1 + static_cast<Qty>(rng() % 100);
            ops.push_back({Op::Add, {next, s, p, q, TimeInForce::GTC}});
            live.push_back(next++);
        }
    }
    return ops;
}

template <bool Timed>
static double run(const std::vector<Op>& ops, std::vector<std::uint32_t>* lat, Counter& c) {
    OrderBook<Counter> book({1, 100'000, 1 << 21});
    auto t0 = Clock::now();
    for (const Op& op : ops) {
        [[maybe_unused]] Clock::time_point a;
        if constexpr (Timed) a = Clock::now();
        if (op.kind == Op::Add) book.submit(op.req); else book.cancel(op.req.id);
        if constexpr (Timed) lat->push_back(static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - a).count()));
    }
    double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    c = book.listener();
    return secs;
}

int main(int argc, char** argv) {
    std::size_t n = argc > 1 ? std::stoull(argv[1]) : 10'000'000;
    auto ops = generate(n, 42);

    Counter warm; run<false>(ops, nullptr, warm);                 // warm caches and page tables
    Counter c;
    double secs = run<false>(ops, nullptr, c);
    std::printf("ops=%zu time=%.3fs throughput=%.2fM ops/s  trades=%llu volume=%llu\n",
                n, secs, static_cast<double>(n) / secs / 1e6, (unsigned long long)c.trades, (unsigned long long)c.volume);

    std::vector<std::uint32_t> lat;
    lat.reserve(n);
    run<true>(ops, &lat, c);
    std::sort(lat.begin(), lat.end());
    auto pct = [&](double p) { return lat[std::min(lat.size() - 1, static_cast<std::size_t>(p * static_cast<double>(lat.size())))]; };
    std::printf("latency ns (incl. ~%s clock overhead): p50=%u p90=%u p99=%u p99.9=%u max=%u\n",
                "20ns", pct(0.50), pct(0.90), pct(0.99), pct(0.999), lat.back());
}
