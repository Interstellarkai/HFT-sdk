# L1 Feed — Top-of-Book Statistics

Read [`L2_AGGREGATOR.md`](L2_AGGREGATOR.md) first. After this doc, open
`src/market/matching_engine.hpp` to see where `L1Feed` is instantiated and how
`update()` is called after order events.

---

## 1. What This Is

`L1Feed` derives scalar signals from the very top of the order book — best bid,
best ask, spread, mid-price, microprice, and VWAP. It also maintains two bounded
rolling windows: one for recent spreads and one for recent trades.

These signals are what most strategy consumers actually use. A strategy does not
need to know about every individual resting order. It needs answers to questions
like: "What is the approximate fair value of this instrument right now?" and
"Is the market wide or tight?" `L1Feed` produces those answers as simple numbers
that can be read and acted on in nanoseconds.

`L1Feed` does not own the book. It holds a `const` reference to the
`L3OrderBook` and queries it on demand via `update()`. All derived signals come
from a single call to `book_.top_of_book(ts)`.

---

## 2. Vocabulary

| Term | Definition |
|---|---|
| **Top of Book (TOB)** | The best (highest) bid price and best (lowest) ask price currently resting in the book, plus associated quantities. |
| **Spread** | `best_ask - best_bid`, in ticks. Wider spread = less liquidity. |
| **Mid-price** | `(best_bid + best_ask) / 2`. A naive estimate of fair value. |
| **Microprice** | A quantity-weighted mid that skews toward the side with more resting volume. Treats the imbalance between bid size and ask size as a signal about near-term price direction. |
| **VWAP** | Volume-Weighted Average Price. `sum(price * qty) / sum(qty)` over a rolling window of recent trades. The canonical execution benchmark. |
| **Rolling window** | A bounded history kept as a `std::deque`. New entries are appended at the back; the oldest are evicted from the front once the cap is exceeded. |
| **Fair value** | A strategy's best estimate of the true price of an instrument, used to decide whether a posted quote is cheap or expensive. |
| **Execution benchmark** | A reference price against which the quality of a trade is measured. Beating VWAP (buying below it, selling above it) is a standard measure of algo performance. |

---

## 3. Data Flow

```
L3OrderBook
    |
    | book_.top_of_book(ts)
    v
TopOfBook { best_bid, best_ask, spread, mid_price, micro_price, valid, timestamp }
    |
    +---> spread_history_ (deque, cap 500)
    |         |
    |         | rolling_spread(window)
    |         v
    |     average of last N spreads
    |
    +---> L1Callback (fires once per update)
    |         |
    |         v
    |     downstream subscriber (matching engine, market data publisher, strategy)
    |
record_trade(price, qty)
    |
    +---> recent_trades_ (deque, cap 1000)
              |
              | vwap()
              v
          sum(price * qty) / sum(qty)
```

---

## 4. Data Structures

### 4a. `TopOfBook` struct

Defined in `src/common/types.hpp`:

```cpp
struct TopOfBook {
  Symbol symbol;           // <- which instrument this snapshot belongs to
  BookLevel best_bid;      // <- { price, qty, order_count } at top of bid side
  BookLevel best_ask;      // <- { price, qty, order_count } at top of ask side
  Price mid_price = 0;     // <- (best_bid.price + best_ask.price) / 2
  Price micro_price = 0;   // <- quantity-weighted mid (see Section 5b)
  Price spread = 0;        // <- best_ask.price - best_bid.price, in ticks
  Timestamp timestamp = 0; // <- nanoseconds since epoch, from the calling update()
  bool valid{false};       // <- false only when BOTH sides of the book are empty (no orders at all)
};
```

The `valid` flag is set to `true` whenever ANY orders exist in the book — including
one-sided states (bid-only or ask-only). It is `false` only when the book has no
orders on either side. Consequently, a one-sided book will have `valid = true` but
`spread = 0`. The real guard against one-sided state is in `L1Feed::update()`:
the condition `last_tob_.valid && last_tob_.spread > 0` rejects both the empty-book
case (via `valid`) and the one-sided case (via `spread > 0`), preventing garbage
data from polluting rolling averages.

### 4b. `recent_trades_` deque

```cpp
struct TradeRecord {
  HFT_sdk::Price price;
  HFT_sdk::Quantity quantity;
};

std::deque<TradeRecord> recent_trades_;                    // <- rolling trade window
static constexpr std::size_t MAX_TRADE_HISTORY = 1000;     // <- hard cap on entries
```

`std::deque` is chosen for its O(1) `push_back` and O(1) `pop_front`. A
`std::vector` would require O(n) shifting on `pop_front`. A circular buffer
would work but adds complexity. At a cap of 1000 trades, memory use is small and
bounded — no unbounded growth possible.

### 4c. `spread_history_` deque

```cpp
std::deque<HFT_sdk::Price> spread_history_;               // <- rolling spread window
static constexpr std::size_t MAX_SPREAD_HISTORY = 500;    // <- hard cap on entries
```

Same rationale as `recent_trades_`. Five hundred samples of `Price` (8-byte
`int64_t`) is 4 KB — trivially small, deterministically bounded.

### 4d. `L1Callback` and `on_update()`

```cpp
using L1Callback = std::function<void(const HFT_sdk::TopOfBook&)>;  // <- one registered consumer

void on_update(L1Callback cb) { callback_ = std::move(cb); }        // <- move to avoid copy of std::function
```

One callback is registered at startup and fires on every `update()` call. This
is a push model: the `L1Feed` delivers fresh data to its consumer without the
consumer needing to poll. The design assumes a single downstream consumer per
`L1Feed` instance.

### 4e. `L1Feed` class declaration

```cpp
class L1Feed {
 public:
  using L1Callback = std::function<void(const HFT_sdk::TopOfBook&)>;

  explicit L1Feed(const L3OrderBook& book);             // <- takes book by const ref, does not own it

  [[nodiscard]] HFT_sdk::TopOfBook update(HFT_sdk::Timestamp ts);  // <- caller must not discard the return
  [[nodiscard]] const HFT_sdk::TopOfBook& last() const { return last_tob_; }

  void on_update(L1Callback cb) { callback_ = std::move(cb); }

  [[nodiscard]] double vwap() const;
  [[nodiscard]] double rolling_spread(std::size_t window) const;

  void record_trade(HFT_sdk::Price price, HFT_sdk::Quantity qty);

 private:
  const L3OrderBook& book_;           // <- non-owning reference, L3OrderBook outlives L1Feed
  HFT_sdk::TopOfBook last_tob_;       // <- cached result of the last update() call
  L1Callback callback_;

  std::deque<TradeRecord> recent_trades_;
  static constexpr std::size_t MAX_TRADE_HISTORY = 1000;

  std::deque<HFT_sdk::Price> spread_history_;
  static constexpr std::size_t MAX_SPREAD_HISTORY = 500;
};
```

---

## 5. Algorithms

### 5a. `update(ts)`

Full implementation from `l1_feed.cpp`:

```cpp
TopOfBook L1Feed::update(Timestamp ts) {
  last_tob_ = book_.top_of_book(ts);    // <- single query to L3 book; result is a value copy

  if (last_tob_.valid && last_tob_.spread > 0) {    // <- guard: only record meaningful spreads
    spread_history_.push_back(last_tob_.spread);    // <- append to back of rolling window
    if (spread_history_.size() > MAX_SPREAD_HISTORY) {
      spread_history_.pop_front();                  // <- evict oldest when cap is reached
    }
  }

  if (callback_) {
    callback_(last_tob_);  // <- push to registered subscriber; no copy, passed by const ref
  }

  return last_tob_;  // <- update() reads from the L3 book and refreshes the cache; last() returns the cache without querying the book. Calling last() immediately after update() returns the same snapshot.
}
```

Sequence: read fresh TOB from L3, conditionally extend spread history, evict if
over cap, fire callback, return the snapshot. The entire function runs in the
matching engine hot path, so there is no allocation and no branching beyond the
two guards.

### 5b. Microprice formula

The microprice is computed inside `L3OrderBook::top_of_book()` and stored in
`TopOfBook::micro_price`. The formula from `l3_order_book.cpp`:

```cpp
// Microprice: size-weighted mid
Quantity total_qty = bb->qty + ba->qty;               // <- sum of best-bid qty and best-ask qty
if (total_qty > 0) {
  tob.micro_price = (bb->price * ba->qty + ba->price * bb->qty) / total_qty;  // <- quantity-weighted
} else {
  tob.micro_price = tob.mid_price;                    // <- fallback if visible qty on both sides is zero (e.g. all-iceberg state)
}
```

In plain terms:

```
micro_price = (bid_price * ask_qty + ask_price * bid_qty) / (bid_qty + ask_qty)
```

Note that the weights are crossed: the bid price is weighted by ask quantity,
and the ask price is weighted by bid quantity. This means a large bid quantity
pulls the microprice toward the ask price, and a large ask quantity pulls it
toward the bid price — reflecting the intuition that a heavy bid side signals
buying pressure, which should shift the fair value estimate upward toward the
ask.

**Worked example:**

```
bid: price = 10005 (= 100.05 with 2 dp), qty = 375
ask: price = 10006 (= 100.06 with 2 dp), qty = 120

micro_price = (10005 * 120 + 10006 * 375) / (375 + 120)
            = (1,200,600 + 3,752,250) / 495
            = 4,952,850 / 495
            = 10005.757... ≈ 10006 (integer ticks)
```

The mid-price would be 10005 (halfway between). The microprice is pulled toward
10006 because the bid side (375) outweighs the ask side (120) — bid-heavy book,
price likely rising.

> **HFT context:** Microprice is the standard fair value estimate for market
> makers running quote-skew (inventory skew) strategies. If microprice > mid,
> the bid side is heavier and the instrument is more likely to tick up. A market
> maker who is accumulating long inventory shades BOTH quotes downward: the ask
> is shaded down (making it easier to offload the long) and the bid is shaded
> down (to slow further accumulation). Conversely, if microprice < mid (ask-heavy,
> price expected to fall), the maker shades ask up (to slow selling that would
> add to a short) and bid up (to encourage buying that covers the short).
> Quoting symmetrically around mid when microprice disagrees leaves money on the
> table — the maker is posting at prices the market has already signalled are
> mis-valued.

### 5c. `vwap()`

Full implementation from `l1_feed.cpp`:

```cpp
double L1Feed::vwap() const {
  if (recent_trades_.empty()) return 0.0;     // <- safe sentinel rather than NaN

  double sum_pv = 0.0;   // <- accumulates price * quantity
  double sum_v  = 0.0;   // <- accumulates quantity
  for (auto& t : recent_trades_) {
    sum_pv += static_cast<double>(t.price)    * static_cast<double>(t.quantity);  // <- cast: int64 * int64 can overflow
    sum_v  += static_cast<double>(t.quantity);
  }
  return sum_v > 0.0 ? sum_pv / sum_v : 0.0; // <- final guard against zero-volume edge case
}
```

VWAP is `sum(price * qty) / sum(qty)` over all trades in `recent_trades_`. Each
entry was appended by `record_trade()` and the window is capped at
`MAX_TRADE_HISTORY = 1000`. The cast to `double` before multiplication prevents
overflow: two `int64_t` values multiplied can exceed `int64_t` range.

A return value of `0.0` means no trades have been recorded yet — callers should
guard against this before using the value as a price. Using `0.0` as a price
input to spread or fair-value calculations will produce nonsensical results.

> **HFT context:** VWAP is the canonical execution benchmark for institutional
> order flow. A buy-side desk that bought 500,000 shares over a morning session
> asks: "Did I beat VWAP?" — meaning, did my average fill price come in below
> the market's own volume-weighted average? If yes, the execution was good. If
> no, the algo was a passive price-taker. Execution algorithms on sell-side desks
> (TWAP, VWAP, Implementation Shortfall) are all evaluated on this axis. In HFT
> market making, VWAP over a short rolling window tells the maker where
> aggressive flow has been trading, which is a proxy for where informed traders
> believe the price should be.

### 5d. `rolling_spread(window)`

Full implementation from `l1_feed.cpp`:

```cpp
double L1Feed::rolling_spread(std::size_t window) const {
  if (spread_history_.empty()) return 0.0;
  std::size_t n = std::min(window, spread_history_.size()); // <- cap at actual history size, not the requested window
  double sum = 0.0;
  auto it = spread_history_.end();               // <- start at one-past-last
  for (std::size_t i = 0; i < n; ++i) {
    --it;                                        // <- step backward: iterates most-recent first
    sum += static_cast<double>(*it);
  }
  return sum / static_cast<double>(n);           // <- average over n most-recent samples
}
```

The function iterates backward from the end of the deque, accumulating the last
`n` spread samples where `n = min(window, history.size())`. This means that if
you request `rolling_spread(100)` but only 40 samples have been recorded so far,
you get the average of those 40 — no undefined behavior, no padding with zeros.

> **HFT context:** Spread width is the primary liquidity proxy in live trading.
> A tight spread means the market is liquid and informed — there is enough
> two-sided interest that market makers are willing to risk a small edge.
> A widening spread is a regime-change signal: it typically precedes a news
> announcement, indicates informed order flow (someone knows something), or
> reflects a one-sided book after a large directional sweep. Market makers use
> rolling spread to set their own quote width (post wider when recent spreads
> widen, to defend against adverse selection) and to scale position size (reduce
> exposure when the market is becoming disorderly).

---

## 6. Design Decisions

**Why `std::deque` for both rolling windows**

`std::deque` provides O(1) `push_back` and O(1) `pop_front` without moving any
existing elements. A `std::vector` would shift all elements O(n) on `pop_front`.
A hand-rolled ring buffer would achieve O(1) at both ends but adds index-wrapping
complexity. The `deque` solution is simple, correct, and fast enough for windows
of 500 and 1000 elements accessed from a single thread.

**Why `MAX_TRADE_HISTORY = 1000` and `MAX_SPREAD_HISTORY = 500`**

Both caps bound memory at compile time. There is no way for `recent_trades_` to
grow past 1000 entries (`8 + 8 = 16` bytes each = 16 KB) or `spread_history_`
past 500 entries (`8` bytes each = 4 KB). In a system where the hot path avoids
all dynamic allocation, having deterministic, small memory footprints for these
windows is intentional. The cap values are large enough to give meaningful
rolling averages and small enough to keep cache pressure negligible.

**Why `L1Feed` does not own the book (`const L3OrderBook&`)**

The `L3OrderBook` is the single source of truth for all book state. `L1Feed` is
a derived-data consumer. Ownership of the book belongs to `MatchingEngine`. By
taking `const L3OrderBook&`, `L1Feed` makes this dependency explicit and prevents
accidental mutation. There is also no object lifetime ambiguity: the book must
outlive the feed, which is guaranteed by construction order in `MatchingEngine`.

**Why `[[nodiscard]]` on `update()` and `last()`**

`update()` returns a `TopOfBook` by value. A caller who writes `feed.update(ts)`
without capturing the return silently discards fresh data and may read stale data
from `last()` on the next line. `[[nodiscard]]` makes this a compile-time
warning. `last()` returns a `const TopOfBook&`; ignoring that return is almost
certainly a bug. The attribute is a zero-cost guard against a class of latent
errors.

**Why callback-driven rather than polling**

One `L1Callback` is registered once and called once per `update()`. The consumer
receives data as soon as it is ready, with no lock, no queue, and no contention.
The design assumes a single downstream consumer per `L1Feed` instance — which
matches the single-threaded hot-path model of the matching engine. If multiple
consumers were needed, the `MarketDataEngine` layer (which uses `HPRingBuffer`
for cross-thread delivery) is the right abstraction, not multiple callbacks.

---

## 7. What to Read Next

- `src/market/matching_engine.hpp` — `MatchingEngine` instantiates `L1Feed` as a
  member and calls `update()` after every order event that changes the top of
  book. This is where the callback is registered and where `record_trade()` is
  called after each fill.

- `src/market/market_data_engine.hpp` — the cross-thread layer. After
  `MatchingEngine` produces a `TopOfBook` via `L1Feed`, `MarketDataEngine`
  packages it into an `EngineEvent` and pushes it onto an `HPRingBuffer` for
  consumption by the market data publisher thread. This is the boundary between
  the single-threaded hot path and the multi-threaded delivery infrastructure.
