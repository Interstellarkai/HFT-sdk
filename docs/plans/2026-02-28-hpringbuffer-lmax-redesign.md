# HPRingBuffer LMAX-Inspired Redesign — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Redesign HPRingBuffer with LMAX Disruptor-inspired cache-line isolation, monotonic sequences, and compile-time wait strategies.

**Architecture:** Header-only SPSC ring buffer. Three changes: (1) pad write/read sequences to separate cache lines, (2) replace wrapping indices with monotonic sequences, (3) add WaitStrategy template parameter with blocking/non-blocking API split. One internal consumer (`market_data_engine.cpp`) needs updating to use `try_push`/`try_pop`.

**Tech Stack:** C++23, `std::atomic`, `alignas`, `_mm_pause` (x86), `std::condition_variable` (BlockWait)

---

## Task 1: Write Wait Strategy Types

**Files:**
- Create: `src/wait_strategies.hpp`

**Step 1: Create `src/wait_strategies.hpp` with all three strategies**

```cpp
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
```

**Step 2: Add `src/wait_strategies.hpp` to CMakeLists.txt**

In `CMakeLists.txt`, add `src/wait_strategies.hpp` to the `add_library(HFT-sdk STATIC ...)` source list, after the `src/HPRingBuffer.hpp` line.

**Step 3: Verify build**

Run:
```bash
cd /Users/limkaisheng/Projects/HFT-exchange/external/HFT-sdk
cmake -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug && cmake --build cmake-build-debug -j$(sysctl -n hw.ncpu)
```
Expected: clean build with no errors.

**Step 4: Commit**

```bash
git add src/wait_strategies.hpp CMakeLists.txt
git commit -m "feat: add wait strategy types for HPRingBuffer (BusySpinWait, YieldWait, BlockWait)"
```

---

## Task 2: Rewrite HPRingBuffer with LMAX-Inspired Design

**Files:**
- Modify: `src/HPRingBuffer.hpp` (full rewrite)

**Step 1: Replace entire contents of `src/HPRingBuffer.hpp`**

```cpp
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
        // Cannot retry std::move in a loop — copy first, then move once
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
```

**Step 2: Verify build**

Run:
```bash
cd /Users/limkaisheng/Projects/HFT-exchange/external/HFT-sdk
cmake --build cmake-build-debug -j$(sysctl -n hw.ncpu)
```
Expected: clean build. No errors.

**Step 3: Commit**

```bash
git add src/HPRingBuffer.hpp
git commit -m "feat: rewrite HPRingBuffer with LMAX-inspired monotonic sequences, cache-line isolation, and wait strategies"
```

---

## Task 3: Update Internal Consumer (MarketDataEngine)

**Files:**
- Modify: `src/market/market_data_engine.cpp:27,42,51,55`

The `MarketDataEngine` is a same-thread producer/consumer. It uses fire-and-forget `push()` (casting away the bool) and non-blocking drain via `pop()`. Both must switch to `try_push` / `try_pop`.

**Step 1: Update all push calls from `(void)md_queue_->push(msg)` to `(void)md_queue_->try_push(msg)`**

Three call sites:
- Line 27: `(void)md_queue_->push(msg);` → `(void)md_queue_->try_push(msg);`
- Line 42: `(void)md_queue_->push(msg);` → `(void)md_queue_->try_push(msg);`
- Line 51: `(void)md_queue_->push(msg);` → `(void)md_queue_->try_push(msg);`

**Step 2: Update the pop call from `md_queue_->pop()` to `md_queue_->try_pop()`**

- Line 55: `while (auto opt = md_queue_->pop())` → `while (auto opt = md_queue_->try_pop())`

**Step 3: Verify build**

Run:
```bash
cd /Users/limkaisheng/Projects/HFT-exchange/external/HFT-sdk
cmake --build cmake-build-debug -j$(sysctl -n hw.ncpu)
```
Expected: clean build. No errors.

**Step 4: Commit**

```bash
git add src/market/market_data_engine.cpp
git commit -m "refactor: update MarketDataEngine to use try_push/try_pop API"
```

---

## Task 4: Update Documentation (HPRINGBUFFER.md)

**Files:**
- Modify: `src/HPRINGBUFFER.md` (full rewrite)

**Step 1: Replace entire contents of `src/HPRINGBUFFER.md`**

```markdown
# HPRingBuffer — LMAX Disruptor-Inspired SPSC Ring Buffer

A **lock-free, cache-line-optimized** Single-Producer Single-Consumer (SPSC) ring buffer for C++23, inspired by the [LMAX Disruptor](https://lmax-exchange.github.io/disruptor/) pattern.

## Key Design Decisions

### Monotonic Sequences (not wrapping indices)

Like the LMAX Disruptor, `write_sequence_` and `read_sequence_` increase monotonically and never wrap. The array index is derived via bitmask: `seq & (Size - 1)`. This makes `size()` branch-free (`write_seq - read_seq`) and eliminates the ambiguity of wrapping head/tail pointers.

### Cache-Line Isolation (false-sharing elimination)

The producer's `write_sequence_` and consumer's `read_sequence_` are each aligned to a 64-byte cache line boundary via `alignas(CACHE_LINE_SIZE)`. This prevents false sharing — without padding, both atomics would share a cache line, causing MESI protocol invalidation traffic between cores on every write.

```
Memory layout:
┌─────────────────────────────────────────────────────┐
│ cache line 0:  write_sequence_ (8B) + 56B padding   │ ← producer-only
├─────────────────────────────────────────────────────┤
│ cache line 1:  read_sequence_  (8B) + 56B padding   │ ← consumer-only
├─────────────────────────────────────────────────────┤
│ cache line 2+: buffer_[Size]                        │ ← data region
└─────────────────────────────────────────────────────┘
```

### Compile-Time Wait Strategies

The third template parameter selects how blocking `push()`/`pop()` spin:

| Strategy | Latency | CPU | Use Case |
|---|---|---|---|
| `BusySpinWait` (default) | ~20-50ns | 100% core | HFT matching engine, market data |
| `YieldWait` | ~1-10µs | moderate | Execution report output, less critical paths |
| `BlockWait` | ~10-50µs | near-zero idle | Testing, development, non-latency-critical |

**Important:** Busy-spin means 100% CPU on that core at all times. This is the correct trade-off for HFT hot paths but should not be used for general-purpose applications.

## API

```cpp
#include "HPRingBuffer.hpp"

// Default: BusySpinWait
HPRingBuffer<int, 1024> buffer;

// Explicit wait strategy
HPRingBuffer<int, 1024, YieldWait> yielding_buffer;
HPRingBuffer<int, 1024, BlockWait> blocking_buffer;
```

### Non-Blocking (try)

```cpp
bool ok = buffer.try_push(42);       // returns false if full
auto val = buffer.try_pop();          // returns std::nullopt if empty
```

### Blocking (spins using WaitStrategy)

```cpp
buffer.push(42);       // spins until slot available, then writes
int val = buffer.pop(); // spins until item available, then returns
```

### Queries

```cpp
buffer.empty();     // true if no items
buffer.full();      // true if at capacity
buffer.size();      // current item count (branch-free)
buffer.capacity();  // Size - 1 (one slot reserved)
```

## Constraints

- `Size` must be a power of 2, greater than 1
- SPSC only — exactly one producer thread and one consumer thread
- `capacity()` is `Size - 1` (one slot is sacrificed to distinguish full from empty)

## Build

Requires C++23. Header-only — just include `HPRingBuffer.hpp`. Depends on `common/constants.hpp` (for `CACHE_LINE_SIZE`) and `wait_strategies.hpp`.

```bash
g++ -std=c++23 -O3 -o my_app my_app.cpp -I/path/to/HFT-sdk/src -pthread
```
```

**Step 2: Commit**

```bash
git add src/HPRINGBUFFER.md
git commit -m "docs: rewrite HPRINGBUFFER.md with LMAX-inspired design documentation"
```

---

## Task 5: Full Build Verification (SDK + Parent Project)

**Files:** None (verification only)

**Step 1: Clean rebuild of the SDK**

Run:
```bash
cd /Users/limkaisheng/Projects/HFT-exchange/external/HFT-sdk
rm -rf cmake-build-debug
cmake -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug && cmake --build cmake-build-debug -j$(sysctl -n hw.ncpu)
```
Expected: clean build, zero warnings related to HPRingBuffer.

**Step 2: Build the parent project (if possible)**

Run:
```bash
cd /Users/limkaisheng/Projects/HFT-exchange
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_TOOLCHAIN_FILE=~/.vcpkg-clion/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-osx \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  -DCMAKE_PREFIX_PATH=~/.vcpkg-clion/vcpkg/installed/x64-osx
cmake --build build -j$(sysctl -n hw.ncpu)
```
Expected: build errors in `main.cpp` where the old `push()`/`pop()` API is called. These are **expected** — the parent project (`main.cpp`) is outside the SDK submodule scope and must be updated separately. The compile errors serve as a forcing function to choose `try_push`/`try_pop` vs `push`/`pop` for each call site.

**Step 3: No commit — verification only**
