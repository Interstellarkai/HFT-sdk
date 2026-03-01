#pragma once
// ============================================================================
// HFT-sdk — Latency Model
// Gaussian + heavy-tail latency simulation with per-client profiles.
// Models network inbound/outbound, matching engine processing, and
// market data dissemination latency. Seeded for deterministic replay.
// ============================================================================

#include <cmath>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "common/constants.hpp"
#include "common/types.hpp"

namespace HFT_sdk {

/// Latency profile for a single client or component.
struct LatencyProfile {
  double mean_ns = 5000.0;    // mean latency (ns)
  double stddev_ns = 1000.0;  // standard deviation (ns)
  double p99_ns = 30000.0;    // target p99 latency
  double p999_ns = 100000.0;  // target p999 latency
  double tail_prob = 0.01;    // probability of tail event
  double min_ns = 500.0;      // minimum latency floor
};

/// Latency components for a single event traversal.
struct LatencyBreakdown {
  std::uint64_t network_inbound_ns = 0;   // FIX gateway -> engine
  std::uint64_t matching_engine_ns = 0;   // matching engine processing
  std::uint64_t network_outbound_ns = 0;  // engine -> FIX client
  std::uint64_t md_dissemination_ns = 0;  // market data distribution
  std::uint64_t total_ns = 0;             // end-to-end
};

/// Latency Model — simulates realistic latency distributions.
class LatencyModel {
 public:
  /// Construct with a random seed for deterministic replay.
  explicit LatencyModel(std::uint64_t seed = 42);

  // ── Per-client latency profiles ────────────────────────────────────
  void set_client_profile(HFT_sdk::TraderId trader_id,
                          const LatencyProfile& profile);
  [[nodiscard]] const LatencyProfile& get_client_profile(
      HFT_sdk::TraderId trader_id) const;

  // ── Component-level profiles ───────────────────────────────────────
  void set_network_profile(const LatencyProfile& profile) {
    network_profile_ = profile;
  }

  void set_matching_profile(const LatencyProfile& profile) {
    matching_profile_ = profile;
  }

  void set_md_profile(const LatencyProfile& profile) { md_profile_ = profile; }

  // ── Sample latency ─────────────────────────────────────────────────
  /// Sample network latency for a specific client.
  [[nodiscard]] std::uint64_t sample_network_latency(
      HFT_sdk::TraderId trader_id);
  /// Sample matching engine processing latency.
  [[nodiscard]] std::uint64_t sample_matching_latency();

  /// Sample market data dissemination latency.
  [[nodiscard]] std::uint64_t sample_md_latency();

  /// Sample full round-trip latency breakdown for a client.
  [[nodiscard]] LatencyBreakdown sample_round_trip(HFT_sdk::TraderId trader_id);

  /// Sample from a specific profile.
  [[nodiscard]] std::uint64_t sample(const LatencyProfile& profile);

  // ── Jitter ─────────────────────────────────────────────────────────
  /// Add random jitter to a base latency.
  [[nodiscard]] std::uint64_t add_jitter(std::uint64_t base_ns,
                                         double jitter_fraction = 0.1);

  // ── Seed management ────────────────────────────────────────────────
  void set_seed(std::uint64_t seed);

  [[nodiscard]] std::uint64_t seed() const { return seed_; }

  // ── Statistics ─────────────────────────────────────────────────────
  [[nodiscard]] std::uint64_t samples_generated() const {
    return samples_generated_;
  }

 private:
  std::uint64_t seed_;
  std::mt19937_64 rng_;

  // Distributions
  std::normal_distribution<double> normal_dist_{0.0, 1.0};
  std::uniform_real_distribution<double> uniform_dist_{0.0, 1.0};
  std::exponential_distribution<double> tail_dist_{1.0};

  // Profiles
  LatencyProfile network_profile_;
  LatencyProfile matching_profile_;
  LatencyProfile md_profile_;
  LatencyProfile default_client_profile_;

  std::unordered_map<HFT_sdk::TraderId, LatencyProfile> client_profiles_;

  std::uint64_t samples_generated_ = 0;

  /// Generate a sample from Gaussian + heavy tail distribution.
  [[nodiscard]] double sample_gaussian_tail(const LatencyProfile& profile);
};

}  // namespace HFT_sdk
