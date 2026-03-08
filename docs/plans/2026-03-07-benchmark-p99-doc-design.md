# BENCHMARK.md Design — `benchmark_p99.hpp` Learner-Friendly Documentation

**Date:** 2026-03-07
**Scope:** `src/benchmark_p99.hpp` only. `ScopeTimer.hpp` cross-referenced but not covered.
**Style:** Mirrors `MEMORYPOOL.md` — Problem-First, progressive intuition-building, top-to-bottom readable.
**Code changes:** None. Doc-only. Shortening opportunity for lines 96–99 noted inline as a teaching point.

---

## Target Reader

A C++ developer who understands templates and the STL but has not worked in HFT before. They should finish
the doc feeling like they could have written `benchmark_p99` themselves.

---

## Approved Structure (Approach A — Problem-First)

### Section 1 — The Problem: Why Measuring Latency is Hard

- Average latency is misleading: one slow GC/OS event inflates the mean
- Introduces p99/p99.9: "your 99th-percentile order is the one that missed the price level"
- Analogy: race lap times — averaging in the pit-stop lap is meaningless
- Motivation for why HFT specifically needs tail-latency metrics

### Section 2 — Why `std::span`, Not a Vector or Raw Pointer

- `span` = non-owning view of contiguous storage
- Caller owns and pre-allocates the buffer (stack array, static array, or heap)
- `vector&` would force a heap allocation per benchmark run — unacceptable on a measured hot path
- Raw pointer loses size info (unsafe); `span` carries both pointer + length at zero cost
- Key pattern: **caller provides storage, function borrows it**

### Section 3 — Warmup: Teaching the CPU to Care

Three physical phenomena warmup addresses, explained intuitively:

1. **CPU frequency scaling (P-states/C-states):** modern CPUs start at a low-power clock speed and ramp
   up to max frequency over the first few milliseconds. Calls before ramp-up are literally slower in Hz.
2. **Instruction cache cold start:** the function's compiled machine code must be fetched from L2/L3/RAM
   on first call. Subsequent calls find it in L1-I cache.
3. **Data cache warming:** the callable's internal state (lookup tables, member variables) and the
   latency buffer itself aren't in L1/L2 on first access.

Warmup runs `f()` enough times for the CPU to reach steady state. Only then do measured iterations
reflect what real trading code experiences tick-to-tick.

### Section 4 — The Measurement Loop and Clock Overhead

- `steady_clock` vs `system_clock`: steady is monotonic (never goes backwards), system can jump with NTP
- `clock::now()` itself costs ~20–30 ns on most hardware
- For functions faster than ~100 ns, timing overhead is a significant fraction of the measurement
- Mention `rdtsc` as the HFT alternative (sub-nanosecond, but non-portable and requires serialization)
- Why `steady_clock` here: portability and correctness over raw speed

### Section 5 — `clamp_index`: Defensive Code and When It Matters

- The hazard: `std::size_t` is unsigned. If `n = 0`, then `n - 1` wraps to `SIZE_MAX`
  (18,446,744,073,709,551,615), and `0.99 * SIZE_MAX` becomes a massive out-of-bounds index
- `clamp_index` catches this: `[[unlikely]]` hints the branch predictor to optimize the normal path
- The `detail::` namespace convention: signals "internal implementation, not public API" — enforced
  by convention, not the language (nothing stops a caller from using `hft_bench::detail::clamp_index`)
- **The twist:** by the time lines 91–94 are reached, the `iterations == 0` early return at line 80
  already guarantees `n >= 1`. `clamp_index` is therefore unreachable in the dangerous case — a
  belt-and-suspenders pattern common in codebases with multiple authors
- Teaching note on lines 96–99: the four-line stats assembly is explicit and readable; it could be
  compressed to a single return with C++20 designated initializers:
  `return {.p99_ns = data[idx_p99], .p999_ns = data[idx_p999], .samples = n};`
  — cleaner, but relies on knowing the struct field order is stable

### Section 6 — Reading the Results: p99 vs p99.9

- Nearest-rank method: `idx_p99 = floor(0.99 * (n - 1))`
- Concrete example: n=1000 → idx_p99=989, idx_p999=998
- Intuition: p99.9 is worst 1-in-1000 call
- On an exchange doing 1M orders/sec, that's ~1000 tail-latency spikes per second
- p99.9 matters for risk systems: a risk check that takes 50 µs at p99.9 can cause an order to miss
  a price level — even if p99 looks fine at 5 µs
- Cross-reference: `ScopeTimer.hpp` for per-call inline timing during live runs

### Section 7 — Quick Reference

Complete, copy-pasteable usage example with pre-allocated buffer, warmup, and output printing.

---

## File Location

`src/BENCHMARK.md` — alongside `src/HPRINGBUFFER.md` and `src/common/MEMORYPOOL.md`

---

## Out of Scope

- `ScopeTimer.hpp` (cross-referenced only)
- Code changes to `benchmark_p99.hpp`
- Performance numbers specific to any hardware
