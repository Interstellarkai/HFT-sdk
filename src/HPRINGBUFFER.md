# HPRingBuffer — LMAX Disruptor-Inspired SPSC Ring Buffer

A **lock-free, cache-line-optimized** Single-Producer Single-Consumer (SPSC) ring buffer for C++23, inspired by
the [LMAX Disruptor](https://lmax-exchange.github.io/disruptor/) pattern.

## Key Design Decisions

### Monotonic Sequences (not wrapping indices)

Like the LMAX Disruptor, `write_sequence_` and `read_sequence_` increase monotonically and never wrap. The array index
is derived via bitmask: `seq & (Size - 1)`. This makes `size()` branch-free (`write_seq - read_seq`) and eliminates the
ambiguity of wrapping head/tail pointers.

### Cache-Line Isolation (false-sharing elimination)

The producer's `write_sequence_` and consumer's `read_sequence_` are each aligned to a 64-byte cache line boundary via
`alignas(CACHE_LINE_SIZE)`. This prevents false sharing — without padding, both atomics would share a cache line,
causing MESI protocol invalidation traffic between cores on every write.

```
Memory layout:
+-----------------------------------------------------+
| cache line 0:  write_sequence_ (8B) + 56B padding   | <- producer-only
+-----------------------------------------------------+
| cache line 1:  read_sequence_  (8B) + 56B padding   | <- consumer-only
+-----------------------------------------------------+
| cache line 2+: buffer_[Size]                         | <- data region
+-----------------------------------------------------+
```

### Compile-Time Wait Strategies

The third template parameter selects how blocking `push()`/`pop()` spin:

| Strategy                 | Latency  | CPU            | Use Case                                     |
|--------------------------|----------|----------------|----------------------------------------------|
| `BusySpinWait` (default) | ~20-50ns | 100% core      | HFT matching engine, market data             |
| `YieldWait`              | ~1-10us  | moderate       | Execution report output, less critical paths |
| `BlockWait`              | ~10-50us | near-zero idle | Testing, development, non-latency-critical   |

**Important:** Busy-spin means 100% CPU on that core at all times. This is the correct trade-off for HFT hot paths but
should not be used for general-purpose applications.

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

Requires C++23. Header-only — just include `HPRingBuffer.hpp`. Depends on `common/constants.hpp` (for `CACHE_LINE_SIZE`)
and `wait_strategies.hpp`.

```bash
g++ -std=c++23 -O3 -o my_app my_app.cpp -I/path/to/HFT-sdk/src -pthread
```

## License

This project is licensed under the MIT License — see the LICENSE file for details.
