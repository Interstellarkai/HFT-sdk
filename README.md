# HFT-sdk

A comprehensive C++23 toolkit for high-frequency trading systems and low-latency exchange simulation. Provides
production-grade building blocks from lock-free data structures to a full matching engine.

## Modules

| Module            | Directory               | Description                                                     |
|-------------------|-------------------------|-----------------------------------------------------------------|
| **HPRingBuffer**  | `src/HPRingBuffer.hpp`  | Lock-free SPSC ring buffer with power-of-2 sizing               |
| **Benchmark P99** | `src/benchmark_p99.hpp` | P99/P99.9 latency benchmarking for callables                    |
| **ScopeTimer**    | `src/ScopeTimer.hpp`    | RAII scope timers with compile-time disable                     |
| **Common**        | `src/common/`           | Core types, constants, clock, and memory pool                   |
| **Order Book**    | `src/orderbook/`        | L3 order book, L2 aggregator, L1 feed                           |
| **Market**        | `src/market/`           | Matching engine, trade engine, market data publisher/engine     |
| **Latency**       | `src/latency/`          | Gaussian + heavy-tail latency model with per-client profiles    |
| **Risk**          | `src/risk/`             | Position limits, rate limits, kill switch, pre-trade checks     |
| **Metrics**       | `src/metrics/`          | Latency histograms, telemetry dashboard, ScopeTimer integration |

> Status: This library is under active development; new tools will be added over time.

## Overview

HFT-sdk is a collection of zero-overhead C++23 abstractions designed for high-frequency trading systems and low-latency
applications. The library focuses on:

- **Lock-free data structures** — SPSC ring buffer for inter-thread communication
- **Performance measurement** — P99/P99.9 latency tracking, RAII scope timers, latency histograms
- **Market microstructure** — L3 order book with queue position tracking, iceberg support, L2/L1 aggregation
- **Matching engine** — Multi-symbol matching with risk checks, trade engine, and market data publishing
- **Latency modeling** — Gaussian + heavy-tail distributions with per-client profiles
- **Risk management** — Position limits, rate limiting, kill switches, pre-trade validation
- **Metrics & telemetry** — Atomic counters, histograms, console dashboard
- **Zero-cost abstractions** — Compile-time configurability and minimal runtime overhead
- **Modern C++23** — Leverages `std::print`, concepts, ranges, and `constexpr` for type safety and performance

## Library Structure

```
HFT-sdk/
├── CMakeLists.txt                  # Build configuration (static library)
├── LICENSE                         # MIT License
├── README.md                       # This file
├── examples/
│   └── p99_example.hpp             # Latency benchmarking example
└── src/
    ├── HPRingBuffer.hpp            # Lock-free SPSC ring buffer
    ├── HPRINGBUFFER.md             # Ring buffer documentation
    ├── ScopeTimer.hpp              # RAII scope timers & thread-local management
    ├── benchmark_p99.hpp           # P99/P99.9 latency benchmarking
    ├── library_anchor.cpp          # Library anchor (for static lib linkage)
    │
    ├── common/                     # ── Shared foundations ──
    │   ├── types.h                 # Core types: Order, Trade, Symbol, enums, events
    │   ├── constants.h             # Compile-time constants (buffer sizes, defaults)
    │   ├── clock.h / clock.cpp     # Wall-clock / simulated clock (ns resolution)
    │   └── memory_pool.h           # Lock-free fixed-capacity object pool
    │
    ├── orderbook/                  # ── Order book hierarchy ──
    │   ├── l3_order_book.h/.cpp    # L3 order book (full order-level, iceberg, FOK/IOC)
    │   ├── l2_aggregator.h/.cpp    # L2 depth snapshot aggregation (Market-by-Price)
    │   └── l1_feed.h/.cpp          # L1 top-of-book, microprice, VWAP, rolling spread
    │
    ├── market/                     # ── Market engine components ──
    │   ├── README.md               # Market module documentation
    │   ├── matching_engine.h/.cpp  # Multi-symbol matching engine with risk integration
    │   ├── trade_engine.h/.cpp     # Position tracking, PnL, mark-to-market
    │   ├── order_book.h/.cpp       # Simple limit order book (lightweight alternative)
    │   ├── market_data_engine.h/.cpp     # MD aggregation with throttling (HPRingBuffer)
    │   └── market_data_publisher.h/.cpp  # Callback-based market data publisher
    │
    ├── latency/                    # ── Latency simulation ──
    │   └── latency_model.h/.cpp    # Gaussian + heavy-tail latency with per-client profiles
    │
    ├── risk/                       # ── Risk management ──
    │   └── risk_engine.h/.cpp      # Position limits, rate limits, kill switch
    │
    └── metrics/                    # ── Telemetry & metrics ──
        ├── latency_histogram.h/.cpp  # O(1) fixed-bucket latency histogram
        └── telemetry.h/.cpp          # Metrics aggregation & console dashboard
```

## Build

Prerequisites: CMake ≥ 3.20 and a C++23-capable compiler (GCC 13+, Clang 17+, MSVC 2022+).

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

The static library target is `HFT-sdk` (alias `HFT::sdk`). Public headers live under `src/`.

## Usage

### High-Performance Ring Buffer (lock-free SPSC queue)

```cpp
#include "HPRingBuffer.hpp"

int main() {
    // Size must be power of 2
    HPRingBuffer<int, 1024> queue;

    // Producer thread
    queue.push(42);
    queue.push(100);

    // Consumer thread
    if (auto item = queue.pop()) {
        // Process item.value()
    }

    // Query state
    std::cout << "Size: " << queue.size() << "\n";
    std::cout << "Capacity: " << queue.capacity() << "\n";
    std::cout << "Empty: " << queue.empty() << "\n";
    std::cout << "Full: " << queue.full() << "\n";
}
```

### Latency Benchmarking

```cpp
#include "benchmark_p99.hpp"
#include <array>
#include <span>

int main() {
    std::array<std::int64_t, 100000> buf{};
    auto stats = hft_bench::benchmark_p99(
        [](int a, int b) noexcept { return a + b; },
        1, 2,
        std::span<std::int64_t>(buf),
        50'000,
        1'000
    );
    // stats.p99_ns / stats.p999_ns hold percentile latencies (ns)
}
```

### Scope Timing (compile-time toggle via `SCOPE_TIMER_DISABLED`)

```cpp
#include "ScopeTimer.hpp"
#include <chrono>

int main() {
    ScopeTimer<std::chrono::microseconds> t{/*raii=*/true};
    // ... work ...
    t.endAndLog();

    NScopeTimers::start("lookup");
    // ... work ...
    NScopeTimers::endAndLog("lookup");
}
```

### L3 Order Book + Matching Engine

```cpp
#include "common/clock.h"
#include "common/types.h"
#include "market/matching_engine.h"

using namespace HFT-sdk;

int main() {
    Clock clock(Clock::Mode::Simulated);
    MatchingEngine engine(clock);
    engine.add_symbol("AAPL");

    // Register callbacks
    engine.on_trade([](const Trade& t) {
        std::cout << "TRADE: " << t.qty << " @ " << t.price << "\n";
    });

    engine.on_l1_update([](const TopOfBook& tob) {
        if (tob.valid)
            std::cout << "BBO: " << tob.best_bid.price << " / " << tob.best_ask.price << "\n";
    });

    // Submit orders
    Order buy{};
    buy.id = 1; buy.trader_id = 100; buy.symbol = Symbol("AAPL");
    buy.side = Side::Buy; buy.type = OrderType::Limit;
    buy.tif = TimeInForce::Day; buy.price = 150; buy.quantity = 100;
    engine.process_new_order(buy);

    Order sell{};
    sell.id = 2; sell.trader_id = 101; sell.symbol = Symbol("AAPL");
    sell.side = Side::Sell; sell.type = OrderType::Limit;
    sell.tif = TimeInForce::IOC; sell.price = 150; sell.quantity = 50;
    engine.process_new_order(sell);  // → triggers trade callback
}
```

### Risk Engine

```cpp
#include "risk/risk_engine.h"

using namespace HFT-sdk;

RiskEngine risk;
risk.setDefaultLimits({.max_position_per_symbol = 1000, .max_order_rate = 500});

// Pre-trade check
auto reason = risk.check_order(order);
if (reason != RejectReason::None) { /* reject */ }

// Kill switch
risk.kill_trader(trader_id);   // rejects all orders from this trader
risk.kill_all();               // global kill switch
```

### Latency Model

```cpp
#include "latency/latency_model.h"

HFT-sdk::LatencyModel model(/*seed=*/42);
auto breakdown = model.sample_round_trip(trader_id);
// breakdown.network_inbound_ns, .matching_engine_ns, .network_outbound_ns, .total_ns
```

### Telemetry Dashboard

```cpp
#include "metrics/telemetry.h"

HFT-sdk::Telemetry telemetry(clock);
telemetry.record_order();
telemetry.record_matching_latency(450);  // 450 ns
telemetry.print_dashboard();             // formatted console output
```

See `examples/p99_example.hpp` for a full benchmarking snippet and `src/market/README.md` for detailed market component
documentation.

## License

MIT — see `LICENSE` for details.
