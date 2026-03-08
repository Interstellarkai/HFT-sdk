# BENCHMARK.md Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Write `src/BENCHMARK.md` — a learner-friendly, top-to-bottom doc for `benchmark_p99.hpp` that builds intuition progressively for C++ devs new to HFT latency measurement patterns.

**Architecture:** Single markdown file at `src/BENCHMARK.md`, alongside `src/HPRINGBUFFER.md` and `src/common/MEMORYPOOL.md`. Style mirrors `MEMORYPOOL.md` exactly: opens with the problem, explains *why* before *how*, uses concrete analogies and ASCII diagrams. No code changes to `benchmark_p99.hpp`.

**Tech Stack:** Markdown, `src/benchmark_p99.hpp` (read-only reference), C++23

---

## Before You Start

Read these files to understand context and style target:
- `src/benchmark_p99.hpp` — the file being documented
- `src/common/MEMORYPOOL.md` — the style reference (structure, tone, depth)
- `src/HPRINGBUFFER.md` — second style reference
- `docs/plans/2026-03-07-benchmark-p99-doc-design.md` — the approved design doc

---

## Task 1: Write Section 1 — The Problem

**Files:**
- Create: `src/BENCHMARK.md`

**Step 1: Create the file with the opening section**

Write `src/BENCHMARK.md` with this exact content for Section 1:

```markdown
# Benchmarking Latency — `benchmark_p99`

A utility for measuring **P99 and P99.9 tail latency** of any callable in nanoseconds.

If you've never worked in HFT before, start here — this document builds up from *why* this
exists before diving into *how* it works.

---

## The Problem: Why Measuring Latency is Hard

In most applications, you'd measure performance by running a function a few times and reporting
the average. In HFT, that's actively misleading.

Suppose your order-placement function runs 999 times in 2 µs, then once takes 500 µs because
the OS scheduler preempted your thread. The average across 1000 calls: **2.5 µs**. That looks
fine. But that one 500 µs call caused a missed fill — the market moved while you were waiting.

The average tells you about the *typical* case. HFT cares about the *worst* case.

> **Hot path**: any code in the critical trading loop, called millions of times per second. A
> single unexpected spike here can mean the difference between filling an order and missing it.

### Percentiles: Measuring the Tail

Instead of averaging, we sort all measurements and look at specific positions in the sorted
list:

- **P99** — the 99th percentile: 99% of calls were *faster* than this. 1 in 100 calls was
  this slow or slower.
- **P99.9** — the 99.9th percentile: 999 out of 1000 calls were faster. The worst 1 in 1000.

Think of it like lap times in a race. Averaging in the lap where the car pitted for tyres tells
you nothing useful about race pace. The interesting question is: *how bad does it get when
things go wrong?* Percentiles answer that directly.

On an exchange processing 1 million orders per second, a 1-in-1000 spike happens 1000 times
every second. P99.9 is not a theoretical worst case — it's the steady-state tail you must
engineer for.

---
```

**Step 2: Verify the file exists and renders correctly**

Open `src/BENCHMARK.md` and read it. Confirm it starts correctly and matches the style of `MEMORYPOOL.md`.

**Step 3: Commit**

```bash
git add src/BENCHMARK.md
git commit -m "docs: add BENCHMARK.md Section 1 — the latency measurement problem"
```

---

## Task 2: Write Section 2 — Why `std::span`

**Files:**
- Modify: `src/BENCHMARK.md` (append)

**Step 1: Append Section 2 to `src/BENCHMARK.md`**

```markdown
## Why `std::span`, Not a Vector or Raw Pointer

`benchmark_p99` takes a `std::span<std::int64_t>` to store per-call latency measurements.
This is a deliberate design choice — here's why each alternative was rejected:

**`std::vector<int64_t>&`** — forces the caller to heap-allocate a vector before calling. In
a benchmarking context this is usually fine, but it means the function's interface requires
heap memory even if the caller has a perfectly good stack array sitting there. It also
obscures ownership: does the function clear it? Resize it? Append to it?

**`int64_t* buf, size_t len`** — a raw pointer pair is what `std::span` compiles down to, but
with no type safety. Nothing stops you from passing the wrong size, or a pointer to the wrong
buffer. You've also lost IDE autocomplete and static analysis hints.

**`std::span<int64_t>`** — a non-owning *view* into contiguous memory you already own. The
caller decides where the memory lives:

```cpp
// Stack-allocated — zero heap involvement
std::array<std::int64_t, 10'000> buf;
auto stats = hft_bench::benchmark_p99(my_fn, std::span{buf}, 10'000, 200);

// Or heap, if you need a very large buffer
std::vector<std::int64_t> buf(1'000'000);
auto stats = hft_bench::benchmark_p99(my_fn, std::span{buf}, 1'000'000, 1000);
```

The function borrows the buffer, writes measurements into it, sorts it in-place, and returns.
The caller retains ownership throughout. This is the idiomatic C++20 pattern for passing
contiguous data without dictating where it lives.

> **Key pattern:** the caller provides storage, the function borrows it. This separates
> *allocation policy* (the caller's concern) from *algorithm* (the function's concern).

Note that `benchmark_p99` sorts the buffer in-place as a side effect — the caller's array
contains sorted latency values after the call returns, which can be useful for further
analysis.

---
```

**Step 2: Commit**

```bash
git add src/BENCHMARK.md
git commit -m "docs: add BENCHMARK.md Section 2 — why std::span"
```

---

## Task 3: Write Section 3 — Warmup

**Files:**
- Modify: `src/BENCHMARK.md` (append)

**Step 1: Append Section 3 to `src/BENCHMARK.md`**

```markdown
## Warmup: Teaching the CPU to Care

`benchmark_p99` accepts a `warmup_iterations` parameter (default: 0). These calls run your
function but discard the timings. Why would you deliberately throw away measurements?

Because the first few calls to any function are physically slower than subsequent ones — for
three distinct hardware reasons:

### 1. CPU Frequency Scaling (P-states and C-states)

Modern CPUs don't run at their maximum clock frequency all the time. When a core is idle or
lightly loaded, the hardware automatically drops it to a lower-power state to save energy.
When work arrives, it ramps back up — but this takes time, often 1–5 milliseconds.

If your benchmark starts the clock immediately, the first calls are measured at a lower clock
speed than steady state. The latency numbers look worse than they really are.

Warmup gives the CPU enough work to reach its target frequency before you start measuring.

### 2. Instruction Cache Cold Start

Your function's compiled machine code needs to be loaded from memory into the CPU's L1
instruction cache before it can execute at full speed. On the first call:

```
Call f()
  → L1-I cache miss
  → fetch from L2 (4–10 cycles)
  → or L3 (30–40 cycles)
  → or RAM (200–300 cycles)
```

On the second call, the code is in L1-I. Execution begins immediately. The difference can be
hundreds of nanoseconds on the first call alone.

### 3. Data Cache Warming

The same principle applies to data. Your callable's internal state — lookup tables, member
variables, the latency buffer you passed in — starts cold. First access fetches from L2/L3/RAM.
After a few calls, it's hot in L1-D.

### How Many Warmup Iterations?

Enough to reach steady state, but not so many that you waste time. A few hundred iterations
is usually sufficient to warm the instruction cache and trigger CPU frequency ramp-up. If
your function accesses a large working set (more than ~32 KB of data), you may need more.

```cpp
// 200 warmup calls, then 10,000 measured
auto stats = hft_bench::benchmark_p99(my_fn, std::span{buf}, 10'000, 200);
```

If you omit warmup (default 0), the first few measured calls will be artificially slow. Your
P99 may be dominated by cold-start effects rather than true tail latency.

---
```

**Step 2: Commit**

```bash
git add src/BENCHMARK.md
git commit -m "docs: add BENCHMARK.md Section 3 — warmup and cold-start effects"
```

---

## Task 4: Write Section 4 — The Measurement Loop and Clock Overhead

**Files:**
- Modify: `src/BENCHMARK.md` (append)

**Step 1: Append Section 4 to `src/BENCHMARK.md`**

```markdown
## The Measurement Loop and Clock Overhead

The core loop in `benchmark_p99` looks like this:

```cpp
auto t0 = clock::now();
std::invoke(f, args...);
auto t1 = clock::now();
latencies_buffer[i] = duration_cast<nanoseconds>(t1 - t0).count();
```

### Why `steady_clock`, Not `system_clock`?

`std::chrono::system_clock` represents wall-clock time — the kind your computer displays. It
can jump backwards when NTP (Network Time Protocol) adjusts the clock, or skip forward during
daylight saving changes. A negative latency measurement would corrupt your entire dataset.

`std::chrono::steady_clock` is monotonic: it never goes backwards. Each call to `now()`
returns a value greater than or equal to the previous one. It's the correct choice for
measuring elapsed time.

### The Overhead Problem

`clock::now()` is not free. On most hardware it costs roughly 20–30 nanoseconds — it must
read a hardware counter and apply calibration. This overhead wraps every measurement:

```
measured latency = true latency of f() + overhead of two clock::now() calls
```

For a function that takes 10 µs (10,000 ns), this overhead is negligible (< 0.3%). For a
function that takes 50 ns — typical for a ring buffer push or a hash table lookup — the
overhead is 60–120% of the measured value. Your p99 is measuring the timer as much as the
function.

HFT practitioners working at this scale often switch to `rdtsc` — the x86 "read timestamp
counter" instruction, which reads directly from the CPU's cycle counter and costs ~3–5 cycles
(< 2 ns at 3 GHz). `benchmark_p99` uses `steady_clock` instead for portability and
correctness: `rdtsc` requires serializing instructions (`cpuid` or `lfence`) to prevent the
CPU from reordering it past your function call, and the cycle-to-nanosecond conversion needs
careful calibration per machine.

> Use `benchmark_p99` for functions with latencies above ~200 ns. For sub-100 ns work,
> consider `rdtsc`-based tooling or ensure you're measuring aggregate throughput instead.

---
```

**Step 2: Commit**

```bash
git add src/BENCHMARK.md
git commit -m "docs: add BENCHMARK.md Section 4 — measurement loop and clock overhead"
```

---

## Task 5: Write Section 5 — `clamp_index` and Internal Invariants

**Files:**
- Modify: `src/BENCHMARK.md` (append)

**Step 1: Append Section 5 to `src/BENCHMARK.md`**

```markdown
## `clamp_index`: Defensive Code and Internal Invariants

After sorting the measurements, `benchmark_p99` computes the percentile indices like this:

```cpp
const auto idx_p99 =
    detail::clamp_index(static_cast<std::size_t>(0.99 * (n - 1)), n);
```

### The Hazard: Unsigned Integer Underflow

`std::size_t` is an *unsigned* integer type. Unsigned integers in C++ do not go negative —
they wrap around. If `n = 0`, then:

```
n - 1 = 0 - 1 = 18,446,744,073,709,551,615  // SIZE_MAX
0.99 * SIZE_MAX ≈ 18,262,276,632,973,355,008  // massively out-of-bounds
```

Indexing into a sorted buffer with that value is undefined behavior — likely a crash.
`clamp_index` catches this:

```cpp
constexpr std::size_t clamp_index(std::size_t idx, std::size_t size) noexcept {
  if (idx >= size) [[unlikely]] {   // [[unlikely]]: optimize the normal path
    return size ? (size - 1) : 0;
  }
  return idx;
}
```

The `[[unlikely]]` attribute is a branch prediction hint. It tells the compiler (and through
it, the CPU's branch predictor) that the `if` body almost never executes. The processor
pre-fetches and speculatively executes the `return idx` path. When the hint is correct —
which it almost always is — there's zero branch misprediction penalty.

### The `detail::` Namespace

`clamp_index` lives in `hft_bench::detail`. This is idiomatic C++ for "internal
implementation — not part of the public API." The `detail` namespace is a convention used
throughout the standard library and major C++ projects. Nothing technically prevents a caller
from writing `hft_bench::detail::clamp_index(...)`, but the namespace signals: *don't depend
on this, it may change.*

### Belt and Suspenders

Here's the interesting part: `clamp_index` is *technically unreachable* in the dangerous case.

Look at the function before the percentile calculation:

```cpp
if (iterations == 0) {
  return LatencyStats{0, 0, 0};  // early return — never reaches clamp_index
}
// ...
const std::size_t n = iterations;  // n >= 1 guaranteed here
```

By the time `clamp_index` runs, `n >= 1` is guaranteed by the early return. The unsigned
underflow path cannot be triggered. `clamp_index` is a *belt-and-suspenders* guard — added
defensively, correct in isolation, but made redundant by the guard above.

This is a common pattern in codebases with multiple authors or a long history. Each guard is
locally correct. Together, they're over-defensive. Neither is wrong — but it's worth
recognising when you see it.

### A Note on the Return Statement

The final four lines assembling the result:

```cpp
LatencyStats stats;
stats.p99_ns  = data[idx_p99];
stats.p999_ns = data[idx_p999];
stats.samples = n;
return stats;
```

...could be written more compactly using C++20 designated initializers:

```cpp
return LatencyStats{.p99_ns = data[idx_p99], .p999_ns = data[idx_p999], .samples = n};
```

Both are correct. The compact form is cleaner; the expanded form is explicit about each field.
Designated initializers are self-documenting (you can't accidentally swap `p99_ns` and
`p999_ns`) and don't depend on knowing the struct's field declaration order.

---
```

**Step 2: Commit**

```bash
git add src/BENCHMARK.md
git commit -m "docs: add BENCHMARK.md Section 5 — clamp_index, detail namespace, belt-and-suspenders"
```

---

## Task 6: Write Section 6 — Reading the Results

**Files:**
- Modify: `src/BENCHMARK.md` (append)

**Step 1: Append Section 6 to `src/BENCHMARK.md`**

```markdown
## Reading the Results: P99 vs P99.9

`benchmark_p99` returns a `LatencyStats`:

```cpp
struct LatencyStats {
  std::int64_t p99_ns{};   // 99th percentile latency in nanoseconds
  std::int64_t p999_ns{};  // 99.9th percentile latency in nanoseconds
  std::size_t  samples{};  // number of measured iterations used
};
```

### How the Indices Are Computed

The method used is **nearest rank**: find the element at position `floor(p * (n - 1))` in
the sorted array, where `p` is the percentile as a fraction.

For `n = 1000` samples:

```
idx_p99  = floor(0.99  * 999) = floor(989.01) = 989  → sorted[989]
idx_p999 = floor(0.999 * 999) = floor(998.001) = 998 → sorted[998]
```

The sorted array has indices 0–999. `sorted[989]` is the 990th fastest measurement (0-indexed).
99% of your calls were at or below this value.

### Why Both?

P99 and P99.9 tell different stories:

| Metric | Frequency at 1M orders/sec | What it means |
|--------|---------------------------|---------------|
| P99    | 10,000 spikes/sec         | Common tail — your typical bad day |
| P99.9  | 1,000 spikes/sec          | Rare spikes — the ones that cause fills to miss |

A system that looks healthy at P99 (say, 5 µs) but degrades badly at P99.9 (say, 500 µs) has
a latency distribution with a heavy tail. This often indicates:

- Lock contention releasing at rare intervals
- OS scheduler preemption at unfortunate moments
- TLB shootdowns from another thread's memory operations
- NUMA effects when the OS migrates a thread to a different memory domain

P99.9 is the metric that catches these. Risk engines in particular need to know the P99.9
latency of their pre-trade checks: a check that occasionally takes 500 µs on a 1M orders/sec
system means 1000 delayed orders per second — each one a potential missed fill.

---
```

**Step 2: Commit**

```bash
git add src/BENCHMARK.md
git commit -m "docs: add BENCHMARK.md Section 6 — reading P99 vs P99.9 results"
```

---

## Task 7: Write Section 7 — Quick Reference

**Files:**
- Modify: `src/BENCHMARK.md` (append)

**Step 1: Append the Quick Reference section and closing to `src/BENCHMARK.md`**

```markdown
## Quick Reference

```cpp
#include "benchmark_p99.hpp"

// Pre-allocate the latency buffer (caller owns storage)
constexpr std::size_t ITERATIONS    = 10'000;
constexpr std::size_t WARMUP        = 200;
std::array<std::int64_t, ITERATIONS> buf;

// Benchmark any callable — here, a lambda capturing a ring buffer
auto stats = hft_bench::benchmark_p99(
    [&]{ return ring_buf.try_pop(); },  // callable
    std::span{buf},                      // storage for per-call timings (ns)
    ITERATIONS,                          // measured iterations
    WARMUP                               // warmup iterations (not measured)
);

std::println("P99:   {} ns", stats.p99_ns);
std::println("P99.9: {} ns", stats.p999_ns);
std::println("n:     {}",    stats.samples);
```

### API at a Glance

```
benchmark_p99(f, args..., latencies_buffer, iterations, warmup_iterations = 0)
  → LatencyStats { p99_ns, p999_ns, samples }

LatencyStats
├── p99_ns   : int64_t  — 99th percentile latency in nanoseconds
├── p999_ns  : int64_t  — 99.9th percentile latency in nanoseconds
└── samples  : size_t   — number of measured iterations used

Constraints:
  - iterations must be ≤ latencies_buffer.size() (clamped silently if larger)
  - latencies_buffer is sorted in-place as a side effect
  - Use ≥ 200 warmup iterations for CPU frequency and cache warming
  - For sub-100 ns functions, clock overhead (~25 ns) is significant
```

### See Also

- `ScopeTimer.hpp` — RAII per-call timer for inline instrumentation during live runs
- `src/HPRINGBUFFER.md` — ring buffer the benchmarker is often used to measure
- `src/common/MEMORYPOOL.md` — another zero-allocation pattern worth benchmarking
```

**Step 2: Read the complete `src/BENCHMARK.md` top to bottom**

Verify:
- All 7 sections are present
- Style matches `MEMORYPOOL.md` (problem-first, explains why before how, concrete examples)
- No broken markdown (unclosed code fences, etc.)
- The Quick Reference code block compiles conceptually (callable, span, iterations, warmup)

**Step 3: Final commit**

```bash
git add src/BENCHMARK.md
git commit -m "docs: add BENCHMARK.md Section 7 — quick reference, complete doc"
```

---

## Done

`src/BENCHMARK.md` is complete. It lives alongside `HPRINGBUFFER.md` as a learner-friendly
entry point for C++ devs new to HFT benchmarking patterns.
