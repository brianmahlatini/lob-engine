#pragma once
#include <cstdint>
#include <limits>

namespace lob {

using Price   = std::int64_t;   // integer ticks: never use floating point for prices
using Qty     = std::int64_t;
using OrderId = std::uint64_t;
using Index   = std::uint32_t;  // pool index: half the size of a pointer, better cache density

inline constexpr Index kNull = std::numeric_limits<Index>::max();

enum class Side : std::uint8_t { Buy, Sell };
enum class TimeInForce : std::uint8_t {
    GTC,  // good-till-cancel: rest any remainder
    IOC,  // immediate-or-cancel: fill what you can, cancel the rest
    FOK,  // fill-or-kill: fill entirely or do nothing
};

enum class Status : std::uint8_t {
    Resting,             // no fill, fully resting
    PartiallyFilled,     // some fill, remainder resting
    Filled,              // fully filled
    Cancelled,           // IOC remainder cancelled (fill may be > 0)
    RejectedDuplicateId,
    RejectedInvalidPrice,
    RejectedInvalidQty,
    RejectedFokUnfillable,
    RejectedCapacity,
    RejectedUnknownOrder,
};

struct OrderRequest {
    OrderId     id;
    Side        side;
    Price       price;  // limit price in ticks
    Qty         qty;
    TimeInForce tif = TimeInForce::GTC;
};

struct ExecReport {
    Status status;
    Qty    filled;
};

struct Trade {
    OrderId maker;
    OrderId taker;
    Side    taker_side;
    Price   price;      // always the maker's price
    Qty     qty;
};

constexpr const char* to_string(Status s) noexcept {
    switch (s) {
        case Status::Resting: return "Resting";
        case Status::PartiallyFilled: return "PartiallyFilled";
        case Status::Filled: return "Filled";
        case Status::Cancelled: return "Cancelled";
        case Status::RejectedDuplicateId: return "RejectedDuplicateId";
        case Status::RejectedInvalidPrice: return "RejectedInvalidPrice";
        case Status::RejectedInvalidQty: return "RejectedInvalidQty";
        case Status::RejectedFokUnfillable: return "RejectedFokUnfillable";
        case Status::RejectedCapacity: return "RejectedCapacity";
        case Status::RejectedUnknownOrder: return "RejectedUnknownOrder";
    }
    return "Unknown";
}

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

}  // namespace lob
