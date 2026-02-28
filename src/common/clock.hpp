#pragma once
// ============================================================================
// HFT-sdk — High-Resolution Clock
// Nanosecond-resolution clock supporting both real wall-clock time
// and deterministic simulated time for replay scenarios.
// ============================================================================

#include "types.hpp"
#include <chrono>
#include <cstdint>
#include <atomic>

namespace HFT_sdk {

/// Clock abstraction supporting both wall-clock and simulated time.
/// In simulation mode, time advances deterministically.
class Clock {
public:
    enum class Mode : std::uint8_t {
        WallClock,    // Real system time
        Simulated     // Manually advanced time for deterministic replay
    };

    explicit Clock(Mode mode = Mode::WallClock);

    /// Get current timestamp in nanoseconds.
    [[nodiscard]] Timestamp now() const;

    /// Advance simulated clock by delta_ns (only in Simulated mode).
    void advance(std::uint64_t delta_ns);

    /// Set simulated time to an absolute value.
    void set_time(Timestamp ts);

    /// Reset to zero (simulated mode).
    void reset();

    [[nodiscard]] Mode mode() const { return mode_; }

    /// Get steady-clock nanoseconds (always real time).
    [[nodiscard]] static Timestamp wall_clock_now();

private:
    Mode mode_;
    alignas(64) std::atomic<Timestamp> simulated_time_{0};
};

} // namespace HFT_sdk
