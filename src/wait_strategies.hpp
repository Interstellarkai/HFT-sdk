#pragma once
// ============================================================================
// HFT-sdk — Wait Strategies for HPRingBuffer
// Compile-time policy types controlling how blocking push/pop spin.
// ============================================================================

#include <thread>
#include <condition_variable>
#include <mutex>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

/// Busy-spin wait: lowest latency, highest CPU usage.
/// Uses _mm_pause() on x86_64 to reduce pipeline stalls during spin.
/// Falls back to std::this_thread::yield() on ARM/other architectures.
struct BusySpinWait {
    static void wait() noexcept {
        #if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
        #else
            std::this_thread::yield();
        #endif
    }

    static void notify() noexcept {}  // no-op: no one to wake
};

/// Yield wait: moderate latency, moderate CPU usage.
/// Relinquishes the current timeslice to the OS scheduler.
struct YieldWait {
    static void wait() noexcept {
        std::this_thread::yield();
    }

    static void notify() noexcept {}  // no-op: no one to wake
};

/// Block wait: highest latency, lowest CPU usage.
/// Uses condition_variable. Suitable for non-latency-critical paths or testing.
/// Note: non-static because it holds a mutex + cv.
class BlockWait {
public:
    void wait() noexcept {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::microseconds(1));
    }

    void notify() noexcept {
        cv_.notify_one();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
};
