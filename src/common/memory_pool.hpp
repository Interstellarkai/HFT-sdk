#pragma once
// ============================================================================
// HFT-sdk — High-Performance Memory Pool
// Pre-allocated, zero-allocation-in-hot-path object pool.
// Lock-free O(1) allocate / deallocate with fixed capacity.
// ============================================================================

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace HFT_sdk {

/// Lock-free object pool with fixed capacity.
/// All memory is pre-allocated at construction time.
/// Allocation and deallocation are O(1) and lock-free.
template <typename T, std::size_t Capacity>
class MemoryPool {
  static_assert(std::is_trivially_destructible_v<T> ||
                    std::is_destructible_v<T>,
                "T must be destructible");

  struct alignas(64) Slot {
    alignas(alignof(T)) std::byte storage[sizeof(T)];
    std::uint32_t next_free;
    bool in_use;
  };

 public:
  MemoryPool() {
    // Build free list
    for (std::size_t i = 0; i < Capacity; ++i) {
      slots_[i].next_free = static_cast<std::uint32_t>(i + 1);
      slots_[i].in_use = false;
    }
    free_head_.store(0, std::memory_order_relaxed);
    allocated_.store(0, std::memory_order_relaxed);
  }

  ~MemoryPool() {
    // Destruct any in-use objects
    if constexpr (!std::is_trivially_destructible_v<T>) {
      for (std::size_t i = 0; i < Capacity; ++i) {
        if (slots_[i].in_use) {
          reinterpret_cast<T*>(slots_[i].storage)->~T();
        }
      }
    }
  }

  MemoryPool(const MemoryPool&) = delete;
  MemoryPool& operator=(const MemoryPool&) = delete;

  /// Allocate a slot and construct T in-place. Returns nullptr if full.
  template <typename... Args>
  T* allocate(Args&&... args) {
    std::uint32_t head = free_head_.load(std::memory_order_acquire);
    while (true) {
      if (head >= Capacity) return nullptr;  // pool exhausted
      std::uint32_t next = slots_[head].next_free;
      if (free_head_.compare_exchange_weak(head, next,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
        slots_[head].in_use = true;
        allocated_.fetch_add(1, std::memory_order_relaxed);
        return new (slots_[head].storage) T(std::forward<Args>(args)...);
      }
    }
  }

  /// Deallocate a previously allocated object.
  void deallocate(T* ptr) {
    if (!ptr) return;

    // Find the slot index
    // offset = (byte address of ptr) − (byte address of slots_[0])
    // idx    = offset / sizeof(Slot)
    auto* raw = reinterpret_cast<std::byte*>(ptr);
    std::size_t offset = static_cast<std::size_t>(
        raw - reinterpret_cast<std::byte*>(&slots_[0]));
    std::size_t idx = offset / sizeof(Slot);

    assert(idx < Capacity && "pointer does not belong to this pool");
    assert(slots_[idx].in_use && "double-free detected");

    if constexpr (!std::is_trivially_destructible_v<T>) {
      ptr->~T();
    }

    slots_[idx].in_use = false;
    std::uint32_t head = free_head_.load(std::memory_order_acquire);
    do {
      slots_[idx].next_free = head;
    } while (!free_head_.compare_exchange_weak(
        head, static_cast<std::uint32_t>(idx), std::memory_order_acq_rel,
        std::memory_order_acquire));
    allocated_.fetch_sub(1, std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t capacity() const { return Capacity; }
  [[nodiscard]] std::size_t allocated() const {
    return allocated_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::size_t available() const { return Capacity - allocated(); }

 private:
  std::array<Slot, Capacity> slots_;
  alignas(64) std::atomic<std::uint32_t> free_head_{0};
  alignas(64) std::atomic<std::size_t> allocated_{0};
};

}  // namespace HFT_sdk
