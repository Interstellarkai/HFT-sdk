#pragma once
// ============================================================================
// HFT-sdk — L2 Aggregator
// Aggregates L3 order book into price-level depth snapshots
// (Market-by-Price) with configurable depth.
// ============================================================================

#include <algorithm>
#include <vector>

#include "common/constants.hpp"
#include "common/types.hpp"
#include "orderbook/l3_order_book.hpp"

namespace HFT_sdk
{

/// L2 Aggregator — produces aggregated depth snapshots from L3 book.
class L2Aggregator
{
public:
    explicit L2Aggregator( const L3OrderBook& book, std::size_t depth = DEFAULT_L2_DEPTH );

    /// Generate a full L2 depth snapshot.
    [[nodiscard]] DepthSnapshot snapshot( Timestamp ts ) const;

    /// Get aggregated bid levels.
    [[nodiscard]] std::vector<BookLevel> bid_levels() const;

    /// Get aggregated ask levels.
    [[nodiscard]] std::vector<BookLevel> ask_levels() const;

    void set_depth( std::size_t depth ) { depth_ = std::min( depth, MAX_L2_DEPTH ); }

    [[nodiscard]] std::size_t depth() const { return depth_; }

private:
    const L3OrderBook& book_;
    std::size_t depth_;
};

}  // namespace HFT_sdk
