#pragma once

#include <cstdint>

#include "common/types.hpp"
#include "strategy/risk.hpp"

namespace me::strategy {

struct Fill {
    Timestamp timestamp{0};
    Side side{Side::Buy};
    Price price{0};
    Qty quantity{0};
};

struct PositionSnapshot {
    std::int32_t inventory{0};
    std::int64_t open_cost_ticks{0};
    std::int64_t realized_pnl_ticks{0};
    std::int64_t unrealized_pnl_ticks{0};
    std::int64_t total_pnl_ticks{0};
    std::int64_t peak_pnl_ticks{0};
    std::int64_t drawdown_ticks{0};
    std::uint64_t gross_notional{0};
    Price average_entry_price{0};
    Timestamp last_fill_timestamp{0};
};

class PositionTracker {
private:
    std::int32_t inventory_{0};
    std::int64_t open_cost_ticks_{0};
    std::int64_t realized_pnl_ticks_{0};
    std::int64_t peak_pnl_ticks_{0};
    std::uint64_t gross_notional_{0};
    Timestamp last_fill_timestamp_{0};

public:
    [[nodiscard]] inline bool on_fill(const Fill& fill) noexcept {
        if (fill.quantity == 0u || fill.price == 0u) [[unlikely]] {
            return false;
        }

        last_fill_timestamp_ = fill.timestamp;
        gross_notional_ += notional(fill.price, fill.quantity);

        if (fill.side == Side::Buy) {
            apply_buy(fill.price, fill.quantity);
        } else {
            apply_sell(fill.price, fill.quantity);
        }

        update_peak(realized_pnl_ticks_);
        return true;
    }

    [[nodiscard]] inline PositionSnapshot mark_to_market(const Price mark_price) noexcept {
        const std::int64_t unrealized = unrealized_pnl(mark_price);
        const std::int64_t total = realized_pnl_ticks_ + unrealized;
        update_peak(total);
        return snapshot(unrealized, total);
    }

    [[nodiscard]] inline PositionSnapshot snapshot(const Price mark_price) const noexcept {
        const std::int64_t unrealized = unrealized_pnl(mark_price);
        const std::int64_t total = realized_pnl_ticks_ + unrealized;
        return snapshot(unrealized, total);
    }

    [[nodiscard]] inline RiskState risk_state(const Price mark_price, const bool manual_kill = false) noexcept {
        const PositionSnapshot snap = mark_to_market(mark_price);
        return RiskState{
            snap.inventory,
            snap.gross_notional,
            snap.realized_pnl_ticks,
            snap.unrealized_pnl_ticks,
            snap.peak_pnl_ticks,
            manual_kill,
        };
    }

    [[nodiscard]] inline std::int32_t inventory() const noexcept {
        return inventory_;
    }

    [[nodiscard]] inline std::int64_t realized_pnl_ticks() const noexcept {
        return realized_pnl_ticks_;
    }

    [[nodiscard]] inline std::uint64_t gross_notional() const noexcept {
        return gross_notional_;
    }

private:
    [[nodiscard]] static inline std::uint64_t notional(const Price price, const Qty quantity) noexcept {
        return static_cast<std::uint64_t>(price) * static_cast<std::uint64_t>(quantity);
    }

    [[nodiscard]] static inline std::int32_t abs_inventory(const std::int32_t inventory) noexcept {
        return inventory < 0 ? -inventory : inventory;
    }

    [[nodiscard]] inline Price average_entry_price() const noexcept {
        const std::int32_t abs_inv = abs_inventory(inventory_);
        if (abs_inv == 0) {
            return 0u;
        }

        const std::int64_t abs_cost = open_cost_ticks_ < 0 ? -open_cost_ticks_ : open_cost_ticks_;
        return static_cast<Price>(abs_cost / abs_inv);
    }

    [[nodiscard]] inline std::int64_t proportional_open_cost(const Qty close_quantity) const noexcept {
        const std::int32_t abs_inv = abs_inventory(inventory_);
        if (abs_inv == 0) [[unlikely]] {
            return 0;
        }

        const std::int64_t abs_cost = open_cost_ticks_ < 0 ? -open_cost_ticks_ : open_cost_ticks_;
        if (close_quantity >= static_cast<Qty>(abs_inv)) {
            return abs_cost;
        }

        return (abs_cost * static_cast<std::int64_t>(close_quantity)) / abs_inv;
    }

    inline void apply_buy(const Price price, Qty quantity) noexcept {
        if (inventory_ < 0) {
            const Qty close_quantity = quantity < static_cast<Qty>(-inventory_)
                ? quantity
                : static_cast<Qty>(-inventory_);
            const std::int64_t closed_cost = proportional_open_cost(close_quantity);
            const std::int64_t fill_notional = static_cast<std::int64_t>(notional(price, close_quantity));

            realized_pnl_ticks_ += closed_cost - fill_notional;
            open_cost_ticks_ += closed_cost;
            inventory_ += static_cast<std::int32_t>(close_quantity);
            quantity -= close_quantity;

            if (inventory_ == 0) {
                open_cost_ticks_ = 0;
            }
        }

        if (quantity > 0u) {
            open_cost_ticks_ += static_cast<std::int64_t>(notional(price, quantity));
            inventory_ += static_cast<std::int32_t>(quantity);
        }
    }

    inline void apply_sell(const Price price, Qty quantity) noexcept {
        if (inventory_ > 0) {
            const Qty close_quantity = quantity < static_cast<Qty>(inventory_)
                ? quantity
                : static_cast<Qty>(inventory_);
            const std::int64_t closed_cost = proportional_open_cost(close_quantity);
            const std::int64_t fill_notional = static_cast<std::int64_t>(notional(price, close_quantity));

            realized_pnl_ticks_ += fill_notional - closed_cost;
            open_cost_ticks_ -= closed_cost;
            inventory_ -= static_cast<std::int32_t>(close_quantity);
            quantity -= close_quantity;

            if (inventory_ == 0) {
                open_cost_ticks_ = 0;
            }
        }

        if (quantity > 0u) {
            open_cost_ticks_ -= static_cast<std::int64_t>(notional(price, quantity));
            inventory_ -= static_cast<std::int32_t>(quantity);
        }
    }

    [[nodiscard]] inline std::int64_t unrealized_pnl(const Price mark_price) const noexcept {
        if (inventory_ == 0 || mark_price == 0u) {
            return 0;
        }

        const std::int64_t mark_notional =
            static_cast<std::int64_t>(mark_price) * static_cast<std::int64_t>(abs_inventory(inventory_));
        const std::int64_t abs_cost = open_cost_ticks_ < 0 ? -open_cost_ticks_ : open_cost_ticks_;

        if (inventory_ > 0) {
            return mark_notional - abs_cost;
        }
        return abs_cost - mark_notional;
    }

    inline void update_peak(const std::int64_t pnl_ticks) noexcept {
        if (pnl_ticks > peak_pnl_ticks_) {
            peak_pnl_ticks_ = pnl_ticks;
        }
    }

    [[nodiscard]] inline PositionSnapshot snapshot(const std::int64_t unrealized,
                                                   const std::int64_t total) const noexcept {
        const std::int64_t drawdown = peak_pnl_ticks_ > total ? peak_pnl_ticks_ - total : 0;
        return PositionSnapshot{
            inventory_,
            open_cost_ticks_,
            realized_pnl_ticks_,
            unrealized,
            total,
            peak_pnl_ticks_,
            drawdown,
            gross_notional_,
            average_entry_price(),
            last_fill_timestamp_,
        };
    }
};

}  // namespace me::strategy
