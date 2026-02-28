#pragma once
// ============================================================================
// HFT-sdk — Compile-Time Constants
// System-wide constants for buffer sizes, pool capacities, latency defaults,
// risk parameters, simulation defaults, and CPU affinity configuration.
// ============================================================================

#include <cstddef>
#include <cstdint>

namespace HFT_sdk
{

// ── System-wide Constants ──────────────────────────────────────────────────
static constexpr std::size_t CACHE_LINE_SIZE      = 64;
static constexpr std::size_t MAX_SYMBOLS          = 64;
static constexpr std::size_t MAX_TRADERS          = 256;
static constexpr std::size_t MAX_ORDERS_PER_BOOK  = 100'000;
static constexpr std::size_t MAX_TRADES_PER_BATCH = 1'024;

// ── Ring Buffer Sizes (must be power of 2) ─────────────────────────────────
static constexpr std::size_t ORDER_QUEUE_SIZE     = 16'384;  // FIX -> engine
static constexpr std::size_t EXEC_QUEUE_SIZE      = 16'384;  // engine -> FIX
static constexpr std::size_t MD_QUEUE_SIZE        = 16'384;  // engine -> MD publisher
static constexpr std::size_t SIM_QUEUE_SIZE       = 8'192;   // simulation -> engine

// ── Memory Pool Sizes ──────────────────────────────────────────────────────
static constexpr std::size_t ORDER_POOL_SIZE      = 500'000;
static constexpr std::size_t TRADE_POOL_SIZE      = 100'000;
static constexpr std::size_t EVENT_POOL_SIZE      = 50'000;

// ── Market Data ────────────────────────────────────────────────────────────
static constexpr std::size_t DEFAULT_L2_DEPTH     = 10;
static constexpr std::size_t MAX_L2_DEPTH         = 20;

// ── Latency Defaults (nanoseconds) ────────────────────────────────────────
static constexpr std::uint64_t DEFAULT_NETWORK_LATENCY_NS       = 5'000;    // 5 µs
static constexpr std::uint64_t DEFAULT_MATCHING_LATENCY_NS      = 500;      // 0.5 µs
static constexpr std::uint64_t DEFAULT_MD_DISSEMINATION_NS      = 2'000;    // 2 µs
static constexpr std::uint64_t DEFAULT_JITTER_STDDEV_NS         = 1'000;    // 1 µs

// ── Risk Defaults ──────────────────────────────────────────────────────────
static constexpr std::int64_t  DEFAULT_POSITION_LIMIT           = 10'000;
static constexpr std::uint32_t DEFAULT_ORDER_RATE_LIMIT         = 1'000;    // per second
static constexpr std::uint32_t DEFAULT_RATE_LIMIT_WINDOW_MS     = 1'000;

// ── Simulation Defaults ────────────────────────────────────────────────────
static constexpr double DEFAULT_ORDER_ARRIVAL_RATE = 100.0;   // orders/second
static constexpr double DEFAULT_CANCEL_RATE        = 0.3;     // 30% of orders get cancelled
static constexpr double DEFAULT_INITIAL_MID_PRICE  = 10000;   // 100.00 in ticks (2 dp)
static constexpr double DEFAULT_TICK_SIZE          = 1;        // 1 tick = 0.01
static constexpr double DEFAULT_SPREAD_TICKS       = 2.0;     // 2 ticks spread
static constexpr double DEFAULT_VOLATILITY         = 0.001;    // 0.1% per step

// ── Thread Configuration ───────────────────────────────────────────────────
static constexpr int CPU_AFFINITY_MATCHING_ENGINE  = 1;
static constexpr int CPU_AFFINITY_FIX_GATEWAY      = 2;
static constexpr int CPU_AFFINITY_MD_PUBLISHER     = 3;
static constexpr int CPU_AFFINITY_SIMULATION       = 4;
static constexpr int CPU_AFFINITY_METRICS          = 5;

}  // namespace HFT_sdk
