#pragma once
// Price-time priority limit order book with a direct-indexed price ladder.
//
// Hot-path properties:
//   * no heap allocation after construction (fixed order pool + free list)
//   * O(1) add, O(1) cancel, O(1) best bid/ask, O(fills) matching
//   * no virtual calls: events go to a Listener resolved at compile time
#include <cassert>
#include <concepts>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "flat_map.hpp"
#include "level_bitset.hpp"
#include "types.hpp"

namespace lob {

template <class L>
concept Listener = requires(L l, const Trade& t) { l.on_trade(t); };

struct NullListener { void on_trade(const Trade&) noexcept {} };

struct BookConfig {
    Price       min_price = 1;          // inclusive, ticks
    Price       max_price = 1 << 20;    // inclusive, ticks
    std::size_t max_orders = 1 << 20;   // resting-order capacity
};

template <Listener L = NullListener>
class OrderBook {
public:
    explicit OrderBook(BookConfig cfg = {}, L listener = {})
        : cfg_(validate(cfg)),
          levels_(static_cast<std::size_t>(cfg.max_price - cfg.min_price + 1)),
          bids_(levels_.size()), asks_(levels_.size()),
          pool_(cfg.max_orders), ids_(cfg.max_orders),
          listener_(std::move(listener)) {
        free_.reserve(cfg.max_orders);
        for (std::size_t i = cfg.max_orders; i-- > 0;) free_.push_back(static_cast<Index>(i));
    }

    ExecReport submit(const OrderRequest& r) {
        if (r.qty <= 0) return {Status::RejectedInvalidQty, 0};
        if (r.price < cfg_.min_price || r.price > cfg_.max_price) return {Status::RejectedInvalidPrice, 0};
        if (ids_.find(r.id)) return {Status::RejectedDuplicateId, 0};
        if (r.tif == TimeInForce::FOK && !can_fill(r)) return {Status::RejectedFokUnfillable, 0};
        if (r.tif == TimeInForce::GTC && free_.empty()) return {Status::RejectedCapacity, 0};

        Qty remaining = match(r);
        Qty filled = r.qty - remaining;
        if (remaining == 0) return {Status::Filled, filled};
        if (r.tif != TimeInForce::GTC) return {Status::Cancelled, filled};
        rest(r.id, r.side, r.price, remaining);
        return {filled ? Status::PartiallyFilled : Status::Resting, filled};
    }

    // Market order: IOC at the worst possible price on the book's band.
    ExecReport submit_market(OrderId id, Side side, Qty qty) {
        return submit({id, side, side == Side::Buy ? cfg_.max_price : cfg_.min_price, qty, TimeInForce::IOC});
    }

    bool cancel(OrderId id) {
        auto idx = ids_.find(id);
        if (!idx) return false;
        unlink(*idx);
        return true;
    }

    // Reducing quantity at the same price keeps time priority (exchange
    // convention). Any price change or size increase loses priority.
    ExecReport modify(OrderId id, Price new_price, Qty new_qty) {
        auto idx = ids_.find(id);
        if (!idx) return {Status::RejectedUnknownOrder, 0};
        if (new_price < cfg_.min_price || new_price > cfg_.max_price)
            return {Status::RejectedInvalidPrice, 0};   // validate before touching the order
        Order& o = pool_[*idx];
        if (new_qty <= 0) { unlink(*idx); return {Status::Cancelled, 0}; }
        if (new_price == o.price && new_qty <= o.qty) {
            level(o.price).total -= o.qty - new_qty;
            o.qty = new_qty;
            return {Status::Resting, 0};
        }
        Side side = o.side;
        unlink(*idx);
        return submit({id, side, new_price, new_qty, TimeInForce::GTC});
    }

    [[nodiscard]] bool  has_bid() const noexcept { return best_bid_ != LevelBitset::npos; }
    [[nodiscard]] bool  has_ask() const noexcept { return best_ask_ != LevelBitset::npos; }
    [[nodiscard]] Price best_bid() const noexcept { return to_price(best_bid_); }
    [[nodiscard]] Price best_ask() const noexcept { return to_price(best_ask_); }
    [[nodiscard]] Qty   depth_at(Price p) const noexcept {
        return (p < cfg_.min_price || p > cfg_.max_price) ? 0 : levels_[to_slot(p)].total;
    }
    [[nodiscard]] std::size_t order_count() const noexcept { return ids_.size(); }
    [[nodiscard]] L& listener() noexcept { return listener_; }

private:
    struct Order {
        OrderId id = 0;
        Qty     qty = 0;
        Price   price = 0;
        Index   prev = kNull, next = kNull;
        Side    side = Side::Buy;
    };
    struct Level {
        Index    head = kNull, tail = kNull;
        Qty      total = 0;
    };

    static BookConfig validate(BookConfig c) {
        if (c.min_price <= 0 || c.max_price < c.min_price || c.max_orders == 0 ||
            c.max_orders >= kNull || c.max_price - c.min_price > (Price{1} << 28))
            throw std::invalid_argument("invalid BookConfig");
        return c;
    }

    std::size_t to_slot(Price p) const noexcept { return static_cast<std::size_t>(p - cfg_.min_price); }
    Price to_price(std::size_t s) const noexcept { return cfg_.min_price + static_cast<Price>(s); }
    Level& level(Price p) noexcept { return levels_[to_slot(p)]; }

    bool crosses(Side taker, std::size_t slot, Price limit) const noexcept {
        Price p = to_price(slot);
        return taker == Side::Buy ? p <= limit : p >= limit;
    }

    // FOK pre-check: walk opposite levels until qty is covered or price stops crossing.
    bool can_fill(const OrderRequest& r) const noexcept {
        const LevelBitset& opp = r.side == Side::Buy ? asks_ : bids_;
        std::size_t s = r.side == Side::Buy ? best_ask_ : best_bid_;
        Qty need = r.qty;
        while (s != LevelBitset::npos && crosses(r.side, s, r.price)) {
            need -= levels_[s].total;
            if (need <= 0) return true;
            s = r.side == Side::Buy ? opp.find_next(s + 1) : (s == 0 ? LevelBitset::npos : opp.find_prev(s - 1));
        }
        return false;
    }

    Qty match(const OrderRequest& r) {
        Qty remaining = r.qty;
        const bool buy = r.side == Side::Buy;
        std::size_t& best = buy ? best_ask_ : best_bid_;
        while (remaining > 0 && best != LevelBitset::npos && crosses(r.side, best, r.price)) {
            Level& lv = levels_[best];
            const Price px = to_price(best);
            while (remaining > 0 && lv.head != kNull) {
                Index mi = lv.head;
                Order& m = pool_[mi];
                Qty q = m.qty < remaining ? m.qty : remaining;
                m.qty -= q;
                lv.total -= q;
                remaining -= q;
                listener_.on_trade(Trade{m.id, r.id, r.side, px, q});
                if (m.qty == 0) unlink(mi);   // may advance `best`
            }
        }
        return remaining;
    }

    void rest(OrderId id, Side side, Price price, Qty qty) {
        Index i = free_.back();
        free_.pop_back();
        std::size_t s = to_slot(price);
        Level& lv = levels_[s];
        pool_[i] = Order{id, qty, price, lv.tail, kNull, side};
        if (lv.tail != kNull) pool_[lv.tail].next = i; else lv.head = i;
        lv.tail = i;
        lv.total += qty;
        ids_.insert(id, i);
        if (side == Side::Buy) {
            bids_.set(s);
            if (best_bid_ == LevelBitset::npos || s > best_bid_) best_bid_ = s;
        } else {
            asks_.set(s);
            if (best_ask_ == LevelBitset::npos || s < best_ask_) best_ask_ = s;
        }
    }

    void unlink(Index i) {
        Order& o = pool_[i];
        std::size_t s = to_slot(o.price);
        Level& lv = levels_[s];
        if (o.prev != kNull) pool_[o.prev].next = o.next; else lv.head = o.next;
        if (o.next != kNull) pool_[o.next].prev = o.prev; else lv.tail = o.prev;
        lv.total -= o.qty;
        ids_.erase(o.id);
        free_.push_back(i);
        if (lv.head != kNull) return;
        lv.total = 0;
        if (o.side == Side::Buy) {
            bids_.clear(s);
            if (s == best_bid_) best_bid_ = s == 0 ? LevelBitset::npos : bids_.find_prev(s - 1);
        } else {
            asks_.clear(s);
            if (s == best_ask_) best_ask_ = asks_.find_next(s + 1);
        }
    }

    BookConfig          cfg_;
    std::vector<Level>  levels_;      // one ladder shared by both sides: a level is never two-sided
    LevelBitset         bids_, asks_;
    std::size_t         best_bid_ = LevelBitset::npos;
    std::size_t         best_ask_ = LevelBitset::npos;
    std::vector<Order>  pool_;
    std::vector<Index>  free_;
    FlatIdMap           ids_;
    L                   listener_;
};

}  // namespace lob
