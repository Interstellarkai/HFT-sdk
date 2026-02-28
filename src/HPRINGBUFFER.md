# High-Performance Ring Buffer in C++

This project implements a **lock-free, low-latency ring buffer** in modern C++ with a focus on performance-critical systems such as high-frequency trading (HFT), real-time processing, and embedded systems.

## 🚀 Features

- **Lock-Free**: Designed for single-producer single-consumer (SPSC) communication with no mutexes or blocking.
- **Low Latency**: Uses relaxed atomics and cache-friendly data layout.
- **Modern C++**: Leverages C++20 features such as `std::atomic`, `constexpr`, `std::optional`, and `[[likely]]` / `[[unlikely]]`.
- **Cache Optimized**: Uses power-of-two sizing and bitmasking for fast index wrapping.
- **No Dependencies**: Pure C++ implementation with no external libraries.

## 🧪 Benchmark

The `main()` function benchmarks the time taken to push and pop 10,000,000 items using the ring buffer.

Sample output on a modern CPU:

Processed 10,000,000 items in 1015 ms

## 🛠 Build Instructions

### Prerequisites

- C++20-compatible compiler (e.g. GCC 10+, Clang 11+, MSVC 2019+)
- CMake (optional, for more structured builds)

### Build with g++

```bash
g++ -std=c++20 -O3 -o ring_buffer main.cpp -pthread
./ring_buffer
```

### 📚 Usage
Include and Instantiate
```bash
#include "HPRingBuffer.hpp"

HPRingBuffer<int, 1024> buffer;

buffer.push(42);
auto val = buffer.pop();
```

### 📝 License
This project is licensed under the MIT License — see the LICENSE file for details.

---

Let me know if you'd like a `CMakeLists.txt` or to tailor the README for a specific use case (e.g., trading, audio, embedded). I can also help create a diagram or performance graph for the repo.
