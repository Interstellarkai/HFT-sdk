#pragma once
// ============================================================================
// HFT-sdk — Trade Engine
// Processes trades, manages per-trader per-symbol positions,
// computes realized/unrealized PnL, and mark-to-market.
// ============================================================================

#include "common/types.h"
#include <unordered_map>
#include <vector>
#include <functional>

namespace HFT_sdk
{

/// Position tracking per trader per symbol.
struct Position {
    Symbol   symbol;
    Quantity net_qty     = 0;   // positive = long, negative = short
    Quantity buy_qty     = 0;
    Quantity sell_qty    = 0;
    double   avg_buy_price  = 0.0;
    double   avg_sell_price = 0.0;
    double   realized_pnl   = 0.0;
    double   unrealized_pnl = 0.0;
};

/// Composite key for trader+symbol position lookup.
struct TraderSymbolKey {
    TraderId trader_id;
    Symbol   symbol;

    bool operator==(const TraderSymbolKey& o) const {
        return trader_id == o.trader_id && symbol == o.symbol;
    }
};

struct TraderSymbolKeyHash {
    std::size_t operator()(const TraderSymbolKey& k) const {
        auto h1 = std::hash<TraderId>{}(k.trader_id);
        auto h2 = SymbolHash{}(k.symbol);
        return h1 ^ (h2 << 1);
    }
};

/// Trade Engine — processes trades and manages positions.
class TradeEngine {
public:
    using TradeCallback = std::function<void(const Trade&)>;
    using PositionCallback = std::function<void(TraderId, const Position&)>;
    TradeEngine() = default;

    /// Process a trade event — update positions for both parties.
    void process_trade(const Trade& trade);

    /// Get position for a trader+symbol.
    [[nodiscard]] const Position* get_position(TraderId trader_id, const Symbol& symbol) const;

    /// Get all positions for a trader.
    [[nodiscard]] std::vector<Position> get_positions(TraderId trader_id) const;

    /// Mark-to-market all positions for a symbol.
    void mark_to_market(const Symbol& symbol, Price current_price);

    /// Register callbacks.
    void on_trade(TradeCallback cb)       { trade_cb_ = std::move(cb); }
    void on_position(PositionCallback cb) { position_cb_ = std::move(cb); }

    /// Statistics.
    [[nodiscard]] std::uint64_t total_trades()  const { return total_trades_; }
    [[nodiscard]] Quantity      total_volume()   const { return total_volume_; }
    [[nodiscard]] double        total_notional() const { return total_notional_; }

private:
    std::unordered_map<TraderSymbolKey, Position, TraderSymbolKeyHash> positions_;

    TradeCallback    trade_cb_;
    PositionCallback position_cb_;

    std::uint64_t total_trades_  = 0;
    Quantity      total_volume_  = 0;
    double        total_notional_ = 0.0;

    void update_position(TraderId trader_id, const Symbol& symbol,
                         Side side, Quantity qty, Price price);
};

}  // namespace HFT_sdk
