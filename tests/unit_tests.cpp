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
    REQUIRE(frame.vpin == 0.0);

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

    frame.vpin = 0.9;
    frame.vpin_mean = 0.4;
    frame.vpin_sigma = 0.25;
    const me::strategy::AvellanedaStoikovMarketMaker market_maker{};
    const me::strategy::Quote quote = market_maker.quote(frame, 10, 0.5);
    REQUIRE(quote.ask_price > quote.bid_price);
    REQUIRE(quote.toxicity_multiplier > 1.0);
    return true;
}

}  // namespace

int main() {
    const bool ok =
        test_pool() &&
        test_ring() &&
        test_parser() &&
        test_order_book() &&
        test_alpha_and_strategy();

    if (!ok) {
        return 1;
    }

    std::cout << "all tests passed\n";
    return 0;
}

