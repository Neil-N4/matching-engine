#pragma once

#include <cstdint>
#include <limits>

namespace me {

using OrderID = std::uint64_t;
using Price = std::uint32_t;
using Qty = std::uint32_t;
using Timestamp = std::uint64_t;

enum class Side : std::uint8_t {
    Buy = 0,
    Sell = 1,
};

enum class EventType : std::uint8_t {
    Add = 0,
    Cancel = 1,
    Execute = 2,
    Quote = 3,
};

enum class BookStatus : std::uint8_t {
    Accepted = 0,
    Filled = 1,
    Partial = 2,
    NotFound = 3,
    Duplicate = 4,
    InvalidQuantity = 5,
    OrderPoolExhausted = 6,
    OrderDirectoryFull = 7,
    PriceLevelFull = 8,
};

struct alignas(64) MarketEvent {
    Timestamp timestamp{0};
    Price price{0};
    Qty quantity{0};
    Price best_bid{0};
    Price best_ask{0};
    Qty best_bid_quantity{0};
    Qty best_ask_quantity{0};
    Side side{Side::Buy};
    EventType type{EventType::Add};
};

static_assert(alignof(MarketEvent) == 64);

inline constexpr char side_to_char(const Side side) noexcept {
    return side == Side::Buy ? 'B' : 'S';
}

inline constexpr Side opposite(const Side side) noexcept {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

inline constexpr bool is_bid(const Side side) noexcept {
    return side == Side::Buy;
}

inline constexpr Price invalid_price() noexcept {
    return std::numeric_limits<Price>::max();
}

}  // namespace me

