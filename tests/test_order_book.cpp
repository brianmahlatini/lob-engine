#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <lob/order_book.hpp>

#include <deque>
#include <map>
#include <random>
#include <vector>

using namespace lob;

struct Recorder {
    std::vector<Trade> trades;
    void on_trade(const Trade& t) { trades.push_back(t); }
};

using Book = OrderBook<Recorder>;
static BookConfig small() { return {1, 1000, 4096}; }

TEST_CASE("resting orders set best bid/ask") {
    Book b(small());
    CHECK(b.submit({1, Side::Buy, 100, 10}).status == Status::Resting);
    CHECK(b.submit({2, Side::Sell, 105, 5}).status == Status::Resting);
    CHECK(b.submit({3, Side::Buy, 101, 1}).status == Status::Resting);
    CHECK(b.best_bid() == 101);
    CHECK(b.best_ask() == 105);
    CHECK(b.depth_at(100) == 10);
}

TEST_CASE("price-time priority and maker price") {
    Book b(small());
    b.submit({1, Side::Sell, 100, 5});
    b.submit({2, Side::Sell, 100, 5});
    b.submit({3, Side::Sell, 99, 2});
    auto r = b.submit({10, Side::Buy, 100, 8});
    CHECK(r.status == Status::Filled);
    CHECK(r.filled == 8);
    auto& t = b.listener().trades;
    REQUIRE(t.size() == 3);
    CHECK((t[0].maker == 3 && t[0].price == 99 && t[0].qty == 2));
    CHECK((t[1].maker == 1 && t[1].qty == 5));
    CHECK((t[2].maker == 2 && t[2].qty == 1));
    CHECK(b.depth_at(100) == 4);
    CHECK(b.best_ask() == 100);
}

TEST_CASE("partial fill rests remainder") {
    Book b(small());
    b.submit({1, Side::Sell, 100, 3});
    auto r = b.submit({2, Side::Buy, 101, 10});
    CHECK(r.status == Status::PartiallyFilled);
    CHECK(r.filled == 3);
    CHECK(b.best_bid() == 101);
    CHECK(b.depth_at(101) == 7);
    CHECK_FALSE(b.has_ask());
}

TEST_CASE("IOC and market orders never rest") {
    Book b(small());
    b.submit({1, Side::Sell, 100, 3});
    auto r = b.submit({2, Side::Buy, 100, 10, TimeInForce::IOC});
    CHECK(r.status == Status::Cancelled);
    CHECK(r.filled == 3);
    CHECK_FALSE(b.has_bid());
    b.submit({3, Side::Buy, 50, 4});
    b.submit({4, Side::Buy, 40, 4});
    auto m = b.submit_market(5, Side::Sell, 6);
    CHECK(m.status == Status::Filled);
    CHECK(b.best_bid() == 40);
    CHECK(b.depth_at(40) == 2);
}

TEST_CASE("FOK is all-or-nothing") {
    Book b(small());
    b.submit({1, Side::Sell, 100, 3});
    b.submit({2, Side::Sell, 101, 3});
    CHECK(b.submit({3, Side::Buy, 101, 7, TimeInForce::FOK}).status == Status::RejectedFokUnfillable);
    CHECK(b.listener().trades.empty());
    CHECK(b.submit({4, Side::Buy, 100, 4, TimeInForce::FOK}).status == Status::RejectedFokUnfillable);
    CHECK(b.submit({5, Side::Buy, 101, 6, TimeInForce::FOK}).status == Status::Filled);
    CHECK(b.order_count() == 0);
}

TEST_CASE("cancel and modify") {
    Book b(small());
    b.submit({1, Side::Buy, 100, 5});
    b.submit({2, Side::Buy, 100, 5});
    CHECK(b.modify(1, 100, 3).status == Status::Resting);   // keeps priority
    CHECK(b.depth_at(100) == 8);
    b.submit({9, Side::Sell, 100, 3});
    CHECK(b.listener().trades.back().maker == 1);
    CHECK(b.modify(2, 102, 5).status == Status::Resting);   // reprice
    CHECK(b.best_bid() == 102);
    CHECK(b.depth_at(100) == 0);
    CHECK(b.modify(2, 5000, 1).status == Status::RejectedInvalidPrice);
    CHECK(b.best_bid() == 102);                            // untouched on reject
    CHECK(b.cancel(2));
    CHECK_FALSE(b.cancel(2));
    CHECK_FALSE(b.has_bid());
    CHECK(b.modify(77, 100, 1).status == Status::RejectedUnknownOrder);
}

TEST_CASE("rejections") {
    Book b(small());
    CHECK(b.submit({1, Side::Buy, 0, 1}).status == Status::RejectedInvalidPrice);
    CHECK(b.submit({1, Side::Buy, 1001, 1}).status == Status::RejectedInvalidPrice);
    CHECK(b.submit({1, Side::Buy, 10, 0}).status == Status::RejectedInvalidQty);
    b.submit({1, Side::Buy, 10, 1});
    CHECK(b.submit({1, Side::Buy, 10, 1}).status == Status::RejectedDuplicateId);
    Book tiny({1, 100, 2});
    tiny.submit({1, Side::Buy, 10, 1});
    tiny.submit({2, Side::Buy, 10, 1});
    CHECK(tiny.submit({3, Side::Buy, 10, 1}).status == Status::RejectedCapacity);
    CHECK_THROWS_AS(Book({10, 5, 10}), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Differential test: a deliberately naive std::map reference book must agree
// with the optimised engine on every trade, status and top-of-book, across
// hundreds of thousands of random operations.
struct RefBook {
    struct O { OrderId id; Qty qty; };
    std::map<Price, std::deque<O>, std::greater<>> bids;
    std::map<Price, std::deque<O>> asks;
    std::map<OrderId, std::pair<Side, Price>> where;
    std::vector<Trade> trades;

    template <class M>
    Qty take(M& book, const OrderRequest& r) {
        Qty rem = r.qty;
        while (rem && !book.empty()) {
            auto it = book.begin();
            if (r.side == Side::Buy ? it->first > r.price : it->first < r.price) break;
            auto& q = it->second;
            while (rem && !q.empty()) {
                Qty x = std::min(rem, q.front().qty);
                trades.push_back({q.front().id, r.id, r.side, it->first, x});
                rem -= x;
                if ((q.front().qty -= x) == 0) { where.erase(q.front().id); q.pop_front(); }
            }
            if (q.empty()) book.erase(it);
        }
        return rem;
    }
    Qty avail(const OrderRequest& r) {
        Qty s = 0;
        auto sum = [&](auto& book) {
            for (auto& [p, q] : book) {
                if (r.side == Side::Buy ? p > r.price : p < r.price) break;
                for (auto& o : q) s += o.qty;
            }
        };
        if (r.side == Side::Buy) sum(asks); else sum(bids);
        return s;
    }
    Status submit(const OrderRequest& r) {
        if (where.count(r.id)) return Status::RejectedDuplicateId;
        if (r.tif == TimeInForce::FOK && avail(r) < r.qty) return Status::RejectedFokUnfillable;
        Qty rem = r.side == Side::Buy ? take(asks, r) : take(bids, r);
        if (!rem) return Status::Filled;
        if (r.tif != TimeInForce::GTC) return Status::Cancelled;
        if (r.side == Side::Buy) bids[r.price].push_back({r.id, rem});
        else asks[r.price].push_back({r.id, rem});
        where[r.id] = {r.side, r.price};
        return rem == r.qty ? Status::Resting : Status::PartiallyFilled;
    }
    bool cancel(OrderId id) {
        auto it = where.find(id);
        if (it == where.end()) return false;
        auto erase_from = [&](auto& book) {
            auto& q = book[it->second.second];
            for (auto o = q.begin(); o != q.end(); ++o) if (o->id == id) { q.erase(o); break; }
            if (q.empty()) book.erase(it->second.second);
        };
        if (it->second.first == Side::Buy) erase_from(bids); else erase_from(asks);
        where.erase(it);
        return true;
    }
};

TEST_CASE("differential: engine matches naive reference") {
    std::mt19937_64 rng(12345);
    Book b({1, 2000, 1 << 16});
    RefBook ref;
    std::vector<OrderId> live;
    OrderId next = 1;
    for (int step = 0; step < 300000; ++step) {
        int op = static_cast<int>(rng() % 10);
        if (op < 7 || live.empty()) {
            Side s = rng() & 1 ? Side::Buy : Side::Sell;
            Price p = 1000 + static_cast<Price>(rng() % 41) - 20;
            Qty q = 1 + static_cast<Qty>(rng() % 50);
            auto tif = static_cast<TimeInForce>(rng() % 10 < 8 ? 0 : 1 + rng() % 2);
            OrderRequest r{next++, s, p, q, tif};
            auto got = b.submit(r).status;
            auto want = ref.submit(r);
            REQUIRE(got == want);
            if (got == Status::Resting || got == Status::PartiallyFilled) live.push_back(r.id);
        } else {
            std::size_t k = rng() % live.size();
            OrderId id = live[k];
            live[k] = live.back();
            live.pop_back();
            REQUIRE(b.cancel(id) == ref.cancel(id));
        }
        REQUIRE(b.has_bid() == !ref.bids.empty());
        REQUIRE(b.has_ask() == !ref.asks.empty());
        if (b.has_bid()) REQUIRE(b.best_bid() == ref.bids.begin()->first);
        if (b.has_ask()) REQUIRE(b.best_ask() == ref.asks.begin()->first);
    }
    auto& t = b.listener().trades;
    REQUIRE(t.size() == ref.trades.size());
    for (std::size_t i = 0; i < t.size(); ++i) {
        REQUIRE(t[i].maker == ref.trades[i].maker);
        REQUIRE(t[i].taker == ref.trades[i].taker);
        REQUIRE(t[i].price == ref.trades[i].price);
        REQUIRE(t[i].qty == ref.trades[i].qty);
    }
    CHECK(b.order_count() == ref.where.size());
}

TEST_CASE("level bitset boundaries") {
    LevelBitset bs(10000);
    CHECK(bs.find_next(0) == LevelBitset::npos);
    CHECK(bs.find_prev(9999) == LevelBitset::npos);
    for (std::size_t i : {0u, 63u, 64u, 4095u, 4096u, 9999u}) bs.set(i);
    CHECK(bs.find_next(1) == 63);
    CHECK(bs.find_next(65) == 4095);
    CHECK(bs.find_next(4097) == 9999);
    CHECK(bs.find_prev(9998) == 4096);
    CHECK(bs.find_prev(62) == 0);
    bs.clear(4096);
    CHECK(bs.find_prev(9998) == 4095);
}

TEST_CASE("flat id map churn") {
    FlatIdMap m(4);
    for (OrderId i = 0; i < 10000; ++i) CHECK(m.insert(i * 7919, static_cast<Index>(i)));
    for (OrderId i = 0; i < 10000; i += 2) CHECK(m.erase(i * 7919));
    for (OrderId i = 0; i < 10000; ++i) {
        auto v = m.find(i * 7919);
        CHECK(static_cast<bool>(v) == (i % 2 == 1));
        if (v) CHECK(*v == i);
    }
    CHECK(m.size() == 5000);
}
