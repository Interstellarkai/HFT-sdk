// ============================================================================
// HFT-sdk — Latency Model Implementation
// ============================================================================

#include "latency_model.hpp"
#include <algorithm>

using namespace HFT_sdk;

LatencyModel::LatencyModel(std::uint64_t seed) : seed_(seed), rng_(seed) {
    // Default component profiles
    network_profile_  = {5000.0, 1000.0, 30000.0, 100000.0, 0.01, 500.0};
    matching_profile_ = {500.0,  100.0,  2000.0,  10000.0,  0.005, 100.0};
    md_profile_       = {2000.0, 500.0,  10000.0, 50000.0,  0.01, 200.0};
    default_client_profile_ = {5000.0, 1000.0, 30000.0, 100000.0, 0.01, 500.0};
}

void LatencyModel::set_client_profile(TraderId trader_id, const LatencyProfile& profile) {
    client_profiles_[trader_id] = profile;
}

const LatencyProfile& LatencyModel::get_client_profile(TraderId trader_id) const {
    auto it = client_profiles_.find(trader_id);
    if (it != client_profiles_.end()) return it->second;
    return default_client_profile_;
}

std::uint64_t LatencyModel::sample_network_latency(TraderId trader_id) {
    auto it = client_profiles_.find(trader_id);
    const auto& profile = (it != client_profiles_.end()) ? it->second : network_profile_;
    return sample(profile);
}

std::uint64_t LatencyModel::sample_matching_latency() {
    return sample(matching_profile_);
}

std::uint64_t LatencyModel::sample_md_latency() {
    return sample(md_profile_);
}

LatencyBreakdown LatencyModel::sample_round_trip(TraderId trader_id) {
    LatencyBreakdown breakdown;
    breakdown.network_inbound_ns  = sample_network_latency(trader_id);
    breakdown.matching_engine_ns  = sample_matching_latency();
    breakdown.network_outbound_ns = sample_network_latency(trader_id);
    breakdown.md_dissemination_ns = sample_md_latency();
    breakdown.total_ns = breakdown.network_inbound_ns +
                         breakdown.matching_engine_ns +
                         breakdown.network_outbound_ns;
    return breakdown;
}

std::uint64_t LatencyModel::sample(const LatencyProfile& profile) {
    samples_generated_++;
    double val = sample_gaussian_tail(profile);
    val = std::max(val, profile.min_ns);
    return static_cast<std::uint64_t>(val);
}

std::uint64_t LatencyModel::add_jitter(std::uint64_t base_ns, double jitter_fraction) {
    double jitter = normal_dist_(rng_) * static_cast<double>(base_ns) * jitter_fraction;
    double result = static_cast<double>(base_ns) + jitter;
    return static_cast<std::uint64_t>(std::max(result, 0.0));
}

void LatencyModel::set_seed(std::uint64_t seed) {
    seed_ = seed;
    rng_.seed(seed);
}

double LatencyModel::sample_gaussian_tail(const LatencyProfile& profile) {
    // With probability (1 - tail_prob), sample from Gaussian
    // With probability tail_prob, sample from heavy tail (exponential)
    double u = uniform_dist_(rng_);

    if (u > profile.tail_prob) {
        // Normal regime
        double z = normal_dist_(rng_);
        return profile.mean_ns + z * profile.stddev_ns;
    } else {
        // Tail regime — exponential distribution scaled to produce p99/p999 spikes
        double tail_scale = (profile.p99_ns - profile.mean_ns) / 2.3;  // ~p99 of exp
        double tail_val = tail_dist_(rng_) * tail_scale;
        return profile.mean_ns + tail_val;
    }
}
