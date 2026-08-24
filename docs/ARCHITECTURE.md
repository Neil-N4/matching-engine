# Architecture Notes

## Hot Path

The matching hot path is single-writer and allocation-free after object construction:

1. `ItchParser::parse` decodes raw NASDAQ ITCH 5.0 add, execute, cancel, and replace message bytes with fixed offsets and big-endian scalar reads.
2. `OrderBook::add_order`, `cancel_order`, `execute_order`, `replace_order`, and `match_market_order` mutate intrusive order queues.
3. `LockFreeSPSC::write` publishes a `MarketEvent` to the alpha thread through atomic head/tail sequence counters.

Order nodes come from `FixedSlabPool<Order, N>`. Order lookup and price-level lookup use fixed-capacity open-addressed tables. There is no `std::vector`, `std::unordered_map`, `malloc`, or allocator-backed container in the book path.

The order-ID table uses backward-shift deletion, so long add/cancel sessions with unique order IDs do not accumulate tombstones and degrade future lookup probes. The price-level table keeps level object addresses stable because live `Order` nodes hold direct `PriceLevel*` links.

Limit orders default to `TimeInForce::Gtc`. `TimeInForce::Ioc` executes immediately against crossing levels and expires any residual quantity without resting it. `TimeInForce::Fok` first checks fixed-table crossing liquidity and expires without mutation unless the full requested quantity is available.

ITCH `U` order replace is implemented as cancel-replace at book level: the old order ID must exist, the new order ID must not collide, the new size must be non-zero, and the order keeps its original side. The operation reuses the existing slab-allocated `Order` node, rewrites ID/price/quantity, moves it to the tail of the target price level, and updates aggregate side volume and best bid/ask state. Same-price replaces still lose FIFO priority, matching the expected L3 semantics.

## Price Levels

Each side owns a fixed price-level directory. A price level stores FIFO `head` and `tail` pointers into intrusive `Order` nodes, plus aggregate volume and order count. Best bid/ask are maintained eagerly on add and recomputed only when the current best level is removed.

## Alpha Pipeline

The alpha thread consumes `MarketEvent` records and computes:

- Order book imbalance: `(bid_qty - ask_qty) / (bid_qty + ask_qty)`
- Micro-price: ask-weighted-by-bid-depth plus bid-weighted-by-ask-depth
- Order-flow imbalance: queue-pressure contribution from best bid/ask price and quantity changes, plus an EMA smoother
- Micro-price volatility: log-return, rolling realized volatility, and EWMA volatility from the online micro-price stream
- VPIN: rolling normalized absolute buy/sell imbalance over fixed volume buckets

`AtomicFeatureFrame` publishes the latest signal frame as atomics, including packed double fields, so downstream strategy code can poll without a mutex.

## Strategy

`AvellanedaStoikovMarketMaker` computes reservation price and total spread from inventory, volatility, gamma, kappa, and time remaining. It keeps a configured volatility baseline but can widen quotes when the signal pipeline's EWMA micro-price volatility rises above that baseline. It also widens spreads when VPIN exceeds its rolling baseline by positive z-score.

`PositionTracker` consumes strategy fills where `Side::Buy` increases inventory and `Side::Sell` decreases inventory. It maintains signed open cost, average entry price, realized PnL, mark-to-market unrealized PnL, gross notional, peak PnL, and drawdown. It can export a `RiskState` directly, so risk checks operate on current marked PnL instead of stale realized-only accounting.

`RiskController` sits after quote generation. It returns a `RiskDecision` with quote action and bid/ask sizes:

- `QuoteBoth` under normal inventory, notional, PnL, and VPIN conditions.
- `BidOnly` or `AskOnly` near hard inventory bounds.
- `PullQuotes` for toxic VPIN z-scores, invalid quotes, or gross-notional breaches.
- `KillSwitch` for manual kill or drawdown-limit breaches.

The position tracker and risk controller are scalar and header-only. They do not allocate, throw, or use string state; runtime labels are returned by fixed `const char*` helpers for logs and demo output.
