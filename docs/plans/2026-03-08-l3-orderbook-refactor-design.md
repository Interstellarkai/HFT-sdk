# L3 Order Book Refactor — Design Document

**Date:** 2026-03-08
**Scope:** `src/orderbook/l3_order_book.hpp` / `.cpp`, `src/orderbook/L3_ORDER_BOOK.md`
**Approach:** B (Elegance + const-correctness + update_queue_positions fix)

---

## Motivation

The current implementation has three categories of issues:

1. **Symmetric duplication** — `best_bid`/`best_ask` and `bid_depth`/`ask_depth` are identical modulo
   which map they read. The cancel removal block is duplicated across the bid and ask branches of
   `cancel_order`. Iceberg replenishment is an inline 6-line block inside the hot matching loop.

2. **`const`-correctness gaps** — Range-for loops over read-only containers are missing `const` on the
   binding (`for (auto& ...)` instead of `for (const auto& ...)`). Local copies of primitive values
   (`level_price`, `match_qty`) are not marked `const`, making intent ambiguous.

3. **Wasted O(n) work in cancel path** — `update_queue_positions` is called even when the price level
   is about to be erased (i.e., the canceled order was the last at that price). The full O(n) queue
   reindex happens before the level is immediately removed, making the work pointless.

---

## Design (Approach B)

### 1. Symmetric Method Unification

Introduce two private template helpers in `.cpp`:

```cpp
template <typename PriceLevels>
std::optional<BookLevel> best_level(const PriceLevels& levels) const;

template <typename PriceLevels>
std::vector<BookLevel> depth(const PriceLevels& levels, std::size_t n) const;
```

Public methods become one-liners delegating to these helpers:

```cpp
std::optional<BookLevel> best_bid() const { return best_level(bids_); }
std::optional<BookLevel> best_ask() const { return best_level(asks_); }
std::vector<BookLevel> bid_depth(std::size_t n) const { return depth(bids_, n); }
std::vector<BookLevel> ask_depth(std::size_t n) const { return depth(asks_, n); }
```

Template bodies live in `.cpp` with explicit instantiations — same pattern already used by
`match_against`. No header bloat, no compile-time cost for includers.

### 2. Cancel Removal Extraction + `update_queue_positions` Fix

Extract the ~12-line duplicated cancel removal block into a private helper:

```cpp
template <typename PriceLevels>
void remove_from_level(PriceLevels& levels, Price price, OrderQueue::iterator it);
```

Inside this helper, the key performance fix — skip the O(n) reindex when the level will be erased:

```cpp
queue.erase(it);
if (queue.empty())
    levels.erase(level_it);          // level gone — no reindex needed
else
    update_queue_positions(side, price);  // only when level still has orders
```

**Performance impact:** For the common cancel case where the order is the only one at its price
(typical in sparse books), this changes cancel from O(n) to O(1).

### 3. Iceberg Replenishment Extraction

Extract from the hot matching loop into a private helper:

```cpp
static void replenish_iceberg(L3Order& o);
```

The hot loop `match_against` gains no performance change — the compiler inlines the call — but the
inner loop body becomes easier to audit and test independently.

### 4. `const`-Correctness

- All read-only range-for loops: `for (const auto& [price, queue] : ...)`
- Local primitive copies in hot path: `const Price level_price = ...`, `const Quantity match_qty = ...`
- New template helpers marked `[[nodiscard]]`

---

## TDD Approach

Tests are written **before** the refactor in `tests/test_l3_order_book.cpp`. The test suite
captures current observable behavior so that the refactor can be verified to preserve it exactly.

### Test groups

| Group | What it covers |
|-------|---------------|
| `L3BookBasic` | Construction, empty book queries, best_bid/ask on empty book |
| `L3BookAdd` | New order → New status, partial fill → PartiallyFilled, full fill → Filled |
| `L3BookCancel` | Cancel known order → Canceled; cancel unknown → Rejected; bid/ask order count |
| `L3BookTIF` | IOC cancel-on-remainder; FOK reject when insufficient liquidity; FOK fill when sufficient |
| `L3BookMatching` | Price-time priority (oldest fills first); sweep across price levels |
| `L3BookIceberg` | Replenishment after visible depleted; hidden quantity consumed |
| `L3BookQueuePos` | `queue_position()` and `quantity_ahead()` correct after add/cancel |
| `L3BookCallbacks` | `on_trade` fires per match; `on_top_of_book` fires on state change |
| `L3BookReplace` | Cancel-replace succeeds; goes to back of queue at new price |
| `L3BookDuplicate` | Duplicate order ID → Rejected |

---

## Documentation Updates to `L3_ORDER_BOOK.md`

- Section 4 ("Data Structures"): update `match_against` explanation to cover the new `best_level`
  and `depth` helpers using the same explicit-instantiation pattern.
- Section 6 ("Design Decisions"): add subsection explaining the `update_queue_positions` short-circuit.
- New Section 7: **"Future Enhancement: Pool Allocator for `OrderQueue`"** (Approach C).

---

## Approach C — Pool Allocator (Deferred, Documented for Awareness)

`std::list<L3Order>` allocates each node separately on the heap. During the matching inner loop,
walking the list at a price level dereferences heap pointers scattered across RAM, causing cache
misses proportional to queue depth.

**The fix:** replace the allocator:

```cpp
// Current
using OrderQueue = std::list<L3Order>;

// Approach C
using OrderQueue = std::list<L3Order, PoolAllocator<L3Order>>;
```

A `PoolAllocator<L3Order>` backed by `memory_pool.hpp` (already in HFT-sdk) pre-allocates a
contiguous slab. All `L3Order` nodes are drawn from this slab, making sequential list traversal
cache-friendly.

**Why deferred:**
- Pool sizing must be chosen at compile time or construction time; wrong sizing causes pool
  exhaustion or wasted memory.
- Allocator propagation through `std::list` move/swap has subtle correctness requirements
  (`std::allocator_traits::propagate_on_container_move_assignment` etc.).
- `std::list::iterator` validity guarantees are preserved — pool node addresses are stable — so
  `order_index_` iterators remain valid. This is the key safety property that makes the change
  eventually feasible.

**Expected benefit:** ~20–40% reduction in matching loop latency for deep queues (>10 orders at
a price level), due to eliminating cache misses on list node traversal. Minimal impact for sparse
books (1–2 orders per level), which is the common HFT market-making case.

---

## Files Changed

| File | Change |
|------|--------|
| `src/orderbook/l3_order_book.hpp` | Add private helpers; `const`-correct query signatures |
| `src/orderbook/l3_order_book.cpp` | Implement helpers; fix `update_queue_positions` short-circuit |
| `src/orderbook/L3_ORDER_BOOK.md` | Update design decisions; add Approach C section |
| `tests/test_l3_order_book.cpp` | New: full behavioral test suite (written first) |
| `CMakeLists.txt` | Add `tests/test_l3_order_book.cpp` to `hft_sdk_tests` target |