# Design: Orderbook Layer Documentation

**Date:** 2026-03-07
**Scope:** `src/orderbook/` — three learner-friendly markdown docs for C++ devs breaking into HFT

---

## Goal

Write three companion docs placed alongside the source files in `src/orderbook/`. A reader can
open `L3_ORDER_BOOK.md` before touching the `.hpp`/`.cpp` files and build intuition
progressively, top-to-bottom. The docs also serve someone trying to break into HFT — every design
decision is grounded in why it matters on a real trading desk.

---

## Reading Order

```
L3_ORDER_BOOK.md   ← read first  (the engine room)
       ↓
L2_AGGREGATOR.md   ← read second (the aggregation layer)
       ↓
L1_FEED.md         ← read third  (the broadcast layer)
```

Mirrors the code dependency graph: L2Aggregator and L1Feed both hold a `const L3OrderBook&`.

---

## File Placement

```
src/orderbook/
  l3_order_book.hpp
  l3_order_book.cpp
  L3_ORDER_BOOK.md      ← new
  l2_aggregator.hpp
  l2_aggregator.cpp
  L2_AGGREGATOR.md      ← new
  l1_feed.hpp
  l1_feed.cpp
  L1_FEED.md            ← new
```

---

## Per-Doc Structure (Approach C — Layered Progressive)

Every doc follows this 6-section skeleton, scaled to the complexity of that layer.

### Section 1 — 30-second summary
One paragraph. What this layer is, what problem it solves, where it sits in the stack.
No code. No jargon that hasn't been defined yet.

### Section 2 — Vocabulary table
Small table mapping HFT industry terms to plain English. Defined once, used freely thereafter.
Examples: price-time priority, aggressor, resting order, iceberg, spread, TOB.

### Section 3 — ASCII diagram
Visual of the key data structure or data flow. Enough to form a mental model before reading code.

### Section 4 — Data structures
Walk through the key structs and containers. Show real lines from the source with
`// <- annotation` comments. Explain every field that isn't self-evident.

### Section 5 — Algorithm narrative
Plain-English walkthrough of the main operation (add order, snapshot, update).
Interleaved with short code excerpts at key decision points.
Includes "In the real world" HFT context callouts as block-quoted asides.

### Section 6 — Design decisions
Why choices were made: data structure selection, alignment, O(1) guarantees, callback pattern.
Each decision tied to a real consequence (latency, correctness, allocation).

### Section 7 — What to read next
One or two pointers: the next doc in the sequence, and the file in `src/market/` where
these layers are wired together.

---

## HFT Context Integration

Two mechanisms for weaving in industry context without cluttering the learning flow:

**1. Vocabulary table (Section 2)**
Every doc opens with a small table. Reader builds the right vocabulary before the narrative.

**2. "In the real world" callouts (Sections 5–6)**
Block-quoted asides using the pattern:
```
> **HFT context:** ...
```
Placed at natural moments — after explaining a mechanism, before or after a design decision.
A reader who only wants code comprehension can skip these. A reader breaking into HFT gets
the "why this matters on a real desk" layer.

---

## Per-Doc Content Plan

### L3_ORDER_BOOK.md

**Vocabulary:** price-time priority, resting order, aggressor, iceberg order, queue position,
FOK, IOC, GTC, Day, cancel-replace, execution report

**Diagram:** Two sorted maps (bids descending, asks ascending). Each price level is a
`std::list<L3Order>` in arrival order. A separate `order_index_` hash map points an OrderId
directly to the iterator in the list.

**Data structures to cover:**
- `L3Order` — visible_qty vs hidden_qty vs filled_qty; queue_pos; alignas(64) rationale
- `BidPriceLevels` / `AskPriceLevels` — why `std::map` (sorted iteration) over `unordered_map`
- `order_index_` — the iterator trick that enables O(1) cancel
- `OrderLocation` — side + price + iterator (three fields, one lookup)
- `MatchResult` — trades vector + remaining_qty + fully_filled flag

**Algorithm narrative:** Walk a buy limit order end-to-end:
1. Duplicate check via `order_index_`
2. FOK pre-scan (why pre-scan: avoid partial fills that must be rolled back)
3. `match_against()` — walk asks in price order, walk queue in time order
4. Iceberg replenishment (hidden_qty -> visible_qty in 100-lot chunks)
5. `add_resting_order()` — push to list, store iterator in index
6. `publish_tob()` — fire callback after state settles

**HFT context callouts:**
- Queue position = money: being 1st vs 5th at the same price means different fill probability;
  market makers actively manage queue priority
- O(1) cancel: a slow cancel at a stale price = adverse selection; firms co-locate precisely
  to cancel faster than the market can move
- Iceberg orders: institutions hide size to avoid moving the market against themselves;
  detecting iceberg replenishment is a common alpha signal
- FOK usage: used by algos that need guaranteed execution or nothing (e.g., hedging legs)

**Design decisions:**
- `std::list` + stored iterator → O(1) erase without scanning
- `std::map` → sorted order naturally; `begin()` is always best bid/ask
- `alignas(64)` → one order per cache line; no false sharing in sequential scans
- Template `match_against<PriceLevels>` → same logic for bid and ask sides, zero duplication
- Explicit template instantiation in `.cpp` → keeps build times sane

---

### L2_AGGREGATOR.md

**Vocabulary:** Market-by-Price (MBP), depth snapshot, price level, aggregated quantity,
order count, book imbalance

**Diagram:** Five individual L3 orders at 100.00 (qty 100, 200, 50, 75, 75) collapse to a
single L2 level: `{price: 100.00, qty: 500, orders: 5}`. Show N levels stacked.

**Data structures to cover:**
- `DepthSnapshot` — fixed-size `bids[MAX_DEPTH_LEVELS]` / `asks[MAX_DEPTH_LEVELS]` arrays
- `BookLevel` — price + qty + order_count
- `depth_` cap and `MAX_L2_DEPTH` constant
- The `const L3OrderBook&` reference — L2 has no state of its own

**Algorithm narrative:** `snapshot()` call:
1. Delegate to `book_.bid_depth(depth_)` / `book_.ask_depth(depth_)` (L3 does the work)
2. Copy into fixed-size `DepthSnapshot` arrays (bounded by `MAX_DEPTH_LEVELS`)
3. Return by value — no heap allocation on the hot path

**HFT context callouts:**
- L1 vs L2 vs L3 data tiers: most retail brokers show only L1 (best bid/ask);
  L2 (depth) is a paid feed; L3 (order-level) costs significantly more and is only available
  via direct exchange connections — understanding what data your firm has access to shapes
  what signals you can build
- Book imbalance: `(bid_qty - ask_qty) / (bid_qty + ask_qty)` at top N levels is a classic
  short-term price direction signal; market makers use it to skew quotes

**Design decisions:**
- L2 is a read-only view: no caching, no state — always computed fresh from L3
- Fixed-size arrays in `DepthSnapshot` (not `std::vector`) → no heap allocation, cache-friendly,
  safe to pass across thread boundaries via `HPRingBuffer`
- Configurable `depth_` with `MAX_L2_DEPTH` cap → operators tune how many levels to publish
  without risking unbounded memory

---

### L1_FEED.md

**Vocabulary:** top of book (TOB), spread, mid-price, microprice, VWAP, rolling window,
fair value, execution benchmark

**Diagram:** TOB box showing best bid / best ask → spread formula → mid-price formula →
microprice formula (size-weighted). Separate trade history deque → VWAP accumulation.

**Data structures to cover:**
- `TopOfBook` — all fields: best_bid, best_ask, mid_price, micro_price, spread, valid flag
- `TradeRecord` deque — rolling 1000 trades for VWAP
- `spread_history_` deque — rolling 500 spreads for `rolling_spread(window)`
- `L1Callback` — `std::function<void(const TopOfBook&)>`

**Algorithm narrative:**
1. `update(ts)` — calls `book_.top_of_book(ts)`, appends to spread_history_, fires callback
2. `record_trade(price, qty)` — pushes to recent_trades_, evicts if over MAX_TRADE_HISTORY
3. `vwap()` — sum(price * qty) / sum(qty) over recent_trades_
4. `rolling_spread(window)` — backward iteration over spread_history_ deque

**HFT context callouts:**
- Microprice vs mid-price: mid splits the spread evenly; microprice tilts toward the side with
  more size (if 1000 on bid, 100 on ask, fair value is closer to the ask). Used by market
  makers as a real-time fair value estimate for quoting
- VWAP as execution benchmark: "did I beat VWAP?" is how execution algos are evaluated by
  buy-side desks; a sell that executes above VWAP is considered good
- Rolling spread: tight spreads = liquid market (easy to enter/exit); widening spreads signal
  uncertainty or low liquidity — a risk input for position sizing
- Callback pattern: in production, this callback is the wire to the market data publisher
  which fans out to all downstream subscribers (risk engine, strategy, FIX gateway)

**Design decisions:**
- `std::deque` for rolling windows → O(1) `push_back` + O(1) `pop_front`; no shifting
- MAX_TRADE_HISTORY = 1000, MAX_SPREAD_HISTORY = 500 → bounded memory, no unbounded growth
- L1Feed does not own the book → single source of truth; L1 is a derived view, not authoritative
- `[[nodiscard]]` on `update()` and `last()` → compiler warns if caller ignores the return value

---

## Tone and Conventions

- Every piece of jargon is explained in plain English the first time it appears
- Code excerpts are real lines from the source, not paraphrased
- Inline annotations use `// <- ...` style to extend real comments
- Section headers use `##` and `###` for scannable structure
- "In the real world" callouts use `> **HFT context:**` block quote format
- No emoji. No filler. Direct sentences.

---

## What This is Not

These docs do not replace the source code. They are a primer — read before or alongside the
`.hpp`/`.cpp` files. Implementation details that are self-evident from the code are not
repeated in the docs.
