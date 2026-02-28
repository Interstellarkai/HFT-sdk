#pragma once
// ============================================================================
// HFT-sdk — L1 Feed
// Computes and publishes top-of-book statistics: best bid/ask,
// spread, mid-price, microprice, VWAP, and rolling spread.
// ============================================================================

#include <algorithm>
#include <deque>
#include <functional>

#include "common/types.hpp"
#include "orderbook/l3_order_book.hpp"

namespace HFT_sdk
{

/// L1 Feed — computes and publishes top-of-book statistics.
class L1Feed
{
public:
    using L1Callback = std::function<void( const HFT_sdk::TopOfBook& )>;

    explicit L1Feed( const L3OrderBook& book );

    /// Update and return current L1 data.
    [[nodiscard]] HFT_sdk::TopOfBook update( HFT_sdk::Timestamp ts );

    /// Get last known L1 data (no recomputation).
    [[nodiscard]] const HFT_sdk::TopOfBook& last() const { return last_tob_; }

    /// Register callback for L1 updates.
    void on_update( L1Callback cb ) { callback_ = std::move( cb ); }

    // ── VWAP and statistics ────────────────────────────────────────────
    [[nodiscard]] double vwap() const;
    [[nodiscard]] double rolling_spread( std::size_t window ) const;

    void record_trade( HFT_sdk::Price price, HFT_sdk::Quantity qty );

private:
    const L3OrderBook& book_;
    HFT_sdk::TopOfBook last_tob_;
    L1Callback callback_;

    // Rolling trade data for VWAP
    struct TradeRecord
    {
        HFT_sdk::Price price;
        HFT_sdk::Quantity quantity;
    };

    std::deque<TradeRecord> recent_trades_;
    static constexpr std::size_t MAX_TRADE_HISTORY = 1000;

    // Rolling spread data
    std::deque<HFT_sdk::Price> spread_history_;
    static constexpr std::size_t MAX_SPREAD_HISTORY = 500;
};

}  // namespace HFT_sdk
