# Market Microstructure Components

High-performance C++ implementation of core market microstructure components including a multi-symbol matching engine, trade engine with position tracking, market data aggregation, and order book management. These components provide the foundation for building limit order book simulations and trading systems.

## Overview

This directory contains the market engine layer for HFT-sdk. The matching engine coordinates per-symbol L3 order books (from `../orderbook/`), integrates with the risk engine (from `../risk/`), and publishes execution reports, trades, and market data updates. The design prioritizes performance with O(1) order cancellations, efficient matching, and lock-free market data dissemination.

## Components

This directory contains six main components:

### 1. matching_engine.h/.cpp — Multi-Symbol Matching Engine

The `MatchingEngine` class is the central coordinator for all order processing. It manages per-symbol state (L3 book + L2 aggregator + L1 feed), routes orders, performs risk checks, and publishes events.

**Key Features:**
- Manages multiple symbols, each with an `L3OrderBook`, `L2Aggregator`, and `L1Feed`
- O(1) symbol lookup via `unordered_map`
- O(1) order-to-symbol mapping for cancel routing
- Integrated risk checks via `RiskEngine` (optional)
- Callback-based execution reports, trades, L1, and L2 updates
- Supports NewOrder, Cancel, and Cancel-Replace workflows
- Tracks order/trade/cancel/reject counters

**Public Interface:**

```cpp
class MatchingEngine {
public:
    explicit MatchingEngine(Clock& clock);

    void add_symbol(SymbolId symbol);
    void add_symbol(const Symbol& symbol);
    bool has_symbol(const Symbol& symbol) const;
    std::vector<Symbol> symbols() const;

    // Order processing
    ExecutionReport process_new_order(const Order& order);
    ExecutionReport process_cancel(const CancelRequest& cancel);
    ExecutionReport process_replace(const ReplaceRequest& replace);
    void handle_cancel(const CancelOrder& c);

    // Callbacks
    void on_execution(ExecutionCallback cb);
    void on_trade(TradeCallback cb);
    void on_l1_update(L1Callback cb);
    void on_l2_update(L2Callback cb);

    // Market data queries
    TopOfBook get_top_of_book(const Symbol& symbol) const;
    DepthSnapshot get_depth(const Symbol& symbol) const;
    const L3OrderBook* get_book(const Symbol& symbol) const;

    // Risk engine binding
    void set_risk_engine(RiskEngine* risk);

    // Statistics
    uint64_t total_orders_processed() const;
    uint64_t total_trades() const;
    uint64_t total_cancels() const;
    uint64_t total_rejects() const;
};
```

### 2. trade_engine.h/.cpp — Trade & Position Engine

The `TradeEngine` processes trades, tracks per-trader per-symbol positions, computes realized/unrealized PnL, and supports mark-to-market.

**Key Features:**
- Per-trader per-symbol position tracking (`TraderSymbolKey` composite key)
- Realized PnL computation on position reduction
- Unrealized PnL via mark-to-market
- Trade volume and notional accumulation
- Callback support for trades and position updates

**Public Interface:**

```cpp
class TradeEngine {
public:
    void process_trade(const Trade& trade);
    const Position* get_position(TraderId trader_id, const Symbol& symbol) const;
    std::vector<Position> get_positions(TraderId trader_id) const;
    void mark_to_market(const Symbol& symbol, Price current_price);

    void on_trade(TradeCallback cb);
    void on_position(PositionCallback cb);

    uint64_t total_trades() const;
    Quantity total_volume() const;
    double total_notional() const;
};
```

### 3. order_book.h/.cpp — Simple Limit Order Book

A lightweight `OrderBook` with price-time priority matching. This is a simpler alternative to the full `L3OrderBook` (in `../orderbook/`) for basic simulations.

**Key Features:**
- Separate bid and ask price levels using `std::map` with custom comparators
- O(1) order cancellation via `unordered_map` index
- O(log n) order insertion
- O(1) best bid/ask queries
- Price-time priority within each price level using `std::list`

**Public Interface:**

```cpp
class OrderBook {
public:
    explicit OrderBook(Symbol symbol);
    const Symbol& symbol() const noexcept;

    void addOrder(const BookOrder& ord);
    bool cancelOrder(OrderId id);
    std::pair<std::vector<Trade>*, Quantity> matchIncoming(
        const BookOrder& incoming, std::uint64_t ts_ns);

    std::optional<BookLevel> bestBid() const;
    std::optional<BookLevel> bestAsk() const;
    std::vector<BookLevel> bids(std::size_t depth) const;
    std::vector<BookLevel> asks(std::size_t depth) const;
};
```

### 4. market_data_engine.h/.cpp — Market Data Engine

The `MarketDataEngine` aggregates market data from the matching engine and publishes it via HPRingBuffer for lock-free cross-thread dissemination. Supports configurable throttling.

**Key Features:**
- Lock-free HPRingBuffer (16K slots) for cross-thread MD messages
- Per-symbol throttling with configurable interval
- Supports L1 (TOB), L2 (depth snapshots), and trade events
- Trades are never throttled
- Multiple subscriber callbacks per event type

**Public Interface:**

```cpp
class MarketDataEngine {
public:
    explicit MarketDataEngine(Clock& clock);

    void set_throttle_interval_ns(uint64_t ns);
    void set_l2_depth(std::size_t depth);

    void on_tob_update(const TopOfBook& tob);
    void on_depth_update(const DepthSnapshot& depth);
    void on_trade(const Trade& trade);
    void process_pending();

    void subscribe_l1(L1Handler handler);
    void subscribe_l2(L2Handler handler);
    void subscribe_trades(TradeHandler handler);

    uint64_t messages_published() const;
    uint64_t messages_throttled() const;
};
```

### 5. market_data_publisher.h/.cpp — Callback-Based Market Data Publisher

A lightweight, decoupled publisher that distributes market data via `std::function` callbacks. Used by the matching engine to push updates without tight coupling to subscribers.

**Public Interface:**

```cpp
class MarketDataPublisher {
public:
    void onTopOfBook(TopOfBookHandler cb);
    void onTrade(TradeHandler cb);
    void onDepthSnapshot(DepthSnapshotHandler cb);

    void publishTopOfBook(const TopOfBook& tob) const;
    void publishTrade(const Trade& t) const;
    void publishDepth(const SymbolId& sym,
                      const std::vector<BookLevel>& bids,
                      const std::vector<BookLevel>& asks) const;
};
```

## Files

```
market/
├── README.md                       # This file
├── matching_engine.h/.cpp          # Multi-symbol matching engine
├── trade_engine.h/.cpp             # Position tracking & PnL
├── order_book.h/.cpp               # Simple limit order book
├── market_data_engine.h/.cpp       # MD aggregation with throttling
└── market_data_publisher.h/.cpp    # Callback-based MD publisher
```

## Dependencies on Other HFT-sdk Modules

| Dependency | Used By | Purpose |
|------------|---------|---------|
| `common/types.h` | All | Core types: Order, Trade, Symbol, enums |
| `common/clock.h` | MatchingEngine, MarketDataEngine | Timestamps |
| `common/constants.h` | MarketDataEngine | Default L2 depth, queue sizes |
| `orderbook/l3_order_book.h` | MatchingEngine | Per-symbol order book |
| `orderbook/l2_aggregator.h` | MatchingEngine | L2 depth snapshots |
| `orderbook/l1_feed.h` | MatchingEngine | TOB, microprice, VWAP |
| `risk/risk_engine.h` | MatchingEngine | Pre-trade risk checks |
| `HPRingBuffer.hpp` | MarketDataEngine | Lock-free MD message queue |
| `ScopeTimer.hpp` | MatchingEngine | Performance instrumentation |
## Usage Examples

### Matching Engine with L3 Book

```cpp
#include "common/clock.h"
#include "common/types.h"
#include "market/matching_engine.h"
#include "risk/risk_engine.h"
#include <iostream>

using namespace HFT_sdk;

int main() {
    Clock clock(Clock::Mode::Simulated);
    MatchingEngine engine(clock);

    // Optional: attach risk engine
    RiskEngine risk;
    risk.setDefaultLimits({.max_position_per_symbol = 10000});
    engine.set_risk_engine(&risk);

    // Register callbacks
    engine.on_trade([](const Trade& t) {
        std::cout << "TRADE: " << t.qty << " @ " << t.price << "\n";
    });

    engine.on_execution([](const ExecutionReport& rpt) {
        std::cout << "EXEC: order=" << rpt.order_id
                  << " status=" << static_cast<int>(rpt.status) << "\n";
    });

    engine.on_l1_update([](const TopOfBook& tob) {
        if (tob.valid) {
            std::cout << "BBO: " << tob.best_bid.price
                      << " / " << tob.best_ask.price
                      << " spread=" << tob.spread << "\n";
        }
    });

    // Register symbols
    engine.add_symbol("AAPL");
    engine.add_symbol("GOOGL");

    // Submit a buy order
    Order buy{};
    buy.id = 1; buy.trader_id = 100; buy.symbol = Symbol("AAPL");
    buy.side = Side::Buy; buy.type = OrderType::Limit;
    buy.tif = TimeInForce::Day; buy.price = 150; buy.quantity = 100;
    engine.process_new_order(buy);

    // Submit a crossing sell order → triggers trade
    Order sell{};
    sell.id = 2; sell.trader_id = 101; sell.symbol = Symbol("AAPL");
    sell.side = Side::Sell; sell.type = OrderType::Limit;
    sell.tif = TimeInForce::IOC; sell.price = 150; sell.quantity = 50;
    engine.process_new_order(sell);

    // Cancel remaining order
    CancelRequest cancel{};
    cancel.order_id = 1; cancel.symbol = Symbol("AAPL");
    engine.process_cancel(cancel);

    // Statistics
    std::cout << "Orders: " << engine.total_orders_processed()
              << " Trades: " << engine.total_trades()
              << " Cancels: " << engine.total_cancels() << "\n";
}
```

### Trade Engine with Position Tracking

```cpp
#include "market/trade_engine.h"
#include <iostream>

using namespace HFT_sdk;

int main() {
    TradeEngine trade_engine;

    trade_engine.on_position([](TraderId id, const Position& pos) {
        std::cout << "Trader " << id << " net=" << pos.net_qty
                  << " realized_pnl=" << pos.realized_pnl << "\n";
    });

    Trade trade{};
    trade.incoming_trader = 100; trade.resting_trader = 101;
    trade.symbol = Symbol("AAPL"); trade.aggressor_side = Side::Buy;
    trade.price = 150; trade.qty = 50;
    trade_engine.process_trade(trade);

    // Mark-to-market
    trade_engine.mark_to_market(Symbol("AAPL"), 155);

    auto* pos = trade_engine.get_position(100, Symbol("AAPL"));
    if (pos) {
        std::cout << "Unrealized PnL: " << pos->unrealized_pnl << "\n";
    }
}
```

### Market Data Engine with Throttling

```cpp
#include "market/market_data_engine.h"
#include <iostream>

using namespace HFT_sdk;

int main() {
    Clock clock(Clock::Mode::Simulated);
    hft_sim::MarketDataEngine md_engine(clock);

    md_engine.set_throttle_interval_ns(1'000'000);  // 1ms throttle
    md_engine.set_l2_depth(5);

    md_engine.subscribe_l1([](const TopOfBook& tob) {
        std::cout << "L1 update: mid=" << tob.mid_price << "\n";
    });

    md_engine.subscribe_trades([](const Trade& t) {
        std::cout << "Trade: " << t.qty << " @ " << t.price << "\n";
    });

    // Process queued messages (call from MD publisher thread)
    md_engine.process_pending();
}
```

## Order Matching Logic

### Price-Time Priority

Orders are matched according to **price-time priority** in the L3 order book:

1. **Price Priority**: Better prices match first
   - For bids: Higher prices have priority
   - For asks: Lower prices have priority

2. **Time Priority**: At the same price level, earlier orders match first
   - Orders are stored in a queue (`std::list`) per price level
   - First-in-first-out (FIFO) within each price level

### Order Types

| Type | Behavior |
|------|----------|
| **Limit** | Match at specified price or better; remainder rests on the book |
| **Market** | Match at best available prices; walk the book until filled |

### Time-in-Force

| TIF | Behavior |
|-----|----------|
| **Day** | Remains active until filled or cancelled |
| **GTC** | Good-til-Cancel; persists across sessions |
| **IOC** | Execute immediately; cancel any unfilled portion |
| **FOK** | Fill entire quantity or reject completely |

### Advanced Features

- **Iceberg orders** — visible + hidden quantity; auto-replenish on fill
- **Queue position tracking** — each order knows its position in the price level
- **Quantity-ahead queries** — total visible quantity ahead of a given order
- **Cancel-replace** — atomic cancel + new order with price/quantity changes

## Performance Characteristics

### Complexity Analysis

| Operation | Complexity | Component |
|-----------|------------|-----------|
| `add_order()` | O(log n) + O(k) matching | L3OrderBook |
| `cancel_order()` | O(1) | L3OrderBook |
| `best_bid()`/`best_ask()` | O(1) | L3OrderBook |
| `bid_depth(d)`/`ask_depth(d)` | O(d) | L3OrderBook |
| `process_new_order()` | O(1) routing + book op | MatchingEngine |
| `process_cancel()` | O(1) routing + O(1) cancel | MatchingEngine |
| `process_trade()` | O(1) | TradeEngine |
| `on_tob_update()` | O(1) | MarketDataEngine |

### Performance Optimizations

1. **O(1) Order Cancellation** — `unordered_map<OrderId, OrderLocation>` with direct iterator access
2. **O(1) Symbol Routing** — `unordered_map` for instant order book / cancel routing
3. **Lock-free MD Queue** — `HPRingBuffer<MarketDataMessage, 16384>` for cross-thread data
4. **Cache-line aligned** structures (64 bytes) to prevent false sharing
5. **Per-symbol throttling** — configurable MD publish rate to prevent flooding

## Integration with Other HFT-sdk Modules

### With HPRingBuffer (lock-free inter-thread messaging)

```cpp
#include "market/matching_engine.h"
#include "HPRingBuffer.hpp"
#include "common/types.h"
#include <thread>

using namespace HFT_sdk;

// Producer → consumer via lock-free queue
HPRingBuffer<EngineEvent, 8192> order_queue;
HPRingBuffer<EngineEvent, 8192> exec_queue;

// Consumer loop
void matching_loop(MatchingEngine& engine) {
    while (auto event = order_queue.pop()) {
        if (event->type == EventType::NewOrder) {
            auto rpt = engine.process_new_order(event->order);
            // Push exec report to output queue
        }
    }
}
```

### With Telemetry

```cpp
#include "market/matching_engine.h"
#include "metrics/telemetry.h"

engine.on_trade([&telemetry](const Trade& t) {
    telemetry.record_trade();
    telemetry.record_fill(t.qty, t.price);
});

engine.on_execution([&telemetry](const ExecutionReport& rpt) {
    telemetry.record_order();
    if (rpt.status == OrderStatus::Rejected) telemetry.record_reject();
});
```

## Design Considerations

### Thread Safety

- Components are **NOT thread-safe** by design (single-threaded hot path)
- Use `HPRingBuffer` for inter-thread communication
- `MarketDataEngine` uses an internal lock-free queue for cross-thread MD publishing

### Recommended Threading Model

| Thread | Component | Queue |
|--------|-----------|-------|
| T1 | MatchingEngine + L3OrderBook | Reads from Order Queue |
| T2 | Simulation / FIX Gateway | Writes to Order Queue |
| T3 | MarketDataEngine | Reads from MD Queue |
| T4 | Telemetry | Read-only metrics access |

## License

Part of HFT-sdk — MIT License

Copyright (c) 2025 Omid Ardestani

## See Also

- [HFT-sdk Main Documentation](../../README.md)
- [L3 Order Book](../orderbook/l3_order_book.h) — Full order-level book
- [L2 Aggregator](../orderbook/l2_aggregator.h) — Depth snapshot aggregation
- [L1 Feed](../orderbook/l1_feed.h) — Top-of-book statistics
- [Risk Engine](../risk/risk_engine.h) — Pre-trade risk checks
- [Latency Model](../latency/latency_model.h) — Latency simulation
- [Telemetry](../metrics/telemetry.h) — Metrics & dashboard
- [HPRingBuffer](../HPRingBuffer.hpp) — Lock-free ring buffer
- [ScopeTimer](../ScopeTimer.hpp) — Performance measurement
