// ============================================================================
// HFT-sdk — Matching Engine Implementation
// ============================================================================

#include "matching_engine.hpp"
#include <ScopeTimer.hpp>

#include <risk/risk_engine.hpp>

using namespace HFT_sdk;

MatchingEngine::MatchingEngine( Clock& clock ) : clock_( clock ) {}

// ── Symbol Management ──────────────────────────────────────────────────────
void MatchingEngine::add_symbol( const Symbol& symbol )
{
    if ( symbols_.count( symbol ) )
        return;

    auto& state = symbols_[symbol];
    state.book = std::make_unique<L3OrderBook>( symbol );
    state.l2 = std::make_unique<L2Aggregator>( *state.book );
    state.l1 = std::make_unique<L1Feed>( *state.book );

    // Wire up trade callback
    state.book->on_trade(
        [this]( const Trade& trade )
        {
            trades_generated_++;
            // Update L1 feed with trade data
            auto it = symbols_.find( trade.symbol );
            if ( it != symbols_.end() )
            {
                it->second.l1->record_trade( trade.price, trade.qty );
            }
            if ( trade_cb_ )
                trade_cb_( trade );
        } );

    // Wire up TOB callback
    state.book->on_top_of_book(
        [this]( const TopOfBook& tob )
        {
            if ( l1_cb_ )
                l1_cb_( tob );
        } );
}
void MatchingEngine::add_symbol(SymbolId symbol)
{
    add_symbol( Symbol( symbol.c_str() ) );
}

bool MatchingEngine::has_symbol( const Symbol& symbol ) const
{
    return symbols_.count( symbol ) > 0;
}

std::vector<Symbol> MatchingEngine::symbols() const
{
    std::vector<Symbol> result;
    result.reserve( symbols_.size() );
    for ( auto& [sym, state] : symbols_ )
    {
        result.push_back( sym );
    }
    return result;
}

// ── Order Processing ───────────────────────────────────────────────────────
ExecutionReport MatchingEngine::process_new_order( const Order& order )
{
    orders_processed_++;
    Timestamp ts = clock_.now();

    // Symbol validation
    auto it = symbols_.find( order.symbol );
    if ( it == symbols_.end() )
    {
        ExecutionReport rpt{};
        rpt.order_id = order.id;
        rpt.trader_id = order.trader_id;
        rpt.symbol = order.symbol;
        rpt.status = OrderStatus::Rejected;
        rpt.reject_reason = RejectReason::UnknownSymbol;
        rpt.transact_time = ts;
        rejects_++;
        if ( exec_cb_ )
            exec_cb_( rpt );
        return rpt;
    }

    // Risk checks
    if ( risk_engine_ )
    {
        auto reject = risk_engine_->check_order( order );
        if ( reject != RejectReason::None )
        {
            ExecutionReport rpt{};
            rpt.order_id = order.id;
            rpt.trader_id = order.trader_id;
            rpt.symbol = order.symbol;
            rpt.status = OrderStatus::Rejected;
            rpt.reject_reason = reject;
            rpt.transact_time = ts;
            rejects_++;
            if ( exec_cb_ )
                exec_cb_( rpt );
            return rpt;
        }
    }

    // Submit to order book
    auto rpt = it->second.book->add_order( order, ts );

    // Track order-to-symbol mapping for cancel routing
    if ( rpt.status != OrderStatus::Rejected && rpt.status != OrderStatus::Filled )
    {
        order_symbol_index_[order.id] = order.symbol;
    }

    // Update risk engine with execution
    if ( risk_engine_ && rpt.filled_qty > 0 )
    {
        risk_engine_->on_fill( order.trader_id, order.symbol, order.side, rpt.filled_qty, rpt.last_price );
    }

    // Publish
    if ( exec_cb_ )
        exec_cb_( rpt );
    publish_market_data( order.symbol );

    return rpt;
}

ExecutionReport MatchingEngine::process_cancel( const CancelRequest& cancel )
{
    Timestamp ts = clock_.now();

    // Route to correct symbol book
    Symbol symbol = cancel.symbol;
    if ( symbol == Symbol() )
    {
        // Lookup symbol from order index
        auto idx_it = order_symbol_index_.find( cancel.order_id );
        if ( idx_it == order_symbol_index_.end() )
        {
            ExecutionReport rpt{};
            rpt.order_id = cancel.order_id;
            rpt.status = OrderStatus::Rejected;
            rpt.transact_time = ts;
            return rpt;
        }
        symbol = idx_it->second;
    }

    auto it = symbols_.find( symbol );
    if ( it == symbols_.end() )
    {
        ExecutionReport rpt{};
        rpt.order_id = cancel.order_id;
        rpt.status = OrderStatus::Rejected;
        rpt.reject_reason = RejectReason::UnknownSymbol;
        rpt.transact_time = ts;
        return rpt;
    }

    auto rpt = it->second.book->cancel_order( cancel.order_id, ts );
    if ( rpt.status == OrderStatus::Canceled )
    {
        cancels_processed_++;
        order_symbol_index_.erase( cancel.order_id );
    }

    if ( exec_cb_ )
        exec_cb_( rpt );
    publish_market_data( symbol );

    return rpt;
}

ExecutionReport MatchingEngine::process_replace( const ReplaceRequest& replace )
{
    Timestamp ts = clock_.now();

    auto it = symbols_.find( replace.symbol );
    if ( it == symbols_.end() )
    {
        ExecutionReport rpt{};
        rpt.order_id = replace.order_id;
        rpt.status = OrderStatus::Rejected;
        rpt.reject_reason = RejectReason::UnknownSymbol;
        rpt.transact_time = ts;
        return rpt;
    }

    auto rpt = it->second.book->replace_order( replace, ts );

    if ( exec_cb_ )
        exec_cb_( rpt );
    publish_market_data( replace.symbol );

    return rpt;
}



// ── Market Data ────────────────────────────────────────────────────────────
TopOfBook MatchingEngine::get_top_of_book( const Symbol& symbol ) const
{
    auto it = symbols_.find( symbol );
    if ( it == symbols_.end() )
        return {};
    return it->second.l1->last();
}

DepthSnapshot MatchingEngine::get_depth( const Symbol& symbol ) const
{
    auto it = symbols_.find( symbol );
    if ( it == symbols_.end() )
        return {};
    return it->second.l2->snapshot( clock_.now() );
}

const L3OrderBook* MatchingEngine::get_book( const Symbol& symbol ) const
{
    auto it = symbols_.find( symbol );
    if ( it == symbols_.end() )
        return nullptr;
    return it->second.book.get();
}

void MatchingEngine::publish_market_data( const Symbol& symbol )
{
    auto it = symbols_.find( symbol );
    if ( it == symbols_.end() )
        return;

    Timestamp ts = clock_.now();

    // Update L1
    it->second.l1->update( ts );

    // Publish L2 if callback registered
    if ( l2_cb_ )
    {
        auto depth = it->second.l2->snapshot( ts );
        l2_cb_( depth );
    }
}
