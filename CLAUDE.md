# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

Prerequisites: CMake ≥ 3.20, GCC 13+, Clang 17+, or MSVC 2022+ (C++23 required).

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

The static library target is `HFT-sdk` (CMake alias `HFT::Toolset`). All public headers live under `src/`; consumers include via `target_include_directories` pointing at `src/`.

## Architecture

This is a **header-heavy static library** — most modules are split into `.h`/`.cpp` pairs but the core data structures (`HPRingBuffer`, `ScopeTimer`, `benchmark_p99`, `memory_pool`) are header-only.

All types live in namespace `HFT_sdk`. The entry point for the type system is `src/common/types.h`.

### Module Dependency Graph

```
common/types.h  ←  everything
common/clock.h  ←  matching_engine, market_data_engine, telemetry
common/constants.h  ←  risk_engine, market_data_engine

HPRingBuffer.hpp  ←  market_data_engine (lock-free MD queue)
ScopeTimer.hpp   ←  matching_engine, telemetry

orderbook/l3_order_book  ←  matching_engine  (full L3 book per symbol)
orderbook/l2_aggregator  ←  matching_engine  (depth snapshots)
orderbook/l1_feed        ←  matching_engine  (TOB, microprice, VWAP)

risk/risk_engine  ←  matching_engine (pre-trade checks, optional)

market/matching_engine   ←  coordinates all of the above
market/trade_engine      ←  standalone: PnL/position tracking from trades
market/market_data_engine  ←  HPRingBuffer-backed cross-thread MD
market/market_data_publisher  ←  lightweight callback-based MD
market/order_book        ←  simple alternative to L3 (not used by MatchingEngine)

metrics/telemetry  ←  integrates ScopeTimer + latency_histogram
latency/latency_model  ←  Gaussian + heavy-tail latency simulation
```

### Core Types (`src/common/types.h`)

- `Price = int64_t` — prices in integer ticks (10000 = 100.00 with 2 dp)
- `Symbol` — 8-byte stack-allocated fixed string (max 8 chars, truncates silently); hash via `SymbolHash`
- `EngineEvent` — 64-byte `alignas(64)` tagged union: `Order | CancelRequest | ReplaceRequest | ExecutionReport | Trade | TopOfBook | DepthSnapshot`
- `Order` — 64-byte `alignas(64)` POD; includes `cl_ord_id[20]` for FIX mapping
- `HPRingBuffer<T, N>` — N must be a power of 2; capacity is `N-1` (one slot sacrificed to distinguish full/empty)

### Recommended Threading Model

Components are **not thread-safe** — designed for single-threaded hot paths connected by `HPRingBuffer` (SPSC lock-free queues).

| Thread | Component | Queue direction |
|--------|-----------|-----------------|
| T1 | `MatchingEngine` + `L3OrderBook` | reads `HPRingBuffer` order queue |
| T2 | Order source (FIX / sim) | writes `HPRingBuffer` order queue |
| T3 | `MarketDataEngine` | reads internal `HPRingBuffer` MD queue |
| T4 | `Telemetry` | read-only metrics access |

### Key Design Decisions

- **O(1) cancel** — `L3OrderBook` keeps an `unordered_map<OrderId, OrderLocation>` storing a `std::list::iterator` per order, enabling O(1) removal without scanning price levels.
- **Two order book choices** — `market/order_book.h` is a lightweight `std::map`-based book for simple use cases; `orderbook/l3_order_book.h` is the full L3 book (iceberg, queue position, FOK/IOC). `MatchingEngine` always uses the L3 book.
- **Compile-time constants** — all buffer/pool sizes are in `src/common/constants.h` (e.g., `ORDER_QUEUE_SIZE = 16384`, `ORDER_POOL_SIZE = 500000`).
- **`Clock` abstraction** — `Clock::Mode::Simulated` allows deterministic replay; `Clock::Mode::WallClock` uses `std::chrono::steady_clock`. Must be passed to `MatchingEngine`, `MarketDataEngine`, and `Telemetry`.
- **`EngineEvent` uses `memcpy` for copy/assign** — the union contains non-trivially-copyable members; the class overrides copy constructor and assignment with `std::memcpy` for fixed-size performance.

### Risk Engine Integration

`RiskEngine` is optional — attach via `engine.set_risk_engine(&risk)`. Checks run in this order: kill switch → position limit → rate limit (sliding window `deque<Timestamp>`) → self-trade prevention → price validity → quantity validity. Call `risk.on_fill(...)` after each trade to keep position state current.

### Latency Model

`LatencyModel` produces a `LatencyBreakdown` with `network_inbound_ns`, `matching_engine_ns`, `network_outbound_ns`, `total_ns`. Uses Gaussian base + heavy-tail spike with configurable per-client profiles. Seed in constructor for reproducibility.

### Telemetry

`Telemetry` uses lock-free `std::atomic` counters and a fixed-bucket `LatencyHistogram<MaxVal, BucketWidth>`. Doubles (fill probability, slippage) are stored as fixed-point `int64_t` scaled by `1e9` to allow atomic `fetch_add`. Call `print_dashboard()` from a dedicated metrics thread.