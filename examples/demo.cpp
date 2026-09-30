#include <lob/order_book.hpp>
#include <cstdio>

struct Printer {
    void on_trade(const lob::Trade& t) {
        std::printf("TRADE maker=%llu taker=%llu %s %lld @ %lld\n",
                    (unsigned long long)t.maker, (unsigned long long)t.taker,
                    t.taker_side == lob::Side::Buy ? "BUY" : "SELL",
                    (long long)t.qty, (long long)t.price);
    }
};

int main() {
    using namespace lob;
    OrderBook<Printer> book({1, 20'000, 1024});
    book.submit({1, Side::Sell, 10'050, 100});
    book.submit({2, Side::Sell, 10'049, 40});
    book.submit({3, Side::Buy, 10'000, 70});
    std::printf("top of book: %lld x %lld\n", (long long)book.best_bid(), (long long)book.best_ask());
    auto r = book.submit({4, Side::Buy, 10'050, 90});
    std::printf("order 4 filled %lld, status=%s\n", (long long)r.filled, to_string(r.status));
    std::printf("top of book: %lld x %lld\n", (long long)book.best_bid(), (long long)book.best_ask());
}
