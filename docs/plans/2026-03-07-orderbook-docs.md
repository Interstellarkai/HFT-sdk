# Orderbook Layer Documentation Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Write three learner-friendly companion docs (`L3_ORDER_BOOK.md`, `L2_AGGREGATOR.md`, `L1_FEED.md`) in `src/orderbook/` for C++ devs breaking into HFT, following the approved design at `docs/plans/2026-03-07-orderbook-docs-design.md`.

**Architecture:** Each doc follows the same 6-section skeleton (summary → vocabulary → diagram → data structures → algorithm narrative → design decisions → what to read next). HFT industry context is woven in via vocabulary tables and `> **HFT context:**` block-quote callouts. Docs are placed alongside their source files in `src/orderbook/` for proximity.

**Tech Stack:** Markdown (GitHub-Flavored), real code excerpts from `src/orderbook/*.hpp` and `src/orderbook/*.cpp`.

---

## Reference Files

Before writing, keep these open:
- `src/orderbook/l3_order_book.hpp` — L3 types and public API
- `src/orderbook/l3_order_book.cpp` — matching logic, cancel, replace, query
- `src/orderbook/l2_aggregator.hpp` / `.cpp` — snapshot and depth delegation
- `src/orderbook/l1_feed.hpp` / `.cpp` — update, VWAP, rolling spread
- `docs/plans/2026-03-07-orderbook-docs-design.md` — approved design (source of truth)

---

### Task 1: Write `L3_ORDER_BOOK.md`

**Files:**
- Create: `src/orderbook/L3_ORDER_BOOK.md`

This is the longest and most complex doc. Write all sections in a single pass.

---

**Step 1: Write the file**

`src/orderbook/L3_ORDER_BOOK.md` full content:

```markdown
# L3 Order Book

> Read this before opening `l3_order_book.hpp` or `l3_order_book.cpp`.
> After this doc, continue with [`L2_AGGREGATOR.md`](L2_AGGREGATOR.md).

---

## What This Is

The L3 order book is the engine room. It stores **every individual order** — not
summaries, not totals, but each resting order by its unique ID, price, quantity,
and arrival time. When a new order arrives, the book decides immediately: does it
match against something already sitting on the book, or does it rest and wait?

Everything above this layer (L2 snapshots, L1 statistics) is derived from what
the L3 book knows. The L3 book is the single source of truth.

---

## Vocabulary

| Term | Plain meaning |
|------|---------------|
| **Price-time priority** | At the same price, the order that arrived first gets filled first |
| **Resting order** | An order sitting on the book, waiting for a match |
| **Aggressor** | The incoming order that crossed the spread and triggered a trade |
| **Iceberg order** | An order with a hidden quantity — only a slice is visible to the market at any time |
| **Queue position** | How many orders ahead of yours are resting at the same price |
| **FOK (Fill-or-Kill)** | Fill the entire order immediately or cancel the whole thing |
| **IOC (Immediate-or-Cancel)** | Fill as much as possible immediately, cancel any remainder |
| **GTC (Good-till-Cancelled)** | Rest on the book until explicitly cancelled |
| **Day** | Rest on the book until end of trading day |
| **Cancel-replace** | Atomically cancel an existing order and submit a new one |
| **Execution report** | The acknowledgement returned to the submitter: filled, partially filled, rejected, etc. |
| **Spread** | The gap between the best bid price and the best ask price |

---

## The Data Structure

```
bids_                                asks_
std::map<Price, OrderQueue>          std::map<Price, OrderQueue>
(descending — best bid at top)       (ascending — best ask at top)

  100.05 → [order_A, order_B]          100.06 → [order_C]
  100.04 → [order_D]                   100.07 → [order_E, order_F]
  100.03 → [order_G, order_H, order_I] 100.08 → [order_G2]

order_index_: unordered_map<OrderId, OrderLocation>
  order_A.id  →  { side: Buy, price: 100.05, iterator → order_A in list }
  order_B.id  →  { side: Buy, price: 100.05, iterator → order_B in list }
  order_C.id  →  { side: Sell, price: 100.06, iterator → order_C in list }
  ...
```

The bids map sorts prices **descending** (highest price first) so `bids_.begin()`
is always the best bid. The asks map sorts **ascending** (lowest price first) so
`asks_.begin()` is always the best ask.

Each price level holds a `std::list<L3Order>`. Lists preserve insertion order and,
critically, allow O(1) removal using a stored iterator — the key design decision
explained in the Design Decisions section below.

---

## Data Structures

### `L3Order` — one resting order

```cpp
struct alignas(64) L3Order {
    OrderId  id          = INVALID_ORDER_ID; // unique identifier
    TraderId trader_id   = 0;
    Side     side        = Side::Buy;
    Price    price       = 0;                // integer ticks, e.g. 10006 = 100.06
    Quantity visible_qty = 0;  // <- what the market sees
    Quantity hidden_qty  = 0;  // <- iceberg: concealed portion
    Quantity filled_qty  = 0;  // accumulated fills so far
    Timestamp entry_time = 0;
    uint32_t queue_pos   = 0;  // position within this price level (0 = first)
    bool     is_iceberg  = false;
    char     cl_ord_id[20] = {}; // FIX client order ID
};
```

`alignas(64)` places each order on its own cache line (64 bytes = one cache line on
x86). Sequential scans through a price level's queue touch one cache line per order
with no false sharing.

`visible_qty` + `hidden_qty` = total remaining quantity. Market participants (and the
matching engine for price checks) only see `visible_qty`. `hidden_qty` is the reserve
that replenishes `visible_qty` when it hits zero.

### `BidPriceLevels` and `AskPriceLevels`

```cpp
using OrderQueue      = std::list<L3Order>;
using BidPriceLevels  = std::map<Price, OrderQueue, std::greater<Price>>; // descending
using AskPriceLevels  = std::map<Price, OrderQueue, std::less<Price>>;    // ascending
```

`std::map` (a red-black tree) keeps prices sorted automatically. Insertion is O(log N)
in the number of distinct price levels — typically a small number. Iteration from
`begin()` always starts at the best price.

### `order_index_` — the O(1) cancel trick

```cpp
struct OrderLocation {
    Side              side;
    Price             price;
    OrderQueue::iterator it;  // <- direct pointer into the std::list
};
std::unordered_map<OrderId, OrderLocation> order_index_;
```

When an order is added to a price level's `std::list`, the iterator to its position
in the list is stored immediately in `order_index_`. A `std::list` iterator is stable
— it remains valid as long as the element exists, even if other elements are added or
removed.

Cancel therefore takes one hash lookup + one `list::erase(iterator)` = O(1), no
scanning required.

### `MatchResult` — what matching returns

```cpp
struct MatchResult {
    std::vector<Trade> trades;     // all trades generated in this match cycle
    Quantity remaining_qty = 0;    // unfilled quantity after matching
    bool     fully_filled  = false;
};
```

---

## Algorithm: What Happens When an Order Arrives

Walk through a **buy limit order** at price 100.06, quantity 300.

### 1. Duplicate check

```cpp
if (order_index_.count(order.id)) {
    rpt.status = OrderStatus::Rejected;
    rpt.reject_reason = RejectReason::DuplicateOrderId;
    return rpt;
}
```

Order IDs must be unique. A duplicate is immediately rejected — no matching attempted.

### 2. FOK pre-scan (if applicable)

For Fill-or-Kill orders, the engine first checks whether enough quantity is available
across all matchable price levels before touching any resting orders:

```cpp
if (order.tif == TimeInForce::FOK) {
    Quantity available = 0;
    for (auto& [price, queue] : asks_) {
        if (price > order.price) break;  // past our limit
        for (auto& o : queue) available += o.visible_qty + o.hidden_qty;
        if (available >= order.quantity) break;
    }
    if (available < order.quantity) return result; // cannot fill, abort
}
```

> **HFT context:** FOK orders are used when two legs of a trade must execute together
> or not at all — for example, hedging an options position with an equity leg. Partial
> fills would leave the firm exposed to unhedged risk. The pre-scan avoids the need to
> roll back partial fills, which would be expensive and complicated.

### 3. `match_against()` — the inner loop

The matching engine walks the opposite side of the book, price level by price level,
order by order within each level:

```cpp
while (remaining > 0 && level_it != levels.end()) {
    Price level_price = level_it->first;

    // Stop if limit price is breached
    if (incoming.type == OrderType::Limit) {
        if (incoming.side == Side::Buy  && level_price > incoming.price) break;
        if (incoming.side == Side::Sell && level_price < incoming.price) break;
    }

    auto& queue = level_it->second;
    auto order_it = queue.begin();

    while (remaining > 0 && order_it != queue.end()) {
        auto& resting = *order_it;
        Quantity match_qty = std::min(remaining, resting.visible_qty + resting.hidden_qty);
        // ... create trade, reduce quantities ...
    }
}
```

This is **price-time priority**: the outer loop walks prices best-to-worst; within a
price level, the inner loop walks orders front-to-back (oldest first).

> **HFT context:** Queue position is money. If you are order number 1 at 100.06 and
> a 300-lot sell order arrives, you fill. If you are order number 5 with 200 lots ahead
> of you, you may not. Market makers track their queue position per symbol and per price
> level in real time. When they cancel and re-enter an order (e.g., to update a price),
> they move to the back of the queue — a real cost.

### 4. Iceberg replenishment

After `visible_qty` reaches zero, the hidden reserve refills it in chunks:

```cpp
if (resting.visible_qty == 0 && resting.hidden_qty > 0 && resting.is_iceberg) {
    Quantity replenish = std::min(resting.hidden_qty, static_cast<Quantity>(100));
    resting.visible_qty = replenish;
    resting.hidden_qty -= replenish;
}
```

The order remains in its queue position during replenishment — the iceberg does not
lose time priority.

> **HFT context:** Iceberg orders exist because large visible orders move markets.
> If a 50,000-lot bid appears at 100.05, sellers know they have a buyer and will shade
> their prices up. By showing only 100 lots at a time, the institution conceals its
> full demand. Detecting iceberg replenishment — seeing the same price level refill
> repeatedly — is a known alpha signal used by quantitative firms.

### 5. `add_resting_order()` — parking the remainder

If a limit order has unfilled quantity after matching (and it's GTC or Day), it rests:

```cpp
queue.push_back(l3);
auto it = std::prev(queue.end());           // iterator to the new element
it->queue_pos = queue.size() - 1;           // position within this level
order_index_[order.id] = {side, price, it}; // store for O(1) future cancel
```

The iterator is stored immediately. Future cancels use this iterator directly.

### 6. Time-in-force handling after matching

| TIF | Remaining qty > 0 behaviour |
|-----|-----------------------------|
| GTC / Day | Rest on book |
| IOC | Cancel remainder, keep any partial fills |
| FOK | Reject entire order (no partial fill permitted) |

### 7. `publish_tob()` — notify listeners

After every state-changing operation, the best bid/ask is recomputed and the
`TopOfBookCallback` fires. This drives L1Feed and any downstream subscribers.

---

## Design Decisions

### Why `std::list` + stored iterator for O(1) cancel?

`std::list::erase(iterator)` is O(1) — no shifting, no scanning. The stored iterator
points directly to the element. Compare to `std::vector`: erasing from the middle is
O(N) due to shifting. For an exchange that may process thousands of cancels per second
per symbol, O(N) cancel is unacceptable.

> **HFT context:** Cancel latency is a first-order concern. A market maker quotes both
> bid and ask continuously. When the fair value moves, the maker must cancel stale
> quotes before the market trades against them at the wrong price — this is called
> adverse selection. Firms co-locate at the exchange precisely to send cancels faster
> than the market can react.

### Why `std::map` over `unordered_map` for price levels?

`std::map` keeps prices sorted. `begin()` is always the best price — no searching
required. Matching iterates from best to worst price naturally. With `unordered_map`
you'd need to find the best price on every match cycle. The number of distinct price
levels active at any time is small (typically tens to low hundreds), so tree overhead
is negligible.

### Why `alignas(64)` on `L3Order`?

One `L3Order` occupies exactly one 64-byte cache line. When the matching engine scans
a price level's order queue, it loads exactly one cache line per order — no wasted
bandwidth, no false sharing between adjacent orders in the list.

### Why a template for `match_against`?

```cpp
template <typename PriceLevels>
void L3OrderBook::match_against(PriceLevels& levels, ...);
```

The matching logic is identical for buy orders (matching against asks) and sell orders
(matching against bids). The only difference is the map type (`BidPriceLevels` vs
`AskPriceLevels`). The template eliminates duplication without runtime cost.

### Why explicit template instantiation in `.cpp`?

```cpp
// l3_order_book.cpp, bottom of file:
template void L3OrderBook::match_against<L3OrderBook::BidPriceLevels>(...);
template void L3OrderBook::match_against<L3OrderBook::AskPriceLevels>(...);
```

The template is defined in the `.cpp`, not the header, keeping implementation details
out of the public interface and keeping compile times predictable.

---

## What to Read Next

- **[`L2_AGGREGATOR.md`](L2_AGGREGATOR.md)** — how the L3 book is collapsed into
  price-level depth snapshots for downstream consumers
- **`src/market/matching_engine.hpp`** — where `L3OrderBook`, `L2Aggregator`, and
  `L1Feed` are instantiated and wired together with the risk engine and telemetry
```

---

**Step 2: Verify the file exists and renders**

```bash
wc -l src/orderbook/L3_ORDER_BOOK.md
```
Expected: > 200 lines.

Open the file in a Markdown previewer and confirm:
- All 6 sections present
- ASCII diagram renders as a code block
- Vocabulary table renders as a table
- HFT context callouts appear as block quotes
- Code excerpts have syntax highlighting (` ```cpp `)

**Step 3: Commit**

```bash
git add src/orderbook/L3_ORDER_BOOK.md
git commit -m "docs: add L3_ORDER_BOOK.md — learner guide to the L3 order book"
```

---

### Task 2: Write `L2_AGGREGATOR.md`

**Files:**
- Create: `src/orderbook/L2_AGGREGATOR.md`

---

**Step 1: Write the file**

`src/orderbook/L2_AGGREGATOR.md` full content:

```markdown
# L2 Aggregator

> Read [`L3_ORDER_BOOK.md`](L3_ORDER_BOOK.md) first.
> After this doc, continue with [`L1_FEED.md`](L1_FEED.md).

---

## What This Is

The L2 aggregator is a thin read-only lens over the L3 book. It collapses individual
orders into **price-level totals** — aggregating all the quantities resting at each
price into a single number, per price, per side. This is called a **Market-by-Price**
(MBP) view, and it is what most market participants actually receive.

The L2 aggregator holds no state of its own. Every call recomputes from the L3 book.
It is a view, not a cache.

---

## Vocabulary

| Term | Plain meaning |
|------|---------------|
| **Market-by-Price (MBP)** | A view of the book that shows total quantity at each price, not individual orders |
| **Market-by-Order (MBO)** | A view that shows every individual order — this is what L3 provides |
| **Depth snapshot** | A point-in-time capture of N price levels on both sides of the book |
| **Price level** | A single price point and the aggregated quantity resting there |
| **Book imbalance** | The relative difference in quantity between bid side and ask side |
| **Order count** | How many individual orders make up the quantity at a given price level |

---

## The Aggregation

```
L3 Book (Market-by-Order)          L2 View (Market-by-Price)

Bids at 100.05:                    Bid levels (top 3):
  order_A: qty=100                   100.05  qty=375  orders=3
  order_B: qty=200     ──────────>   100.04  qty=150  orders=1
  order_C: qty=75                    100.03  qty=420  orders=3

Bids at 100.04:
  order_D: qty=150

Bids at 100.03:
  order_G: qty=200
  order_H: qty=120
  order_I: qty=100
```

Five individual orders at 100.05 become one number: 375. The identity of each order,
its arrival time, and its queue position are invisible at L2.

---

## Data Structures

### `DepthSnapshot` — the wire format

```cpp
struct DepthSnapshot {
    Symbol    symbol;
    Timestamp timestamp;
    BookLevel bids[MAX_DEPTH_LEVELS]; // <- fixed-size array, not std::vector
    BookLevel asks[MAX_DEPTH_LEVELS];
    uint32_t  bid_levels = 0;         // how many entries in bids[] are valid
    uint32_t  ask_levels = 0;
};
```

`bids` and `asks` are fixed-size C arrays, not `std::vector`. This matters for the
hot path: no heap allocation, no pointer indirection, safe to copy into an
`HPRingBuffer` slot by value.

### `BookLevel` — one aggregated price level

```cpp
struct BookLevel {
    Price    price;
    Quantity qty;          // total visible quantity at this price
    uint32_t order_count;  // number of individual orders making up qty
};
```

`order_count` lets a subscriber infer whether a large quantity is one iceberg or many
small orders — a meaningful signal even at L2.

### `L2Aggregator` — the class

```cpp
class L2Aggregator {
  explicit L2Aggregator(const L3OrderBook& book,
                        std::size_t depth = DEFAULT_L2_DEPTH);

  [[nodiscard]] DepthSnapshot snapshot(Timestamp ts) const;
  [[nodiscard]] std::vector<BookLevel> bid_levels() const;
  [[nodiscard]] std::vector<BookLevel> ask_levels() const;

 private:
  const L3OrderBook& book_; // <- read-only reference, never modified
  std::size_t depth_;        // configurable: how many levels to include
};
```

`depth_` is capped at `MAX_L2_DEPTH` (defined in `src/common/constants.hpp`).
Operators tune this at startup via the `l2_depth` config key.

---

## Algorithm: `snapshot()`

The entire implementation is five lines:

```cpp
DepthSnapshot L2Aggregator::snapshot(Timestamp ts) const {
    DepthSnapshot snap{};
    snap.symbol    = book_.symbol();
    snap.timestamp = ts;

    auto bids = book_.bid_depth(depth_); // <- L3 does the aggregation work
    auto asks = book_.ask_depth(depth_);

    snap.bid_levels = static_cast<uint32_t>(bids.size());
    snap.ask_levels = static_cast<uint32_t>(asks.size());

    for (std::size_t i = 0; i < bids.size() && i < MAX_DEPTH_LEVELS; ++i)
        snap.bids[i] = bids[i];
    for (std::size_t i = 0; i < asks.size() && i < MAX_DEPTH_LEVELS; ++i)
        snap.asks[i] = asks[i];

    return snap;
}
```

`book_.bid_depth(n)` (defined in `l3_order_book.cpp`) iterates the sorted bid map
from best to worst price, aggregating `visible_qty` across all orders at each level,
returning up to `n` `BookLevel` structs.

The L2 aggregator copies those into a fixed-size `DepthSnapshot` and returns. No
allocation on the hot path.

> **HFT context:** Most of the world trades on L2 data. A typical retail brokerage
> shows you the top 5 bid and ask levels. Institutional traders pay for direct exchange
> feeds which include L2 with order counts. L3 (order-level, MBO) is only available
> via expensive co-located direct connections — the same connections that let HFT firms
> cancel in microseconds. Understanding which data tier your strategy requires is a
> fundamental infrastructure cost decision.

---

## Design Decisions

### Why is L2Aggregator stateless (no caching)?

Every `snapshot()` call recomputes from the L3 book. There is no cached depth array.
This means L2 is always consistent with L3 — no stale data, no invalidation logic.
The L3 `bid_depth()` / `ask_depth()` calls are O(depth) iterations over a sorted map:
fast enough to call on every market data publish cycle.

If you needed higher throughput, you could maintain an incremental L2 state updated
on each L3 event. This implementation deliberately keeps it simple.

### Why fixed-size arrays in `DepthSnapshot` instead of `std::vector`?

`DepthSnapshot` is designed to travel through `HPRingBuffer<DepthSnapshot, N>` — the
lock-free SPSC queue used for cross-thread market data. Ring buffer slots are
fixed-size. A `std::vector` inside a ring buffer slot would require a heap allocation
per snapshot and ownership management across threads — both unacceptable on a hot path.
A fixed array avoids all of this.

> **HFT context:** In production exchange market data protocols (ITCH, PITCH, OPRA),
> depth snapshots are fixed-length binary messages for exactly this reason — no dynamic
> allocation, deterministic size, safe to memcpy.

### Why is `depth_` configurable?

Publishing 20 levels of depth is more expensive than publishing 5. Operators tune this
based on what downstream consumers actually need. Strategy code that only trades
against the top 2 levels wastes CPU and bandwidth consuming 10.

---

## What to Read Next

- **[`L1_FEED.md`](L1_FEED.md)** — how the top of the L2 book is condensed further
  into scalar signals: spread, mid-price, microprice, and VWAP
- **`src/market/matching_engine.hpp`** — where `L2Aggregator` is instantiated and its
  `snapshot()` output is published to the market data engine
```

---

**Step 2: Verify the file exists**

```bash
wc -l src/orderbook/L2_AGGREGATOR.md
```
Expected: > 120 lines.

Confirm all sections render: vocabulary table, ASCII aggregation diagram, code blocks,
HFT context callouts.

**Step 3: Commit**

```bash
git add src/orderbook/L2_AGGREGATOR.md
git commit -m "docs: add L2_AGGREGATOR.md — learner guide to L2 depth aggregation"
```

---

### Task 3: Write `L1_FEED.md`

**Files:**
- Create: `src/orderbook/L1_FEED.md`

---

**Step 1: Write the file**

`src/orderbook/L1_FEED.md` full content:

```markdown
# L1 Feed

> Read [`L2_AGGREGATOR.md`](L2_AGGREGATOR.md) first.
> After this doc, open `src/market/matching_engine.hpp` to see where all three layers
> are wired together.

---

## What This Is

The L1 feed is the broadcast layer. It takes the top of the L3 book — just the single
best bid price and best ask price — and derives a set of scalar signals from it: the
spread, the mid-price, the microprice, and (from recorded trades) the VWAP.

These signals are what most consumers actually use. A strategy does not need to know
that there are 47 orders at 100.05; it needs to know that fair value is approximately
100.055 and the spread is 0.01.

L1Feed also maintains two rolling windows: one over recent spreads (for spread
statistics) and one over recent trades (for VWAP). Both are bounded in size.

---

## Vocabulary

| Term | Plain meaning |
|------|---------------|
| **Top of book (TOB)** | The single best bid and best ask — the tightest prices available |
| **Spread** | `best_ask - best_bid` — the cost of crossing the market immediately |
| **Mid-price** | `(best_bid + best_ask) / 2` — the geometric center of the spread |
| **Microprice** | Size-weighted mid: tilts toward the side with more quantity |
| **VWAP** | Volume-Weighted Average Price — the average trade price weighted by size |
| **Rolling window** | A fixed-length recent history (oldest entries drop off automatically) |
| **Fair value** | A model's estimate of where the true price of an asset should be |
| **Execution benchmark** | A reference price used to evaluate how well a trade was executed |

---

## Data Flow

```
L3OrderBook
    │
    │  book_.top_of_book(ts)
    ▼
┌─────────────────────────────────────────────┐
│  TopOfBook                                  │
│    best_bid:   { price: 100.05, qty: 375 }  │
│    best_ask:   { price: 100.06, qty: 120 }  │
│    spread:     1   (tick units)             │
│    mid_price:  100055  (integer ticks)      │
│    micro_price: ← size-weighted (see below) │
└───────────┬─────────────────────────────────┘
            │ stored in spread_history_ deque
            │ fires L1Callback
            ▼
    downstream subscribers
    (market data publisher, strategy, risk engine)

record_trade(price, qty)
    │
    ▼
recent_trades_ deque (max 1000)
    │
    └─→ vwap() = Σ(price × qty) / Σ(qty)
```

---

## Data Structures

### `TopOfBook` — the core output

```cpp
struct TopOfBook {
    Symbol    symbol;
    Timestamp timestamp;
    BookLevel best_bid;     // price + qty + order_count at best bid
    BookLevel best_ask;     // price + qty + order_count at best ask
    Price     mid_price;    // (best_bid.price + best_ask.price) / 2
    Price     micro_price;  // size-weighted mid (see algorithm below)
    Price     spread;       // best_ask.price - best_bid.price
    bool      valid = false; // false if book has no bid or no ask
};
```

`valid` is false when the book is one-sided (e.g., all bids, no asks). Consumers
must check this before using the price fields.

### `TradeRecord` deque — VWAP history

```cpp
struct TradeRecord {
    Price    price;
    Quantity quantity;
};
std::deque<TradeRecord> recent_trades_; // max 1000 entries
```

`std::deque` gives O(1) `push_back` (new trade) and O(1) `pop_front` (evict oldest).
No shifting. The window is bounded at `MAX_TRADE_HISTORY = 1000`.

### `spread_history_` deque — rolling spread

```cpp
std::deque<Price> spread_history_; // max 500 entries
```

Each `update()` call appends the current spread. `rolling_spread(window)` reads the
last `window` entries backward and returns their average. Bounded at
`MAX_SPREAD_HISTORY = 500`.

### `L1Callback`

```cpp
using L1Callback = std::function<void(const TopOfBook&)>;
```

Registered via `on_update(cb)`. Fires on every `update()` call if the book is valid.
In the full system, the callback is the wire to the market data publisher thread.

---

## Algorithm

### `update(ts)` — the main call

```cpp
TopOfBook L1Feed::update(Timestamp ts) {
    last_tob_ = book_.top_of_book(ts);  // read fresh from L3

    if (last_tob_.valid && last_tob_.spread > 0) {
        spread_history_.push_back(last_tob_.spread); // append to rolling window
        if (spread_history_.size() > MAX_SPREAD_HISTORY)
            spread_history_.pop_front();              // evict oldest
    }

    if (callback_) callback_(last_tob_); // fire to downstream consumers
    return last_tob_;
}
```

Called by the matching engine after every order event that changes the top of book.

### Microprice formula

Defined in `L3OrderBook::top_of_book()` and exposed through `TopOfBook::micro_price`:

```cpp
// bid_qty = quantity at best bid; ask_qty = quantity at best ask
Quantity total_qty = bid_qty + ask_qty;
micro_price = (bid_price * ask_qty + ask_price * bid_qty) / total_qty;
```

If the bid has 375 lots and the ask has 120 lots, the microprice tilts toward the bid:

```
micro_price = (100.05 × 120 + 100.06 × 375) / 495
            ≈ 100.058
```

The microprice is closer to the bid because there is more quantity there — supply
is larger, fair value leans slightly toward the bigger side.

> **HFT context:** Microprice was popularized by Stoikov and Avellaneda. Market makers
> use it as a real-time fair value estimate: if micro_price > mid_price, the bid side
> is heavier and a short-term price rise is more likely. A maker would skew their ask
> quote down slightly (quote more aggressively) while the microprice is elevated. This
> is called **inventory skew** or **quote skew**.

### `vwap()` — volume-weighted average price

```cpp
double L1Feed::vwap() const {
    double sum_pv = 0.0, sum_v = 0.0;
    for (auto& t : recent_trades_) {
        sum_pv += static_cast<double>(t.price) * static_cast<double>(t.quantity);
        sum_v  += static_cast<double>(t.quantity);
    }
    return sum_v > 0.0 ? sum_pv / sum_v : 0.0;
}
```

This is the standard VWAP formula. It is called on the rolling window of the last
1000 trades — not the full session, just the recent history.

> **HFT context:** VWAP is the most common execution benchmark in institutional
> trading. A buy-side desk measures its trader's performance by asking: "did you buy
> below the day's VWAP?" Execution algorithms (VWAP algos) break large orders into
> slices timed to match the historical volume profile of the day, targeting an
> execution price that beats the benchmark.

### `rolling_spread(window)` — backward iteration

```cpp
double L1Feed::rolling_spread(std::size_t window) const {
    std::size_t n = std::min(window, spread_history_.size());
    double sum = 0.0;
    auto it = spread_history_.end();
    for (std::size_t i = 0; i < n; ++i) {
        --it;
        sum += static_cast<double>(*it);
    }
    return sum / static_cast<double>(n);
}
```

Iterates backward from the most recent spread. Does not copy the deque.

> **HFT context:** Spread width is a proxy for liquidity and uncertainty. A market
> making strategy monitors rolling spread to detect regime changes: a sudden spread
> widening often precedes an informed order flow burst (news, large institutional
> order). Widening spreads are a signal to reduce position size or widen quotes.

---

## Design Decisions

### Why `std::deque` for rolling windows?

`std::deque` gives O(1) `push_back` and O(1) `pop_front`. Both operations happen
on every update. A `std::vector` would require O(N) shifting on `pop_front`. A
circular buffer (`std::array` + manual index) would work too, but `std::deque` is
simpler and the difference is negligible at these window sizes.

### Why MAX_TRADE_HISTORY = 1000 and MAX_SPREAD_HISTORY = 500?

Fixed caps prevent unbounded memory growth regardless of how long the simulation
runs. The values are empirically reasonable: 1000 recent trades give a meaningful
VWAP; 500 spread samples give a smooth rolling average. Both are
`static constexpr` — zero runtime overhead.

### Why does L1Feed not own the book?

`L1Feed` holds a `const L3OrderBook&`. It cannot modify the book. The book is the
single source of truth; L1 is a derived view. This enforces a clear ownership
boundary: the matching engine owns the book and calls `l1.update()` when needed.

### Why `[[nodiscard]]` on `update()` and `last()`?

```cpp
[[nodiscard]] TopOfBook update(Timestamp ts);
[[nodiscard]] const TopOfBook& last() const;
```

If a caller writes `l1.update(ts);` without capturing the return value, the compiler
emits a warning. This prevents a common mistake where the caller forgets to use the
updated TOB.

### Why is L1Feed callback-driven?

Rather than polling `last()` from multiple threads, `L1Feed` pushes to a single
registered callback. In the full system, that callback enqueues the `TopOfBook` into
the `HPRingBuffer` market data queue, which the market data publisher thread drains.
One push, one consumer, no contention.

---

## What to Read Next

- **`src/market/matching_engine.hpp`** — `L3OrderBook`, `L2Aggregator`, and `L1Feed`
  are all members here; the matching engine wires them together and drives the
  `update()` / `snapshot()` call sequence after every order event
- **`src/market/market_data_engine.hpp`** — the cross-thread `HPRingBuffer`-backed
  layer that receives L1 callbacks and fans out to all subscribers
```

---

**Step 2: Verify the file exists**

```bash
wc -l src/orderbook/L1_FEED.md
```
Expected: > 180 lines.

Confirm all sections render: vocabulary table, data flow ASCII diagram, code blocks
with `cpp` syntax highlighting, HFT context callouts as block quotes.

**Step 3: Commit**

```bash
git add src/orderbook/L1_FEED.md
git commit -m "docs: add L1_FEED.md — learner guide to L1 top-of-book feed and statistics"
```

---

### Task 4: Final review pass

**Step 1: Check cross-links**

Verify all relative links between docs work:
- `L3_ORDER_BOOK.md` links to `L2_AGGREGATOR.md` ✓
- `L2_AGGREGATOR.md` links back to `L3_ORDER_BOOK.md` and forward to `L1_FEED.md` ✓
- `L1_FEED.md` links back to `L2_AGGREGATOR.md` ✓

**Step 2: Verify file list**

```bash
ls src/orderbook/
```

Expected output includes:
```
L1_FEED.md
L2_AGGREGATOR.md
L3_ORDER_BOOK.md
l1_feed.cpp
l1_feed.hpp
l2_aggregator.cpp
l2_aggregator.hpp
l3_order_book.cpp
l3_order_book.hpp
```

**Step 3: Check all code excerpts are accurate**

Spot-check 3–4 code blocks in each doc against the actual source files. Confirm
line numbers and variable names match exactly.

**Step 4: Commit design doc**

```bash
git add docs/plans/2026-03-07-orderbook-docs-design.md docs/plans/2026-03-07-orderbook-docs.md
git commit -m "docs: add orderbook documentation design and implementation plan"
```
