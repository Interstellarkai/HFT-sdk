# DESIGN_PATTERN.md — Design Doc

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:writing-plans to create the implementation plan.

**Goal:** Create `DESIGN_PATTERN.md` at the HFT-sdk repo root — a single educational document covering every significant design pattern used in the codebase, aimed at new joiners interested in HFT.

**Structure:** Approach C — short annotated flow diagram up front for orientation, followed by a pattern catalogue (one section per pattern). Content may overlap with existing per-component docs intentionally — this is the one-stop read.

**Output file:** `/DESIGN_PATTERN.md` (repo root)

---

## Document Structure

### Part 1 — Introduction (~200 words)

One paragraph explaining what this doc is and how to use it:
- Read this before the per-component docs (L3_ORDER_BOOK.md, MATCHING_ENGINE.md, etc.)
- Each pattern section is self-contained and cross-references the relevant source files
- Patterns are ordered most architecturally significant → most implementation-detail

Index table: one row per pattern, columns: Pattern | Category | Where in codebase.

### Part 2 — The Flow (annotated Mermaid diagram)

Mermaid `flowchart LR` with four subgraphs:

```
T2 (Producer) ──push()──▶ HPRingBuffer order_queue ──pop()──▶ T1 MatchingEngine (Consumer→Producer)
                                                                   │ Facade → Risk → L3Book → L2Agg → Callbacks
                                                              ──push()──▶ HPRingBuffer md_queue ──pop()──▶ T3 MarketDataEngine
```

Each node in T1 labelled with the pattern it uses (Facade, Template Method+Policy, Stateless View, Observer/Type Erasure).

Two paragraphs after the diagram:
1. The **synchronous path** — everything inside T1 runs inline, callbacks fire before `process_new_order` returns
2. The **async path** — DepthSnapshot is copied into a ring buffer slot and consumed by T3 on its own schedule

### Part 3 — Pattern Catalogue (12 patterns)

Each pattern section follows a fixed template:
```
## N. Pattern Name

**Category:** GoF Behavioural / Structural / Concurrency / Modern C++ / HFT idiom
**Files:** src/path/to/file.hpp, src/path/to/file.cpp

### What it is
One paragraph: general definition of the pattern.

### How it appears here
Code snippet from the actual codebase. Explanation of the specific usage.

### Why this pattern (not the alternative)
One paragraph: the trade-off that made this the right choice in an HFT context.
```

#### Pattern list (in document order):

1. **Observer / Callback** — `std::function` stored in `trade_cb_`, `tob_cb_`, `depth_cb_` (L3OrderBook); `exec_cb_`, `trade_cb_`, `l1_cb_`, `l2_cb_` (MatchingEngine). Alternative considered: virtual base + vtable dispatch (adds indirection, heap allocation for subscriber list). `std::function` chosen for single-subscriber simplicity and zero-overhead when cb is null.

2. **Facade / Coordinator** — `MatchingEngine` as single entry point: hides per-symbol `L3OrderBook`, `L2Aggregator`, `L1Feed`, and `RiskEngine` behind four public methods (`process_new_order`, `process_cancel`, `process_replace`, `get_depth`). Alternative: callers operate directly on `L3OrderBook`. Rejected because risk checks, routing, and MD publishing would have to be duplicated by every caller.

3. **SPSC / Disruptor** — `HPRingBuffer<T, Size, WaitStrategy>`. Lock-free by design: two cache-line-isolated atomics (`write_sequence_`, `read_sequence_`), power-of-2 slot count for bitmask modulus, no heap allocation per message. Alternative: `std::queue` + mutex. Rejected: mutex introduces kernel transitions and cache-line contention — unacceptable on the matching hot path.

4. **Shared-Nothing Concurrency** — each data structure (L3OrderBook, L2Aggregator, L1Feed) is owned exclusively by one thread (T1). No mutex anywhere in the hot path. Cross-thread data transfer only via HPRingBuffer. Alternative: shared_ptr + mutex on L3OrderBook. Rejected: locks serialize threads and add latency spikes.

5. **Stateless View / Lens** — `L2Aggregator` holds `const L3OrderBook&` and recomputes on every `snapshot()` call. No cache, no shadow copy. Files: `src/orderbook/l2_aggregator.hpp/.cpp`. Alternative: cache a `DepthSnapshot` and update incrementally. Rejected: requires subscribing to L3 events, managing consistency, and adds complexity with no benefit when T1 calls snapshot after every order.

6. **Template Method via C++ Templates** — `match_against<PriceLevels>`, `best_level<PriceLevels>`, `depth<PriceLevels>`, `remove_from_level<PriceLevels>` in `l3_order_book.cpp`. Template parameter is either `BidPriceLevels` or `AskPriceLevels`. Algorithm body is identical; only the map's sort order differs. Explicit instantiations at bottom of .cpp keep headers clean. Alternative: duplicate bid/ask implementations. Rejected: any bug fix must be applied twice.

7. **Policy-Based Design** — `BidPriceLevels = std::map<Price, OrderQueue, std::greater<Price>>` vs `AskPriceLevels = std::map<Price, OrderQueue, std::less<Price>>`. Sort order (the "policy") is injected as a template argument, not a runtime flag. Also: `HPRingBuffer<T, Size, WaitStrategy>` where `WaitStrategy` is `BusySpinWait`, `YieldWait`, or `BlockWait`. Zero runtime cost — branch is eliminated at compile time. Alternative: `if (side == Buy)` inside a single map with runtime comparator. Rejected: runtime comparator costs a virtual dispatch per comparison.

8. **Iterator Stability Index** — `order_index_: unordered_map<OrderId, OrderLocation>` where `OrderLocation` stores a `std::list<L3Order>::iterator`. `std::list` guarantees iterator validity across insert/erase of other nodes — the index can store iterators and they remain valid for the order's lifetime. Enables O(1) cancel: no price-level scan needed. Alternative: `std::vector` per level. Rejected: vector reallocation invalidates iterators.

9. **RAII** — `ScopeTimer<Duration>` starts a timer on construction, stops and logs on destruction. `std::unique_ptr<L3OrderBook>` in `SymbolState` — book is created once, destroyed with the engine, no manual delete. Files: `src/ScopeTimer.hpp`, `src/market/matching_engine.hpp`. Alternative: manual start/stop + delete. Rejected: exception paths skip cleanup.

10. **Optional / Null Object** — `best_bid()` and `best_ask()` return `std::optional<BookLevel>`. Caller checks `if (bb && ba)` before deriving spread/microprice. Files: `src/orderbook/l3_order_book.hpp`. Alternative: sentinel `BookLevel{0, 0, 0}` or raw pointer. Rejected: sentinel requires callers to know the magic value; raw pointer implies heap allocation.

11. **Type Erasure** — `std::function<void(const Trade&)>` erases the concrete callable type (lambda, functor, free function, member function via bind). The L3OrderBook does not know and does not care what the callback is — it only knows the signature. Files: `l3_order_book.hpp`, `matching_engine.hpp`. Alternative: template parameter for callback type. Rejected: forces callback type into the class template parameter, breaking binary compatibility and complicating header dependencies.

12. **Value Semantics for Performance** — `Symbol` (8-byte fixed string, stack-allocated, no heap), `Order` and `EngineEvent` (`alignas(64)` PODs, copy via `std::memcpy`). Designed to pass and copy like primitives. `EngineEvent` overrides copy constructor/assignment with `memcpy` for fixed-size performance. Files: `src/common/types.hpp`. Alternative: `std::string` for Symbol, `std::shared_ptr<Order>`. Rejected: heap allocation and reference counting on the hot path.

---

## Cross-References

After the catalogue, a final section: "Where to go next" — links to L3_ORDER_BOOK.md, L2_AGGREGATOR.md, L1_FEED.md, MATCHING_ENGINE.md, HPRINGBUFFER.md with one-line descriptions.
