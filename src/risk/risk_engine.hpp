#pragma once
// ============================================================================
// HFT-sdk — Risk Engine
// Pre-trade risk checks: position limits, order rate limits (sliding
// window), per-trader & global kill switch, price/quantity validation.
// ============================================================================

#include <atomic>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "common/constants.hpp"
#include "common/types.hpp"

namespace HFT_sdk
{

/// Per-trader risk limits.
struct TraderRiskLimits
{
    Quantity max_position_per_symbol = DEFAULT_POSITION_LIMIT;
    Quantity max_net_position        = DEFAULT_POSITION_LIMIT * 10;
    std::uint32_t max_order_rate     = DEFAULT_ORDER_RATE_LIMIT;
    std::uint32_t rate_window_ms     = DEFAULT_RATE_LIMIT_WINDOW_MS;
    bool self_trade_prevention       = true;
};

/// Per-trader risk state.
struct TraderRiskState
{
    std::unordered_map<Symbol, Quantity, SymbolHash> positions;  // net position per symbol
    std::deque<Timestamp> order_timestamps;                      // for rate limiting
    Quantity net_position = 0;
    bool killed           = false;  // kill switch state
};

/// Risk Engine — pre-trade risk checks.
class RiskEngine
{
public:
    RiskEngine();

    // ── Configuration ──────────────────────────────────────────────────
    void setTraderLimits( TraderId trader_id, const TraderRiskLimits& limits );
    void setDefaultLimits( const TraderRiskLimits& limits );

    // ── Pre-trade checks ───────────────────────────────────────────────
    /// Check an incoming order against all risk constraints.
    /// Returns RejectReason::None if the order passes all checks.
    [[nodiscard]] RejectReason check_order( const Order& order );

    // ── Post-trade updates ─────────────────────────────────────────────
    /// Update risk state after a fill.
    void on_fill( TraderId trader_id, const Symbol& symbol, Side side, Quantity qty, Price price );

    // ── Kill switch ────────────────────────────────────────────────────
    /// Activate kill switch for a specific trader (rejects all new orders).
    void kill_trader( TraderId trader_id );

    /// Activate global kill switch (rejects all orders).
    void kill_all();

    /// Deactivate kill switch for a trader.
    void unkill_trader( TraderId trader_id );

    /// Deactivate global kill switch.
    void unkill_all();

    [[nodiscard]] bool is_killed( TraderId trader_id ) const;

    [[nodiscard]] bool is_global_kill() const { return global_kill_.load( std::memory_order_relaxed ); }

    // ── Query ──────────────────────────────────────────────────────────
    [[nodiscard]] Quantity get_position( TraderId trader_id, const Symbol& symbol ) const;

    [[nodiscard]] std::uint64_t orders_checked() const { return orders_checked_; }

    [[nodiscard]] std::uint64_t orders_rejected() const { return orders_rejected_; }

private:
    TraderRiskLimits default_limits_;
    std::unordered_map<TraderId, TraderRiskLimits> trader_limits_;
    std::unordered_map<TraderId, TraderRiskState> trader_states_;
    std::atomic<bool> global_kill_{ false };

    std::uint64_t orders_checked_  = 0;
    std::uint64_t orders_rejected_ = 0;

    const TraderRiskLimits& get_limits( TraderId trader_id ) const;
    TraderRiskState& get_state( TraderId trader_id );

    // Individual checks
    [[nodiscard]] RejectReason check_kill_switch( TraderId trader_id ) const;
    [[nodiscard]] RejectReason check_position_limit( TraderId trader_id, const Order& order );
    [[nodiscard]] RejectReason check_rate_limit( TraderId trader_id, Timestamp now );
    [[nodiscard]] RejectReason check_self_trade( TraderId trader_id, const Order& order ) const;
    [[nodiscard]] RejectReason check_price_validity( const Order& order ) const;
    [[nodiscard]] RejectReason check_quantity_validity( const Order& order ) const;
};

}  // namespace HFT_sdk
