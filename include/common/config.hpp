#pragma once

#include <cstddef>
#include <cstdint>

namespace me::config {

inline constexpr std::size_t kDefaultMaxOrders = 1u << 20u;
inline constexpr std::size_t kDefaultOrderDirectoryCapacity = 1u << 22u;
inline constexpr std::size_t kDefaultPriceLevelCapacity = 1u << 16u;
inline constexpr std::size_t kTelemetryQueueCapacity = 1u << 16u;

inline constexpr std::uint32_t kVpinBucketVolume = 10'000u;
inline constexpr std::size_t kVpinWindowBuckets = 64u;

inline constexpr double kDefaultGamma = 0.10;
inline constexpr double kDefaultKappa = 1.50;
inline constexpr double kDefaultVolatility = 0.018;
inline constexpr double kDefaultHorizon = 1.0;
inline constexpr double kDefaultToxicityScale = 0.35;
inline constexpr double kPriceScale = 10'000.0;

}  // namespace me::config

