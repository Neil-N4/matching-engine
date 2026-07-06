#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "common/config.hpp"
#include "common/types.hpp"

namespace me::alpha {

struct alignas(64) FeatureFrame {
    Timestamp timestamp{0};
    Price best_bid{0};
    Price best_ask{0};
    Qty best_bid_quantity{0};
    Qty best_ask_quantity{0};
    double obi{0.0};
    double micro_price{0.0};
    double vpin{0.0};
    double vpin_mean{0.0};
    double vpin_sigma{0.0};
};

class alignas(64) AtomicFeatureFrame {
private:
    alignas(64) std::atomic<std::uint64_t> sequence_{0};
    alignas(64) std::atomic<Timestamp> timestamp_{0};
    std::atomic<Price> best_bid_{0};
    std::atomic<Price> best_ask_{0};
    std::atomic<Qty> best_bid_quantity_{0};
    std::atomic<Qty> best_ask_quantity_{0};
    std::atomic<std::uint64_t> obi_bits_{pack_double(0.0)};
    std::atomic<std::uint64_t> micro_price_bits_{pack_double(0.0)};
    std::atomic<std::uint64_t> vpin_bits_{pack_double(0.0)};
    std::atomic<std::uint64_t> vpin_mean_bits_{pack_double(0.0)};
    std::atomic<std::uint64_t> vpin_sigma_bits_{pack_double(0.0)};

public:
    AtomicFeatureFrame() = default;
    AtomicFeatureFrame(const AtomicFeatureFrame&) = delete;
    AtomicFeatureFrame& operator=(const AtomicFeatureFrame&) = delete;

    inline void publish(const FeatureFrame& frame) noexcept {
        const std::uint64_t seq = sequence_.load(std::memory_order_relaxed);
        sequence_.store(seq + 1u, std::memory_order_release);

        timestamp_.store(frame.timestamp, std::memory_order_relaxed);
        best_bid_.store(frame.best_bid, std::memory_order_relaxed);
        best_ask_.store(frame.best_ask, std::memory_order_relaxed);
        best_bid_quantity_.store(frame.best_bid_quantity, std::memory_order_relaxed);
        best_ask_quantity_.store(frame.best_ask_quantity, std::memory_order_relaxed);
        obi_bits_.store(pack_double(frame.obi), std::memory_order_relaxed);
        micro_price_bits_.store(pack_double(frame.micro_price), std::memory_order_relaxed);
        vpin_bits_.store(pack_double(frame.vpin), std::memory_order_relaxed);
        vpin_mean_bits_.store(pack_double(frame.vpin_mean), std::memory_order_relaxed);
        vpin_sigma_bits_.store(pack_double(frame.vpin_sigma), std::memory_order_relaxed);

        sequence_.store(seq + 2u, std::memory_order_release);
    }

    [[nodiscard]] inline FeatureFrame read() const noexcept {
        FeatureFrame frame{};
        std::uint64_t before = 0;
        std::uint64_t after = 0;

        do {
            before = sequence_.load(std::memory_order_acquire);
            if ((before & 1u) != 0u) [[unlikely]] {
                continue;
            }

            frame.timestamp = timestamp_.load(std::memory_order_relaxed);
            frame.best_bid = best_bid_.load(std::memory_order_relaxed);
            frame.best_ask = best_ask_.load(std::memory_order_relaxed);
            frame.best_bid_quantity = best_bid_quantity_.load(std::memory_order_relaxed);
            frame.best_ask_quantity = best_ask_quantity_.load(std::memory_order_relaxed);
            frame.obi = unpack_double(obi_bits_.load(std::memory_order_relaxed));
            frame.micro_price = unpack_double(micro_price_bits_.load(std::memory_order_relaxed));
            frame.vpin = unpack_double(vpin_bits_.load(std::memory_order_relaxed));
            frame.vpin_mean = unpack_double(vpin_mean_bits_.load(std::memory_order_relaxed));
            frame.vpin_sigma = unpack_double(vpin_sigma_bits_.load(std::memory_order_relaxed));

            after = sequence_.load(std::memory_order_acquire);
        } while (before != after || (after & 1u) != 0u);

        return frame;
    }

private:
    [[nodiscard]] static constexpr std::uint64_t pack_double(const double value) noexcept {
        return std::bit_cast<std::uint64_t>(value);
    }

    [[nodiscard]] static constexpr double unpack_double(const std::uint64_t value) noexcept {
        return std::bit_cast<double>(value);
    }
};

template <Qty BucketVolume = config::kVpinBucketVolume,
          std::size_t WindowBuckets = config::kVpinWindowBuckets>
class alignas(64) OnlineSignals {
    static_assert(BucketVolume > 0u, "BucketVolume must be non-zero");
    static_assert(WindowBuckets > 0u, "WindowBuckets must be non-zero");

private:
    std::array<double, WindowBuckets> bucket_toxicity_{};
    std::size_t next_bucket_{0};
    std::size_t filled_buckets_{0};
    double rolling_sum_{0.0};
    double rolling_sum_sq_{0.0};
    Qty bucket_buy_volume_{0};
    Qty bucket_sell_volume_{0};
    Qty bucket_total_volume_{0};
    FeatureFrame last_{};

public:
    [[nodiscard]] inline FeatureFrame on_event(const MarketEvent& event) noexcept {
        if (event.type == EventType::Execute) [[likely]] {
            update_vpin(event);
        }

        FeatureFrame frame{};
        frame.timestamp = event.timestamp;
        frame.best_bid = event.best_bid;
        frame.best_ask = event.best_ask;
        frame.best_bid_quantity = event.best_bid_quantity;
        frame.best_ask_quantity = event.best_ask_quantity;
        frame.obi = compute_obi(event.best_bid_quantity, event.best_ask_quantity);
        frame.micro_price = compute_micro_price(event.best_bid,
                                                event.best_ask,
                                                event.best_bid_quantity,
                                                event.best_ask_quantity);
        frame.vpin = current_vpin();
        frame.vpin_mean = current_mean();
        frame.vpin_sigma = current_sigma();
        last_ = frame;
        return frame;
    }

    [[nodiscard]] inline FeatureFrame last() const noexcept {
        return last_;
    }

    [[nodiscard]] inline double current_vpin() const noexcept {
        if (filled_buckets_ == 0u) [[unlikely]] {
            return 0.0;
        }
        return rolling_sum_ / static_cast<double>(filled_buckets_);
    }

    [[nodiscard]] inline double current_mean() const noexcept {
        return current_vpin();
    }

    [[nodiscard]] inline double current_sigma() const noexcept {
        if (filled_buckets_ < 2u) [[unlikely]] {
            return 0.0;
        }

        const double mean = current_mean();
        const double variance = (rolling_sum_sq_ / static_cast<double>(filled_buckets_)) - (mean * mean);
        return variance <= 0.0 ? 0.0 : std::sqrt(variance);
    }

    [[nodiscard]] static inline double compute_obi(const Qty bid_volume, const Qty ask_volume) noexcept {
        const std::uint64_t denom = static_cast<std::uint64_t>(bid_volume) + ask_volume;
        if (denom == 0u) [[unlikely]] {
            return 0.0;
        }
        return (static_cast<double>(bid_volume) - static_cast<double>(ask_volume)) /
               static_cast<double>(denom);
    }

    [[nodiscard]] static inline double compute_micro_price(const Price bid,
                                                           const Price ask,
                                                           const Qty bid_volume,
                                                           const Qty ask_volume) noexcept {
        const std::uint64_t denom = static_cast<std::uint64_t>(bid_volume) + ask_volume;
        if (denom == 0u || bid == 0u || ask == 0u) [[unlikely]] {
            return 0.0;
        }

        const double weighted_ticks =
            (static_cast<double>(ask) * static_cast<double>(bid_volume) +
             static_cast<double>(bid) * static_cast<double>(ask_volume)) /
            static_cast<double>(denom);
        return weighted_ticks / config::kPriceScale;
    }

private:
    inline void update_vpin(const MarketEvent& event) noexcept {
        Qty remaining = event.quantity;
        const bool buyer_initiated = event.side == Side::Sell;

        while (remaining > 0u) {
            const Qty space = BucketVolume - bucket_total_volume_;
            const Qty take = remaining < space ? remaining : space;

            if (buyer_initiated) {
                bucket_buy_volume_ += take;
            } else {
                bucket_sell_volume_ += take;
            }

            bucket_total_volume_ += take;
            remaining -= take;

            if (bucket_total_volume_ == BucketVolume) [[unlikely]] {
                close_bucket();
            }
        }
    }

    inline void close_bucket() noexcept {
        const double imbalance =
            abs_diff(bucket_buy_volume_, bucket_sell_volume_) / static_cast<double>(BucketVolume);

        if (filled_buckets_ == WindowBuckets) {
            const double old = bucket_toxicity_[next_bucket_];
            rolling_sum_ -= old;
            rolling_sum_sq_ -= old * old;
        } else {
            ++filled_buckets_;
        }

        bucket_toxicity_[next_bucket_] = imbalance;
        rolling_sum_ += imbalance;
        rolling_sum_sq_ += imbalance * imbalance;
        next_bucket_ = (next_bucket_ + 1u) % WindowBuckets;

        bucket_buy_volume_ = 0u;
        bucket_sell_volume_ = 0u;
        bucket_total_volume_ = 0u;
    }

    [[nodiscard]] static inline double abs_diff(const Qty lhs, const Qty rhs) noexcept {
        return lhs >= rhs ? static_cast<double>(lhs - rhs) : static_cast<double>(rhs - lhs);
    }
};

}  // namespace me::alpha

