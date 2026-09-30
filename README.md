# lob-engine

A header-only C++20 limit order book and matching engine with price-time priority, built for predictable low latency: no heap allocation on the hot path, O(1) add and cancel, O(1) best bid/ask, and zero virtual dispatch.

```
$ ./build/lob_bench 10000000
ops=10000000 time=0.717s throughput=13.94M ops/s  trades=1824112 volume=46526526
latency ns (incl. ~20ns clock overhead): p50=198 p90=396 p99=595 p99.9=781
```
*Single shared vCPU, GCC 13 `-O3`, synthetic flow: 35% cancels, ~10% aggressive orders crossing the spread, the rest passive adds within 50 ticks of mid.*

## Features

- Limit orders with GTC, IOC and FOK time-in-force; market orders
- Cancel, and modify with exchange-standard priority rules (size-down at the same price keeps queue position; reprice or size-up re-queues)
- Trades always execute at the resting (maker) price
- Explicit, typed results for every rejection: duplicate id, invalid price or qty, unfillable FOK, capacity, unknown order
- Compile-time event listener via a C++20 concept, so trade callbacks inline completely

## Architecture

```
  submit ──► validate ──► [FOK pre-check] ──► match against opposite side ──► rest remainder (GTC)
                                                   │
             direct-indexed price ladder:  levels_[price - min_price]
             each level = intrusive FIFO of pool indices (head, tail, total qty)
             two-level bitset per side → next non-empty level in a few word scans
             FlatIdMap: OrderId → pool index (open addressing, backward-shift delete)
```

| Component | Choice | Why |
|---|---|---|
| Price ladder | `std::vector<Level>` indexed by tick | O(1) level lookup; contiguous memory instead of `std::map` pointer chasing |
| Best-price search | Two-level 64-bit bitset + `countr_zero`/`countl_zero` | When the best level empties, the next is found in a handful of instructions even across wide gaps |
| Order storage | Pre-allocated pool with a free list; 32-bit indices | No `new`/`delete` after construction; half-size links fit more orders per cache line |
| Id index | Custom linear-probing map with backward-shift deletion | No tombstones, so heavy cancel churn does not degrade probe lengths the way it does with tombstone schemes |
| Events | `template <Listener L>` | Listener calls resolve statically and inline; no vtable or `std::function` overhead |

## Design decisions and trade-offs

**Integer ticks, never floating point.** Prices are `int64_t` ticks, so there are no rounding errors in matching or equality.

**Bounded price band.** The direct-indexed ladder needs a known `[min_price, max_price]`. Exchanges already enforce price bands and circuit breakers, so this matches reality; the cost is memory proportional to the band (about 24 bytes per tick for the shared ladder plus bits for each side). A single ladder is shared by both sides because a price can never hold bids and asks at once: an order that would make it two-sided matches first.

**Fixed capacity.** `max_orders` is allocated up front and orders beyond it are rejected with `RejectedCapacity` rather than triggering a reallocation mid-session. Predictable latency beats elastic growth for this workload.

**FOK pre-check.** Fill-or-kill walks opposite levels summing liquidity before touching the book, so a rejected FOK has no side effects and emits no trades.

**Modify validates first.** A reprice to an out-of-band price is rejected before the original order is unlinked, so a bad modify can never silently cancel a live order.

## Correctness

`tests/test_order_book.cpp` (doctest) covers priority, partial fills, all time-in-force modes, market orders, modify priority rules, every rejection path, and boundary cases for the bitset and id map.

The core safety net is a **differential test**: a deliberately simple `std::map` + `std::deque` reference book and the optimised engine are driven with the same 300,000 random operations. Every status, every trade (maker, taker, price, qty), and the top of book after every step must match exactly. CI runs the whole suite on GCC and Clang, with and without AddressSanitizer + UndefinedBehaviorSanitizer.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release     # add -DLOB_NATIVE=ON to tune for your CPU
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/lob_demo
./build/lob_bench 10000000
```

## Usage

```cpp
#include <lob/order_book.hpp>

struct MyListener { void on_trade(const lob::Trade& t) { /* publish to market data */ } };

lob::OrderBook<MyListener> book({.min_price = 1, .max_price = 1'000'000, .max_orders = 1 << 20});
auto r = book.submit({.id = 42, .side = lob::Side::Buy, .price = 10'050, .qty = 100});
book.cancel(42);
```

## Next steps

Multi-instrument routing with one book per core, an ITCH/OUCH-style binary protocol front end, self-trade prevention, and stop orders.

## License

MIT
