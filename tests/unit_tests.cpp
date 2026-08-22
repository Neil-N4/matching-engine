#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>

#include "alpha/signals.hpp"
#include "common/memory_pool.hpp"
#include "common/ring_buffer.hpp"
#include "engine/order_book.hpp"
#include "engine/parser.hpp"
#include "strategy/avellaneda_stoikov.hpp"
#include "strategy/position.hpp"
#include "strategy/risk.hpp"

namespace {

#define REQUIRE(expr)                                                                          \
    do {                                                                                       \
        if (!(expr)) {                                                                         \
            std::cerr << __FILE__ << ':' << __LINE__ << " requirement failed: " << #expr       \
                      << '\n';                                                                 \
            return false;                                                                      \
        }                                                                                      \
    } while (false)

struct TinyNode {
    std::uint64_t value{0};
};

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

RawMessage add_msg(const me::Timestamp timestamp,
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

RawMessage exec_msg(const me::Timestamp timestamp, const me::OrderID id, const me::Qty quantity) noexcept {
    RawMessage message{};
    message.length = me::itch::ItchParser::kOrderExecutedMinBytes;
    message.bytes[0] = static_cast<std::byte>('E');
    write_be(message.bytes.data() + me::itch::ItchParser::kTimestampOffset,
             timestamp,
             me::itch::ItchParser::kTimestampBytes);
    write_be(message.bytes.data() + me::itch::ItchParser::kOrderIdOffset,
             id,
             me::itch::ItchParser::kOrderIdBytes);
    write_be(message.bytes.data() + me::itch::ItchParser::kExecutedQtyOffset,
             quantity,
             me::itch::ItchParser::kQtyBytes);
    return message;
}

bool near(const double lhs, const double rhs, const double eps = 1.0e-9) noexcept {
    return std::fabs(lhs - rhs) <= eps;
}

bool test_pool() {
    me::FixedSlabPool<TinyNode, 2> pool;
    TinyNode* a = pool.allocate(TinyNode{1});
    TinyNode* b = pool.allocate(TinyNode{2});
    TinyNode* c = pool.allocate(TinyNode{3});
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c == nullptr);
    REQUIRE(pool.available() == 0u);
    pool.deallocate(a);
    REQUIRE(pool.available() == 1u);
    c = pool.allocate(TinyNode{4});
    REQUIRE(c != nullptr);
    REQUIRE(c->value == 4u);
    pool.deallocate(b);
    pool.deallocate(c);
    REQUIRE(pool.available() == 2u);
    return true;
}

bool test_ring() {
    me::LockFreeSPSC<me::MarketEvent, 2> queue;
    me::MarketEvent first{};
    me::MarketEvent second{};
    me::MarketEvent third{};
    first.timestamp = 1u;
    second.timestamp = 2u;
    third.timestamp = 3u;

    REQUIRE(queue.write(first));
    REQUIRE(queue.write(second));
    REQUIRE(!queue.write(third));
    me::MarketEvent out{};
    REQUIRE(queue.read(out));
    REQUIRE(out.timestamp == 1u);
    REQUIRE(queue.read(out));
    REQUIRE(out.timestamp == 2u);
    REQUIRE(!queue.read(out));
    return true;
}

bool test_parser() {
    const RawMessage raw = add_msg(0x010203040506ULL, 42u, me::Side::Sell, 100u, 1'234'500u);
    me::itch::ParsedMessage parsed{};
    REQUIRE(me::itch::ItchParser::parse(raw.bytes.data(), raw.length, parsed));
    REQUIRE(parsed.kind == me::itch::MessageKind::AddOrder);
    REQUIRE(parsed.timestamp == 0x010203040506ULL);
    REQUIRE(parsed.order_id == 42u);
    REQUIRE(parsed.side == me::Side::Sell);
    REQUIRE(parsed.quantity == 100u);
    REQUIRE(parsed.price == 1'234'500u);

    const RawMessage exec = exec_msg(99u, 42u, 75u);
    REQUIRE(me::itch::ItchParser::parse(exec.bytes.data(), exec.length, parsed));
    REQUIRE(parsed.kind == me::itch::MessageKind::OrderExecuted);
    REQUIRE(parsed.quantity == 75u);
    return true;
}

bool test_order_book() {
    me::OrderBook<32, 64, 16> book;
    REQUIRE(book.add_order(1u, me::Side::Buy, 100u, 10u) == me::BookStatus::Accepted);
    REQUIRE(book.add_order(2u, me::Side::Buy, 101u, 20u) == me::BookStatus::Accepted);
    REQUIRE(book.add_order(3u, me::Side::Sell, 103u, 30u) == me::BookStatus::Accepted);
    REQUIRE(book.add_order(4u, me::Side::Sell, 102u, 40u) == me::BookStatus::Accepted);

    me::TopOfBook top = book.top_of_book();
    REQUIRE(top.best_bid == 101u);
    REQUIRE(top.best_ask == 102u);
    REQUIRE(top.best_bid_quantity == 20u);
    REQUIRE(top.best_ask_quantity == 40u);

    REQUIRE(book.execute_order(4u, 15u) == me::BookStatus::Partial);
    top = book.top_of_book();
    REQUIRE(top.best_ask == 102u);
    REQUIRE(top.best_ask_quantity == 25u);

    REQUIRE(book.cancel_order(4u, 25u) == me::BookStatus::Filled);
    top = book.top_of_book();
    REQUIRE(top.best_ask == 103u);
    REQUIRE(top.best_ask_quantity == 30u);

    const me::ExecutionReport report = book.submit_limit_order(5u, me::Side::Buy, 103u, 12u);
    REQUIRE(report.status == me::BookStatus::Filled);
    REQUIRE(report.filled_quantity == 12u);
    top = book.top_of_book();
    REQUIRE(top.best_ask == 103u);
    REQUIRE(top.best_ask_quantity == 18u);

    const me::ExecutionReport duplicate = book.submit_limit_order(1u, me::Side::Buy, 103u, 5u);
    REQUIRE(duplicate.status == me::BookStatus::Duplicate);
    REQUIRE(duplicate.filled_quantity == 0u);
    top = book.top_of_book();
    REQUIRE(top.best_ask == 103u);
    REQUIRE(top.best_ask_quantity == 18u);
    return true;
}

bool test_time_in_force() {
    {
        me::OrderBook<32, 64, 16> book;
        REQUIRE(book.add_order(1u, me::Side::Sell, 101u, 10u) == me::BookStatus::Accepted);

        const me::ExecutionReport ioc =
            book.submit_limit_order(2u, me::Side::Buy, 101u, 15u, me::TimeInForce::Ioc);
        REQUIRE(ioc.status == me::BookStatus::Partial);
        REQUIRE(ioc.filled_quantity == 10u);
        REQUIRE(book.find_order(2u) == nullptr);
        REQUIRE(book.live_orders() == 0u);
    }

    {
        me::OrderBook<32, 64, 16> book;
        REQUIRE(book.add_order(1u, me::Side::Sell, 105u, 10u) == me::BookStatus::Accepted);

        const me::ExecutionReport ioc =
            book.submit_limit_order(2u, me::Side::Buy, 104u, 10u, me::TimeInForce::Ioc);
        REQUIRE(ioc.status == me::BookStatus::Expired);
        REQUIRE(ioc.filled_quantity == 0u);
        REQUIRE(book.find_order(2u) == nullptr);
        REQUIRE(book.live_orders() == 1u);
    }

    {
        me::OrderBook<32, 64, 16> book;
        REQUIRE(book.add_order(1u, me::Side::Sell, 101u, 10u) == me::BookStatus::Accepted);

        const me::ExecutionReport fok =
            book.submit_limit_order(2u, me::Side::Buy, 101u, 15u, me::TimeInForce::Fok);
        REQUIRE(fok.status == me::BookStatus::Expired);
        REQUIRE(fok.filled_quantity == 0u);
        REQUIRE(book.executed_quantity() == 0u);
        const me::Order* ask = book.find_order(1u);
        REQUIRE(ask != nullptr);
        REQUIRE(ask->quantity == 10u);
    }

    {
        me::OrderBook<32, 64, 16> book;
        REQUIRE(book.add_order(1u, me::Side::Sell, 101u, 5u) == me::BookStatus::Accepted);
        REQUIRE(book.add_order(2u, me::Side::Sell, 102u, 7u) == me::BookStatus::Accepted);

        const me::ExecutionReport fok =
            book.submit_limit_order(3u, me::Side::Buy, 102u, 12u, me::TimeInForce::Fok);
        REQUIRE(fok.status == me::BookStatus::Filled);
        REQUIRE(fok.filled_quantity == 12u);
        REQUIRE(book.live_orders() == 0u);
    }

    return true;
}

bool test_alpha_and_strategy() {
    me::alpha::OnlineSignals<100u, 4u> signals;
    me::MarketEvent event{};
    event.timestamp = 1u;
    event.type = me::EventType::Execute;
    event.side = me::Side::Sell;
    event.quantity = 40u;
    event.best_bid = 100u;
    event.best_ask = 102u;
    event.best_bid_quantity = 100u;
    event.best_ask_quantity = 50u;

    me::alpha::FeatureFrame frame = signals.on_event(event);
    REQUIRE(near(frame.obi, 1.0 / 3.0));
    REQUIRE(near(frame.micro_price, 101.33333333333333 / me::config::kPriceScale));
    REQUIRE(near(frame.ofi, 0.0));
    REQUIRE(near(frame.ofi_ema, 0.0));
    REQUIRE(near(frame.micro_return, 0.0));
    REQUIRE(near(frame.realized_volatility, 0.0));
    REQUIRE(near(frame.ewma_volatility, 0.0));
    REQUIRE(frame.vpin == 0.0);

    event.type = me::EventType::Add;
    event.best_bid_quantity = 130u;
    event.best_ask_quantity = 45u;
    frame = signals.on_event(event);
    REQUIRE(near(frame.ofi, 35.0));
    REQUIRE(near(frame.ofi_ema, 7.0));
    REQUIRE(frame.micro_return > 0.0);
    REQUIRE(frame.realized_volatility > 0.0);
    REQUIRE(frame.ewma_volatility > 0.0);
    const double first_vol = frame.ewma_volatility;

    event.best_bid = 101u;
    event.best_bid_quantity = 80u;
    frame = signals.on_event(event);
    REQUIRE(near(frame.ofi, 80.0));
    REQUIRE(near(frame.ofi_ema, 21.6));
    REQUIRE(frame.micro_return > 0.0);
    REQUIRE(frame.realized_volatility > 0.0);
    REQUIRE(frame.ewma_volatility > 0.0);
    REQUIRE(!near(frame.ewma_volatility, first_vol, 1.0e-12));

    event.type = me::EventType::Execute;
    event.timestamp = 2u;
    event.quantity = 60u;
    frame = signals.on_event(event);
    REQUIRE(near(frame.vpin, 1.0));
    REQUIRE(near(frame.vpin_mean, 1.0));

    me::alpha::AtomicFeatureFrame atomic_frame;
    atomic_frame.publish(frame);
    const me::alpha::FeatureFrame loaded = atomic_frame.read();
    REQUIRE(loaded.timestamp == frame.timestamp);
    REQUIRE(near(loaded.micro_price, frame.micro_price));
    REQUIRE(near(loaded.ofi, frame.ofi));
    REQUIRE(near(loaded.ofi_ema, frame.ofi_ema));
    REQUIRE(near(loaded.micro_return, frame.micro_return));
    REQUIRE(near(loaded.realized_volatility, frame.realized_volatility));
    REQUIRE(near(loaded.ewma_volatility, frame.ewma_volatility));

    frame.vpin = 0.9;
    frame.vpin_mean = 0.4;
    frame.vpin_sigma = 0.25;
    const me::strategy::AvellanedaStoikovMarketMaker market_maker{};
    const me::strategy::Quote quote = market_maker.quote(frame, 10, 0.5);
    REQUIRE(quote.ask_price > quote.bid_price);
    REQUIRE(quote.toxicity_multiplier > 1.0);
    return true;
}

bool test_risk_controller() {
    me::strategy::RiskConfig config{};
    config.max_abs_inventory = 100;
    config.soft_abs_inventory = 60;
    config.base_quote_quantity = 40u;
    config.max_quote_quantity = 50u;
    config.max_gross_notional = 1'000'000u;
    config.max_drawdown_ticks = 500;
    config.toxicity_pause_zscore = 2.0;

    const me::strategy::RiskController risk(config);
    const me::strategy::Quote quote{100.0, 99.95, 100.05, 0.10, 1.0};
    me::alpha::FeatureFrame frame{};
    frame.vpin = 0.3;
    frame.vpin_mean = 0.3;
    frame.vpin_sigma = 0.1;

    me::strategy::RiskState state{};
    me::strategy::RiskDecision decision = risk.evaluate(quote, frame, state);
    REQUIRE(decision.action == me::strategy::RiskAction::QuoteBoth);
    REQUIRE(decision.reason == me::strategy::RiskReason::None);
    REQUIRE(decision.bid_quantity == 40u);
    REQUIRE(decision.ask_quantity == 40u);

    state.inventory = 100;
    decision = risk.evaluate(quote, frame, state);
    REQUIRE(decision.action == me::strategy::RiskAction::AskOnly);
    REQUIRE(decision.reason == me::strategy::RiskReason::InventoryLimit);
    REQUIRE(decision.bid_quantity == 0u);
    REQUIRE(decision.ask_quantity == 40u);

    state.inventory = 80;
    decision = risk.evaluate(quote, frame, state);
    REQUIRE(decision.action == me::strategy::RiskAction::QuoteBoth);
    REQUIRE(decision.bid_quantity == 20u);
    REQUIRE(decision.ask_quantity == 40u);

    state.inventory = 0;
    frame.vpin = 0.8;
    decision = risk.evaluate(quote, frame, state);
    REQUIRE(decision.action == me::strategy::RiskAction::PullQuotes);
    REQUIRE(decision.reason == me::strategy::RiskReason::ToxicFlow);

    frame.vpin = 0.3;
    state.peak_pnl_ticks = 1'000;
    state.realized_pnl_ticks = 400;
    decision = risk.evaluate(quote, frame, state);
    REQUIRE(decision.action == me::strategy::RiskAction::KillSwitch);
    REQUIRE(decision.reason == me::strategy::RiskReason::DrawdownLimit);

    state.peak_pnl_ticks = 0;
    state.realized_pnl_ticks = 0;
    state.gross_notional = 1'000'000u;
    decision = risk.evaluate(quote, frame, state);
    REQUIRE(decision.action == me::strategy::RiskAction::PullQuotes);
    REQUIRE(decision.reason == me::strategy::RiskReason::NotionalLimit);
    return true;
}

bool test_strategy_uses_signal_volatility() {
    me::alpha::FeatureFrame frame{};
    frame.best_bid = 1'000'000u;
    frame.best_ask = 1'000'200u;
    frame.best_bid_quantity = 100u;
    frame.best_ask_quantity = 100u;
    frame.micro_price = 100.01;

    me::strategy::AvellanedaStoikovConfig config{};
    config.volatility = 0.001;
    config.max_signal_volatility = 0.050;
    const me::strategy::AvellanedaStoikovMarketMaker market_maker(config);

    const me::strategy::Quote quiet = market_maker.quote(frame, 0, 0.5);
    REQUIRE(near(quiet.effective_volatility, 0.001));

    frame.ewma_volatility = 0.020;
    const me::strategy::Quote volatile_quote = market_maker.quote(frame, 0, 0.5);
    REQUIRE(near(volatile_quote.effective_volatility, 0.020));
    REQUIRE(volatile_quote.total_spread > quiet.total_spread);

    frame.ewma_volatility = 0.500;
    const me::strategy::Quote capped = market_maker.quote(frame, 0, 0.5);
    REQUIRE(near(capped.effective_volatility, 0.050));
    return true;
}

bool test_position_tracker() {
    me::strategy::PositionTracker tracker;

    REQUIRE(tracker.on_fill({1u, me::Side::Buy, 100u, 10u}));
    me::strategy::PositionSnapshot snap = tracker.snapshot(105u);
    REQUIRE(snap.inventory == 10);
    REQUIRE(snap.average_entry_price == 100u);
    REQUIRE(snap.realized_pnl_ticks == 0);
    REQUIRE(snap.unrealized_pnl_ticks == 50);
    REQUIRE(snap.total_pnl_ticks == 50);
    REQUIRE(snap.gross_notional == 1'000u);

    REQUIRE(tracker.on_fill({2u, me::Side::Sell, 110u, 4u}));
    snap = tracker.mark_to_market(105u);
    REQUIRE(snap.inventory == 6);
    REQUIRE(snap.average_entry_price == 100u);
    REQUIRE(snap.realized_pnl_ticks == 40);
    REQUIRE(snap.unrealized_pnl_ticks == 30);
    REQUIRE(snap.total_pnl_ticks == 70);
    REQUIRE(snap.peak_pnl_ticks == 70);

    REQUIRE(tracker.on_fill({3u, me::Side::Sell, 90u, 10u}));
    snap = tracker.mark_to_market(95u);
    REQUIRE(snap.inventory == -4);
    REQUIRE(snap.average_entry_price == 90u);
    REQUIRE(snap.realized_pnl_ticks == -20);
    REQUIRE(snap.unrealized_pnl_ticks == -20);
    REQUIRE(snap.total_pnl_ticks == -40);
    REQUIRE(snap.drawdown_ticks == 110);

    me::strategy::RiskState risk_state = tracker.risk_state(95u);
    REQUIRE(risk_state.inventory == -4);
    REQUIRE(risk_state.gross_notional == 2'340u);
    REQUIRE(risk_state.realized_pnl_ticks == -20);
    REQUIRE(risk_state.unrealized_pnl_ticks == -20);
    REQUIRE(risk_state.peak_pnl_ticks == 70);

    REQUIRE(!tracker.on_fill({4u, me::Side::Buy, 0u, 1u}));
    REQUIRE(!tracker.on_fill({4u, me::Side::Buy, 100u, 0u}));
    return true;
}

}  // namespace

int main() {
    const bool ok =
        test_pool() &&
        test_ring() &&
        test_parser() &&
        test_order_book() &&
        test_time_in_force() &&
        test_alpha_and_strategy() &&
        test_risk_controller() &&
        test_strategy_uses_signal_volatility() &&
        test_position_tracker();

    if (!ok) {
        return 1;
    }

    std::cout << "all tests passed\n";
    return 0;
}
