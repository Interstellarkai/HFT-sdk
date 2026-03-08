# MATCHING_ENGINE.md Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Write `src/market/MATCHING_ENGINE.md` — a learner-friendly entry-point doc for the full HFT-sdk, with a dedicated callback wiring section as the architectural centrepiece.

**Architecture:** Single markdown file in `src/market/` alongside the source. Follows the 7-section skeleton established by the orderbook doc series (`L3_ORDER_BOOK.md`, `L2_AGGREGATOR.md`, `L1_FEED.md`). Tuned for entry-point role: the algorithm narrative stops at `book->add_order()` and defers to `L3_ORDER_BOOK.md`; the callback section explains the full three-layer wiring chain.

**Tech Stack:** Markdown only. Source refs: `src/market/matching_engine.hpp`, `src/market/matching_engine.cpp`. Design doc: `docs/plans/2026-03-08-matching-engine-doc-design.md`.

---

## Reference Files

Read these before writing any section:

- `docs/plans/2026-03-08-matching-engine-doc-design.md` — approved design (section specs, diagrams, bullet points)
- `src/market/matching_engine.hpp` — public interface, callback types, member declarations
- `src/market/matching_engine.cpp` — `add_symbol()`, `process_new_order()`, `publish_market_data()` implementations
- `src/orderbook/L3_ORDER_BOOK.md` — tone reference; the doc this one introduces readers to
- `docs/plans/2026-03-07-orderbook-docs-design.md` — tone/conventions spec (no emoji, `// <-` annotations, `> **HFT context:**` callouts)

---

### Task 1: Create the file with Section 1 (30-second summary) and Section 2 (Vocabulary)

**Files:**
- Create: `src/market/MATCHING_ENGINE.md`

**Step 1: Write Section 1 — 30-second summary**

One paragraph. Must cover: MatchingEngine is the coordinator, owns one SymbolState (L3+L2+L1) per symbol, routes orders, runs optional risk checks, fires callbacks. Does NOT implement matching — that is in L3OrderBook. This doc is the entry point.

```markdown
# Matching Engine — Learner Guide

Read this before opening `matching_engine.hpp`. This document explains what the
`MatchingEngine` does, how it coordinates the orderbook layers beneath it, and how
events flow out of it to downstream consumers via callbacks. After reading this, see
[L3_ORDER_BOOK.md](../orderbook/L3_ORDER_BOOK.md) for how matching actually works
inside the order book.

---

## 1. What This Is

`MatchingEngine` is the coordinator of the entire HFT-sdk. For each trading symbol it
owns one `L3OrderBook` (the matching engine proper), one `L2Aggregator` (aggregated
depth), and one `L1Feed` (top-of-book and derived statistics). When an order arrives,
`MatchingEngine` validates the symbol, runs optional pre-trade risk checks, routes the
order to the correct per-symbol book, and fires callbacks to notify every downstream
consumer — execution reports back to the trader, trade events to the market data engine,
L1 and L2 updates to any subscriber. The matching logic itself — price-time priority,
iceberg replenishment, FOK/IOC handling — all lives in `L3OrderBook`. `MatchingEngine`
orchestrates; the book matches.

---
```

**Step 2: Write Section 2 — Vocabulary table**

```markdown
## 2. Vocabulary

| Term | Plain-English meaning |
|------|-----------------------|
| **symbol state** | The bundle of L3 book + L2 aggregator + L1 feed for one trading symbol. Each symbol the engine tracks has exactly one `SymbolState`. |
| **execution report** | The acknowledgement sent back after any order event: new order ack, partial fill, full fill, cancel ack, or reject. Modelled as `ExecutionReport` in this SDK. |
| **order routing** | Finding the correct per-symbol book to forward an order to. The engine keeps a map from `OrderId` to `Symbol` so that cancel requests can be routed even if the caller does not supply a symbol. |
| **pre-trade risk check** | Validation run *before* the order touches the book: position limits, order rate limits, self-trade prevention, kill switch. Enforced by `RiskEngine`. |
| **kill switch** | An emergency flag on `RiskEngine` that immediately rejects all incoming orders regardless of their content. |
| **callback registration** | Storing a callable (`std::function`) on an object at startup, to be invoked later when a specific event occurs. The caller registers once; the callee fires the stored function as needed. |
| **market data dissemination** | Publishing L1 and L2 updates to downstream subscribers after each order event changes the book state. |
| **L1 / L2 / L3** | Granularity levels of order book data. L1: best bid and ask only. L2: all price levels with total quantity at each. L3: every individual resting order in queue order. Each level is derived from the one below it. |

---
```

**Step 3: Verify**

Read the written sections. Check:
- No jargon used before it is defined in the vocabulary table
- Paragraph does not mention matching internals (those belong in L3_ORDER_BOOK.md)
- Table terms match what appears in the source (e.g. `ExecutionReport`, `RiskEngine`, `SymbolState`)

**Step 4: Commit**

```bash
git add src/market/MATCHING_ENGINE.md
git commit -m "docs: add MATCHING_ENGINE.md sections 1-2 (summary and vocabulary)"
```

---

### Task 2: Section 3 (ASCII diagram) and Section 4 (Data structures)

**Files:**
- Modify: `src/market/MATCHING_ENGINE.md`

**Step 1: Write Section 3 — ASCII diagram**

Show the two-layer picture: per-symbol state inside the engine, four event streams out.

```markdown
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
  │         ↑  order_symbol_index_  ↑                   │
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
```

**Step 2: Write Section 4 — Data structures**

Use real lines from `matching_engine.hpp` with `// <-` annotations.

```markdown
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

O(1) amortised lookup per order. `SymbolHash` is a custom hasher for the 8-byte
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

Each is a `std::function`. All four default to empty (falsy). The engine checks
`if (cb_)` before every call, so any callback is optional at zero overhead.
Registration is done once at startup via `on_execution()`, `on_trade()`,
`on_l1_update()`, `on_l2_update()`.

---
```

**Step 3: Verify**

- Code excerpts match actual lines in `matching_engine.hpp` exactly
- Annotations extend real comments rather than paraphrase
- `order_symbol_index_` explanation covers the FIX cancel routing motivation

**Step 4: Commit**

```bash
git add src/market/MATCHING_ENGINE.md
git commit -m "docs: add MATCHING_ENGINE.md sections 3-4 (diagram and data structures)"
```

---

### Task 3: Section 5 — Algorithm narrative (order lifecycle, ME layer only)

**Files:**
- Modify: `src/market/MATCHING_ENGINE.md`

**Step 1: Write the process_new_order() walkthrough**

Trace each step in `matching_engine.cpp::process_new_order()`. Stop at `book->add_order()` and redirect to L3_ORDER_BOOK.md.

```markdown
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

This is where matching happens. `L3OrderBook::add_order()` applies price-time priority,
handles IOC/FOK/GTC time-in-force, replenishes iceberg orders, and fires `trade_cb_`
for every fill. See [L3_ORDER_BOOK.md](../orderbook/L3_ORDER_BOOK.md) for the full
narrative.

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

Risk checks at step 2 read position state. `on_fill()` writes it. Calling this after
the book settles ensures the risk engine's position tracking stays current for the next
order from the same trader.

### Step 6 — Execution report and market data

```cpp
if (exec_cb_) exec_cb_(rpt);
publish_market_data(order.symbol);
```

`exec_cb_` always fires — even on a partial fill, even if the order still rests.
`publish_market_data()` refreshes the L1 feed and, if `l2_cb_` is registered, snapshots
the L2 book and fires it.

---

### Cancels and Replaces

`process_cancel()` follows the same structure with one addition: if the cancel request
omits the symbol, `order_symbol_index_` is consulted to find it. On success the
`order_symbol_index_` entry is erased.

`process_replace()` delegates to `L3OrderBook::replace_order()`, which is a cancel
followed by a new `add_order` at the new price. The replaced order loses its queue
priority.

---
```

**Step 2: Verify**

- Code excerpts match actual lines in `matching_engine.cpp`
- Section stops at `book->add_order()` — no matching loop internals here
- `process_cancel()` and `process_replace()` covered in brief summary paragraphs only

**Step 3: Commit**

```bash
git add src/market/MATCHING_ENGINE.md
git commit -m "docs: add MATCHING_ENGINE.md section 5 (algorithm narrative)"
```

---

### Task 4: Section 6 — Callback deep-dive (the centrepiece)

**Files:**
- Modify: `src/market/MATCHING_ENGINE.md`

**Step 1: Write the three-layer wiring diagram**

```markdown
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
    auto it = symbols_.find(trade.symbol);
    if (it != symbols_.end()) {
        it->second.l1->record_trade(trade.price, trade.qty);  // <- update L1 stats
    }
    if (trade_cb_) trade_cb_(trade);  // <- forward to whoever registered with the engine
});
```

The same pattern applies to the top-of-book callback: `L3OrderBook::tob_cb_` forwards
to `MatchingEngine::l1_cb_`.
```

**Step 2: Write the four callbacks with what each carries**

```markdown
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

Fires after `publish_market_data()` updates the L1 feed via `l1->update(ts)`, which
calls `L3OrderBook::top_of_book()` to read the current best bid and ask. Carries best
bid price and size, best ask price and size, spread, mid-price, and microprice.

```cpp
engine.on_l1_update([](const TopOfBook& tob) {
    // tob.best_bid.price, tob.best_bid.qty
    // tob.best_ask.price, tob.best_ask.qty
    // tob.spread, tob.mid_price, tob.micro_price
});
```

#### l2_cb_ — DepthSnapshot

Fires from `publish_market_data()` only if registered. Carries a fixed-size snapshot
of the N best bid and ask price levels. N is configured via `L2Aggregator`'s `depth_`
setting.

```cpp
engine.on_l2_update([](const DepthSnapshot& depth) {
    // depth.bids[0..N-1]: price levels sorted best-first
    // depth.asks[0..N-1]: price levels sorted best-first
    // each BookLevel: {price, qty, order_count}
});
```
```

**Step 3: Write the registration pattern and the bool guard**

```markdown
### Registration

Each callback is registered once at startup:

```cpp
engine.on_trade(std::move(my_handler));   // stores into trade_cb_
```

Inside the engine, each setter is:

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
does not register a handler, the check costs one branch and nothing more. There is no
null dereference risk and no need for a null-object pattern.

### Synchronous Execution on the Hot Path

All callbacks execute on the matching engine thread, inline with order processing.
`trade_cb_` fires mid-match-loop — before the resting order's quantity is reduced.
This means callback handlers must complete quickly. Any blocking operation (I/O, lock
contention, `std::cout`) adds directly to matching latency. In the reference
application, the callbacks write to `HPRingBuffer` queues (lock-free, nanosecond
overhead) and the actual I/O happens on separate threads.

> **HFT context:** In production matching engines, the callback is typically the write
> into a lock-free ring buffer. The market data publisher thread reads from the other
> end and fans out to all downstream subscribers. This keeps the matching thread
> deterministically fast while still delivering low-latency updates.

---
```

**Step 4: Verify**

- Three-layer wiring diagram matches actual code in `matching_engine.cpp:24-37`
- All four callbacks described with real field names from `types.h`
- Bool guard explanation is correct (`std::function` is falsy when empty)
- Synchronous hot-path callout is present
- HFT context callout uses `> **HFT context:**` format

**Step 5: Commit**

```bash
git add src/market/MATCHING_ENGINE.md
git commit -m "docs: add MATCHING_ENGINE.md section 6 (callback deep-dive)"
```

---

### Task 5: Section 7 (Design decisions) and Section 8 (What to read next)

**Files:**
- Modify: `src/market/MATCHING_ENGINE.md`

**Step 1: Write Section 7 — Design decisions**

```markdown
## 7. Design Decisions

### unordered_map for symbol and order routing

```cpp
std::unordered_map<Symbol, SymbolState, SymbolHash> symbols_;
std::unordered_map<OrderId, Symbol> order_symbol_index_;
```

Both maps are accessed on the hot path for every order. `std::map` would cost O(log n)
per lookup; `unordered_map` is O(1) amortised. For `symbols_` the map is populated
at startup and never grown during trading — no rehash risk on the hot path.

### std::function callbacks over virtual dispatch

Virtual dispatch requires a vtable pointer dereference and prevents inlining. A
`std::function` holding a small lambda can be inlined by the compiler and has no
vtable overhead. The `if (cb_)` guard makes callbacks optional at the cost of a single
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
books are never moved after creation — the callbacks registered in `add_symbol()` capture
`this` by pointer, which remains valid for the engine's lifetime.

### order_symbol_index_ kept separate from SymbolState

An alternative design would store a reverse map inside each book. But `OrderId`
uniqueness is engine-wide, not per-symbol, and cancel requests that omit the symbol
need a flat O(1) lookup without knowing which book to search first.

---
```

**Step 2: Write Section 8 — What to read next**

```markdown
## 8. What to Read Next

This doc is the entry point. The components `MatchingEngine` coordinates each have
their own learner guide:

```
MATCHING_ENGINE.md    ← you are here
       ↓
L3_ORDER_BOOK.md      ← the matching loop, iceberg orders, O(1) cancel
(src/orderbook/L3_ORDER_BOOK.md)
       ↓
L2_AGGREGATOR.md      ← how L3 order data becomes depth snapshots
(src/orderbook/L2_AGGREGATOR.md)
       ↓
L1_FEED.md            ← top-of-book, microprice, VWAP
(src/orderbook/L1_FEED.md)
```

Read them in order — each doc references the one above it, and the dependency graph
mirrors the reading order.
```

**Step 3: Verify the complete document end-to-end**

- Read `src/market/MATCHING_ENGINE.md` from top to bottom
- Check: no jargon used before defined in Section 2
- Check: code excerpts match actual source files exactly
- Check: Section 5 does not describe matching internals (stops at `book->add_order()`)
- Check: Section 6 callback chain matches `matching_engine.cpp:24-37` exactly
- Check: Section 8 links are correct relative paths

**Step 4: Final commit**

```bash
git add src/market/MATCHING_ENGINE.md
git commit -m "docs: add MATCHING_ENGINE.md sections 7-8 (design decisions and reading order)"
```

---

## Summary

| Task | Output | Commit message |
|------|--------|---------------|
| 1 | Sections 1-2: summary + vocabulary | `docs: add MATCHING_ENGINE.md sections 1-2` |
| 2 | Sections 3-4: diagram + data structures | `docs: add MATCHING_ENGINE.md sections 3-4` |
| 3 | Section 5: algorithm narrative | `docs: add MATCHING_ENGINE.md section 5` |
| 4 | Section 6: callback deep-dive | `docs: add MATCHING_ENGINE.md section 6` |
| 5 | Sections 7-8: design decisions + reading order | `docs: add MATCHING_ENGINE.md sections 7-8` |
