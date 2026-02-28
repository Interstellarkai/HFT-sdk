// ============================================================================
// HFT-sdk — L1 Feed Implementation
// ============================================================================

#include "l1_feed.hpp"

#include <numeric>

using namespace HFT_sdk;

L1Feed::L1Feed( const L3OrderBook& book ) : book_( book ) {}

TopOfBook L1Feed::update( Timestamp ts )
{
    last_tob_ = book_.top_of_book( ts );

    if ( last_tob_.valid && last_tob_.spread > 0 )
    {
        spread_history_.push_back( last_tob_.spread );
        if ( spread_history_.size() > MAX_SPREAD_HISTORY )
        {
            spread_history_.pop_front();
        }
    }

    if ( callback_ )
    {
        callback_( last_tob_ );
    }

    return last_tob_;
}

double L1Feed::vwap() const
{
    if ( recent_trades_.empty() )
        return 0.0;

    double sum_pv = 0.0;
    double sum_v  = 0.0;
    for ( auto& t : recent_trades_ )
    {
        sum_pv += static_cast<double>( t.price ) * static_cast<double>( t.quantity );
        sum_v += static_cast<double>( t.quantity );
    }
    return sum_v > 0.0 ? sum_pv / sum_v : 0.0;
}

double L1Feed::rolling_spread( std::size_t window ) const
{
    if ( spread_history_.empty() )
        return 0.0;
    std::size_t n = std::min( window, spread_history_.size() );
    double sum    = 0.0;
    auto it       = spread_history_.end();
    for ( std::size_t i = 0; i < n; ++i )
    {
        --it;
        sum += static_cast<double>( *it );
    }
    return sum / static_cast<double>( n );
}

void L1Feed::record_trade( Price price, Quantity qty )
{
    recent_trades_.push_back( { price, qty } );
    if ( recent_trades_.size() > MAX_TRADE_HISTORY )
    {
        recent_trades_.pop_front();
    }
}
