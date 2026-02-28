#pragma once
// ============================================================================
// HFT-sdk — Multi-Symbol Matching Engine
// Coordinates per-symbol L3 order books, L1/L2 feeds, risk checks,
// and publishes execution reports, trades, and market data updates.
// ============================================================================

#include <orderbook/l1_feed.hpp>
#include <orderbook/l2_aggregator.hpp>
#include <orderbook/l3_order_book.hpp>

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "clock.hpp"
#include "market_data_publisher.hpp"
#include "order_book.hpp"
#include "types.hpp"

namespace HFT_sdk
{

/// Forward declarations
class RiskEngine;

/// Per-symbol book + market data state.
struct SymbolState
{
    std::unique_ptr<L3OrderBook> book;
    std::unique_ptr<L2Aggregator> l2;
    std::unique_ptr<L1Feed> l1;
};

class MatchingEngine
{
public:
    using ExecutionCallback = std::function<void( const ExecutionReport& )>;
    using TradeCallback     = std::function<void( const Trade& )>;
    using L1Callback        = std::function<void( const TopOfBook& )>;
    using L2Callback        = std::function<void( const DepthSnapshot& )>;

    explicit MatchingEngine( Clock& clock );

    void add_symbol( SymbolId symbol );
    void add_symbol( const Symbol& symbol );
    [[nodiscard]] bool has_symbol( const Symbol& symbol ) const;
    [[nodiscard]] std::vector<Symbol> symbols() const;

    // ── Callbacks ──────────────────────────────────────────────────────
    void on_execution( ExecutionCallback cb ) { exec_cb_ = std::move( cb ); }

    void on_trade( TradeCallback cb ) { trade_cb_ = std::move( cb ); }

    void on_l1_update( L1Callback cb ) { l1_cb_ = std::move( cb ); }

    void on_l2_update( L2Callback cb ) { l2_cb_ = std::move( cb ); }

    // ── Order Processing ───────────────────────────────────────────────
    /// Process a new order. Returns execution report.
    ExecutionReport process_new_order( const Order& order );

    /// Process a cancel request.
    ExecutionReport process_cancel( const CancelRequest& cancel );

    /// Process a cancel-replace request.
    ExecutionReport process_replace( const ReplaceRequest& replace );

    // ── Market Data Query ──────────────────────────────────────────────
    [[nodiscard]] TopOfBook get_top_of_book( const Symbol& symbol ) const;
    [[nodiscard]] DepthSnapshot get_depth( const Symbol& symbol ) const;
    [[nodiscard]] const L3OrderBook* get_book( const Symbol& symbol ) const;

    // ── Risk Engine binding ────────────────────────────────────────────
    void set_risk_engine( RiskEngine* risk ) { risk_engine_ = risk; }

    // ── Statistics ─────────────────────────────────────────────────────
    [[nodiscard]] std::uint64_t total_orders_processed() const { return orders_processed_; }

    [[nodiscard]] std::uint64_t total_trades() const { return trades_generated_; }

    [[nodiscard]] std::uint64_t total_cancels() const { return cancels_processed_; }

    [[nodiscard]] std::uint64_t total_rejects() const { return rejects_; }

private:
    Clock& clock_;
    std::unordered_map<Symbol, SymbolState, SymbolHash> symbols_;

    // O(1) order-to-symbol routing
    std::unordered_map<OrderId, Symbol> order_symbol_index_;

    // Risk engine (optional)
    RiskEngine* risk_engine_ = nullptr;

    // Callbacks
    ExecutionCallback exec_cb_;
    TradeCallback trade_cb_;
    L1Callback l1_cb_;
    L2Callback l2_cb_;

    // Counters
    std::uint64_t orders_processed_  = 0;
    std::uint64_t trades_generated_  = 0;
    std::uint64_t cancels_processed_ = 0;
    std::uint64_t rejects_           = 0;

    void publish_market_data( const Symbol& symbol );
};

}  // namespace HFT_sdk
