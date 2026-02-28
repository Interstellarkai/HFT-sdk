// ============================================================================
// HFT-sdk — L2 Aggregator Implementation
// ============================================================================

#include "l2_aggregator.hpp"

using namespace HFT_sdk;

L2Aggregator::L2Aggregator( const L3OrderBook& book, std::size_t depth ) : book_( book ), depth_( std::min( depth, MAX_L2_DEPTH ) ) {}

DepthSnapshot L2Aggregator::snapshot( Timestamp ts ) const
{
    DepthSnapshot snap{};
    snap.symbol    = book_.symbol();
    snap.timestamp = ts;

    auto bids = book_.bid_depth( depth_ );
    auto asks = book_.ask_depth( depth_ );

    snap.bid_levels = static_cast<std::uint32_t>( bids.size() );
    snap.ask_levels = static_cast<std::uint32_t>( asks.size() );

    for ( std::size_t i = 0; i < bids.size() && i < MAX_DEPTH_LEVELS; ++i )
    {
        snap.bids[i] = bids[i];
    }
    for ( std::size_t i = 0; i < asks.size() && i < MAX_DEPTH_LEVELS; ++i )
    {
        snap.asks[i] = asks[i];
    }

    return snap;
}

std::vector<BookLevel> L2Aggregator::bid_levels() const
{
    return book_.bid_depth( depth_ );
}

std::vector<BookLevel> L2Aggregator::ask_levels() const
{
    return book_.ask_depth( depth_ );
}
