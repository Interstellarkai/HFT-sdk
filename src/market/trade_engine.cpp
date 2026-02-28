// ============================================================================
// HFT-sdk — Trade Engine Implementation
// ============================================================================

#include "trade_engine.h"

using namespace HFT_sdk;

void TradeEngine::process_trade(const Trade& trade) {
    total_trades_++;
    total_volume_  += trade.qty;
    total_notional_ += static_cast<double>(trade.qty) * static_cast<double>(trade.price);

    // Update positions for both parties
    // Incoming (aggressor)
    update_position(trade.incoming_trader, trade.symbol,
                    trade.aggressor_side, trade.qty, trade.price);

    // Resting (passive) — opposite side
    HFT_sdk::Side passive_side = (trade.aggressor_side == HFT_sdk::Side::Buy) ? HFT_sdk::Side::Sell : HFT_sdk::Side::Buy;
    update_position(trade.resting_trader, trade.symbol,
                    passive_side, trade.qty, trade.price);

    if (trade_cb_) trade_cb_(trade);
}

const Position* TradeEngine::get_position(HFT_sdk::TraderId trader_id, const HFT_sdk::Symbol& symbol) const {
    TraderSymbolKey key{trader_id, symbol};
    auto it = positions_.find(key);
    if (it == positions_.end()) return nullptr;
    return &it->second;
}

std::vector<Position> TradeEngine::get_positions(HFT_sdk::TraderId trader_id) const {
    std::vector<Position> result;
    for (auto& [key, pos] : positions_) {
        if (key.trader_id == trader_id) {
            result.push_back(pos);
        }
    }
    return result;
}

void TradeEngine::mark_to_market(const HFT_sdk::Symbol& symbol, HFT_sdk::Price current_price) {
    for (auto& [key, pos] : positions_) {
        if (key.symbol == symbol) {
            if (pos.net_qty > 0) {
                // Long position
                pos.unrealized_pnl = static_cast<double>(pos.net_qty) *
                    (static_cast<double>(current_price) - pos.avg_buy_price);
            } else if (pos.net_qty < 0) {
                // Short position
                pos.unrealized_pnl = static_cast<double>(-pos.net_qty) *
                    (pos.avg_sell_price - static_cast<double>(current_price));
            } else {
                pos.unrealized_pnl = 0.0;
            }
        }
    }
}

void TradeEngine::update_position(HFT_sdk::TraderId trader_id, const HFT_sdk::Symbol& symbol,
                                   HFT_sdk::Side side, HFT_sdk::Quantity qty, HFT_sdk::Price price) {
    TraderSymbolKey key{trader_id, symbol};
    auto& pos = positions_[key];
    pos.symbol = symbol;

    double fill_price = static_cast<double>(price);
    double fill_qty   = static_cast<double>(qty);

    if (side == HFT_sdk::Side::Buy) {
        // If reducing a short position, realize PnL
        if (pos.net_qty < 0) {
            HFT_sdk::Quantity close_qty = std::min(qty, -pos.net_qty);
            pos.realized_pnl += static_cast<double>(close_qty) *
                (pos.avg_sell_price - fill_price);
        }

        // Update average buy price
        if (pos.net_qty >= 0) {
            double old_cost = pos.avg_buy_price * static_cast<double>(pos.buy_qty);
            pos.buy_qty += qty;
            pos.avg_buy_price = (old_cost + fill_price * fill_qty) /
                                static_cast<double>(pos.buy_qty);
        }

        pos.net_qty += qty;
    } else {
        // If reducing a long position, realize PnL
        if (pos.net_qty > 0) {
            HFT_sdk::Quantity close_qty = std::min(qty, pos.net_qty);
            pos.realized_pnl += static_cast<double>(close_qty) *
                (fill_price - pos.avg_buy_price);
        }

        // Update average sell price
        if (pos.net_qty <= 0) {
            double old_cost = pos.avg_sell_price * static_cast<double>(pos.sell_qty);
            pos.sell_qty += qty;
            pos.avg_sell_price = (old_cost + fill_price * fill_qty) /
                                 static_cast<double>(pos.sell_qty);
        }

        pos.net_qty -= qty;
    }

    if (position_cb_) position_cb_(trader_id, pos);
}
