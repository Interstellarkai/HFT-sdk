# Matching Engine — Learner Guide

Read this before opening `matching_engine.hpp`. This document explains what the
`MatchingEngine` does, how it coordinates the orderbook layers beneath it, and how
events flow out of it to downstream consumers via callbacks. After reading this, see
[L3_ORDER_BOOK.md](../orderbook/L3_ORDER_BOOK.md) for how matching actually works
inside the order book.

---

## 1. What This Is

`MatchingEngine` is the coordinator of the entire HFT-sdk. For each trading symbol it
owns one `L3OrderBook` (the per-symbol order book), one `L2Aggregator` (aggregated
depth), and one `L1Feed` (top-of-book and derived statistics). When an order arrives,
`MatchingEngine` validates the symbol, runs optional pre-trade risk checks, routes the
order to the correct per-symbol book, and fires separate registered callbacks to notify downstream consumers — execution reports back to the trader, trade events to subscribers, L1 and L2 market data updates to any registered listener. The matching logic itself — price-time priority,
iceberg replenishment, FOK/IOC handling — all lives in `L3OrderBook`. `MatchingEngine`
orchestrates; the book matches.

---

## 2. Vocabulary

| Term | Plain-English meaning |
|------|-----------------------|
| **symbol state** | The bundle of L3 book + L2 aggregator + L1 feed for one trading symbol. Each symbol the engine tracks has exactly one `SymbolState`. |
| **execution report** | The acknowledgement sent back after any order event: new order ack, partial fill, full fill, cancel ack, or reject. Modelled as `ExecutionReport` in this SDK. |
| **order routing** | Dispatching an incoming order, cancel, or replace request to the correct per-symbol `L3OrderBook`. |
| **pre-trade risk check** | Validation run *before* the order touches the book: position limits, order rate limits, self-trade prevention, kill switch. Enforced by `RiskEngine`. |
| **kill switch** | An emergency flag on `RiskEngine` that immediately rejects all incoming orders regardless of their content. |
| **callback registration** | Storing a callable (`std::function`) on an object at startup, to be invoked later when a specific event occurs. The caller registers once; the callee fires the stored function as needed. |
| **market data dissemination** | Publishing L1 and L2 updates to downstream subscribers after each order event changes the book state. |
| **L1 / L2 / L3** | Granularity levels of order book data. L1: best bid and ask only. L2: all price levels with total quantity at each. L3: every individual resting order in queue order. Each level is derived from the one below it. |

---

## 3. Architecture Diagram

```
  ┌─────────────────────────────────────────────────────┐
  │                   MatchingEngine                    │
  │  ┌────────────┐  ┌────────────┐  ┌────────────┐    │
  │  │SymbolState │  │SymbolState │  │SymbolState │    │
  │  │ L3OrderBook│  │ L3OrderBook│  │ L3OrderBook│    │
  │  │ L2Aggreg.  │  │ L2Aggreg.  │  │ L2Aggreg.  │    │
  │  │ L1Feed     │  │ L1Feed     │  │ L1Feed     │    │
  │  └────────────┘  └────────────┘  └────────────┘    │
  │                                                     │
  │  order_symbol_index_: OrderId → Symbol  (routing)  │
  └─────────────────────┬───────────────────────────────┘
   callbacks fire       ├───────────────────▶ exec_cb_  (ExecutionReport)
   synchronously on     ├───────────────────▶ trade_cb_ (Trade)
   every order event    ├───────────────────▶ l1_cb_    (TopOfBook)
                        └───────────────────▶ l2_cb_    (DepthSnapshot)
```

`order_symbol_index_` is a flat `unordered_map<OrderId, Symbol>` that sits outside
the per-symbol state. It is the engine's routing table for cancel requests that do
not carry a symbol field.

---

## 4. Data Structures

### SymbolState

```cpp
struct SymbolState {
  std::unique_ptr<L3OrderBook> book;   // <- the order book for this symbol
  std::unique_ptr<L2Aggregator> l2;   // <- aggregated depth derived from book
  std::unique_ptr<L1Feed> l1;         // <- top-of-book, microprice, VWAP
};
```

Grouping the three objects into one struct keeps `symbols_` clean — one map lookup
retrieves everything needed to process an order for that symbol.

`unique_ptr` ownership means the books are created once in `add_symbol()` and live
until the engine is destroyed. Because the books are never moved after creation, any
iterators or references into them (e.g. the callback lambdas capturing `this`) remain
valid for the engine's lifetime.

### symbols_ — per-symbol routing

```cpp
std::unordered_map<Symbol, SymbolState, SymbolHash> symbols_;
```

O(1) expected lookup per order — the map is populated at startup and never grown during trading, so there is no rehash risk on the hot path. `SymbolHash` is a custom hasher for the 8-byte
fixed-size `Symbol` type. A symbol is registered via `add_symbol()` and never removed
during normal operation.

### order_symbol_index_ — cancel routing

```cpp
std::unordered_map<OrderId, Symbol> order_symbol_index_;
```

FIX cancel requests (`35=F`) may omit the symbol field. Without this index the engine
would have to scan every symbol's book to find the order — O(symbols × book depth).
With the index it is O(1). An entry is added when an order rests on the book (not
immediately filled or rejected) and erased when the order is canceled or fully filled.

### Callback fields

```cpp
ExecutionCallback exec_cb_;   // <- void(const ExecutionReport&)
TradeCallback     trade_cb_;  // <- void(const Trade&)
L1Callback        l1_cb_;     // <- void(const TopOfBook&)
L2Callback        l2_cb_;     // <- void(const DepthSnapshot&)
```

Each type alias (`ExecutionCallback`, `TradeCallback`, etc.) is defined in `MatchingEngine`'s public section via `using`. All four fields default to empty (falsy). The engine checks `if (cb_)` before every call, so any callback is optional with negligible overhead. Registration is done once at startup via `on_execution()`, `on_trade()`, `on_l1_update()`, `on_l2_update()`.

---

## 5. Order Lifecycle — The MatchingEngine Layer

This section traces a new limit order through `process_new_order()`. Read this to
understand what the engine *owns*; the matching loop inside `add_order()` is covered
in [L3_ORDER_BOOK.md](../orderbook/L3_ORDER_BOOK.md).

### Step 1 — Symbol lookup

```cpp
auto it = symbols_.find(order.symbol);
if (it == symbols_.end()) {
    // build ExecutionReport, set status = Rejected, reason = UnknownSymbol
    if (exec_cb_) exec_cb_(rpt);
    return rpt;
}
```

If the symbol was not registered via `add_symbol()`, the order is rejected immediately.
`exec_cb_` fires even on reject — the caller always receives an outcome.

### Step 2 — Pre-trade risk checks

```cpp
if (risk_engine_) {
    auto reject = risk_engine_->check_order(order);
    if (reject != RejectReason::None) {
        // build reject report, fire exec_cb_, return
    }
}
```

`RiskEngine` is optional. If attached (`set_risk_engine(&risk)`), it runs its check
chain: kill switch → position limit → rate limit → self-trade prevention → price
validity → quantity validity. The first failing check produces a `RejectReason`. The
engine builds a reject `ExecutionReport`, fires `exec_cb_`, and returns without
touching the book.

### Step 3 — Book submission

```cpp
auto rpt = it->second.book->add_order(order, ts);
```

This is where matching happens. The book applies price-time priority, handles
time-in-force rules, and fires callbacks for each fill — see
[L3_ORDER_BOOK.md](../orderbook/L3_ORDER_BOOK.md) for the full narrative.

### Step 4 — order_symbol_index_ update

```cpp
if (rpt.status != OrderStatus::Rejected &&
    rpt.status != OrderStatus::Filled) {
    order_symbol_index_[order.id] = order.symbol;
}
```

Only resting orders are indexed. Fully filled or rejected orders do not need cancel
routing, so indexing them would waste memory and require an extra erase on fill.

### Step 5 — Risk engine post-fill update

```cpp
if (risk_engine_ && rpt.filled_qty > 0) {
    risk_engine_->on_fill(order.trader_id, order.symbol, order.side,
                          rpt.filled_qty, rpt.last_price);
}
```

Risk checks at step 2 read position state. `on_fill()` writes it. Calling this after `add_order()` returns ensures position state reflects the completed
fill before the next order from the same trader is checked.

### Step 6 — Execution report and market data

```cpp
if (exec_cb_) exec_cb_(rpt);
publish_market_data(order.symbol);
```

`exec_cb_` always fires — even on a partial fill, even if the order still rests.
`publish_market_data()` calls `l1->update(ts)` (refreshing internal L1 state) and, if
`l2_cb_` is registered, snapshots the depth and fires it. Note: `l1_cb_` fires
separately — it is wired via the book's `on_top_of_book` callback in `add_symbol()`,
and triggers during `add_order()` whenever the best bid or ask changes.

---

### Cancels and Replaces

`process_cancel()` follows the same structure with one addition: if the cancel request
omits the symbol, `order_symbol_index_` is consulted to find it. On success the
`order_symbol_index_` entry is erased.

`process_replace()` delegates to `L3OrderBook::replace_order()`, which is a cancel
followed by a new `add_order` at the new price. The replaced order loses its queue
priority. The original `order_symbol_index_` entry remains valid — the replacement
keeps the same `OrderId`.

---

## 6. Callbacks — How Events Flow Out

Callbacks are the engine's event bus. Instead of the `MatchingEngine` calling into
specific downstream systems directly, it stores a `std::function` for each event type.
At startup the caller registers what it wants to happen; at runtime the engine fires
the stored functions. The components are fully decoupled: the engine does not know or
care what the callbacks do.

### The Wiring Chain

When a match occurs, the event travels through three layers:

```
L3OrderBook::trade_cb_          ← registered at add_symbol() time
        │
        │  fires synchronously mid-match-loop, once per fill
        ▼
MatchingEngine lambda
  ├── trades_generated_++
  ├── l1->record_trade(trade.price, trade.qty)
  └── if (trade_cb_) trade_cb_(trade)   ← forward to app layer
                │
                ▼
        app layer callback
        (telemetry, FIX gateway, MarketDataEngine, ...)
```

The lambda in the middle is registered inside `add_symbol()`:

```cpp
state.book->on_trade([this](const Trade& trade) {
    trades_generated_++;
    // Update L1 feed with trade data
    auto it = symbols_.find(trade.symbol);
    if (it != symbols_.end()) {
      it->second.l1->record_trade(trade.price, trade.qty);  // <- update L1 stats
    }
    if (trade_cb_) trade_cb_(trade);  // <- forward to whoever registered with the engine
});
```

The same pattern applies to the top-of-book callback: `L3OrderBook::tob_cb_` forwards
to `MatchingEngine::l1_cb_`.

### The Four Callbacks

#### exec_cb_ — ExecutionReport

Fires on every order event without exception: new order acknowledgement, partial fill,
full fill, cancel acknowledgement, reject. This is the execution feedback channel back
to the order originator (e.g. the FIX gateway thread).

```cpp
engine.on_execution([](const ExecutionReport& rpt) {
    // rpt.status: New | PartiallyFilled | Filled | Canceled | Rejected
    // rpt.filled_qty, rpt.last_price: fill details if applicable
    // rpt.reject_reason: RejectReason if status == Rejected
});
```

#### trade_cb_ — Trade

Fires once per matched fill inside the L3 book, forwarded through the lambda above.
A single incoming order may produce multiple Trade events if it sweeps several resting
orders at different prices.

```cpp
engine.on_trade([](const Trade& t) {
    // t.price, t.qty: the fill
    // t.resting_id, t.incoming_id: which two orders matched
    // t.aggressor_side: which side was the incoming order
});
```

#### l1_cb_ — TopOfBook

Fires via the book's `on_top_of_book` callback, wired in `add_symbol()`. It triggers
during `add_order()` whenever the best bid or ask changes — earlier in the lifecycle than
`l2_cb_`, which fires from `publish_market_data()` after the order fully settles. Carries
best bid price and size, best ask price and size, spread, mid-price, and microprice.

```cpp
engine.on_l1_update([](const TopOfBook& tob) {
    // tob.best_bid.price, tob.best_bid.qty
    // tob.best_ask.price, tob.best_ask.qty
    // tob.spread, tob.mid_price, tob.micro_price
});
```

#### l2_cb_ — DepthSnapshot

Fires from `publish_market_data()` only if registered. Carries a fixed-size snapshot
of the N best bid and ask price levels. N is configured via `L2Aggregator`'s depth
setting.

```cpp
engine.on_l2_update([](const DepthSnapshot& depth) {
    // depth.bids[0..N-1]: price levels sorted best-first
    // depth.asks[0..N-1]: price levels sorted best-first
    // each BookLevel: {price, qty, order_count}
});
```

### Registration

Each callback is registered once at startup:

```cpp
engine.on_trade(std::move(my_handler));   // stores into trade_cb_
```

Inside the engine, each setter moves the callable in:

```cpp
void on_trade(TradeCallback cb) { trade_cb_ = std::move(cb); }
```

`std::move` transfers the callable without copying it — important if the lambda
captures state by value.

### The `std::function` Bool Guard

Every callback site in the engine looks like this:

```cpp
if (trade_cb_) trade_cb_(trade);
```

`std::function` converts to `false` when it holds no target (default-constructed or
assigned `nullptr`). The guard makes every callback genuinely optional: if the caller
does not register a handler, the check costs one branch. There is no null dereference
risk and no need for a null-object pattern.

### Synchronous Execution on the Hot Path

All callbacks execute on the matching engine thread, inline with order processing.
`trade_cb_` fires mid-match-loop — before the resting order's quantity is reduced.
Callback handlers must complete quickly. Any blocking operation (I/O, lock contention,
`std::cout`) adds directly to matching latency. In the reference application, the
callbacks write into `HPRingBuffer` queues (lock-free, nanosecond overhead) and the
actual I/O happens on separate threads.

> **HFT context:** In production matching engines, the callback is typically the write
> into a lock-free ring buffer. The market data publisher thread reads from the other
> end and fans out to all downstream subscribers. This keeps the matching thread
> deterministically fast while still delivering low-latency updates.

---

## 7. Design Decisions

### unordered_map for symbol and order routing

```cpp
std::unordered_map<Symbol, SymbolState, SymbolHash> symbols_;
std::unordered_map<OrderId, Symbol> order_symbol_index_;
```

Both maps are accessed on the hot path for every order. `std::map` would cost O(log n)
per lookup; `unordered_map` is O(1) expected. For `symbols_` the map is populated at
startup and never grown during trading — no rehash risk on the hot path.

### std::function callbacks over virtual dispatch

Virtual dispatch requires a vtable pointer dereference and prevents inlining. A `std::function` requires no base class or vtable pointer on `MatchingEngine` itself, and the Small Buffer Optimisation avoids a heap allocation for small callables. The `if (cb_)` guard makes callbacks optional at the cost of a single
branch — cheaper than a null-object vtable call.

### RiskEngine as a nullable raw pointer

```cpp
RiskEngine* risk_engine_ = nullptr;
```

`RiskEngine` is optional. A `unique_ptr` would require a non-null object to be
heap-allocated even in configurations that do not use risk checks. A raw pointer with
`if (risk_engine_)` guards is the simplest expression of "this dependency may not
exist." There is no shared ownership, so `unique_ptr` ownership semantics are not needed.

### unique_ptr members in SymbolState

Each book is owned exclusively by `MatchingEngine`. `unique_ptr` makes that ownership
explicit and ensures the books are destroyed when the engine is destroyed. Critically,
`MatchingEngine` itself holds a `Clock&` reference member, making it non-moveable — so `this`, captured by the lambdas registered in `add_symbol()`, remains valid for the engine's lifetime.

### order_symbol_index_ kept separate from SymbolState

An alternative design would store a reverse map inside each book. But `OrderId`
uniqueness is engine-wide, not per-symbol, and cancel requests that omit the symbol
need a flat O(1) lookup without knowing which book to search first.

---

## 8. What to Read Next

This doc is the entry point. The components `MatchingEngine` coordinates each have
their own learner guide:

```
MATCHING_ENGINE.md    <- you are here
       |
L3_ORDER_BOOK.md      <- the matching loop, iceberg orders, O(1) cancel
(src/orderbook/L3_ORDER_BOOK.md)
       |
L2_AGGREGATOR.md      <- how L3 order data becomes depth snapshots
(src/orderbook/L2_AGGREGATOR.md)
       |
L1_FEED.md            <- top-of-book, microprice, VWAP
(src/orderbook/L1_FEED.md)
```

Read them in order — each doc references the one above it, and the reading order follows the conceptual layering from raw order data up to derived views.
