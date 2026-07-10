# Architecture Notes

## Hot Path

The matching hot path is single-writer and allocation-free after object construction:

1. `ItchParser::parse` decodes raw NASDAQ ITCH 5.0 message bytes with fixed offsets and big-endian scalar reads.
2. `OrderBook::add_order`, `cancel_order`, `execute_order`, and `match_market_order` mutate intrusive order queues.
3. `LockFreeSPSC::write` publishes a `MarketEvent` to the alpha thread through atomic head/tail sequence counters.

Order nodes come from `FixedSlabPool<Order, N>`. Order lookup and price-level lookup use fixed-capacity open-addressed tables. There is no `std::vector`, `std::unordered_map`, `malloc`, or allocator-backed container in the book path.

The order-ID table uses backward-shift deletion, so long add/cancel sessions with unique order IDs do not accumulate tombstones and degrade future lookup probes. The price-level table keeps level object addresses stable because live `Order` nodes hold direct `PriceLevel*` links.

## Price Levels

Each side owns a fixed price-level directory. A price level stores FIFO `head` and `tail` pointers into intrusive `Order` nodes, plus aggregate volume and order count. Best bid/ask are maintained eagerly on add and recomputed only when the current best level is removed.

## Alpha Pipeline

The alpha thread consumes `MarketEvent` records and computes:

- Order book imbalance: `(bid_qty - ask_qty) / (bid_qty + ask_qty)`
- Micro-price: ask-weighted-by-bid-depth plus bid-weighted-by-ask-depth
- VPIN: rolling normalized absolute buy/sell imbalance over fixed volume buckets

`AtomicFeatureFrame` publishes the latest signal frame as atomics, including packed double fields, so downstream strategy code can poll without a mutex.

## Strategy

`AvellanedaStoikovMarketMaker` computes reservation price and total spread from inventory, volatility, gamma, kappa, and time remaining. It widens spreads when VPIN exceeds its rolling baseline by positive z-score.
