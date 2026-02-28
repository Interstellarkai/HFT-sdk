// ============================================================================
// HFT-sdk — Clock Implementation
// ============================================================================

#include "clock.h"

namespace HFT_sdk {

Clock::Clock(Mode mode) : mode_(mode) {
    if (mode_ == Mode::Simulated) {
        simulated_time_.store(0, std::memory_order_relaxed);
    }
}

Timestamp Clock::now() const {
    if (mode_ == Mode::Simulated) {
        return simulated_time_.load(std::memory_order_acquire);
    }
    return wall_clock_now();
}

void Clock::advance(std::uint64_t delta_ns) {
    if (mode_ == Mode::Simulated) {
        simulated_time_.fetch_add(delta_ns, std::memory_order_acq_rel);
    }
}

void Clock::set_time(Timestamp ts) {
    if (mode_ == Mode::Simulated) {
        simulated_time_.store(ts, std::memory_order_release);
    }
}

void Clock::reset() {
    if (mode_ == Mode::Simulated) {
        simulated_time_.store(0, std::memory_order_release);
    }
}

Timestamp Clock::wall_clock_now() {
    auto tp = std::chrono::steady_clock::now();
    return static_cast<Timestamp>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            tp.time_since_epoch()).count());
}

} // namespace HFT_sdk
