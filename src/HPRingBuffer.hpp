#pragma once
// ============================================================================
// HFT-sdk — High-Performance Ring Buffer (HPRingBuffer)
// Lock-free SPSC (Single-Producer Single-Consumer) ring buffer with
// power-of-2 sizing for optimal modular arithmetic via bitmask.
// Designed for zero-allocation, cache-friendly inter-thread messaging.
// ============================================================================

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>


///
/// \brief High Performance Ring Buffer
///

template<typename T, std::size_t Size>
class HPRingBuffer {
    static_assert(Size && ((Size & (Size - 1)) == 0), "Size must be a power of two for optimal performance");

public:
    constexpr HPRingBuffer() noexcept : head_(0), tail_(0) {}

    [[nodiscard]] constexpr bool push(const T& item) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = increment(head);
        if (next == tail_.load(std::memory_order_acquire)) [[unlikely]] {
            return false;  // Full
        }

        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] constexpr bool push(T&& item) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = increment(head);
        if (next == tail_.load(std::memory_order_acquire)) [[unlikely]] {
            return false;  // Full
        }

        buffer_[head] = std::move(item);
        head_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] constexpr std::optional<T> pop() noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) [[unlikely]] {
            return std::nullopt;  // Empty
        }

        T value = std::move(buffer_[tail]);
        tail_.store(increment(tail), std::memory_order_release);
        return value;
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] constexpr bool full() const noexcept {
        const auto next = increment(head_.load(std::memory_order_relaxed));
        return next == tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        const auto h = head_.load(std::memory_order_acquire);
        const auto t = tail_.load(std::memory_order_acquire);
        return (h >= t) ? (h - t) : (Size - (t - h));
    }

    [[nodiscard]] constexpr std::size_t capacity() const noexcept {
        return Size - 1; // One slot is unused to distinguish full/empty
    }

private:
    std::array<T, Size> buffer_;
    std::atomic<std::size_t> head_;
    std::atomic<std::size_t> tail_;

    [[nodiscard]] constexpr std::size_t increment(std::size_t idx) const noexcept {
        return (idx + 1) & (Size - 1); // wrap-around using bitmask
    }
};
