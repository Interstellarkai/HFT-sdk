// ============================================================================
// HFT-sdk — Market Data Engine Implementation
// ============================================================================

#include "market_data_engine.hpp"

using namespace HFT_sdk;
using namespace hft_sim;

MarketDataEngine::MarketDataEngine(Clock& clock)
    : clock_(clock)
    , md_queue_(std::make_unique<HPRingBuffer<MarketDataMessage, 16384>>())
{}

void MarketDataEngine::on_tob_update(const TopOfBook& tob) {
    Timestamp now = clock_.now();
    if (should_throttle(tob.symbol, now)) {
        messages_throttled_++;
        return;
    }

    MarketDataMessage msg;
    msg.type      = MDUpdateType::IncrementalRefresh;
    msg.symbol    = tob.symbol;
    msg.timestamp = now;
    msg.tob       = tob;
    (void)md_queue_->try_push(msg);
}

void MarketDataEngine::on_depth_update(const DepthSnapshot& depth) {
    Timestamp now = clock_.now();
    if (should_throttle(depth.symbol, now)) {
        messages_throttled_++;
        return;
    }

    MarketDataMessage msg;
    msg.type      = MDUpdateType::SnapshotFull;
    msg.symbol    = depth.symbol;
    msg.timestamp = now;
    msg.depth     = depth;
    (void)md_queue_->try_push(msg);
}

void MarketDataEngine::on_trade(const Trade& trade) {
    MarketDataMessage msg;
    msg.type      = MDUpdateType::TradeReport;
    msg.symbol    = trade.symbol;
    msg.timestamp = clock_.now();
    msg.trade     = trade;
    (void)md_queue_->try_push(msg);  // trades are never throttled
}

void MarketDataEngine::process_pending() {
    while (auto opt = md_queue_->try_pop()) {
        auto& msg = *opt;
        messages_published_++;

        switch (msg.type) {
        case MDUpdateType::IncrementalRefresh:
            for (auto& handler : l1_handlers_) {
                handler(msg.tob);
            }
            break;

        case MDUpdateType::SnapshotFull:
            for (auto& handler : l2_handlers_) {
                handler(msg.depth);
            }
            break;

        case MDUpdateType::TradeReport:
            for (auto& handler : trade_handlers_) {
                handler(msg.trade);
            }
            break;
        }
    }
}

bool MarketDataEngine::should_throttle(const Symbol& symbol, Timestamp now) {
    if (throttle_ns_ == 0) return false;

    auto it = last_publish_time_.find(symbol);
    if (it == last_publish_time_.end()) {
        last_publish_time_[symbol] = now;
        return false;
    }

    if (now - it->second < throttle_ns_) {
        return true;
    }

    it->second = now;
    return false;
}
