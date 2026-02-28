// ============================================================================
// HFT-sdk — Telemetry Implementation
// ============================================================================

#include "telemetry.h"

using namespace HFT_sdk;

Telemetry::Telemetry( const Clock& clock ) : clock_( clock ) {}

Telemetry::~Telemetry() = default;

// ── Latency Recording ──────────────────────────────────────────────────────
void Telemetry::record_e2e_latency( std::int64_t ns )
{
    e2e_latency_.record( ns );
}

void Telemetry::record_matching_latency( std::int64_t ns )
{
    matching_latency_.record( ns );
}

void Telemetry::record_md_latency( std::int64_t ns )
{
    md_latency_.record( ns );
}

void Telemetry::record_fix_inbound_latency( std::int64_t ns )
{
    fix_in_latency_.record( ns );
}

void Telemetry::record_fix_outbound_latency( std::int64_t ns )
{
    fix_out_latency_.record( ns );
}

void Telemetry::record_fill( Quantity qty, Price price )
{
    total_fill_qty_.fetch_add( static_cast<std::uint64_t>( qty ), std::memory_order_relaxed );
    total_fill_notional_.fetch_add( static_cast<std::uint64_t>( qty * price ), std::memory_order_relaxed );
}

void Telemetry::record_queue_position( std::uint32_t pos )
{
    queue_pos_hist_.record( static_cast<std::int64_t>( pos ) );
}

void Telemetry::record_fill_probability( double prob )
{
    fill_prob_sum_.fetch_add( static_cast<std::int64_t>( prob * kFixedPointScale ),
                              std::memory_order_relaxed );
    fill_prob_count_.fetch_add( 1, std::memory_order_relaxed );
}

void Telemetry::record_slippage( double ticks )
{
    slippage_sum_.fetch_add( static_cast<std::int64_t>( ticks * kFixedPointScale ),
                             std::memory_order_relaxed );
    slippage_count_.fetch_add( 1, std::memory_order_relaxed );
}

// ── Timer Integration ──────────────────────────────────────────────────────
void Telemetry::start_timer( const std::string& name )
{
    std::lock_guard<std::mutex> lock( timer_mutex_ );
    active_timers_[name] = { std::chrono::steady_clock::now() };
}

void Telemetry::end_timer( const std::string& name )
{
    auto end = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock( timer_mutex_ );
    auto it = active_timers_.find( name );
    if ( it != active_timers_.end() )
    {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>( end - it->second.start ).count();
        // Route to appropriate histogram based on name
        if ( name.find( "e2e" ) != std::string::npos )
        {
            e2e_latency_.record( ns );
        }
        else if ( name.find( "match" ) != std::string::npos )
        {
            matching_latency_.record( ns );
        }
        else if ( name.find( "md" ) != std::string::npos )
        {
            md_latency_.record( ns );
        }
        active_timers_.erase( it );
    }
}

// ── Export ──────────────────────────────────────────────────────────────────
void Telemetry::print_dashboard() const
{
    auto e2e = e2e_latency_.stats();
    auto me  = matching_latency_.stats();
    auto md  = md_latency_.stats();

    std::cout << "\n╔══════════════════════════════════════════════════════════╗\n";
    std::cout << "║          HFT Exchange Simulator — Dashboard              ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════╣\n";

    std::cout << "║ THROUGHPUT                                               ║\n";
    std::cout << "║   Orders: " << std::setw( 12 ) << orders_.load() << "   Trades: " << std::setw( 12 ) << trades_.load() << "        ║\n";
    std::cout << "║   Cancels:" << std::setw( 12 ) << cancels_.load() << "   Rejects:" << std::setw( 12 ) << rejects_.load() << "        ║\n";

    std::cout << "╠══════════════════════════════════════════════════════════╣\n";
    std::cout << "║ LATENCY (ns)           p50        p99       p999        ║\n";
    std::cout << "║   End-to-end:   " << std::setw( 10 ) << e2e.p50_ns << std::setw( 11 ) << e2e.p99_ns << std::setw( 11 ) << e2e.p999_ns
              << "        ║\n";
    std::cout << "║   Matching:     " << std::setw( 10 ) << me.p50_ns << std::setw( 11 ) << me.p99_ns << std::setw( 11 ) << me.p999_ns
              << "        ║\n";
    std::cout << "║   Market Data:  " << std::setw( 10 ) << md.p50_ns << std::setw( 11 ) << md.p99_ns << std::setw( 11 ) << md.p999_ns
              << "        ║\n";

    std::cout << "╠══════════════════════════════════════════════════════════╣\n";
    std::cout << "║ FILL METRICS                                            ║\n";
    std::cout << "║   Volume:     " << std::setw( 14 ) << total_fill_qty_.load() << "                          ║\n";

    // Lock-free snapshot of atomic aggregates
    auto fp_count = fill_prob_count_.load( std::memory_order_relaxed );
    auto fp_sum   = fill_prob_sum_.load( std::memory_order_relaxed );
    auto sl_count = slippage_count_.load( std::memory_order_relaxed );
    auto sl_sum   = slippage_sum_.load( std::memory_order_relaxed );

    if ( fp_count > 0 )
    {
        double avg_fp = static_cast<double>( fp_sum ) / ( static_cast<double>( fp_count ) * kFixedPointScale );
        std::cout << "║   Avg Fill P: " << std::setw( 14 ) << std::fixed << std::setprecision( 4 )
                  << avg_fp << "                          ║\n";
    }
    if ( sl_count > 0 )
    {
        double avg_sl = static_cast<double>( sl_sum ) / ( static_cast<double>( sl_count ) * kFixedPointScale );
        std::cout << "║   Avg Slip:   " << std::setw( 14 ) << std::fixed << std::setprecision( 4 )
                  << avg_sl << " ticks                    ║\n";
    }

    std::cout << "╚══════════════════════════════════════════════════════════╝\n";
}

void Telemetry::print_latency_summary() const
{
    auto e2e = e2e_latency_.stats();
    auto me  = matching_latency_.stats();

    std::cout << "  E2E   — p50: " << e2e.p50_ns << " ns, p99: " << e2e.p99_ns << " ns, p999: " << e2e.p999_ns << " ns (" << e2e.samples
              << " samples)\n";
    std::cout << "  Match — p50: " << me.p50_ns << " ns, p99: " << me.p99_ns << " ns, p999: " << me.p999_ns << " ns (" << me.samples << " samples)\n";
}

void Telemetry::reset()
{
    e2e_latency_.reset();
    matching_latency_.reset();
    md_latency_.reset();
    fix_in_latency_.reset();
    fix_out_latency_.reset();
    queue_pos_hist_.reset();

    orders_.store( 0 );
    trades_.store( 0 );
    cancels_.store( 0 );
    rejects_.store( 0 );
    total_fill_qty_.store( 0 );
    total_fill_notional_.store( 0 );

    fill_prob_sum_.store( 0, std::memory_order_relaxed );
    fill_prob_count_.store( 0, std::memory_order_relaxed );
    slippage_sum_.store( 0, std::memory_order_relaxed );
    slippage_count_.store( 0, std::memory_order_relaxed );
}
