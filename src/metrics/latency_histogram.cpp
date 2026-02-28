// ============================================================================
// HFT-sdk — Latency Histogram (explicit template instantiations)
// ============================================================================

#include "latency_histogram.hpp"

// Template instantiation for common bucket configurations
namespace HFT_sdk {
    // Default: 10000 buckets * 100ns = 1ms max with 100ns resolution
    template class LatencyHistogram<10000, 100>;
    // Fine: 100000 buckets * 10ns = 1ms max with 10ns resolution
    template class LatencyHistogram<100000, 10>;
    // Coarse: 1000 buckets * 1000ns = 1ms max with 1µs resolution
    template class LatencyHistogram<1000, 1000>;
}
