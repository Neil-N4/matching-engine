#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif
#include <thread>

#include "alpha/signals.hpp"
#include "common/config.hpp"
#include "common/ring_buffer.hpp"
#include "engine/order_book.hpp"
#include "engine/parser.hpp"
#include "strategy/avellaneda_stoikov.hpp"
#include "strategy/position.hpp"
#include "strategy/risk.hpp"

namespace {

using RuntimeBook = me::OrderBook<1u << 16u, 1u << 18u, 1u << 14u>;
using RuntimeQueue = me::LockFreeSPSC<me::MarketEvent, me::config::kTelemetryQueueCapacity>;

struct RawItchMessage {
    std::array<std::byte, me::itch::ItchParser::kAddOrderMinBytes> bytes{};
    std::size_t length{0};
};

void pin_thread_to_core(const int core_id) noexcept {
#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    const pthread_t current_thread = pthread_self();
    static_cast<void>(pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset));
#else
    static_cast<void>(core_id);
#endif
}

void write_be(std::byte* out, std::uint64_t value, const std::size_t width) noexcept {
    for (std::size_t i = 0; i < width; ++i) {
        const std::size_t shift = (width - i - 1u) * 8u;
        out[i] = static_cast<std::byte>((value >> shift) & 0xffu);
    }
}

RawItchMessage make_add(const me::Timestamp timestamp,
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

RawItchMessage make_execute(const me::Timestamp timestamp,
                            const me::OrderID id,
                            const me::Qty quantity) noexcept {
    RawItchMessage message{};
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

RawItchMessage make_cancel(const me::Timestamp timestamp,
                           const me::OrderID id,
                           const me::Qty quantity) noexcept {
    RawItchMessage message{};
    message.length = me::itch::ItchParser::kOrderCancelMinBytes;
    message.bytes[0] = static_cast<std::byte>('X');
    write_be(message.bytes.data() + me::itch::ItchParser::kTimestampOffset,
             timestamp,
             me::itch::ItchParser::kTimestampBytes);
    write_be(message.bytes.data() + me::itch::ItchParser::kOrderIdOffset,
             id,
             me::itch::ItchParser::kOrderIdBytes);
    write_be(message.bytes.data() + me::itch::ItchParser::kCancelQtyOffset,
             quantity,
             me::itch::ItchParser::kQtyBytes);
    return message;
}

RawItchMessage make_delete(const me::Timestamp timestamp, const me::OrderID id) noexcept {
    RawItchMessage message{};
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

RawItchMessage make_replace(const me::Timestamp timestamp,
                            const me::OrderID old_id,
                            const me::OrderID new_id,
                            const me::Qty quantity,
                            const me::Price price) noexcept {
    RawItchMessage message{};
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

void publish_event(RuntimeQueue& queue, const me::MarketEvent& event) noexcept {
    while (!queue.write(event)) {
        std::this_thread::yield();
    }
}

me::MarketEvent decorate_event(const me::MarketEvent& base, const me::TopOfBook& top) noexcept {
    me::MarketEvent event = base;
    event.best_bid = top.best_bid;
    event.best_ask = top.best_ask;
    event.best_bid_quantity = top.best_bid_quantity;
    event.best_ask_quantity = top.best_ask_quantity;
    return event;
}

me::Price mark_price_from_frame(const me::alpha::FeatureFrame& frame, const me::Price fallback) noexcept {
    if (frame.micro_price > 0.0) {
        return static_cast<me::Price>((frame.micro_price * me::config::kPriceScale) + 0.5);
    }
    if (frame.best_bid != 0u && frame.best_ask != 0u) {
        return static_cast<me::Price>(
            (static_cast<std::uint64_t>(frame.best_bid) + frame.best_ask) / 2u);
    }
    return fallback;
}

const char* book_status_to_string(const me::BookStatus status) noexcept {
    switch (status) {
        case me::BookStatus::Accepted:
            return "accepted";
        case me::BookStatus::Filled:
            return "filled";
        case me::BookStatus::Partial:
            return "partial";
        case me::BookStatus::NotFound:
            return "not_found";
        case me::BookStatus::Duplicate:
            return "duplicate";
        case me::BookStatus::InvalidQuantity:
            return "invalid_quantity";
        case me::BookStatus::OrderPoolExhausted:
            return "order_pool_exhausted";
        case me::BookStatus::OrderDirectoryFull:
            return "order_directory_full";
        case me::BookStatus::PriceLevelFull:
            return "price_level_full";
        case me::BookStatus::Expired:
            return "expired";
    }
    return "unknown";
}

}  // namespace

int main() {
    static RuntimeBook book;
    static RuntimeQueue engine_to_alpha;
    static me::alpha::AtomicFeatureFrame latest_features;

    std::atomic<bool> engine_done{false};
    me::strategy::Quote final_quote{};
    me::strategy::RiskDecision final_risk{};
    me::strategy::PositionSnapshot final_position{};

    std::thread engine_worker([&]() noexcept {
        pin_thread_to_core(1);

        const std::array<RawItchMessage, 11> feed{
            make_add(1'000u, 1001u, me::Side::Buy, 4'000u, 1'001'000u),
            make_add(1'100u, 1002u, me::Side::Buy, 6'000u, 1'000'900u),
            make_add(1'200u, 2001u, me::Side::Sell, 5'000u, 1'001'500u),
            make_add(1'300u, 2002u, me::Side::Sell, 5'500u, 1'001'600u),
            make_add(1'325u, 2005u, me::Side::Sell, 800u, 1'001'800u),
            make_replace(1'350u, 2002u, 2004u, 5'200u, 1'001'450u),
            make_execute(1'400u, 2001u, 5'000u),
            make_delete(1'450u, 2005u),
            make_execute(1'500u, 1001u, 4'000u),
            make_cancel(1'600u, 1002u, 1'500u),
            make_add(1'700u, 2003u, me::Side::Sell, 7'000u, 1'001'400u),
        };

        for (const RawItchMessage& raw : feed) {
            me::itch::ParsedMessage parsed{};
            if (!me::itch::ItchParser::parse(raw.bytes.data(), raw.length, parsed)) [[unlikely]] {
                continue;
            }

            me::MarketEvent base{};
            base.timestamp = parsed.timestamp;
            base.quantity = parsed.quantity;

            if (parsed.kind == me::itch::MessageKind::AddOrder) {
                const me::BookStatus status =
                    book.add_order(parsed.order_id, parsed.side, parsed.price, parsed.quantity);
                if (status != me::BookStatus::Accepted) [[unlikely]] {
                    continue;
                }
                base.type = me::EventType::Add;
                base.side = parsed.side;
                base.price = parsed.price;
            } else if (parsed.kind == me::itch::MessageKind::OrderReplace) {
                const me::Order* const order = book.find_order(parsed.order_id);
                if (order == nullptr) [[unlikely]] {
                    continue;
                }

                base.type = me::EventType::Replace;
                base.side = order->side;
                base.price = parsed.price;
                const me::BookStatus status =
                    book.replace_order(parsed.order_id, parsed.new_order_id, parsed.price, parsed.quantity);
                if (status != me::BookStatus::Accepted) [[unlikely]] {
                    continue;
                }
            } else if (parsed.kind == me::itch::MessageKind::OrderDelete) {
                const me::Order* const order = book.find_order(parsed.order_id);
                if (order == nullptr) [[unlikely]] {
                    continue;
                }

                base.type = me::EventType::Delete;
                base.side = order->side;
                base.price = order->price;
                base.quantity = order->quantity;
                const me::BookStatus status = book.delete_order(parsed.order_id);
                if (status != me::BookStatus::Filled) [[unlikely]] {
                    continue;
                }
            } else {
                const me::Order* const order = book.find_order(parsed.order_id);
                if (order == nullptr) [[unlikely]] {
                    continue;
                }

                base.side = order->side;
                base.price = order->price;
                if (parsed.kind == me::itch::MessageKind::OrderExecuted) {
                    base.type = me::EventType::Execute;
                    static_cast<void>(book.execute_order(parsed.order_id, parsed.quantity));
                } else if (parsed.kind == me::itch::MessageKind::OrderCancel) {
                    base.type = me::EventType::Cancel;
                    static_cast<void>(book.cancel_order(parsed.order_id, parsed.quantity));
                }
            }

            publish_event(engine_to_alpha, decorate_event(base, book.top_of_book()));
        }

        engine_done.store(true, std::memory_order_release);
    });

    std::thread alpha_worker([&]() noexcept {
        pin_thread_to_core(2);

        me::alpha::OnlineSignals<> signals;
        const me::strategy::AvellanedaStoikovMarketMaker market_maker{};
        const me::strategy::RiskController risk_controller{};
        me::strategy::PositionTracker position_tracker;
        me::MarketEvent event{};

        while (!engine_done.load(std::memory_order_acquire) || !engine_to_alpha.empty()) {
            if (engine_to_alpha.read(event)) {
                if (event.type == me::EventType::Execute) {
                    static_cast<void>(position_tracker.on_fill({
                        event.timestamp,
                        event.side,
                        event.price,
                        event.quantity,
                    }));
                }
                const me::alpha::FeatureFrame frame = signals.on_event(event);
                latest_features.publish(frame);
                const me::Price mark_price = mark_price_from_frame(frame, event.price);
                final_position = position_tracker.mark_to_market(mark_price);
                const me::strategy::RiskState risk_state = position_tracker.risk_state(mark_price);
                final_quote = market_maker.quote(frame, risk_state.inventory, 0.20);
                final_risk = risk_controller.evaluate(final_quote, frame, risk_state);
            } else {
                std::this_thread::yield();
            }
        }
    });

    engine_worker.join();
    alpha_worker.join();

    me::OrderBook<16u, 32u, 16u> tif_demo;
    static_cast<void>(tif_demo.add_order(90'001u, me::Side::Sell, 1'002'000u, 10u));
    const me::ExecutionReport ioc_demo =
        tif_demo.submit_limit_order(90'002u, me::Side::Buy, 1'002'000u, 12u, me::TimeInForce::Ioc);
    static_cast<void>(tif_demo.add_order(90'003u, me::Side::Sell, 1'002'500u, 5u));
    const me::ExecutionReport fok_demo =
        tif_demo.submit_limit_order(90'004u, me::Side::Buy, 1'002'500u, 6u, me::TimeInForce::Fok);

    const me::alpha::FeatureFrame frame = latest_features.read();
    std::cout << "best_bid=" << frame.best_bid
              << " best_ask=" << frame.best_ask
              << " obi=" << frame.obi
              << " micro_price=" << frame.micro_price
              << " ofi=" << frame.ofi
              << " ofi_ema=" << frame.ofi_ema
              << " micro_return=" << frame.micro_return
              << " realized_vol=" << frame.realized_volatility
              << " ewma_vol=" << frame.ewma_volatility
              << " vpin=" << frame.vpin
              << " quote_bid=" << final_quote.bid_price
              << " quote_ask=" << final_quote.ask_price
              << " quote_vol=" << final_quote.effective_volatility
              << " risk_action=" << me::strategy::RiskController::to_string(final_risk.action)
              << " risk_reason=" << me::strategy::RiskController::to_string(final_risk.reason)
              << " bid_qty=" << final_risk.bid_quantity
              << " ask_qty=" << final_risk.ask_quantity
              << " inventory=" << final_position.inventory
              << " realized_pnl_ticks=" << final_position.realized_pnl_ticks
              << " unrealized_pnl_ticks=" << final_position.unrealized_pnl_ticks
              << " drawdown_ticks=" << final_position.drawdown_ticks
              << " ioc_status=" << book_status_to_string(ioc_demo.status)
              << " ioc_fill=" << ioc_demo.filled_quantity
              << " fok_status=" << book_status_to_string(fok_demo.status)
              << " fok_fill=" << fok_demo.filled_quantity
              << '\n';

    return 0;
}
