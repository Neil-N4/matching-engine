#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>

#include "alpha/signals.hpp"
#include "engine/order_book.hpp"
#include "engine/parser.hpp"
#include "strategy/avellaneda_stoikov.hpp"

namespace {

constexpr std::size_t kIterations = 200'000u;
constexpr std::size_t kFastOpsPerSample = 64u;
constexpr std::size_t kMessageRingSize = 4096u;
constexpr me::Price kAskPrice = 1'001'000u;
constexpr me::Price kBidPrice = 1'000'900u;

struct RawItchMessage {
    std::array<std::byte, me::itch::ItchParser::kAddOrderMinBytes> bytes{};
    std::size_t length{0};
};

void write_be(std::byte* out, std::uint64_t value, const std::size_t width) noexcept {
    for (std::size_t i = 0; i < width; ++i) {
        const std::size_t shift = (width - i - 1u) * 8u;
        out[i] = static_cast<std::byte>((value >> shift) & 0xffu);
    }
}

[[nodiscard]] RawItchMessage make_add(const me::Timestamp timestamp,
                                      const me::OrderID id,
                                      const me::Side side,
                                      const me::Qty quantity,
                                      const me::Price price) noexcept {
    RawItchMessage message{};
    message.length = me::itch::ItchParser::kAddOrderMinBytes;
    message.bytes[0] = static_cast<std::byte>('A');
    write_be(message.bytes.data() + me::itch::ItchParser::kTimestampOffset,
             timestamp,
             me::itch::ItchParser::kTimestampBytes);
    write_be(message.bytes.data() + me::itch::ItchParser::kOrderIdOffset,
             id,
             me::itch::ItchParser::kOrderIdBytes);
    message.bytes[me::itch::ItchParser::kAddSideOffset] =
        static_cast<std::byte>(me::side_to_char(side));
    write_be(message.bytes.data() + me::itch::ItchParser::kAddQtyOffset,
             quantity,
             me::itch::ItchParser::kQtyBytes);
    write_be(message.bytes.data() + me::itch::ItchParser::kAddPriceOffset,
             price,
             me::itch::ItchParser::kPriceBytes);
    return message;
}

[[nodiscard]] std::uint64_t nanos_since(const std::chrono::steady_clock::time_point start,
                                        const std::chrono::steady_clock::time_point end) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

}  // namespace

#ifdef MATCHING_ENGINE_WITH_GOOGLE_BENCHMARK
#include <benchmark/benchmark.h>

namespace {

using GoogleBenchBook = me::OrderBook<1u << 18u, 1u << 20u, 1u << 12u>;

void BM_HotFifoFill(benchmark::State& state) {
    static GoogleBenchBook book;
    static me::OrderID next_id = 1u;

    for (auto _ : state) {
        state.PauseTiming();
        const me::BookStatus add_status =
            book.add_order(next_id++, me::Side::Sell, kAskPrice, 1u);
        state.ResumeTiming();

        const me::ExecutionReport report = book.match_market_order(me::Side::Buy, 1u);
        benchmark::DoNotOptimize(report);
        if (add_status != me::BookStatus::Accepted || report.filled_quantity != 1u) [[unlikely]] {
            state.SkipWithError("hot FIFO benchmark failed to prepare/fill one resting order");
            break;
        }
    }
}

void BM_AddCancelChurn(benchmark::State& state) {
    static GoogleBenchBook book;
    static me::OrderID next_id = 10'000'000u;

    for (auto _ : state) {
        const me::OrderID id = next_id++;
        const me::Price price = kAskPrice + static_cast<me::Price>(id & 1023u);
        const me::BookStatus add_status = book.add_order(id, me::Side::Sell, price, 1u);
        const me::BookStatus cancel_status = book.cancel_order(id, 1u);
        benchmark::DoNotOptimize(cancel_status);
        if (add_status != me::BookStatus::Accepted || cancel_status != me::BookStatus::Filled) [[unlikely]] {
            state.SkipWithError("add/cancel churn benchmark failed");
            break;
        }
    }
}

void BM_ItchAddDecode(benchmark::State& state) {
    const RawItchMessage raw = make_add(1'000u, 42u, me::Side::Buy, 100u, kBidPrice);
    me::itch::ParsedMessage parsed{};

    for (auto _ : state) {
        const bool ok = me::itch::ItchParser::parse(raw.bytes.data(), raw.length, parsed);
        benchmark::DoNotOptimize(parsed);
        if (!ok) [[unlikely]] {
            state.SkipWithError("ITCH add parser returned false");
            break;
        }
    }
}

void BM_AlphaSignalAndQuote(benchmark::State& state) {
    me::alpha::OnlineSignals<> signals;
    const me::strategy::AvellanedaStoikovMarketMaker maker{};
    me::MarketEvent event{};
    event.type = me::EventType::Execute;
    event.best_bid = kBidPrice;
    event.best_ask = kAskPrice;
    event.best_bid_quantity = 6'000u;
    event.best_ask_quantity = 5'000u;

    for (auto _ : state) {
        event.timestamp += 1u;
        event.side = (event.timestamp & 1u) == 0u ? me::Side::Buy : me::Side::Sell;
        event.quantity = 200u + static_cast<me::Qty>(event.timestamp & 127u);
        const me::alpha::FeatureFrame frame = signals.on_event(event);
        const me::strategy::Quote quote = maker.quote(frame, 25, 0.5);
        benchmark::DoNotOptimize(quote);
    }
}

}  // namespace

BENCHMARK(BM_HotFifoFill);
BENCHMARK(BM_AddCancelChurn);
BENCHMARK(BM_ItchAddDecode);
BENCHMARK(BM_AlphaSignalAndQuote);
BENCHMARK_MAIN();

#else

namespace {

using FifoBook = me::OrderBook<kIterations + 1024u, 1u << 20u, 1u << 12u>;
using ChurnBook = me::OrderBook<1u << 12u, 1u << 20u, 1u << 12u>;

static std::array<std::uint64_t, kIterations> samples{};
static std::array<RawItchMessage, kMessageRingSize> add_messages{};

void initialize_add_messages() noexcept {
    static bool initialized = false;
    if (initialized) {
        return;
    }

    for (std::size_t i = 0; i < kMessageRingSize; ++i) {
        add_messages[i] = make_add(static_cast<me::Timestamp>(1'000u + i),
                                   static_cast<me::OrderID>(i + 1u),
                                   (i & 1u) == 0u ? me::Side::Buy : me::Side::Sell,
                                   100u + static_cast<me::Qty>(i & 127u),
                                   kBidPrice + static_cast<me::Price>(i & 255u));
    }
    initialized = true;
}

template <typename Fn>
bool run_scenario(const char* name, const std::size_t ops_per_sample, Fn&& fn) {
    const auto benchmark_start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kIterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        bool ok = true;
        for (std::size_t op = 0; op < ops_per_sample; ++op) {
            ok = fn((i * ops_per_sample) + op);
            if (!ok) [[unlikely]] {
                break;
            }
        }
        const auto end = std::chrono::steady_clock::now();
        samples[i] = nanos_since(start, end);
        if (!ok) [[unlikely]] {
            std::cerr << "scenario=" << name << " failed_at=" << i << '\n';
            return false;
        }
    }
    const auto benchmark_end = std::chrono::steady_clock::now();

    std::sort(samples.begin(), samples.end());
    const double median = static_cast<double>(samples[kIterations / 2u]) /
                          static_cast<double>(ops_per_sample);
    const double p99 = static_cast<double>(samples[(kIterations * 99u) / 100u]) /
                       static_cast<double>(ops_per_sample);
    const double seconds =
        static_cast<double>(nanos_since(benchmark_start, benchmark_end)) / 1'000'000'000.0;
    const double total_ops = static_cast<double>(kIterations * ops_per_sample);
    const double throughput = total_ops / seconds;

    std::cout << "scenario=" << name
              << " samples=" << kIterations
              << " ops_per_sample=" << ops_per_sample
              << " median_ns=" << median
              << " p99_ns=" << p99
              << " throughput_events_per_sec=" << throughput
              << '\n';
    return true;
}

bool run_hot_fifo_fill() {
    static FifoBook book;
    for (std::size_t i = 0; i < kIterations; ++i) {
        const me::BookStatus status =
            book.add_order(static_cast<me::OrderID>(i + 1u), me::Side::Sell, kAskPrice, 1u);
        if (status != me::BookStatus::Accepted) [[unlikely]] {
            return false;
        }
    }

    return run_scenario("hot_fifo_fill", 1u, [](const std::size_t) noexcept {
        const me::ExecutionReport report = book.match_market_order(me::Side::Buy, 1u);
        return report.filled_quantity == 1u;
    });
}

bool run_add_cancel_churn() {
    static ChurnBook book;
    return run_scenario("add_cancel_churn", 1u, [](const std::size_t i) noexcept {
        const me::OrderID id = static_cast<me::OrderID>(10'000'000u + i);
        const me::Price price = kAskPrice + static_cast<me::Price>(i & 1023u);
        const me::BookStatus add_status = book.add_order(id, me::Side::Sell, price, 1u);
        const me::BookStatus cancel_status = book.cancel_order(id, 1u);
        return add_status == me::BookStatus::Accepted && cancel_status == me::BookStatus::Filled;
    });
}

bool run_itch_add_decode() {
    initialize_add_messages();
    return run_scenario("itch_add_decode", kFastOpsPerSample, [](const std::size_t i) noexcept {
        me::itch::ParsedMessage parsed{};
        const RawItchMessage& raw = add_messages[i & (kMessageRingSize - 1u)];
        return me::itch::ItchParser::parse(raw.bytes.data(), raw.length, parsed);
    });
}

bool run_alpha_signal_quote() {
    static me::alpha::OnlineSignals<> signals;
    static me::strategy::AvellanedaStoikovMarketMaker maker;
    me::MarketEvent event{};
    event.type = me::EventType::Execute;
    event.best_bid = kBidPrice;
    event.best_ask = kAskPrice;
    event.best_bid_quantity = 6'000u;
    event.best_ask_quantity = 5'000u;

    return run_scenario("alpha_signal_quote", kFastOpsPerSample, [&](const std::size_t i) noexcept {
        event.timestamp = static_cast<me::Timestamp>(i + 1u);
        event.side = (i & 1u) == 0u ? me::Side::Sell : me::Side::Buy;
        event.quantity = 200u + static_cast<me::Qty>(i & 127u);
        const me::alpha::FeatureFrame frame = signals.on_event(event);
        const me::strategy::Quote quote = maker.quote(frame, 25, 0.5);
        return quote.ask_price > quote.bid_price;
    });
}

}  // namespace

int main() {
    const bool ok =
        run_hot_fifo_fill() &&
        run_add_cancel_churn() &&
        run_itch_add_decode() &&
        run_alpha_signal_quote();
    return ok ? 0 : 1;
}

#endif
