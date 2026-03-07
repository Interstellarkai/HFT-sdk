# L3 Order Book — Learner Guide

Read this before opening `l3_order_book.hpp`. This document explains what the L3
book is, why it is structured the way it is, and how every line of matching logic
connects to real HFT practice. After reading this, see [L2_AGGREGATOR.md](L2_AGGREGATOR.md)
for how the L3 book's raw order data gets aggregated into the depth snapshots that
trading systems consume downstream.

---

## 1. What This Is

An **L3 order book** (Level 3) is the most granular view of a market. It records
every individual resting order — not just the total quantity at each price, but
which specific order sits at which position in the queue at that price.

Compare the three levels:

| Level | What you see |
|-------|--------------|
| L1    | Best bid price + size, best ask price + size only |
| L2    | All price levels with total quantity at each level |
| L3    | All price levels, each with the individual orders in queue order |

The L3 book is the **source of truth**. L2 and L1 are derived from it by
aggregating quantities. If you want to know how much size is ahead of your order
before it gets filled, you need L3. The `L3OrderBook` class in this SDK is what
the matching engine uses internally; the aggregated views are produced by the
`L2Aggregator` on top of it.

---

## 2. Vocabulary

| Term | Plain-English meaning |
|------|-----------------------|
| **price-time priority** | Orders at the same price are filled oldest-first (FIFO). If two bids are both at $100.00, the one that arrived first fills first. |
| **resting order** | A limit order that has been placed on the book and is waiting for a counterparty to match it. |
| **aggressor** | The incoming order that initiates a trade by crossing the spread. The resting order was already sitting there; the aggressor arrives and hits it. |
| **iceberg order** | An order with a visible (displayed) portion and a hidden reserve. As the visible portion fills, a slice of the hidden portion is automatically replenished into the visible quantity. Institutions use these to hide their true size. |
| **queue position** | How many orders are ahead of yours at the same price level. Position 0 means you are at the front of the queue and fill first. |
| **FOK (Fill or Kill)** | Time-in-force instruction: fill the entire order immediately or reject it outright. No partial fills; no resting. |
| **IOC (Immediate or Cancel)** | Fill whatever is available immediately; cancel any unfilled remainder. Partial fills are accepted. |
| **GTC (Good Till Canceled)** | The order rests on the book until it fills, is explicitly canceled, or the session ends. |
| **Day** | Like GTC but the order is automatically canceled at end of the trading day. |
| **cancel-replace** | Atomically cancel an existing order and submit a new one. Because the cancel and resubmit happen as a unit, the operation is a single API call (`replace_order`), but internally it is cancel + new `add_order`. Importantly, a cancel-replace loses queue priority — the new order goes to the back of the queue at the new price. |
| **execution report** | The acknowledgement message sent back to a trader after any order event (new, partial fill, full fill, cancel, reject). Modeled as `ExecutionReport` in this SDK. |
| **spread** | The gap between the best bid and best ask, measured in ticks. A tighter spread means lower transaction cost. In the `TopOfBook` struct, `spread = best_ask.price - best_bid.price`. |

---

## 3. In-Memory Layout

This ASCII diagram shows the in-memory layout for a single symbol. Real field
values are representative examples.

```
bids_  (std::map, descending — highest bid first)
─────────────────────────────────────────────────────────────────────
Price 10050  →  [OrderQueue: std::list<L3Order>]
                  ┌──────────────────────────────────────────────────┐
                  │ L3Order { id=101, visible_qty=200, queue_pos=0 } │  ← front of queue (fills first)
                  │ L3Order { id=104, visible_qty=150, queue_pos=1 } │
                  └──────────────────────────────────────────────────┘
Price 10040  →  [OrderQueue]
                  ┌──────────────────────────────────────────────────┐
                  │ L3Order { id=99,  visible_qty=500, queue_pos=0 } │
                  └──────────────────────────────────────────────────┘

asks_  (std::map, ascending — lowest ask first)
─────────────────────────────────────────────────────────────────────
Price 10060  →  [OrderQueue]
                  ┌──────────────────────────────────────────────────┐
                  │ L3Order { id=200, visible_qty=300, queue_pos=0 } │  ← best ask, fills first
                  │ L3Order { id=207, visible_qty=100,              │
                  │           hidden_qty=900, is_iceberg=true,      │
                  │           queue_pos=1                  }         │
                  └──────────────────────────────────────────────────┘
Price 10070  →  [OrderQueue]
                  ┌──────────────────────────────────────────────────┐
                  │ L3Order { id=211, visible_qty=250, queue_pos=0 } │
                  └──────────────────────────────────────────────────┘

order_index_  (std::unordered_map<OrderId, OrderLocation>)
─────────────────────────────────────────────────────────────────────
  101  →  { side=Buy,  price=10050, it=<iterator to L3Order 101 in bids_[10050]> }
  104  →  { side=Buy,  price=10050, it=<iterator to L3Order 104 in bids_[10050]> }
   99  →  { side=Buy,  price=10040, it=<iterator to L3Order 99  in bids_[10040]> }
  200  →  { side=Sell, price=10060, it=<iterator to L3Order 200 in asks_[10060]> }
  207  →  { side=Sell, price=10060, it=<iterator to L3Order 207 in asks_[10060]> }
  211  →  { side=Sell, price=10070, it=<iterator to L3Order 211 in asks_[10070]> }
```

When a cancel arrives for order 104, the engine looks up `order_index_[104]`,
gets the `std::list::iterator` directly, calls `list::erase(it)`, then erases
the entry from `order_index_`. No scanning of any queue needed.

---

## 4. Data Structures

### 4.1 `L3Order` — the per-order record

```cpp
struct alignas(64) L3Order {                    // <- forced onto its own cache line
  HFT_sdk::OrderId  id          = HFT_sdk::INVALID_ORDER_ID;
  HFT_sdk::TraderId trader_id   = 0;
  HFT_sdk::Side     side        = HFT_sdk::Side::Buy;
  HFT_sdk::Price    price       = 0;
  HFT_sdk::Quantity visible_qty = 0;   // <- displayed quantity; what the market sees
  HFT_sdk::Quantity hidden_qty  = 0;   // <- iceberg reserve; invisible to other participants
  HFT_sdk::Quantity filled_qty  = 0;   // <- cumulative fills so far
  HFT_sdk::Timestamp entry_time = 0;   // <- used to enforce price-time priority
  std::uint32_t queue_pos       = 0;   // <- 0 = front of queue at this price level
  bool is_iceberg               = false;
  char cl_ord_id[20]            = {};  // <- FIX client order ID, no heap allocation
};
```

Field-by-field:

- `visible_qty` — the quantity other market participants see in the book. This is
  what `bid_depth()` / `ask_depth()` report. When an aggressor hits this order,
  visible quantity is reduced first.
- `hidden_qty` — the iceberg reserve. Invisible to other participants. After
  `visible_qty` drains to zero, the book replenishes a new slice from `hidden_qty`
  into `visible_qty` (in chunks of 100 in the current implementation).
- `queue_pos` — zero-indexed position in the `std::list` at this price level.
  Updated by `update_queue_positions()` after any cancel removes an order.
- `alignas(64)` — aligns the struct to a 64-byte boundary so a single `L3Order`
  occupies exactly one CPU cache line. This prevents false sharing when multiple
  orders at adjacent addresses are accessed by the matching thread.

### 4.2 `BidPriceLevels` / `AskPriceLevels` — the two sides of the book

```cpp
using OrderQueue    = std::list<L3Order>;

using BidPriceLevels =
    std::map<HFT_sdk::Price, OrderQueue, std::greater<HFT_sdk::Price>>;
// <- descending comparator: highest bid price at begin()

using AskPriceLevels =
    std::map<HFT_sdk::Price, OrderQueue, std::less<HFT_sdk::Price>>;
// <- ascending comparator: lowest ask price at begin()
```

Both maps use `begin()` as the "best price" for their side. That lets the
matching loop always start at `levels.begin()` without any special-casing.

### 4.3 `order_index_` — the iterator trick that makes cancel O(1)

```cpp
struct OrderLocation {
  HFT_sdk::Side          side;   // <- which map to look in (bids_ or asks_)
  HFT_sdk::Price         price;  // <- which price level in that map
  OrderQueue::iterator   it;     // <- direct pointer into the std::list node
};

std::unordered_map<HFT_sdk::OrderId, OrderLocation> order_index_;
```

`std::list` guarantees that iterators remain valid after insertions and after
erasing *other* nodes. An iterator stored in `OrderLocation::it` stays valid for
the lifetime of its node. This means:

1. `order_index_[id]` gives you `OrderLocation` in O(1) average time.
2. `list::erase(loc.it)` removes the node in O(1).
3. No scanning of the price-level queue is ever needed for a cancel.

### 4.4 `MatchResult` — what matching returns

```cpp
struct MatchResult {
  std::vector<HFT_sdk::Trade> trades;       // <- one Trade per resting order matched
  HFT_sdk::Quantity           remaining_qty = 0;
  bool                        fully_filled  = false;
};
```

`trades` is a list because a single incoming order can match against multiple
resting orders at potentially multiple price levels (a sweep). Each element
corresponds to one resting order that was fully or partially consumed.

---

## 5. Algorithm: What Happens When an Order Arrives

The public entry point is `add_order(const Order& order, Timestamp ts)`. Here is
the complete journey of a buy limit order.

### Step 1 — Duplicate check

```cpp
if (order_index_.count(order.id)) {
    rpt.status = OrderStatus::Rejected;
    rpt.reject_reason = RejectReason::DuplicateOrderId;
    rpt.leaves_qty = 0;
    return rpt;
}
```

If the order ID is already in the index, the book rejects it immediately. Order
IDs must be unique across the lifetime of the session.

### Step 2 — FOK pre-scan

For Fill-or-Kill orders, the engine first checks whether enough resting quantity
exists before touching any order state:

```cpp
if (order.tif == TimeInForce::FOK) {
    Quantity available = 0;
    if (order.side == Side::Buy) {
        for (auto& [price, queue] : asks_) {
            if (order.type == OrderType::Limit && price > order.price) break;
            // <- stop scanning when ask prices exceed the limit price
            for (auto& o : queue) available += o.visible_qty + o.hidden_qty;
            if (available >= order.quantity) break;
        }
    }
    // ... mirror logic for sells
    if (available < order.quantity) {
        result.fully_filled = false;
        return result;  // <- FOK rejected, no trades written, book untouched
    }
}
```

The scan counts both `visible_qty` and `hidden_qty` because icebergs are
matchable even if their hidden portion is not displayed.

> **HFT context:** FOK is used when you need to hedge all legs of a position
> simultaneously. If you are buying 10,000 shares of AAPL to delta-hedge an
> options position, a partial fill of 3,000 shares leaves you exposed to the
> market moving before the rest fills. FOK ensures you either hedge fully or not
> at all. The pre-scan avoids writing any trade records that would then need to
> be rolled back — the book is never modified if FOK cannot be satisfied.

### Step 3 — `match_against()` inner loop

After the FOK pre-scan (or immediately for GTC/IOC), matching dispatches to the
templated inner loop:

```cpp
template <typename PriceLevels>
void L3OrderBook::match_against(PriceLevels& levels, const Order& incoming,
                                Quantity& remaining, std::vector<Trade>& trades,
                                Timestamp ts) {
    auto level_it = levels.begin(); // <- start at best price
    while (remaining > 0 && level_it != levels.end()) {
        Price level_price = level_it->first;

        // Price check for limit orders
        if (incoming.type == OrderType::Limit) {
            if (incoming.side == Side::Buy  && level_price > incoming.price) break;
            if (incoming.side == Side::Sell && level_price < incoming.price) break;
            // <- stop when the resting price crosses our limit
        }

        auto& queue = level_it->second;
        auto order_it = queue.begin(); // <- oldest order at this price (queue position 0)
        while (remaining > 0 && order_it != queue.end()) {
            auto& resting = *order_it;
            Quantity resting_available = resting.visible_qty + resting.hidden_qty;
            Quantity match_qty = std::min(remaining, resting_available);
            // ... build Trade, fire trade_cb_, reduce quantities
        }
    }
}
```

Orders within a price level are consumed front-to-back (price-time priority). The
inner `queue.begin()` always points to the order with `queue_pos == 0`.

> **HFT context:** Queue position is literally money for a market maker. A maker
> sitting at position 0 on the best bid will fill before everyone else when a
> sell order arrives. Being first in queue means you capture the spread on every
> fill and have lower adverse selection risk — the order that hits you is less
> likely to be an informed trader who knows the price is about to move against
> you. Latency arms races at co-location facilities are largely about getting
> order acknowledgements back fast enough to be early in the queue. A single
> microsecond of extra latency can push your order one position back, reducing
> fill probability meaningfully.

### Step 4 — Iceberg replenishment

Inside the inner loop, after reducing the resting order's quantity:

```cpp
// Iceberg replenishment
if (resting.visible_qty == 0 && resting.hidden_qty > 0 &&
    resting.is_iceberg) {
    Quantity replenish =
        std::min(resting.hidden_qty, static_cast<Quantity>(100));
    // <- replenish up to 100 units from the hidden reserve
    resting.visible_qty = replenish;
    resting.hidden_qty -= replenish;
}
```

The replenished order stays in its current list position and keeps its `queue_pos`.
It does not move to the back of the queue. Only when the full order (visible +
hidden) reaches zero does it get erased.

> **HFT context:** Large institutions — pension funds, index funds — use iceberg
> orders to hide true size when accumulating or distributing positions. If a fund
> needs to sell 500,000 shares of MSFT, showing all 500,000 at once signals the
> trade to the market and causes other participants to front-run by selling first,
> driving the price down before the fund finishes. An iceberg showing only 1,000
> shares at a time conceals the total. However, sophisticated HFT systems detect
> iceberg patterns: if the same price level keeps replenishing at the same fixed
> size (100 units here), it is a statistical signal. This observation is called
> "iceberg detection" and is used as an alpha signal — a short-term predictor of
> price direction.

### Step 5 — `add_resting_order()` — storing the iterator

For Limit GTC or Day orders with remaining quantity after matching:

```cpp
void L3OrderBook::add_resting_order(const Order& order, Quantity remaining,
                                    Timestamp ts) {
    // ... build L3Order l3 from the incoming order fields ...

    if (order.side == Side::Buy) {
        auto& queue = bids_[order.price];  // <- creates price level if it doesn't exist
        queue.push_back(l3);
        auto it = std::prev(queue.end());  // <- iterator to the newly added node
        it->queue_pos = static_cast<std::uint32_t>(queue.size() - 1);
        order_index_[order.id] = {Side::Buy, order.price, it};
        // <- store the iterator so cancel_order can find and erase in O(1)
        ++bid_order_count_;
    }
    // ... mirror for asks ...
}
```

The key line is `order_index_[order.id] = {Side::Buy, order.price, it}`. From
this point on, the iterator `it` is the order's permanent address in the book.

> **Note:** The current `add_resting_order` implementation hardcodes `is_iceberg = false` and `hidden_qty = 0`. Iceberg support in the matching engine is present — the replenishment logic works correctly — but the public `add_order` path does not yet expose a way to submit an iceberg order. To test iceberg matching, construct an `L3Order` directly and insert it into the book's internal data structures.

### Step 6 — Time-in-force dispatch table

| TIF | Remaining after matching | Action |
|-----|--------------------------|--------|
| GTC | > 0 | `add_resting_order()` — order lives until explicitly canceled |
| Day | > 0 | `add_resting_order()` — order lives until end of session |
| IOC | > 0 | Cancel remainder; `leaves_qty = 0`; status = Canceled (or PartiallyFilled if some filled) |
| FOK | > 0 | Reject; `filled_qty = 0`; `leaves_qty = 0`; status = Rejected (pre-scan should have caught this) |
| FOK | = 0 | Fully filled normally |

### Step 7 — `publish_tob()` — fire callbacks after state settles

```cpp
void L3OrderBook::publish_tob(Timestamp ts) {
    if (tob_cb_) {
        tob_cb_(top_of_book(ts));
    }
}
```

`publish_tob` fires when the book state actually changed — either trades occurred
or a new order is resting. Rejected orders and fully-cancelled IOC/FOK orders do
not trigger a TOB update. The `top_of_book()` method also computes `micro_price`
(the size-weighted mid-price) from the current best bid and ask quantities, which
is a cleaner short-term price predictor than the arithmetic mid.

---

## 6. Design Decisions

### Why `std::list` + stored iterator = O(1) cancel

The critical property of `std::list` is node stability: inserting or erasing any
node does not invalidate iterators to other nodes. This means an iterator stored
at order submission time remains valid until that specific order is erased — even
if thousands of other orders are added or canceled in between.

The alternative — `std::vector` or `std::deque` — would invalidate all iterators
on any insertion, making it impossible to cache a direct pointer to an order's
position. You would have to search the vector on every cancel, which is O(n) in
queue depth.

> **HFT context:** Cancel latency is not an afterthought. A market maker who
> posts quotes on both sides of the book must cancel stale quotes within
> microseconds when the underlying price moves. If cancel processing is slow —
> even by a few microseconds — the maker gets "picked off" by an informed trader
> who sees the old stale quote and fills it before the maker can withdraw. This
> is called adverse selection. Co-location facilities exist partly to reduce the
> round-trip time of a cancel-on-signal cycle to sub-100-microsecond latency.
> O(1) cancel in the matching engine is the engine's contribution to keeping
> that cycle fast.

### Why `std::map` over `std::unordered_map` for price levels

`std::map` keeps price levels sorted at all times. The matching loop needs to
iterate from best price outward, level by level. With `std::map`, `begin()` is
always the best price and `++level_it` moves to the next-best price. With
`std::unordered_map`, you would have to sort the keys before each match — O(k
log k) in the number of touched price levels, and with heap allocation each time.

The tree traversal cost of `std::map` (O(log k) for lookup/insert/erase by
price) is acceptable because price-level operations are far less frequent than
order-level operations. New price levels are created only when an order arrives
at a price not already in the book; they are erased only when the last order at
that price fills or cancels.

### Why `alignas(64)` on `L3Order`

In the matching engine's single-threaded hot path, loading a resting order from
the `std::list` requires a pointer dereference into arbitrary memory. With
`alignas(64)`, each `L3Order` is guaranteed to start on a 64-byte boundary —
meaning one cache line fetch loads the entire order. Without alignment, an order
that spans two cache lines would cost two fetches. This matters when scanning
through a price level's queue during matching.

Inside a `std::list`, each node is a heap allocation. The `alignas(64)`
annotation causes `operator new` (via the allocator) to return a 64-byte-aligned
address for each node, ensuring the single-fetch guarantee holds for every order
in the book.

### Why template `match_against<PriceLevels>` + explicit instantiation in `.cpp`

```cpp
// In l3_order_book.hpp:
template <typename PriceLevels>
void match_against(PriceLevels& levels, ...);

// In l3_order_book.cpp (explicit instantiations):
template void L3OrderBook::match_against<L3OrderBook::BidPriceLevels>(
    BidPriceLevels&, const Order&, Quantity&, std::vector<Trade>&, Timestamp);
template void L3OrderBook::match_against<L3OrderBook::AskPriceLevels>(
    AskPriceLevels&, const Order&, Quantity&, std::vector<Trade>&, Timestamp);
```

The matching logic for bids and asks is identical except for the map type
(`BidPriceLevels` vs `AskPriceLevels`). Using a template avoids duplicating the
inner loop, which is the most latency-sensitive code in the entire book. The
compiler generates two specialized versions — one for bids, one for asks — each
fully inlined with the correct comparator baked in, producing the same machine
code as if you had written two separate functions by hand.

Explicit instantiation in the `.cpp` file keeps the template body out of the
header, reducing compile times for translation units that include
`l3_order_book.hpp` without needing to see the implementation.

---

## 7. What to Read Next

- [L2_AGGREGATOR.md](L2_AGGREGATOR.md) — how `L3OrderBook`'s raw per-order data
  is collapsed into price-level depth snapshots (`BookLevel` vectors) that
  downstream consumers receive.
- `src/market/matching_engine.hpp` — the `MatchingEngine` class that owns one
  `L3OrderBook` per symbol, integrates the `RiskEngine` for pre-trade checks,
  drives the simulation clock, and publishes `EngineEvent` structs onto the
  `HPRingBuffer` queues that connect all six threads in the system.
