#pragma once
// ============================================================================
// HFT-sdk — L3 Order Book
// Full order-level book with queue position tracking, iceberg support,
// FOK/IOC/GTC/Day time-in-force, and cancel-replace. Provides callbacks
// for trade, top-of-book, and depth events.
// ============================================================================

#include <algorithm>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "constants.h"
#include "types.h"

namespace HFT_sdk
{
/// Individual order entry in the L3 book.
struct alignas( 64 ) L3Order
{
    HFT_sdk::OrderId id           = HFT_sdk::INVALID_ORDER_ID;
    HFT_sdk::TraderId trader_id   = 0;
    HFT_sdk::Side side            = HFT_sdk::Side::Buy;
    HFT_sdk::Price price          = 0;
    HFT_sdk::Quantity visible_qty = 0;  // displayed quantity
    HFT_sdk::Quantity hidden_qty  = 0;  // iceberg hidden portion
    HFT_sdk::Quantity filled_qty  = 0;
    HFT_sdk::Timestamp entry_time = 0;
    std::uint32_t queue_pos          = 0;  // position in price-level queue
    bool is_iceberg                  = false;
    char cl_ord_id[20]               = {};
};

/// Aggregate info at a single price level.
struct PriceLevelInfo
{
    HFT_sdk::Price price            = 0;
    HFT_sdk::Quantity total_visible = 0;
    HFT_sdk::Quantity total_hidden  = 0;
    std::uint32_t order_count          = 0;
};

/// Result of a match operation.
struct MatchResult
{
    std::vector<HFT_sdk::Trade> trades;
    HFT_sdk::Quantity remaining_qty = 0;
    bool fully_filled                  = false;
};

/// L3 Order Book — price-time priority with full order-level granularity.
class L3OrderBook
{
public:
    // Callback types
    using TradeCallback     = std::function<void( const HFT_sdk::Trade& )>;
    using TopOfBookCallback = std::function<void( const HFT_sdk::TopOfBook& )>;
    using DepthCallback     = std::function<void(
        const HFT_sdk::Symbol&, const std::vector<HFT_sdk::BookLevel>& bids, const std::vector<HFT_sdk::BookLevel>& asks )>;
    explicit L3OrderBook( const HFT_sdk::Symbol& symbol );

    // ── Order operations ───────────────────────────────────────────────
    /// Submit a new order. Returns execution report.
    HFT_sdk::ExecutionReport add_order( const HFT_sdk::Order& order, HFT_sdk::Timestamp ts );

    /// Cancel an existing order.
    HFT_sdk::ExecutionReport cancel_order( HFT_sdk::OrderId id, HFT_sdk::Timestamp ts );

    /// Modify an existing order (cancel-replace).
    HFT_sdk::ExecutionReport replace_order( const HFT_sdk::ReplaceRequest& req, HFT_sdk::Timestamp ts );

    // ── Query ──────────────────────────────────────────────────────────
    [[nodiscard]] const HFT_sdk::Symbol& symbol() const { return symbol_; }

    [[nodiscard]] std::optional<HFT_sdk::BookLevel> best_bid() const;
    [[nodiscard]] std::optional<HFT_sdk::BookLevel> best_ask() const;
    [[nodiscard]] HFT_sdk::TopOfBook top_of_book( HFT_sdk::Timestamp ts ) const;

    [[nodiscard]] std::vector<HFT_sdk::BookLevel> bid_depth( std::size_t levels ) const;
    [[nodiscard]] std::vector<HFT_sdk::BookLevel> ask_depth( std::size_t levels ) const;

    /// Get L3 orders at a specific price level.
    [[nodiscard]] std::vector<L3Order> orders_at_price( HFT_sdk::Side side, HFT_sdk::Price price ) const;

    /// Queue position of a specific order.
    [[nodiscard]] std::uint32_t queue_position( HFT_sdk::OrderId id ) const;
    /// Total quantity ahead of an order in its price level.
    [[nodiscard]] HFT_sdk::Quantity quantity_ahead( HFT_sdk::OrderId id ) const;

    [[nodiscard]] std::size_t total_bid_orders() const { return bid_order_count_; }

    [[nodiscard]] std::size_t total_ask_orders() const { return ask_order_count_; }

    // ── Callbacks ──────────────────────────────────────────────────────
    void on_trade( TradeCallback cb ) { trade_cb_ = std::move( cb ); }

    void on_top_of_book( TopOfBookCallback cb ) { tob_cb_ = std::move( cb ); }

    void on_depth( DepthCallback cb ) { depth_cb_ = std::move( cb ); }

    // ── Statistics ─────────────────────────────────────────────────────
    [[nodiscard]] std::uint64_t total_trades() const { return trade_count_; }

    [[nodiscard]] HFT_sdk::Quantity total_volume() const { return total_volume_; }

    [[nodiscard]] HFT_sdk::Price last_trade_price() const { return last_trade_price_; }

private:
    HFT_sdk::Symbol symbol_;
    // ── Book data structures ───────────────────────────────────────────
    using OrderQueue     = std::list<L3Order>;
    using BidPriceLevels = std::map<HFT_sdk::Price, OrderQueue, std::greater<HFT_sdk::Price>>;
    using AskPriceLevels = std::map<HFT_sdk::Price, OrderQueue, std::less<HFT_sdk::Price>>;

    BidPriceLevels bids_;
    AskPriceLevels asks_;

    // O(1) order lookup: OrderId -> location in book
    struct OrderLocation
    {
        HFT_sdk::Side side;
        HFT_sdk::Price price;
        OrderQueue::iterator it;
    };

    std::unordered_map<HFT_sdk::OrderId, OrderLocation> order_index_;
    // ── Matching logic ─────────────────────────────────────────────────
    MatchResult match_incoming( const HFT_sdk::Order& order, HFT_sdk::Timestamp ts );

    template <typename PriceLevels>
    void match_against( PriceLevels& levels,
                        const HFT_sdk::Order& incoming,
                        HFT_sdk::Quantity& remaining,
                        std::vector<HFT_sdk::Trade>& trades,
                        HFT_sdk::Timestamp ts );

    void add_resting_order( const HFT_sdk::Order& order, HFT_sdk::Quantity remaining, HFT_sdk::Timestamp ts );
    void update_queue_positions( HFT_sdk::Side side, HFT_sdk::Price price );
    void publish_tob( HFT_sdk::Timestamp ts );
    // ── Counters ───────────────────────────────────────────────────────
    std::uint64_t trade_count_          = 0;
    std::uint64_t next_trade_id_        = 1;
    HFT_sdk::Quantity total_volume_  = 0;
    HFT_sdk::Price last_trade_price_ = 0;
    std::size_t bid_order_count_        = 0;
    std::size_t ask_order_count_        = 0;

    // ── Callbacks ──────────────────────────────────────────────────────
    TradeCallback trade_cb_;
    TopOfBookCallback tob_cb_;
    DepthCallback depth_cb_;
};

}  // namespace HFT_sdk
