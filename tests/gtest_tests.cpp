#include <array>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "alpha/signals.hpp"
#include "engine/order_book.hpp"
#include "engine/parser.hpp"
#include "strategy/avellaneda_stoikov.hpp"

namespace {

struct RawMessage {
    std::array<std::byte, me::itch::ItchParser::kAddOrderMinBytes> bytes{};
    std::size_t length{0};
};

void write_be(std::byte* out, std::uint64_t value, const std::size_t width) noexcept {
    for (std::size_t i = 0; i < width; ++i) {
        const std::size_t shift = (width - i - 1u) * 8u;
        out[i] = static_cast<std::byte>((value >> shift) & 0xffu);
    }
}

RawMessage make_add(const me::Timestamp timestamp,
                    const me::OrderID id,
                    const me::Side side,
                    const me::Qty quantity,
                    const me::Price price) noexcept {
    RawMessage message{};
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

}  // namespace

TEST(ItchParser, ParsesAddOrderAndRejectsMalformedFrames) {
    const RawMessage raw = make_add(0x010203040506ULL, 1234u, me::Side::Buy, 250u, 1'234'500u);

    me::itch::ParsedMessage parsed{};
    ASSERT_TRUE(me::itch::ItchParser::parse(raw.bytes.data(), raw.length, parsed));
    EXPECT_EQ(parsed.kind, me::itch::MessageKind::AddOrder);
    EXPECT_EQ(parsed.timestamp, 0x010203040506ULL);
    EXPECT_EQ(parsed.order_id, 1234u);
    EXPECT_EQ(parsed.side, me::Side::Buy);
    EXPECT_EQ(parsed.quantity, 250u);
    EXPECT_EQ(parsed.price, 1'234'500u);

    EXPECT_FALSE(me::itch::ItchParser::parse(raw.bytes.data(), 4u, parsed));

    RawMessage unknown = raw;
    unknown.bytes[0] = static_cast<std::byte>('Z');
    EXPECT_FALSE(me::itch::ItchParser::parse(unknown.bytes.data(), unknown.length, parsed));
}

TEST(OrderBook, PreservesFifoPriorityAtPriceLevel) {
    me::OrderBook<16, 32, 16> book;
    ASSERT_EQ(book.add_order(1u, me::Side::Sell, 101u, 5u), me::BookStatus::Accepted);
    ASSERT_EQ(book.add_order(2u, me::Side::Sell, 101u, 5u), me::BookStatus::Accepted);

    const me::ExecutionReport report = book.submit_limit_order(100u, me::Side::Buy, 101u, 6u);
    EXPECT_EQ(report.status, me::BookStatus::Filled);
    EXPECT_EQ(report.filled_quantity, 6u);
    EXPECT_EQ(book.find_order(1u), nullptr);

    const me::Order* second = book.find_order(2u);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->quantity, 4u);

    const me::TopOfBook top = book.top_of_book();
    EXPECT_EQ(top.best_ask, 101u);
    EXPECT_EQ(top.best_ask_quantity, 4u);
}

TEST(OrderBook, RejectsDuplicateMarketableOrderBeforeMutatingBook) {
    me::OrderBook<16, 32, 16> book;
    ASSERT_EQ(book.add_order(1u, me::Side::Buy, 99u, 10u), me::BookStatus::Accepted);
    ASSERT_EQ(book.add_order(2u, me::Side::Sell, 100u, 10u), me::BookStatus::Accepted);

    const me::ExecutionReport duplicate = book.submit_limit_order(1u, me::Side::Buy, 100u, 5u);
    EXPECT_EQ(duplicate.status, me::BookStatus::Duplicate);
    EXPECT_EQ(duplicate.filled_quantity, 0u);

    const me::Order* ask = book.find_order(2u);
    ASSERT_NE(ask, nullptr);
    EXPECT_EQ(ask->quantity, 10u);
    EXPECT_EQ(book.executed_quantity(), 0u);
}

TEST(OnlineSignals, ComputesRollingVpinWindow) {
    me::alpha::OnlineSignals<100u, 2u> signals;

    me::MarketEvent event{};
    event.type = me::EventType::Execute;
    event.best_bid = 100u;
    event.best_ask = 102u;
    event.best_bid_quantity = 100u;
    event.best_ask_quantity = 100u;

    event.side = me::Side::Sell;
    event.quantity = 60u;
    me::alpha::FeatureFrame frame = signals.on_event(event);
    EXPECT_DOUBLE_EQ(frame.vpin, 0.0);

    event.side = me::Side::Buy;
    event.quantity = 40u;
    frame = signals.on_event(event);
    EXPECT_NEAR(frame.vpin, 0.2, 1.0e-12);

    event.side = me::Side::Sell;
    event.quantity = 100u;
    frame = signals.on_event(event);
    EXPECT_NEAR(frame.vpin, 0.6, 1.0e-12);
    EXPECT_NEAR(frame.vpin_sigma, 0.4, 1.0e-12);

    event.side = me::Side::Buy;
    event.quantity = 100u;
    frame = signals.on_event(event);
    EXPECT_NEAR(frame.vpin, 1.0, 1.0e-12);
    EXPECT_NEAR(frame.vpin_sigma, 0.0, 1.0e-12);
}

TEST(Strategy, WidensQuoteWhenVpinZScoreIsPositive) {
    me::alpha::FeatureFrame quiet{};
    quiet.best_bid = 1'000'000u;
    quiet.best_ask = 1'000'200u;
    quiet.best_bid_quantity = 100u;
    quiet.best_ask_quantity = 100u;
    quiet.micro_price = 100.01;
    quiet.vpin = 0.3;
    quiet.vpin_mean = 0.3;
    quiet.vpin_sigma = 0.1;

    me::alpha::FeatureFrame toxic = quiet;
    toxic.vpin = 0.6;

    const me::strategy::AvellanedaStoikovMarketMaker maker{};
    const me::strategy::Quote quiet_quote = maker.quote(quiet, 0, 0.25);
    const me::strategy::Quote toxic_quote = maker.quote(toxic, 0, 0.25);

    EXPECT_GT(toxic_quote.total_spread, quiet_quote.total_spread);
    EXPECT_GT(toxic_quote.toxicity_multiplier, 1.0);
}

