#pragma once
// ============================================================================
// HFT-sdk — High-Performance Ring Buffer (HPRingBuffer)
// Lock-free SPSC (Single-Producer Single-Consumer) ring buffer inspired by
// the LMAX Disruptor. Uses monotonically increasing sequence numbers,
// cache-line-isolated atomics, and compile-time wait strategies.
// ============================================================================

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>

#include "common/constants.hpp"
#include "wait_strategies.hpp"

/// \brief High-Performance SPSC Ring Buffer (LMAX Disruptor-inspired)
///
/// \tparam T           Element type
/// \tparam Size        Number of slots (must be power of 2). Usable capacity is Size - 1.
/// \tparam WaitStrategy Compile-time policy for blocking push/pop spin behavior.
///                      Built-in: BusySpinWait (default), YieldWait, BlockWait.
template<typename T, std::size_t Size, typename WaitStrategy = BusySpinWait>
class HPRingBuffer {
    static_assert(Size > 1 && (Size & (Size - 1)) == 0,
                  "Size must be a power of two greater than 1");

    static constexpr std::size_t mask_ = Size - 1;

public:
    constexpr HPRingBuffer() noexcept = default;

    // ── Non-blocking API ────────────────────────────────────────────────

    /// Try to push an item. Returns false if buffer is full.
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const auto ws = write_sequence_.load(std::memory_order_relaxed);
        if (ws - read_sequence_.load(std::memory_order_acquire) >= Size - 1) [[unlikely]] {
            return false;  // Full
        }
        buffer_[ws & mask_] = item;
        write_sequence_.store(ws + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_push(T&& item) noexcept {
        const auto ws = write_sequence_.load(std::memory_order_relaxed);
        if (ws - read_sequence_.load(std::memory_order_acquire) >= Size - 1) [[unlikely]] {
            return false;  // Full
        }
        buffer_[ws & mask_] = std::move(item);
        write_sequence_.store(ws + 1, std::memory_order_release);
        return true;
    }

    /// Try to pop an item. Returns std::nullopt if buffer is empty.
    [[nodiscard]] std::optional<T> try_pop() noexcept {
        const auto rs = read_sequence_.load(std::memory_order_relaxed);
        if (rs == write_sequence_.load(std::memory_order_acquire)) [[unlikely]] {
            return std::nullopt;  // Empty
        }
        T value = std::move(buffer_[rs & mask_]);
        read_sequence_.store(rs + 1, std::memory_order_release);
        return value;
    }

    // ── Blocking API (spins using WaitStrategy) ─────────────────────────

    /// Push an item, spinning until a slot is available.
    void push(const T& item) noexcept {
        while (!try_push(item)) {
            wait_.wait();
        }
    }

    void push(T&& item) noexcept {
        // Cannot retry std::move in a loop — move once when slot available
        while (true) {
            const auto ws = write_sequence_.load(std::memory_order_relaxed);
            if (ws - read_sequence_.load(std::memory_order_acquire) < Size - 1) {
                buffer_[ws & mask_] = std::move(item);
                write_sequence_.store(ws + 1, std::memory_order_release);
                return;
            }
            wait_.wait();
        }
    }

    /// Pop an item, spinning until one is available.
    [[nodiscard]] T pop() noexcept {
        while (true) {
            const auto rs = read_sequence_.load(std::memory_order_relaxed);
            if (rs != write_sequence_.load(std::memory_order_acquire)) {
                T value = std::move(buffer_[rs & mask_]);
                read_sequence_.store(rs + 1, std::memory_order_release);
                return value;
            }
            wait_.wait();
        }
    }

    // ── Queries ─────────────────────────────────────────────────────────

    [[nodiscard]] bool empty() const noexcept {
        return write_sequence_.load(std::memory_order_acquire)
            == read_sequence_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool full() const noexcept {
        return (write_sequence_.load(std::memory_order_acquire)
              - read_sequence_.load(std::memory_order_acquire)) >= (Size - 1);
    }

    /// Number of items currently in the buffer. Branch-free with monotonic sequences.
    [[nodiscard]] std::size_t size() const noexcept {
        return write_sequence_.load(std::memory_order_acquire)
             - read_sequence_.load(std::memory_order_acquire);
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Size - 1;
    }

private:
    // ── Cache-line-isolated sequence counters ───────────────────────────
    // Each sequence lives on its own 64-byte cache line to prevent false
    // sharing between the producer (write_sequence_) and consumer (read_sequence_).
    alignas(HFT_sdk::CACHE_LINE_SIZE) std::atomic<std::size_t> write_sequence_{0};
    alignas(HFT_sdk::CACHE_LINE_SIZE) std::atomic<std::size_t> read_sequence_{0};

    // ── Data region ─────────────────────────────────────────────────────
    alignas(HFT_sdk::CACHE_LINE_SIZE) std::array<T, Size> buffer_{};

    // ── Wait strategy instance ──────────────────────────────────────────
    [[no_unique_address]] WaitStrategy wait_{};
};
