#pragma once

#include <cmath>
#include <cstdint>

#include "alpha/signals.hpp"
#include "common/config.hpp"

namespace me::strategy {

struct AvellanedaStoikovConfig {
    double gamma{config::kDefaultGamma};
    double kappa{config::kDefaultKappa};
    double volatility{config::kDefaultVolatility};
    double horizon{config::kDefaultHorizon};
    double toxicity_scale{config::kDefaultToxicityScale};
    double min_spread{1.0 / config::kPriceScale};
};

struct Quote {
    double reservation_price{0.0};
    double bid_price{0.0};
    double ask_price{0.0};
    double total_spread{0.0};
    double toxicity_multiplier{1.0};
};

class AvellanedaStoikovMarketMaker {
private:
    AvellanedaStoikovConfig config_{};

public:
    explicit AvellanedaStoikovMarketMaker(const AvellanedaStoikovConfig& config = {}) noexcept
        : config_(config) {}

    [[nodiscard]] inline Quote quote(const alpha::FeatureFrame& frame,
                                     const std::int32_t inventory,
                                     const double elapsed_fraction) const noexcept {
        const double s = reference_price(frame);
        const double time_remaining = remaining_time(elapsed_fraction);
        const double variance = config_.volatility * config_.volatility;
        const double gamma = config_.gamma <= 0.0 ? 1.0e-9 : config_.gamma;
        const double kappa = config_.kappa <= 0.0 ? 1.0e-9 : config_.kappa;

        const double reservation =
            s - static_cast<double>(inventory) * gamma * variance * time_remaining;

        double total_spread =
            gamma * variance * time_remaining +
            (2.0 / gamma) * std::log(1.0 + (gamma / kappa));
        if (total_spread < config_.min_spread) [[unlikely]] {
            total_spread = config_.min_spread;
        }

        const double toxicity = toxicity_multiplier(frame);
        total_spread *= toxicity;
        const double half_spread = 0.5 * total_spread;

        return Quote{
            reservation,
            reservation - half_spread,
            reservation + half_spread,
            total_spread,
            toxicity,
        };
    }

    [[nodiscard]] inline const AvellanedaStoikovConfig& config() const noexcept {
        return config_;
    }

private:
    [[nodiscard]] inline double remaining_time(const double elapsed_fraction) const noexcept {
        if (elapsed_fraction <= 0.0) {
            return config_.horizon;
        }
        if (elapsed_fraction >= 1.0) {
            return 0.0;
        }
        return config_.horizon * (1.0 - elapsed_fraction);
    }

    [[nodiscard]] inline double reference_price(const alpha::FeatureFrame& frame) const noexcept {
        if (frame.micro_price > 0.0) [[likely]] {
            return frame.micro_price;
        }
        if (frame.best_bid != 0u && frame.best_ask != 0u) {
            return (static_cast<double>(frame.best_bid) + static_cast<double>(frame.best_ask)) /
                   (2.0 * config::kPriceScale);
        }
        return 0.0;
    }

    [[nodiscard]] inline double toxicity_multiplier(const alpha::FeatureFrame& frame) const noexcept {
        if (frame.vpin_sigma <= 1.0e-12) {
            return 1.0;
        }

        const double z = (frame.vpin - frame.vpin_mean) / frame.vpin_sigma;
        if (z <= 0.0) {
            return 1.0;
        }
        return 1.0 + config_.toxicity_scale * z;
    }
};

}  // namespace me::strategy

