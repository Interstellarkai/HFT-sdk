// ============================================================================
// HFT-sdk — Limit Order Book Implementation
// ============================================================================

#include "order_book.h"

#include <algorithm>

using namespace HFT_sdk;

OrderBook::OrderBook( Symbol symbol ) : symbol_( std::move( symbol ) ) {}

void OrderBook::addOrder( const BookOrder& ord )
{
    if ( ord.side == Side::Buy )
    {
        bids_[ord.price].push_back( ord );
        order_index_[ord.id] = OrderLocation{ ord.side, ord.price, std::prev( bids_[ord.price].end() ) };
    }
    else
    {
        asks_[ord.price].push_back( ord );
        order_index_[ord.id] = OrderLocation{ ord.side, ord.price, std::prev( asks_[ord.price].end() ) };
    }
}

bool OrderBook::cancelOrder( OrderId id )
{
    auto it = order_index_.find( id );
    if ( it == order_index_.end() )
    {
        return false;
    }

    const auto& loc = it->second;
    if ( loc.side == Side::Buy )
    {
        auto& queue = bids_[loc.price];
        queue.erase( loc.it );
        if ( queue.empty() )
        {
            bids_.erase( loc.price );
        }
    }
    else
    {
        auto& queue = asks_[loc.price];
        queue.erase( loc.it );
        if ( queue.empty() )
        {
            asks_.erase( loc.price );
        }
    }

    order_index_.erase( it );
    return true;
}

std::pair<std::vector<Trade>*, Quantity> OrderBook::matchIncoming( const BookOrder& incoming, std::uint64_t ts_ns )
{
    auto* trades       = new std::vector<Trade>();
    Quantity remaining = incoming.qty;

    if ( incoming.side == Side::Buy )
    {
        // Match against asks (sell side)
        while ( remaining > 0 && !asks_.empty() )
        {
            auto& [price, queue] = *asks_.begin();
            if ( price > incoming.price && incoming.type == OrderType::Limit )
            {
                break;  // No more matches for limit order
            }

            while ( remaining > 0 && !queue.empty() )
            {
                auto& resting        = queue.front();
                Quantity matched_qty = std::min( remaining, resting.qty );

                Trade trade;
                trade.resting_id         = resting.id;
                trade.incoming_id        = incoming.id;
                trade.symbol             = symbol_;
                trade.price              = price;
                trade.qty                = matched_qty;
                trade.aggressor_side     = Side::Buy;
                trade.match_timestamp_ns = ts_ns;
                trades->push_back( trade );

                remaining -= matched_qty;
                resting.qty -= matched_qty;

                if ( resting.qty == 0 )
                {
                    order_index_.erase( resting.id );
                    queue.pop_front();
                }
            }

            if ( queue.empty() )
            {
                asks_.erase( asks_.begin() );
            }
        }
    }
    else
    {
        // Match against bids (buy side)
        while ( remaining > 0 && !bids_.empty() )
        {
            auto& [price, queue] = *bids_.begin();
            if ( price < incoming.price && incoming.type == OrderType::Limit )
            {
                break;  // No more matches for limit order
            }

            while ( remaining > 0 && !queue.empty() )
            {
                auto& resting        = queue.front();
                Quantity matched_qty = std::min( remaining, resting.qty );

                Trade trade;
                trade.resting_id         = resting.id;
                trade.incoming_id        = incoming.id;
                trade.symbol             = symbol_;
                trade.aggressor_side     = Side::Sell;
                trade.price              = price;
                trade.qty                = matched_qty;
                trade.match_timestamp_ns = ts_ns;
                trades->push_back( trade );

                remaining -= matched_qty;
                resting.qty -= matched_qty;

                if ( resting.qty == 0 )
                {
                    order_index_.erase( resting.id );
                    queue.pop_front();
                }
            }

            if ( queue.empty() )
            {
                bids_.erase( bids_.begin() );
            }
        }
    }

    return { trades, remaining };
}

std::optional<BookLevel> OrderBook::bestBid() const
{
    if ( bids_.empty() )
    {
        return std::nullopt;
    }
    const auto& [price, queue] = *bids_.begin();
    Quantity total_qty         = 0;
    for ( const auto& ord : queue )
    {
        total_qty += ord.qty;
    }
    return BookLevel{ price, total_qty };
}

std::optional<BookLevel> OrderBook::bestAsk() const
{
    if ( asks_.empty() )
    {
        return std::nullopt;
    }
    const auto& [price, queue] = *asks_.begin();
    Quantity total_qty         = 0;
    for ( const auto& ord : queue )
    {
        total_qty += ord.qty;
    }
    return BookLevel{ price, total_qty };
}

std::vector<BookLevel> OrderBook::bids( std::size_t depth ) const
{
    std::vector<BookLevel> result;
    result.reserve( depth );
    std::size_t count = 0;
    for ( const auto& [price, queue] : bids_ )
    {
        if ( count >= depth )
            break;
        Quantity total_qty = 0;
        for ( const auto& ord : queue )
        {
            total_qty += ord.qty;
        }
        result.push_back( BookLevel{ price, total_qty } );
        ++count;
    }
    return result;
}

std::vector<BookLevel> OrderBook::asks( std::size_t depth ) const
{
    std::vector<BookLevel> result;
    result.reserve( depth );
    std::size_t count = 0;
    for ( const auto& [price, queue] : asks_ )
    {
        if ( count >= depth )
            break;
        Quantity total_qty = 0;
        for ( const auto& ord : queue )
        {
            total_qty += ord.qty;
        }
        result.push_back( BookLevel{ price, total_qty } );
        ++count;
    }
    return result;
}
