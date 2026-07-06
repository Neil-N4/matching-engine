#pragma once

#include <cstddef>
#include <cstdint>

#include "common/types.hpp"

namespace me::itch {

enum class MessageKind : std::uint8_t {
    Unknown = 0,
    AddOrder = 'A',
    OrderExecuted = 'E',
    OrderCancel = 'X',
};

struct ParsedMessage {
    MessageKind kind{MessageKind::Unknown};
    Timestamp timestamp{0};
    OrderID order_id{0};
    Side side{Side::Buy};
    Qty quantity{0};
    Price price{0};
};

namespace detail {

[[nodiscard]] inline std::uint64_t read_be(const std::byte* data, const std::size_t width) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        value = (value << 8u) | static_cast<std::uint64_t>(data[i]);
    }
    return value;
}

[[nodiscard]] inline Side parse_side(const std::byte value) noexcept {
    return static_cast<char>(value) == 'B' ? Side::Buy : Side::Sell;
}

}  // namespace detail

class ItchParser {
public:
    static constexpr std::size_t kTimestampOffset = 5u;
    static constexpr std::size_t kOrderIdOffset = 11u;
    static constexpr std::size_t kAddSideOffset = 19u;
    static constexpr std::size_t kAddQtyOffset = 20u;
    static constexpr std::size_t kAddPriceOffset = 32u;
    static constexpr std::size_t kExecutedQtyOffset = 19u;
    static constexpr std::size_t kCancelQtyOffset = 19u;

    static constexpr std::size_t kTimestampBytes = 6u;
    static constexpr std::size_t kOrderIdBytes = 8u;
    static constexpr std::size_t kQtyBytes = 4u;
    static constexpr std::size_t kPriceBytes = 4u;

    static constexpr std::size_t kAddOrderMinBytes = 36u;
    static constexpr std::size_t kOrderExecutedMinBytes = 31u;
    static constexpr std::size_t kOrderCancelMinBytes = 23u;

    [[nodiscard]] static inline bool parse(const std::byte* data,
                                           const std::size_t length,
                                           ParsedMessage& out) noexcept {
        if (data == nullptr || length == 0u) [[unlikely]] {
            return false;
        }

        const char type = static_cast<char>(data[0]);
        switch (type) {
            case 'A':
                return parse_add(data, length, out);
            case 'E':
                return parse_executed(data, length, out);
            case 'X':
                return parse_cancel(data, length, out);
            default:
                out = {};
                return false;
        }
    }

private:
    [[nodiscard]] static inline bool parse_add(const std::byte* data,
                                               const std::size_t length,
                                               ParsedMessage& out) noexcept {
        if (length < kAddOrderMinBytes) [[unlikely]] {
            return false;
        }

        out.kind = MessageKind::AddOrder;
        out.timestamp = detail::read_be(data + kTimestampOffset, kTimestampBytes);
        out.order_id = detail::read_be(data + kOrderIdOffset, kOrderIdBytes);
        out.side = detail::parse_side(data[kAddSideOffset]);
        out.quantity = static_cast<Qty>(detail::read_be(data + kAddQtyOffset, kQtyBytes));
        out.price = static_cast<Price>(detail::read_be(data + kAddPriceOffset, kPriceBytes));
        return true;
    }

    [[nodiscard]] static inline bool parse_executed(const std::byte* data,
                                                    const std::size_t length,
                                                    ParsedMessage& out) noexcept {
        if (length < kOrderExecutedMinBytes) [[unlikely]] {
            return false;
        }

        out.kind = MessageKind::OrderExecuted;
        out.timestamp = detail::read_be(data + kTimestampOffset, kTimestampBytes);
        out.order_id = detail::read_be(data + kOrderIdOffset, kOrderIdBytes);
        out.quantity = static_cast<Qty>(detail::read_be(data + kExecutedQtyOffset, kQtyBytes));
        out.price = 0u;
        return true;
    }

    [[nodiscard]] static inline bool parse_cancel(const std::byte* data,
                                                  const std::size_t length,
                                                  ParsedMessage& out) noexcept {
        if (length < kOrderCancelMinBytes) [[unlikely]] {
            return false;
        }

        out.kind = MessageKind::OrderCancel;
        out.timestamp = detail::read_be(data + kTimestampOffset, kTimestampBytes);
        out.order_id = detail::read_be(data + kOrderIdOffset, kOrderIdBytes);
        out.quantity = static_cast<Qty>(detail::read_be(data + kCancelQtyOffset, kQtyBytes));
        out.price = 0u;
        return true;
    }
};

}  // namespace me::itch

