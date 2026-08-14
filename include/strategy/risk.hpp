#pragma once

#include <cstdint>

#include "alpha/signals.hpp"
#include "common/types.hpp"
#include "strategy/avellaneda_stoikov.hpp"

namespace me::strategy {

enum class RiskAction : std::uint8_t {
    QuoteBoth = 0,
    BidOnly = 1,
    AskOnly = 2,
    PullQuotes = 3,
    KillSwitch = 4,
};

enum class RiskReason : std::uint8_t {
    None = 0,
    InvalidQuote = 1,
    ToxicFlow = 2,
    InventoryLimit = 3,
    NotionalLimit = 4,
    DrawdownLimit = 5,
    ManualKill = 6,
};

struct RiskConfig {
    std::int32_t max_abs_inventory{1'000};
    std::int32_t soft_abs_inventory{700};
    Qty base_quote_quantity{100};
    Qty max_quote_quantity{500};
    std::uint64_t max_gross_notional{250'000'000u};
    std::int64_t max_drawdown_ticks{50'000};
    double toxicity_pause_zscore{3.0};
};

struct RiskState {
    std::int32_t inventory{0};
    std::uint64_t gross_notional{0};
    std::int64_t realized_pnl_ticks{0};
    std::int64_t peak_pnl_ticks{0};
    bool manual_kill{false};
};

struct RiskDecision {
    RiskAction action{RiskAction::PullQuotes};
    RiskReason reason{RiskReason::InvalidQuote};
    Qty bid_quantity{0};
    Qty ask_quantity{0};
};

class RiskController {
private:
    RiskConfig config_{};

public:
    explicit RiskController(const RiskConfig& config = {}) noexcept
        : config_(sanitize(config)) {}

    [[nodiscard]] inline RiskDecision evaluate(const Quote& quote,
                                               const alpha::FeatureFrame& frame,
                                               const RiskState& state) const noexcept {
        if (state.manual_kill) [[unlikely]] {
            return {RiskAction::KillSwitch, RiskReason::ManualKill, 0u, 0u};
        }
        if (!valid_quote(quote)) [[unlikely]] {
            return {RiskAction::PullQuotes, RiskReason::InvalidQuote, 0u, 0u};
        }
        if (drawdown_ticks(state) >= config_.max_drawdown_ticks) [[unlikely]] {
            return {RiskAction::KillSwitch, RiskReason::DrawdownLimit, 0u, 0u};
        }
        if (state.gross_notional >= config_.max_gross_notional) [[unlikely]] {
            return {RiskAction::PullQuotes, RiskReason::NotionalLimit, 0u, 0u};
        }
        if (toxicity_zscore(frame) >= config_.toxicity_pause_zscore) [[unlikely]] {
            return {RiskAction::PullQuotes, RiskReason::ToxicFlow, 0u, 0u};
        }

        const Qty bid_size = clipped_quantity(scaled_bid_quantity(state.inventory));
        const Qty ask_size = clipped_quantity(scaled_ask_quantity(state.inventory));

        if (state.inventory >= config_.max_abs_inventory) [[unlikely]] {
            return {RiskAction::AskOnly, RiskReason::InventoryLimit, 0u, ask_size};
        }
        if (state.inventory <= -config_.max_abs_inventory) [[unlikely]] {
            return {RiskAction::BidOnly, RiskReason::InventoryLimit, bid_size, 0u};
        }
        if (bid_size == 0u && ask_size == 0u) [[unlikely]] {
            return {RiskAction::PullQuotes, RiskReason::InventoryLimit, 0u, 0u};
        }
        if (bid_size == 0u) {
            return {RiskAction::AskOnly, RiskReason::InventoryLimit, 0u, ask_size};
        }
        if (ask_size == 0u) {
            return {RiskAction::BidOnly, RiskReason::InventoryLimit, bid_size, 0u};
        }

        return {RiskAction::QuoteBoth, RiskReason::None, bid_size, ask_size};
    }

    [[nodiscard]] inline const RiskConfig& config() const noexcept {
        return config_;
    }

    [[nodiscard]] static inline const char* to_string(const RiskAction action) noexcept {
        switch (action) {
            case RiskAction::QuoteBoth:
                return "quote_both";
            case RiskAction::BidOnly:
                return "bid_only";
            case RiskAction::AskOnly:
                return "ask_only";
            case RiskAction::PullQuotes:
                return "pull_quotes";
            case RiskAction::KillSwitch:
                return "kill_switch";
        }
        return "unknown";
    }

    [[nodiscard]] static inline const char* to_string(const RiskReason reason) noexcept {
        switch (reason) {
            case RiskReason::None:
                return "none";
            case RiskReason::InvalidQuote:
                return "invalid_quote";
            case RiskReason::ToxicFlow:
                return "toxic_flow";
            case RiskReason::InventoryLimit:
                return "inventory_limit";
            case RiskReason::NotionalLimit:
                return "notional_limit";
            case RiskReason::DrawdownLimit:
                return "drawdown_limit";
            case RiskReason::ManualKill:
                return "manual_kill";
        }
        return "unknown";
    }

private:
    [[nodiscard]] static inline RiskConfig sanitize(RiskConfig config) noexcept {
        if (config.max_abs_inventory < 1) {
            config.max_abs_inventory = 1;
        }
        if (config.soft_abs_inventory < 0) {
            config.soft_abs_inventory = 0;
        }
        if (config.soft_abs_inventory > config.max_abs_inventory) {
            config.soft_abs_inventory = config.max_abs_inventory;
        }
        if (config.base_quote_quantity == 0u) {
            config.base_quote_quantity = 1u;
        }
        if (config.max_quote_quantity == 0u) {
            config.max_quote_quantity = config.base_quote_quantity;
        }
        if (config.base_quote_quantity > config.max_quote_quantity) {
            config.base_quote_quantity = config.max_quote_quantity;
        }
        if (config.max_gross_notional == 0u) {
            config.max_gross_notional = 1u;
        }
        if (config.max_drawdown_ticks < 1) {
            config.max_drawdown_ticks = 1;
        }
        if (config.toxicity_pause_zscore <= 0.0) {
            config.toxicity_pause_zscore = 1.0;
        }
        return config;
    }

    [[nodiscard]] static inline bool valid_quote(const Quote& quote) noexcept {
        return quote.bid_price > 0.0 &&
               quote.ask_price > 0.0 &&
               quote.ask_price > quote.bid_price;
    }

    [[nodiscard]] static inline std::int64_t drawdown_ticks(const RiskState& state) noexcept {
        if (state.peak_pnl_ticks <= state.realized_pnl_ticks) {
            return 0;
        }
        return state.peak_pnl_ticks - state.realized_pnl_ticks;
    }

    [[nodiscard]] static inline double toxicity_zscore(const alpha::FeatureFrame& frame) noexcept {
        if (frame.vpin_sigma <= 1.0e-12) {
            return 0.0;
        }
        const double zscore = (frame.vpin - frame.vpin_mean) / frame.vpin_sigma;
        return zscore > 0.0 ? zscore : 0.0;
    }

    [[nodiscard]] inline Qty clipped_quantity(const Qty quantity) const noexcept {
        if (quantity > config_.max_quote_quantity) {
            return config_.max_quote_quantity;
        }
        return quantity;
    }

    [[nodiscard]] inline Qty scaled_bid_quantity(const std::int32_t inventory) const noexcept {
        if (inventory >= config_.soft_abs_inventory) {
            const std::int32_t remaining = config_.max_abs_inventory - inventory;
            return scaled_quantity_from_remaining(remaining);
        }
        return config_.base_quote_quantity;
    }

    [[nodiscard]] inline Qty scaled_ask_quantity(const std::int32_t inventory) const noexcept {
        if (inventory <= -config_.soft_abs_inventory) {
            const std::int32_t remaining = config_.max_abs_inventory + inventory;
            return scaled_quantity_from_remaining(remaining);
        }
        return config_.base_quote_quantity;
    }

    [[nodiscard]] inline Qty scaled_quantity_from_remaining(const std::int32_t remaining) const noexcept {
        if (remaining <= 0) {
            return 0u;
        }

        const std::int32_t span = config_.max_abs_inventory - config_.soft_abs_inventory;
        if (span <= 0) {
            return config_.base_quote_quantity;
        }

        const auto numerator = static_cast<std::uint64_t>(remaining) * config_.base_quote_quantity;
        const auto quantity = static_cast<Qty>(numerator / static_cast<std::uint64_t>(span));
        return quantity == 0u ? 1u : quantity;
    }
};

}  // namespace me::strategy

