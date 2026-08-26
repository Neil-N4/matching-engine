#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "alpha/signals.hpp"
#include "engine/order_book.hpp"
#include "engine/parser.hpp"
#include "strategy/avellaneda_stoikov.hpp"
#include "strategy/position.hpp"
#include "strategy/risk.hpp"

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

RawMessage make_replace(const me::Timestamp timestamp,
                        const me::OrderID old_id,
                        const me::OrderID new_id,
                        const me::Qty quantity,
                        const me::Price price) noexcept {
    RawMessage message{};
    message.length = me::itch::ItchParser::kOrderReplaceMinBytes;
    message.bytes[0] = static_cast<std::byte>('U');
    write_be(message.bytes.data() + me::itch::ItchParser::kTimestampOffset,
             timestamp,
             me::itch::ItchParser::kTimestampBytes);
    write_be(message.bytes.data() + me::itch::ItchParser::kOrderIdOffset,
             old_id,
             me::itch::ItchParser::kOrderIdBytes);
    write_be(message.bytes.data() + me::itch::ItchParser::kReplaceNewOrderIdOffset,
             new_id,
             me::itch::ItchParser::kOrderIdBytes);
    write_be(message.bytes.data() + me::itch::ItchParser::kReplaceQtyOffset,
             quantity,
             me::itch::ItchParser::kQtyBytes);
    write_be(message.bytes.data() + me::itch::ItchParser::kReplacePriceOffset,
             price,
             me::itch::ItchParser::kPriceBytes);
    return message;
}

RawMessage make_delete(const me::Timestamp timestamp, const me::OrderID id) noexcept {
    RawMessage message{};
    message.length = me::itch::ItchParser::kOrderDeleteMinBytes;
    message.bytes[0] = static_cast<std::byte>('D');
    write_be(message.bytes.data() + me::itch::ItchParser::kTimestampOffset,
             timestamp,
             me::itch::ItchParser::kTimestampBytes);
    write_be(message.bytes.data() + me::itch::ItchParser::kOrderIdOffset,
             id,
             me::itch::ItchParser::kOrderIdBytes);
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

TEST(ItchParser, ParsesOrderReplace) {
    const RawMessage raw = make_replace(100u, 42u, 43u, 125u, 1'235'000u);

    me::itch::ParsedMessage parsed{};
    ASSERT_TRUE(me::itch::ItchParser::parse(raw.bytes.data(), raw.length, parsed));
    EXPECT_EQ(parsed.kind, me::itch::MessageKind::OrderReplace);
    EXPECT_EQ(parsed.timestamp, 100u);
    EXPECT_EQ(parsed.order_id, 42u);
    EXPECT_EQ(parsed.new_order_id, 43u);
    EXPECT_EQ(parsed.quantity, 125u);
    EXPECT_EQ(parsed.price, 1'235'000u);

    EXPECT_FALSE(me::itch::ItchParser::parse(raw.bytes.data(), 10u, parsed));
}

TEST(ItchParser, ParsesOrderDelete) {
    const RawMessage raw = make_delete(98u, 42u);

    me::itch::ParsedMessage parsed{};
    ASSERT_TRUE(me::itch::ItchParser::parse(raw.bytes.data(), raw.length, parsed));
    EXPECT_EQ(parsed.kind, me::itch::MessageKind::OrderDelete);
    EXPECT_EQ(parsed.timestamp, 98u);
    EXPECT_EQ(parsed.order_id, 42u);
    EXPECT_EQ(parsed.quantity, 0u);
    EXPECT_EQ(parsed.price, 0u);

    EXPECT_FALSE(me::itch::ItchParser::parse(raw.bytes.data(), 10u, parsed));
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

TEST(OrderBook, DeletesFullOrderAndUpdatesBestLevelsWithoutExecutionAccounting) {
    me::OrderBook<32, 64, 16> book;
    ASSERT_EQ(book.add_order(1u, me::Side::Buy, 100u, 10u), me::BookStatus::Accepted);
    ASSERT_EQ(book.add_order(2u, me::Side::Buy, 101u, 20u), me::BookStatus::Accepted);
    ASSERT_EQ(book.add_order(3u, me::Side::Sell, 105u, 30u), me::BookStatus::Accepted);

    EXPECT_EQ(book.delete_order(2u), me::BookStatus::Filled);
    EXPECT_EQ(book.find_order(2u), nullptr);
    me::TopOfBook top = book.top_of_book();
    EXPECT_EQ(top.best_bid, 100u);
    EXPECT_EQ(top.best_bid_quantity, 10u);
    EXPECT_EQ(top.best_ask, 105u);
    EXPECT_EQ(book.total_bid_volume(), 10u);
    EXPECT_EQ(book.live_orders(), 2u);
    EXPECT_EQ(book.free_order_slots(), 30u);
    EXPECT_EQ(book.executed_quantity(), 0u);
    EXPECT_EQ(book.executed_notional(), 0u);

    EXPECT_EQ(book.delete_order(3u), me::BookStatus::Filled);
    top = book.top_of_book();
    EXPECT_EQ(top.best_ask, 0u);
    EXPECT_EQ(top.best_ask_quantity, 0u);
    EXPECT_EQ(book.total_ask_volume(), 0u);

    EXPECT_EQ(book.delete_order(100u), me::BookStatus::NotFound);
    EXPECT_EQ(book.add_order(4u, me::Side::Sell, 104u, 15u), me::BookStatus::Accepted);
    top = book.top_of_book();
    EXPECT_EQ(top.best_ask, 104u);
    EXPECT_EQ(top.best_ask_quantity, 15u);
}

TEST(OrderBook, ReplacesOrderIdPriceAndQuantityWithoutAllocatingNewSlot) {
    me::OrderBook<32, 64, 16> book;
    ASSERT_EQ(book.add_order(1u, me::Side::Buy, 100u, 10u), me::BookStatus::Accepted);

    EXPECT_EQ(book.replace_order(1u, 11u, 101u, 12u), me::BookStatus::Accepted);
    EXPECT_EQ(book.find_order(1u), nullptr);

    const me::Order* replaced = book.find_order(11u);
    ASSERT_NE(replaced, nullptr);
    EXPECT_EQ(replaced->side, me::Side::Buy);
    EXPECT_EQ(replaced->price, 101u);
    EXPECT_EQ(replaced->quantity, 12u);

    const me::TopOfBook top = book.top_of_book();
    EXPECT_EQ(top.best_bid, 101u);
    EXPECT_EQ(top.best_bid_quantity, 12u);
    EXPECT_EQ(book.total_bid_volume(), 12u);
    EXPECT_EQ(book.free_order_slots(), 31u);
}

TEST(OrderBook, SamePriceReplaceLosesFifoPriority) {
    me::OrderBook<32, 64, 16> book;
    ASSERT_EQ(book.add_order(1u, me::Side::Sell, 101u, 5u), me::BookStatus::Accepted);
    ASSERT_EQ(book.add_order(2u, me::Side::Sell, 101u, 5u), me::BookStatus::Accepted);
    ASSERT_EQ(book.replace_order(1u, 3u, 101u, 5u), me::BookStatus::Accepted);

    const me::ExecutionReport report =
        book.submit_limit_order(10u, me::Side::Buy, 101u, 5u, me::TimeInForce::Ioc);

    EXPECT_EQ(report.status, me::BookStatus::Filled);
    EXPECT_EQ(report.filled_quantity, 5u);
    EXPECT_EQ(book.find_order(2u), nullptr);
    EXPECT_NE(book.find_order(3u), nullptr);
}

TEST(OrderBook, ReplaceRejectsDuplicateMissingAndInvalidOrders) {
    me::OrderBook<32, 64, 16> book;
    ASSERT_EQ(book.add_order(1u, me::Side::Buy, 100u, 10u), me::BookStatus::Accepted);
    ASSERT_EQ(book.add_order(2u, me::Side::Buy, 99u, 10u), me::BookStatus::Accepted);

    EXPECT_EQ(book.replace_order(1u, 2u, 101u, 10u), me::BookStatus::Duplicate);
    EXPECT_EQ(book.replace_order(100u, 3u, 101u, 10u), me::BookStatus::NotFound);
    EXPECT_EQ(book.replace_order(1u, 3u, 101u, 0u), me::BookStatus::InvalidQuantity);
}

TEST(OrderBook, ImmediateOrCancelDoesNotRestResidualQuantity) {
    me::OrderBook<32, 64, 16> book;
    ASSERT_EQ(book.add_order(1u, me::Side::Sell, 101u, 10u), me::BookStatus::Accepted);

    const me::ExecutionReport report =
        book.submit_limit_order(2u, me::Side::Buy, 101u, 15u, me::TimeInForce::Ioc);

    EXPECT_EQ(report.status, me::BookStatus::Partial);
    EXPECT_EQ(report.filled_quantity, 10u);
    EXPECT_EQ(book.find_order(2u), nullptr);
    EXPECT_EQ(book.live_orders(), 0u);
}

TEST(OrderBook, ImmediateOrCancelExpiresWhenNothingCrosses) {
    me::OrderBook<32, 64, 16> book;
    ASSERT_EQ(book.add_order(1u, me::Side::Sell, 105u, 10u), me::BookStatus::Accepted);

    const me::ExecutionReport report =
        book.submit_limit_order(2u, me::Side::Buy, 104u, 10u, me::TimeInForce::Ioc);

    EXPECT_EQ(report.status, me::BookStatus::Expired);
    EXPECT_EQ(report.filled_quantity, 0u);
    EXPECT_EQ(book.find_order(2u), nullptr);
    EXPECT_EQ(book.live_orders(), 1u);
}

TEST(OrderBook, FillOrKillDoesNotMutateBookWhenInsufficientLiquidity) {
    me::OrderBook<32, 64, 16> book;
    ASSERT_EQ(book.add_order(1u, me::Side::Sell, 101u, 10u), me::BookStatus::Accepted);

    const me::ExecutionReport report =
        book.submit_limit_order(2u, me::Side::Buy, 101u, 15u, me::TimeInForce::Fok);

    EXPECT_EQ(report.status, me::BookStatus::Expired);
    EXPECT_EQ(report.filled_quantity, 0u);
    EXPECT_EQ(book.executed_quantity(), 0u);

    const me::Order* ask = book.find_order(1u);
    ASSERT_NE(ask, nullptr);
    EXPECT_EQ(ask->quantity, 10u);
}

TEST(OrderBook, FillOrKillCanConsumeMultipleCrossingLevels) {
    me::OrderBook<32, 64, 16> book;
    ASSERT_EQ(book.add_order(1u, me::Side::Sell, 101u, 5u), me::BookStatus::Accepted);
    ASSERT_EQ(book.add_order(2u, me::Side::Sell, 102u, 7u), me::BookStatus::Accepted);

    const me::ExecutionReport report =
        book.submit_limit_order(3u, me::Side::Buy, 102u, 12u, me::TimeInForce::Fok);

    EXPECT_EQ(report.status, me::BookStatus::Filled);
    EXPECT_EQ(report.filled_quantity, 12u);
    EXPECT_EQ(book.live_orders(), 0u);
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

TEST(OnlineSignals, ComputesOrderFlowImbalanceAndEma) {
    me::alpha::OnlineSignals<100u, 2u> signals;

    me::MarketEvent event{};
    event.type = me::EventType::Add;
    event.best_bid = 100u;
    event.best_ask = 102u;
    event.best_bid_quantity = 100u;
    event.best_ask_quantity = 50u;

    me::alpha::FeatureFrame frame = signals.on_event(event);
    EXPECT_DOUBLE_EQ(frame.ofi, 0.0);
    EXPECT_DOUBLE_EQ(frame.ofi_ema, 0.0);

    event.best_bid_quantity = 130u;
    event.best_ask_quantity = 45u;
    frame = signals.on_event(event);
    EXPECT_DOUBLE_EQ(frame.ofi, 35.0);
    EXPECT_DOUBLE_EQ(frame.ofi_ema, 7.0);

    event.best_bid = 101u;
    event.best_bid_quantity = 80u;
    frame = signals.on_event(event);
    EXPECT_DOUBLE_EQ(frame.ofi, 80.0);
    EXPECT_DOUBLE_EQ(frame.ofi_ema, 21.6);

    me::alpha::AtomicFeatureFrame atomic_frame;
    atomic_frame.publish(frame);
    const me::alpha::FeatureFrame loaded = atomic_frame.read();
    EXPECT_DOUBLE_EQ(loaded.ofi, frame.ofi);
    EXPECT_DOUBLE_EQ(loaded.ofi_ema, frame.ofi_ema);
}

TEST(OnlineSignals, ComputesMicroPriceVolatility) {
    me::alpha::OnlineSignals<100u, 2u> signals;

    me::MarketEvent event{};
    event.type = me::EventType::Add;
    event.best_bid = 100u;
    event.best_ask = 102u;
    event.best_bid_quantity = 100u;
    event.best_ask_quantity = 50u;

    me::alpha::FeatureFrame frame = signals.on_event(event);
    EXPECT_DOUBLE_EQ(frame.micro_return, 0.0);
    EXPECT_DOUBLE_EQ(frame.realized_volatility, 0.0);
    EXPECT_DOUBLE_EQ(frame.ewma_volatility, 0.0);

    const double first_micro = frame.micro_price;
    event.best_bid_quantity = 130u;
    event.best_ask_quantity = 45u;
    frame = signals.on_event(event);
    const double expected_return = std::log(frame.micro_price / first_micro);
    EXPECT_NEAR(frame.micro_return, expected_return, 1.0e-15);
    EXPECT_NEAR(frame.realized_volatility, std::fabs(expected_return), 1.0e-15);
    EXPECT_NEAR(frame.ewma_volatility, std::fabs(expected_return), 1.0e-15);

    const double previous_micro = frame.micro_price;
    event.best_bid = 101u;
    event.best_bid_quantity = 80u;
    frame = signals.on_event(event);
    const double second_return = std::log(frame.micro_price / previous_micro);
    const double expected_realized =
        std::sqrt(((expected_return * expected_return) + (second_return * second_return)) / 2.0);
    const double expected_ewma = std::sqrt(
        (me::config::kVolatilityEmaAlpha * second_return * second_return) +
        ((1.0 - me::config::kVolatilityEmaAlpha) * expected_return * expected_return));

    EXPECT_NEAR(frame.micro_return, second_return, 1.0e-15);
    EXPECT_NEAR(frame.realized_volatility, expected_realized, 1.0e-15);
    EXPECT_NEAR(frame.ewma_volatility, expected_ewma, 1.0e-15);

    me::alpha::AtomicFeatureFrame atomic_frame;
    atomic_frame.publish(frame);
    const me::alpha::FeatureFrame loaded = atomic_frame.read();
    EXPECT_DOUBLE_EQ(loaded.micro_return, frame.micro_return);
    EXPECT_DOUBLE_EQ(loaded.realized_volatility, frame.realized_volatility);
    EXPECT_DOUBLE_EQ(loaded.ewma_volatility, frame.ewma_volatility);
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

TEST(Strategy, UsesSignalVolatilityWhenItExceedsBaseline) {
    me::alpha::FeatureFrame frame{};
    frame.best_bid = 1'000'000u;
    frame.best_ask = 1'000'200u;
    frame.best_bid_quantity = 100u;
    frame.best_ask_quantity = 100u;
    frame.micro_price = 100.01;

    me::strategy::AvellanedaStoikovConfig config{};
    config.volatility = 0.001;
    config.max_signal_volatility = 0.050;
    const me::strategy::AvellanedaStoikovMarketMaker maker(config);

    const me::strategy::Quote quiet = maker.quote(frame, 0, 0.5);
    EXPECT_DOUBLE_EQ(quiet.effective_volatility, 0.001);

    frame.ewma_volatility = 0.020;
    const me::strategy::Quote active = maker.quote(frame, 0, 0.5);
    EXPECT_DOUBLE_EQ(active.effective_volatility, 0.020);
    EXPECT_GT(active.total_spread, quiet.total_spread);

    frame.ewma_volatility = 0.500;
    const me::strategy::Quote capped = maker.quote(frame, 0, 0.5);
    EXPECT_DOUBLE_EQ(capped.effective_volatility, 0.050);
}

TEST(RiskController, ScalesAndRestrictsQuotesByInventory) {
    me::strategy::RiskConfig config{};
    config.max_abs_inventory = 100;
    config.soft_abs_inventory = 60;
    config.base_quote_quantity = 40u;
    config.max_quote_quantity = 50u;

    const me::strategy::RiskController risk(config);
    const me::strategy::Quote quote{100.0, 99.95, 100.05, 0.10, 1.0};
    const me::alpha::FeatureFrame frame{};

    me::strategy::RiskState state{};
    state.inventory = 80;
    me::strategy::RiskDecision decision = risk.evaluate(quote, frame, state);
    EXPECT_EQ(decision.action, me::strategy::RiskAction::QuoteBoth);
    EXPECT_EQ(decision.reason, me::strategy::RiskReason::None);
    EXPECT_EQ(decision.bid_quantity, 20u);
    EXPECT_EQ(decision.ask_quantity, 40u);

    state.inventory = 100;
    decision = risk.evaluate(quote, frame, state);
    EXPECT_EQ(decision.action, me::strategy::RiskAction::AskOnly);
    EXPECT_EQ(decision.reason, me::strategy::RiskReason::InventoryLimit);
    EXPECT_EQ(decision.bid_quantity, 0u);
    EXPECT_EQ(decision.ask_quantity, 40u);

    state.inventory = -100;
    decision = risk.evaluate(quote, frame, state);
    EXPECT_EQ(decision.action, me::strategy::RiskAction::BidOnly);
    EXPECT_EQ(decision.reason, me::strategy::RiskReason::InventoryLimit);
    EXPECT_EQ(decision.bid_quantity, 40u);
    EXPECT_EQ(decision.ask_quantity, 0u);
}

TEST(RiskController, PullsQuotesForToxicityNotionalAndDrawdown) {
    me::strategy::RiskConfig config{};
    config.max_gross_notional = 1'000u;
    config.max_drawdown_ticks = 50;
    config.toxicity_pause_zscore = 2.0;

    const me::strategy::RiskController risk(config);
    const me::strategy::Quote quote{100.0, 99.95, 100.05, 0.10, 1.0};

    me::alpha::FeatureFrame frame{};
    frame.vpin = 0.8;
    frame.vpin_mean = 0.3;
    frame.vpin_sigma = 0.1;

    me::strategy::RiskState state{};
    me::strategy::RiskDecision decision = risk.evaluate(quote, frame, state);
    EXPECT_EQ(decision.action, me::strategy::RiskAction::PullQuotes);
    EXPECT_EQ(decision.reason, me::strategy::RiskReason::ToxicFlow);

    frame.vpin = 0.3;
    state.gross_notional = 1'000u;
    decision = risk.evaluate(quote, frame, state);
    EXPECT_EQ(decision.action, me::strategy::RiskAction::PullQuotes);
    EXPECT_EQ(decision.reason, me::strategy::RiskReason::NotionalLimit);

    state.gross_notional = 0u;
    state.peak_pnl_ticks = 100;
    state.realized_pnl_ticks = 40;
    decision = risk.evaluate(quote, frame, state);
    EXPECT_EQ(decision.action, me::strategy::RiskAction::KillSwitch);
    EXPECT_EQ(decision.reason, me::strategy::RiskReason::DrawdownLimit);

    state.peak_pnl_ticks = 0;
    state.realized_pnl_ticks = 0;
    state.manual_kill = true;
    decision = risk.evaluate(quote, frame, state);
    EXPECT_EQ(decision.action, me::strategy::RiskAction::KillSwitch);
    EXPECT_EQ(decision.reason, me::strategy::RiskReason::ManualKill);
}

TEST(PositionTracker, TracksLongRealizedAndUnrealizedPnl) {
    me::strategy::PositionTracker tracker;

    ASSERT_TRUE(tracker.on_fill({1u, me::Side::Buy, 100u, 10u}));
    me::strategy::PositionSnapshot snap = tracker.snapshot(105u);
    EXPECT_EQ(snap.inventory, 10);
    EXPECT_EQ(snap.average_entry_price, 100u);
    EXPECT_EQ(snap.realized_pnl_ticks, 0);
    EXPECT_EQ(snap.unrealized_pnl_ticks, 50);
    EXPECT_EQ(snap.total_pnl_ticks, 50);

    ASSERT_TRUE(tracker.on_fill({2u, me::Side::Sell, 110u, 4u}));
    snap = tracker.mark_to_market(105u);
    EXPECT_EQ(snap.inventory, 6);
    EXPECT_EQ(snap.realized_pnl_ticks, 40);
    EXPECT_EQ(snap.unrealized_pnl_ticks, 30);
    EXPECT_EQ(snap.total_pnl_ticks, 70);
    EXPECT_EQ(snap.peak_pnl_ticks, 70);
}

TEST(PositionTracker, HandlesCrossingFromLongToShortAndExportsRiskState) {
    me::strategy::PositionTracker tracker;

    ASSERT_TRUE(tracker.on_fill({1u, me::Side::Buy, 100u, 10u}));
    ASSERT_TRUE(tracker.on_fill({2u, me::Side::Sell, 110u, 4u}));
    ASSERT_TRUE(tracker.on_fill({3u, me::Side::Sell, 90u, 10u}));

    const me::strategy::PositionSnapshot snap = tracker.mark_to_market(95u);
    EXPECT_EQ(snap.inventory, -4);
    EXPECT_EQ(snap.average_entry_price, 90u);
    EXPECT_EQ(snap.realized_pnl_ticks, -20);
    EXPECT_EQ(snap.unrealized_pnl_ticks, -20);
    EXPECT_EQ(snap.total_pnl_ticks, -40);
    EXPECT_EQ(snap.drawdown_ticks, 110);

    const me::strategy::RiskState risk_state = tracker.risk_state(95u);
    EXPECT_EQ(risk_state.inventory, -4);
    EXPECT_EQ(risk_state.gross_notional, 2'340u);
    EXPECT_EQ(risk_state.realized_pnl_ticks, -20);
    EXPECT_EQ(risk_state.unrealized_pnl_ticks, -20);
    EXPECT_EQ(risk_state.peak_pnl_ticks, 70);
}
