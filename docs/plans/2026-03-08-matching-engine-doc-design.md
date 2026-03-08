# Design: MATCHING_ENGINE.md

**Date:** 2026-03-08
**Scope:** `src/market/matching_engine.hpp/.cpp` — one learner-friendly entry-point doc for the full SDK

---

## Goal

Write `src/market/MATCHING_ENGINE.md` — a learner-friendly guide that a new joiner reads
*before* opening the source files. It serves as the **entry point** for the entire HFT-sdk:
it shows the big picture of how `MatchingEngine` coordinates all subsystems, and directs the
reader to the right doc for each subsystem after. The callback wiring pattern is given its own
dedicated section as the architectural centrepiece.

---

## Reading Order (in the broader SDK)

```
MATCHING_ENGINE.md    ← entry point (this doc)
       ↓
L3_ORDER_BOOK.md      ← the matching loop, iceberg orders, O(1) cancel
       ↓
L2_AGGREGATOR.md      ← depth snapshots derived from L3 data
       ↓
L1_FEED.md            ← TOB, microprice, VWAP
```

Mirrors the dependency graph: MatchingEngine owns and wires the three orderbook layers.

---

## File Placement

```
src/market/
  matching_engine.hpp
  matching_engine.cpp
  MATCHING_ENGINE.md      ← new
  README.md               ← existing overview, unchanged
```

---

## Doc Structure (7-Section Skeleton)

Follows the same skeleton as the orderbook docs, tuned for entry-point role.

### Section 1 — 30-second summary

One paragraph. `MatchingEngine` is the coordinator: it owns one `L3OrderBook` +
`L2Aggregator` + `L1Feed` per symbol, routes incoming orders/cancels/replaces to the
right book, runs optional pre-trade risk checks, and fires callbacks for every event
downstream consumers care about. It does not implement matching itself — that lives in
`L3OrderBook`. After reading this doc the reader knows where everything connects and
which doc to read next for each subsystem.

### Section 2 — Vocabulary table

| Term | Plain-English meaning |
|------|-----------------------|
| symbol state | the bundle of L3 book + L2 aggregator + L1 feed for one trading symbol |
| execution report | the acknowledgement sent back after any order event (new, fill, cancel, reject) |
| order routing | finding the correct per-symbol book to send an order to |
| pre-trade risk check | validation run *before* the order hits the book (position limits, rate limits, kill switch) |
| kill switch | an emergency flag on the risk engine that rejects all orders immediately |
| callback registration | storing a callable (`std::function`) on an object at startup, to be fired later when an event occurs |
| market data dissemination | publishing L1/L2 updates to downstream consumers after each order event |
| L1 / L2 / L3 | granularity levels of order book data (best bid/ask only → all price levels → individual orders) |

### Section 3 — ASCII diagram

Two-layer diagram: per-symbol state inside the engine, four event streams out.

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

### Section 4 — Data structures

Walk through with real code lines and `// <- annotation` comments:

- `SymbolState` — the three-field bundle (`unique_ptr<L3OrderBook>`, `unique_ptr<L2Aggregator>`,
  `unique_ptr<L1Feed>`); why grouping them in one struct keeps `symbols_` clean
- `symbols_` (`unordered_map<Symbol, SymbolState, SymbolHash>`) — O(1) per-symbol lookup;
  each entry created once in `add_symbol()`, never moved
- `order_symbol_index_` (`unordered_map<OrderId, Symbol>`) — why cancel routing needs a
  separate index; entry added when order rests, erased on cancel/fill
- The four `std::function` callback fields (`exec_cb_`, `trade_cb_`, `l1_cb_`, `l2_cb_`) —
  shown as real member declarations with annotations

### Section 5 — Algorithm narrative (MatchingEngine layer only)

Trace `process_new_order()` step by step, stopping at `book->add_order()`:

1. Symbol lookup — `symbols_.find(order.symbol)`; reject `UnknownSymbol` if not found
2. Risk check — `risk_engine_->check_order(order)`; build reject report and fire `exec_cb_` if any check fails
3. `book->add_order(order, ts)` — **this is where matching happens; see L3_ORDER_BOOK.md**
4. `order_symbol_index_` update — only if order rests (status not Filled/Rejected)
5. `risk_engine_->on_fill()` — keep position state current after any fill
6. `exec_cb_` fire — always, regardless of outcome
7. `publish_market_data()` — `l1->update()` + `l2->snapshot()` → `l2_cb_`

Brief paragraph each for `process_cancel()` and `process_replace()` as the same
pattern with fewer steps.

### Section 6 — Callback deep-dive (dedicated section, the centrepiece)

The three-layer wiring chain with diagram:

```
L3OrderBook::trade_cb_          (registered at add_symbol() time)
        │
        │  fires synchronously mid-match-loop for every fill
        ▼
MatchingEngine lambda
  ├── increments trades_generated_
  ├── calls l1->record_trade(price, qty)
  └── forwards to MatchingEngine::trade_cb_
        │
        ▼
app layer  (telemetry / FIX gateway / MarketDataEngine)
```

Cover:

- **The `std::function` bool guard** — `if (trade_cb_) trade_cb_(trade)`: `std::function`
  is falsy when empty; the guard makes every callback genuinely optional at zero overhead
- **Registration pattern** — `engine.on_trade([](const Trade& t){ ... })` called once at
  startup, stored, never changed again; the lambda is moved into the `std::function` via
  `std::move(cb)` in each `on_*` setter
- **Synchronous firing on the hot path** — callbacks execute on the matching engine thread,
  inside the match loop; handlers must be fast
- **All four callbacks and what each carries:**
  - `exec_cb_` — `ExecutionReport`: per-order outcome (new ack, partial fill, full fill,
    cancel ack, reject); every order event fires this
  - `trade_cb_` — `Trade`: a match event between two orders; fires once per fill inside
    the L3 matching loop then forwarded here
  - `l1_cb_` — `TopOfBook`: best bid/ask update; fires after book state settles via the
    TOB callback wired in `add_symbol()`
  - `l2_cb_` — `DepthSnapshot`: full N-level depth snapshot; fires from `publish_market_data()`
    after every order event if registered

HFT context callout: in production these callbacks are the wires to the market data
publisher, risk engine state update, and FIX execution report gateway — all decoupled
from the matching engine by design.

### Section 7 — Design decisions

- **`unordered_map` for `symbols_` and `order_symbol_index_`** — O(1) amortised lookup;
  `std::map` would be O(log n) per order, unacceptable on the hot path
- **`std::function` callbacks over virtual dispatch** — no vtable indirection; the compiler
  can inline small lambdas; the `if (cb)` guard is the entire integration cost
- **`RiskEngine*` raw nullable pointer** — makes risk checks genuinely optional without a
  null-object pattern; `if (risk_engine_)` is the whole integration point; no heap allocation
- **`SymbolState` with `unique_ptr` members** — each book's lifetime is owned by
  `MatchingEngine`; no shared ownership; books are never moved once created (iterators into
  them remain valid for the engine's lifetime)
- **`order_symbol_index_` separate from `SymbolState`** — cancel requests may not carry a
  symbol field (FIX `35=F` often omits it); the index provides O(1) routing without scanning
  all symbol books

### Section 8 — What to read next

```
MATCHING_ENGINE.md    ← you are here
L3_ORDER_BOOK.md      ← matching loop, iceberg orders, O(1) cancel
L2_AGGREGATOR.md      ← how L3 order data becomes depth snapshots
L1_FEED.md            ← TOB, microprice, VWAP
```

---

## Tone and Conventions

Same conventions as the orderbook doc series:

- Every piece of jargon defined in the vocabulary table before first use
- Code excerpts are real lines from the source, not paraphrased
- Inline annotations use `// <- ...` style
- HFT context callouts use `> **HFT context:**` block quote format
- No emoji. No filler. Direct sentences.
- Section headers `##` / `###` for scannable structure

---

## What This is Not

This doc is a primer — read before or alongside `.hpp`/`.cpp`. It does not replace the
source. The matching loop internals are covered in L3_ORDER_BOOK.md, not here.
