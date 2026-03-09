# L2 Aggregator — Learner Guide

Read [`L3_ORDER_BOOK.md`](L3_ORDER_BOOK.md) first. The L2 aggregator is a read-only view over the L3 book; you need to understand what it is reading before studying how it collapses that data.

After this doc, continue with [`L1_FEED.md`](L1_FEED.md).

---

## What This Is

The `L2Aggregator` is a thin, read-only lens over an `L3OrderBook`. Its job is to collapse individual order-level detail into **price-level totals** — a representation known as Market-by-Price (MBP).

At L3 you can see every resting order: trader A has 200 shares at \$100.05, trader B has 150 shares at \$100.05, trader C has 400 shares at \$100.00. At L2 those are collapsed: $100.05 shows 350 shares across 2 orders; \$100.00 shows 400 shares across 1 order.

`L2Aggregator` has no state of its own. It holds a `const` reference to the `L3OrderBook` and recomputes everything on demand. There is no cache to invalidate, no shadow copy to keep in sync, no update notifications to handle. Every call to `snapshot()` walks the live L3 book and produces a fresh result. It is a view, not a cache.

---

## Vocabulary

| Term | Definition |
|---|---|
| **Market-by-Price (MBP)** | A book representation that aggregates all orders at the same price into a single level showing total quantity and order count. This is what most clients receive as "L2 data". |
| **Market-by-Order (MBO)** | A book representation that shows each individual resting order with its own quantity and queue position. This is the L3 view. |
| **Depth snapshot** | A point-in-time capture of multiple price levels on both sides of the book, packaged into a single struct for transmission or logging. |
| **Price level** | One row in the book: a single price with the aggregated quantity of all orders resting at that price. |
| **Book imbalance** | The ratio of bid quantity to ask quantity across visible depth levels. A common input to short-term directional signals. |
| **Order count** | The number of individual orders resting at a price level. Even in an MBP feed, order count lets subscribers detect hidden liquidity structure such as icebergs. |

---

## The Aggregation

L3 holds individual orders per price level. L2 collapses each level into a single `BookLevel`.

```
L3 (Market-by-Order)              L2 (Market-by-Price)

BID SIDE
  $100.05  [ord A: 200]  \
  $100.05  [ord B: 150]   >--->  BookLevel { price=10005, qty=350, order_count=2 }
  $100.04  [ord C: 400]   >--->  BookLevel { price=10004, qty=400, order_count=1 }
  $100.03  [ord D: 100]  \
  $100.03  [ord E: 250]   >--->  BookLevel { price=10003, qty=350, order_count=2 }

ASK SIDE
  $100.06  [ord F: 300]   >--->  BookLevel { price=10006, qty=300, order_count=1 }
  $100.07  [ord G: 180]  \
  $100.07  [ord H: 120]   >--->  BookLevel { price=10007, qty=300, order_count=2 }
  $100.08  [ord I: 500]   >--->  BookLevel { price=10008, qty=500, order_count=1 }
```

Bids are sorted highest price first (best bid at index 0). Asks are sorted lowest price first (best ask at index 0). The gap between index 0 on each side is the spread.

---

## Data Structures

### `BookLevel` — one price level in the aggregated book

Defined in `src/common/types.hpp`:

```cpp
struct BookLevel {
  Price price;                   // <- integer ticks, e.g. 10005 = $100.05 at 2dp
  Quantity qty;                  // <- total visible quantity across all orders at this price
  std::uint32_t order_count = 0; // <- number of individual orders resting here
};
```

`order_count` is present even in the MBP view. A subscriber who sees `qty=1000, order_count=1` versus `qty=1000, order_count=10` has very different information: the second case suggests thinner real depth — each order is only 100 shares and any one of them could cancel. Order count also signals potential iceberg activity: because `qty` reflects only visible quantity, an iceberg appears as a small, apparently-thin level with `order_count=1` that keeps refilling at the same price. A level that vanishes and reappears repeatedly at a constant price with consistent size is a classic L2 iceberg signature.

### `DepthSnapshot` — fixed-size snapshot of the full book

Defined in `src/common/types.hpp`:

```cpp
static constexpr std::size_t MAX_DEPTH_LEVELS = 20; // <- compile-time upper bound for array sizing

struct DepthSnapshot {
  Symbol symbol;
  std::array<BookLevel, MAX_DEPTH_LEVELS> bids; // <- fixed C array, not std::vector — no heap allocation
  std::array<BookLevel, MAX_DEPTH_LEVELS> asks; // <- same: capacity always MAX_DEPTH_LEVELS slots
  std::uint32_t bid_levels = 0;  // <- how many entries in bids[] are populated
  std::uint32_t ask_levels = 0;  // <- how many entries in asks[] are populated
  Timestamp timestamp = 0;
};
```

The arrays are always `MAX_DEPTH_LEVELS` slots in size regardless of how many levels are actually populated. `bid_levels` and `ask_levels` tell you how many of those slots contain valid data. Slots beyond those counts are zero-initialized.

Fixed-size C arrays are used here rather than `std::vector` because `DepthSnapshot` must be stored inside `EngineEvent`, which itself lives in `HPRingBuffer` slots. Ring buffer slots are fixed-size by design — every slot in the buffer is the same number of bytes. A `std::vector` embeds a pointer to heap memory; placing it in a ring buffer slot would mean the slot holds a pointer, not the data, breaking the zero-copy, no-allocation guarantee of the hot path.

### `L2Aggregator` — class declaration

Defined in `src/orderbook/l2_aggregator.hpp`:

```cpp
class L2Aggregator {
 public:
  explicit L2Aggregator(const L3OrderBook& book,               // <- takes a const ref: read-only, no ownership
                        std::size_t depth = DEFAULT_L2_DEPTH); // <- DEFAULT_L2_DEPTH = 10

  [[nodiscard]] DepthSnapshot snapshot(Timestamp ts) const;

  [[nodiscard]] std::vector<BookLevel> bid_levels() const;
  [[nodiscard]] std::vector<BookLevel> ask_levels() const;

  void set_depth(std::size_t depth) { depth_ = std::min(depth, MAX_L2_DEPTH); } // <- capped at MAX_L2_DEPTH = 20

  [[nodiscard]] std::size_t depth() const { return depth_; }

 private:
  const L3OrderBook& book_; // <- reference to the live L3 book; L2Aggregator owns nothing
  std::size_t depth_;       // <- configurable: how many levels to publish
};
```

`book_` is a `const` reference. The aggregator cannot modify the L3 book and does not manage its lifetime. The constructor also caps `depth_` at `MAX_L2_DEPTH` (20), which matches the `MAX_DEPTH_LEVELS` array size in `DepthSnapshot`. Passing a larger depth would produce more levels than the snapshot struct can hold, so the cap is a correctness guard, not just a performance limit.

Constants come from `src/common/constants.hpp`:

```cpp
static constexpr std::size_t DEFAULT_L2_DEPTH = 10; // <- default if none specified at construction
static constexpr std::size_t MAX_L2_DEPTH     = 20; // <- hard ceiling; enforced in constructor and set_depth()
```

---

## Algorithm: `snapshot()`

Full implementation from `src/orderbook/l2_aggregator.cpp`:

```cpp
DepthSnapshot L2Aggregator::snapshot(Timestamp ts) const {
  DepthSnapshot snap{};           // <- zero-initialises all fields including the fixed arrays
  snap.symbol = book_.symbol();   // <- copies the 8-byte Symbol from the L3 book
  snap.timestamp = ts;            // <- caller supplies the timestamp (clock abstraction stays outside)

  auto bids = book_.bid_depth(depth_); // <- asks the L3 book for up to depth_ aggregated bid levels
  auto asks = book_.ask_depth(depth_); // <- same for asks; both return std::vector<BookLevel>

  snap.bid_levels = static_cast<std::uint32_t>(bids.size()); // <- actual count returned (may be < depth_)
  snap.ask_levels = static_cast<std::uint32_t>(asks.size());

  for (std::size_t i = 0; i < bids.size() && i < MAX_DEPTH_LEVELS; ++i) {
    snap.bids[i] = bids[i]; // <- copy each BookLevel into the fixed array
  }
  for (std::size_t i = 0; i < asks.size() && i < MAX_DEPTH_LEVELS; ++i) {
    snap.asks[i] = asks[i];
  }

  return snap; // <- returns by value; ~960 bytes (15-16 cache lines) — fast because it stays on-stack until the ring buffer copy
}
```

The method delegates almost all work to `book_.bid_depth(depth_)` and `book_.ask_depth(depth_)` on the `L3OrderBook`. Those methods walk the L3 book's internal sorted maps (`std::map` with `std::greater` for bids, `std::less` for asks), iterate up to `levels` price levels, and for each level sum the `visible_qty` across all `L3Order` entries in the level's `std::list<L3Order>`. The result is a `std::vector<BookLevel>` already in best-to-worst order.

`snapshot()` then copies that vector into the fixed `std::array` fields of `DepthSnapshot`. The copy loop's second guard (`i < MAX_DEPTH_LEVELS`) is belt-and-suspenders: because `depth_` is already capped at `MAX_L2_DEPTH` which equals `MAX_DEPTH_LEVELS`, the vector returned by `bid_depth` cannot be longer than the array. But the explicit guard makes the code safe even if those constants diverge in a future refactor.

> **HFT context:** L1, L2, and L3 data are three distinct commercial tiers. L1 (best bid/offer only) is included in standard retail brokerage data feeds at no additional cost. L2 (depth-of-book, typically 5–20 levels) requires a paid exchange data subscription — on US equities exchanges this typically costs hundreds to thousands of dollars per month per venue. L3 (full order-by-order feed, i.e., MBO) is available only from some exchanges and at substantially higher cost; it is primarily consumed by prop firms and market makers who need to model queue position. The data tier your strategy requires is a fundamental infrastructure cost decision. A simple momentum strategy may run profitably on L1 alone; a market-making strategy typically needs L2 to quote competitively; a strategy that models iceberg orders or queue dynamics needs L3.

---

## Design Decisions

### Why `L2Aggregator` is stateless

The aggregator holds no cached copy of the book. Every call to `snapshot()`, `bid_levels()`, or `ask_levels()` recomputes from the live `L3OrderBook`.

The alternative — caching a `DepthSnapshot` and updating it on each book event — would require the aggregator to subscribe to callbacks from the L3 book, implement incremental update logic, and manage consistency between the cache and the source of truth. That is substantial complexity for a component whose entire public API is "give me the current depth". Statelessness eliminates all of that. The caller decides when a snapshot is needed and pays exactly the cost of one traversal of the top N price levels.

In this codebase the aggregator is called from the matching engine thread after each order event. The L3 book is always current at that point because it was just modified. There is no stale-data problem to solve.

### Why `DepthSnapshot` uses fixed-size arrays instead of `std::vector`

`DepthSnapshot` is placed directly inside `EngineEvent`:

```cpp
struct alignas(64) EngineEvent {
  EventType type = EventType::NewOrder;
  union {
    Order order;
    CancelRequest cancel;
    ReplaceRequest replace;
    ExecutionReport exec_report;
    Trade trade;
    TopOfBook tob;
    DepthSnapshot depth; // <- lives directly inside the union, no pointer indirection
  };
  Timestamp event_time = 0;
  // ...
};
```

`EngineEvent` slots in `HPRingBuffer` must be fixed-size so the ring buffer can be a flat array of structs. A `std::vector` inside `DepthSnapshot` would embed a pointer to heap memory inside the union. Each `DepthSnapshot` crossing from the matching engine thread to the market data thread would involve a heap allocation and a subsequent deallocation — exactly the pattern that the ring buffer design is built to avoid.

Fixed arrays also eliminate ownership ambiguity. The ring buffer slot owns the `DepthSnapshot` struct. There is no destructor work to track, no reference counting, no move semantics to reason about. `EngineEvent` copy and assign are implemented via `std::memcpy` for exactly this reason.

> **HFT context:** Production exchange protocols (NASDAQ ITCH, CBOE PITCH, CME MDP3) transmit fixed-length binary messages over UDP multicast. Every message type has a defined byte length known at compile time. This makes parsing trivial — you read N bytes into a struct — and makes sequencing reliable — you can detect gaps by counting bytes. The HFT-sdk `DepthSnapshot` design reflects this same principle: the in-process representation is already shaped like a wire-protocol message.

### Why `depth_` is configurable

Different consumers need different amounts of depth. A top-of-book arbitrage strategy may only require 1 level. A market maker quoting 5 levels wide needs 5. A risk system computing book imbalance may use 10. Publishing all 20 levels when only 2 are consumed wastes CPU time in `bid_depth` / `ask_depth` traversal and wastes bandwidth or memory when the `DepthSnapshot` is forwarded downstream.

`depth_` can be changed at runtime via `set_depth()` without reconstructing the aggregator, so operators can tune based on observed consumer behaviour. The cap at `MAX_L2_DEPTH` (20) prevents requesting more levels than the `DepthSnapshot` struct can hold.

---

## What to Read Next

- [`L1_FEED.md`](L1_FEED.md) — the top-of-book feed (`TopOfBook`, microprice, spread); L1 is derived from the same L3 book but focuses on a single price level with derived analytics rather than a depth array.
- `src/market/matching_engine.hpp` — the component that owns both the `L3OrderBook` and the `L2Aggregator`, drives the matching loop, and decides when to call `snapshot()` and publish the result into the `HPRingBuffer`.
