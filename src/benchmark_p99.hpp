#pragma once
// ============================================================================
// HFT-sdk — P99 / P99.9 Latency Benchmarking
// Measures callable latency over many iterations and computes
// p99 and p99.9 percentile timings in nanoseconds.
// ============================================================================

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <utility>

namespace hft_bench {

struct LatencyStats {
  std::int64_t p99_ns{};
  std::int64_t p999_ns{};
  std::size_t samples{};  // number of valid samples used
};

namespace detail {

// Clamp index to [0, size - 1]
constexpr std::size_t clamp_index(std::size_t idx, std::size_t size) noexcept {
  if (idx >= size) [[unlikely]] {
    return size ? (size - 1) : 0;
  }
  return idx;
}

}  // namespace detail

/// \brief Benchmark a callable and compute P99 and P99.9 latencies.
/// \tparam F    Callable type; must be invocable with Args...
/// \tparam Args Argument types
///
/// \param f                 Callable to benchmark; will be invoked as
/// f(args...).
/// \param args              Arguments forwarded to f on each iteration.
/// \param latencies_buffer  Span to store per-call latency (nanoseconds).
/// \param iterations        Number of measured iterations (<=
/// latencies_buffer.size()).
/// \param warmup_iterations Number of warmup calls (not measured).
///
/// \return LatencyStats containing p99, p99.9 in nanoseconds.
template <class F, class... Args>
  requires std::invocable<F&, Args...>
LatencyStats benchmark_p99(F&& f, Args&&... args,
                           std::span<std::int64_t> latencies_buffer,
                           std::size_t iterations,
                           std::size_t warmup_iterations = 0) {
  using clock = std::chrono::steady_clock;

  if (iterations > latencies_buffer.size()) {
    iterations = latencies_buffer.size();
  }

  // Warmup (no measurement)
  for (std::size_t i = 0; i < warmup_iterations; ++i) {
    // In HFT code you typically want the compiler to inline and
    // not optimize away the call; using std::invoke is fine here.
    std::invoke(f, args...);
  }

  // Measured iterations
  for (std::size_t i = 0; i < iterations; ++i) {
    auto t0 = clock::now();
    std::invoke(f, args...);
    auto t1 = clock::now();

    auto diff =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    latencies_buffer[i] = diff;
  }

  if (iterations == 0) {
    return LatencyStats{0, 0, 0};
  }

  // Sort in-place — caller owns the storage.
  auto data = latencies_buffer.first(iterations);
  std::ranges::sort(data);

  const std::size_t n = iterations;

  // Percentile indices using "nearest rank" style
  const auto idx_p99 =
      detail::clamp_index(static_cast<std::size_t>(0.99 * (n - 1)), n);
  const auto idx_p999 =
      detail::clamp_index(static_cast<std::size_t>(0.999 * (n - 1)), n);

  LatencyStats stats;
  stats.p99_ns = data[idx_p99];
  stats.p999_ns = data[idx_p999];
  stats.samples = n;

  return stats;
}

}  // namespace hft_bench
