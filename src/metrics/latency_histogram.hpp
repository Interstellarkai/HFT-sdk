#pragma once
// ============================================================================
// HFT-sdk — Latency Histogram
// Fixed-bucket histogram for O(1) recording and percentile computation
// (p50/p90/p95/p99/p999). No heap allocation on the recording path.
// ============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace HFT_sdk {

/// Latency statistics output.
struct LatencyStats {
  std::int64_t min_ns = 0;
  std::int64_t max_ns = 0;
  std::int64_t mean_ns = 0;
  std::int64_t median_ns = 0;
  std::int64_t p50_ns = 0;
  std::int64_t p90_ns = 0;
  std::int64_t p95_ns = 0;
  std::int64_t p99_ns = 0;
  std::int64_t p999_ns = 0;
  std::size_t samples = 0;
};

/// Fixed-size latency histogram (no heap allocation in recording path).
/// Buckets cover 0 to MAX_LATENCY_NS in BUCKET_WIDTH_NS increments.
template <std::size_t NUM_BUCKETS = 10000, std::int64_t BUCKET_WIDTH_NS = 100>
class LatencyHistogram {
 public:
  static constexpr std::int64_t MAX_LATENCY_NS =
      static_cast<std::int64_t>(NUM_BUCKETS) * BUCKET_WIDTH_NS;

  LatencyHistogram() { reset(); }

  /// Record a latency sample. O(1), no allocation.
  void record(std::int64_t latency_ns) {
    total_samples_++;
    sum_ns_ += latency_ns;

    if (latency_ns < min_ns_) min_ns_ = latency_ns;
    if (latency_ns > max_ns_) max_ns_ = latency_ns;

    // Clamp to histogram range
    if (latency_ns < 0) latency_ns = 0;
    std::size_t bucket = static_cast<std::size_t>(latency_ns / BUCKET_WIDTH_NS);
    if (bucket >= NUM_BUCKETS) bucket = NUM_BUCKETS - 1;
    buckets_[bucket]++;
  }

  /// Compute percentile statistics.
  [[nodiscard]] LatencyStats stats() const {
    LatencyStats s;
    s.samples = total_samples_;
    s.min_ns = min_ns_;
    s.max_ns = max_ns_;
    s.mean_ns = (total_samples_ > 0)
                    ? static_cast<std::int64_t>(
                          sum_ns_ / static_cast<double>(total_samples_))
                    : 0;
    s.p50_ns = percentile(0.50);
    s.median_ns = s.p50_ns;
    s.p90_ns = percentile(0.90);
    s.p95_ns = percentile(0.95);
    s.p99_ns = percentile(0.99);
    s.p999_ns = percentile(0.999);
    return s;
  }

  /// Get a specific percentile value.
  [[nodiscard]] std::int64_t percentile(double p) const {
    if (total_samples_ == 0) return 0;

    std::size_t target = static_cast<std::size_t>(
        std::ceil(static_cast<double>(total_samples_) * p));
    if (target == 0) target = 1;

    std::size_t cumulative = 0;
    for (std::size_t i = 0; i < NUM_BUCKETS; ++i) {
      cumulative += buckets_[i];
      if (cumulative >= target) {
        return static_cast<std::int64_t>(i) * BUCKET_WIDTH_NS +
               BUCKET_WIDTH_NS / 2;
      }
    }
    return MAX_LATENCY_NS;
  }

  void reset() {
    buckets_.fill(0);
    total_samples_ = 0;
    sum_ns_ = 0.0;
    min_ns_ = std::numeric_limits<std::int64_t>::max();
    max_ns_ = std::numeric_limits<std::int64_t>::min();
  }

  [[nodiscard]] std::size_t total_samples() const { return total_samples_; }

 private:
  std::array<std::size_t, NUM_BUCKETS> buckets_;
  std::size_t total_samples_ = 0;
  double sum_ns_ = 0.0;
  std::int64_t min_ns_ = std::numeric_limits<std::int64_t>::max();
  std::int64_t max_ns_ = std::numeric_limits<std::int64_t>::min();
};

}  // namespace HFT_sdk
