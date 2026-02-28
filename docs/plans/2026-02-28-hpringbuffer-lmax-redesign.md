# HPRingBuffer LMAX-Inspired Redesign

## Problem

The current `HPRingBuffer` has three issues identified by comparing against the LMAX Disruptor:

1. **False sharing** — `head_` and `tail_` are adjacent `std::atomic<size_t>` members with no cache-line padding. Producer writes to `head_` invalidate the consumer's cached copy of `tail_` and vice versa, causing cache-line bouncing between cores.
2. **Wrapping index arithmetic** — `head_`/`tail_` wrap around via bitmask. This requires conditional logic in `size()` and is less intuitive than LMAX's monotonic sequences.
3. **Ambiguous naming** — `head_`/`tail_` doesn't clearly convey which is producer-owned vs consumer-owned.

Additionally, wait strategy logic (busy-spin, yield, block) is currently scattered across consumer call sites in `main.cpp` rather than encapsulated in the buffer.

## Design

### Approach: LMAX-Inspired SPSC Redesign

Adopt three changes from the LMAX Disruptor pattern, plus a wait strategy system:

### 1. Cache-Line Isolation

Pad `write_sequence_` and `read_sequence_` to separate 64-byte cache lines using `alignas(CACHE_LINE_SIZE)` from `constants.hpp`.

Memory layout:
```
cache line 0:  write_sequence_ (8B) + 56B padding   <- producer-only
cache line 1:  read_sequence_  (8B) + 56B padding   <- consumer-only
cache line 2+: buffer_[Size]                         <- data region
```

### 2. Monotonic Sequences

Replace wrapping head/tail indices with monotonically increasing sequence numbers. Array index derived via `seq & (Size - 1)`. This makes `size()` branch-free: `write_seq - read_seq`.

### 3. Naming

- `head_` -> `write_sequence_` (producer-owned)
- `tail_` -> `read_sequence_` (consumer-owned)
- `increment()` removed (replaced by `seq + 1`)

### 4. Wait Strategy Template Parameter

Three built-in strategies as a compile-time template parameter:

- `BusySpinWait` (default) — `_mm_pause()` on x86, `yield()` on ARM
- `YieldWait` — `std::this_thread::yield()`
- `BlockWait` — `std::condition_variable` based, lowest CPU

### Public API

```cpp
template<typename T, std::size_t Size, typename WaitStrategy = BusySpinWait>
class HPRingBuffer {
public:
    // Non-blocking
    [[nodiscard]] bool try_push(const T& item) noexcept;
    [[nodiscard]] bool try_push(T&& item) noexcept;
    [[nodiscard]] std::optional<T> try_pop() noexcept;

    // Blocking (spins using WaitStrategy)
    void push(const T& item) noexcept;
    void push(T&& item) noexcept;
    [[nodiscard]] T pop() noexcept;

    // Queries
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool full() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] static constexpr std::size_t capacity() noexcept;
};
```

### Breaking Changes

- Old `push()` returned `bool` -> new `push()` returns `void` (blocks until success)
- Old `pop()` returned `std::optional<T>` -> new `pop()` returns `T` (blocks until available)
- Non-blocking versions renamed to `try_push()` / `try_pop()`
- Callers that checked `push()` return value will get compile errors (intentional — forces decision)

### Files Changed

1. `src/HPRingBuffer.hpp` — full rewrite
2. `src/HPRINGBUFFER.md` — updated documentation

### Alternatives Considered

- **Minimal fix (Approach A)**: Only pad cache lines, keep wrapping indices. Rejected — monotonic sequences are strictly simpler.
- **Full Disruptor port (Approach C)**: Batch publishing, sequence barriers, multi-consumer fan-out. Rejected — YAGNI for SPSC use case.
