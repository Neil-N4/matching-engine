#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>

#include "engine/order_book.hpp"

#ifdef MATCHING_ENGINE_WITH_GOOGLE_BENCHMARK
#include <benchmark/benchmark.h>

namespace {

constexpr std::size_t kOrders = 1u << 18u;
using BenchBook = me::OrderBook<1u << 19u, 1u << 20u, 1u << 12u>;

BenchBook& prepared_book() {
    static BenchBook book;
    static bool initialized = false;
    if (!initialized) {
        for (std::size_t i = 0; i < kOrders; ++i) {
            static_cast<void>(book.add_order(static_cast<me::OrderID>(i + 1u),
                                             me::Side::Sell,
                                             1'001'000u,
                                             1u));
        }
        initialized = true;
    }
    return book;
}

void BM_OrderToFill(benchmark::State& state) {
    BenchBook& book = prepared_book();
    for (auto _ : state) {
        benchmark::DoNotOptimize(book.match_market_order(me::Side::Buy, 1u));
    }
}

}  // namespace

BENCHMARK(BM_OrderToFill);
BENCHMARK_MAIN();

#else

namespace {

constexpr std::size_t kIterations = 200'000u;
using BenchBook = me::OrderBook<1u << 20u, 1u << 21u, 1u << 12u>;

[[nodiscard]] std::uint64_t nanos_since(const std::chrono::steady_clock::time_point start,
                                        const std::chrono::steady_clock::time_point end) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

}  // namespace

int main() {
    static BenchBook book;
    static std::array<std::uint64_t, kIterations> samples{};

    for (std::size_t i = 0; i < kIterations; ++i) {
        static_cast<void>(book.add_order(static_cast<me::OrderID>(i + 1u),
                                         me::Side::Sell,
                                         1'001'000u,
                                         1u));
    }

    const auto benchmark_start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kIterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        const me::ExecutionReport report = book.match_market_order(me::Side::Buy, 1u);
        const auto end = std::chrono::steady_clock::now();
        samples[i] = nanos_since(start, end);
        if (report.filled_quantity != 1u) [[unlikely]] {
            std::cerr << "benchmark book exhausted unexpectedly\n";
            return 1;
        }
    }
    const auto benchmark_end = std::chrono::steady_clock::now();

    std::sort(samples.begin(), samples.end());
    const std::uint64_t median = samples[kIterations / 2u];
    const std::uint64_t p99 = samples[(kIterations * 99u) / 100u];
    const double seconds =
        static_cast<double>(nanos_since(benchmark_start, benchmark_end)) / 1'000'000'000.0;
    const double throughput = static_cast<double>(kIterations) / seconds;

    std::cout << "iterations=" << kIterations
              << " median_ns=" << median
              << " p99_ns=" << p99
              << " throughput_events_per_sec=" << throughput
              << '\n';
    return 0;
}

#endif

