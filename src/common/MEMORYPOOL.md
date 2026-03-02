# Memory Pool

A lock-free, fixed-capacity object pool for **zero-allocation hot paths** in low-latency / HFT systems.

If you've never worked in HFT before, start here — this document builds up from *why* this
exists before diving into *how* it works.

---

## The Problem: Why `new` is Too Slow

In most applications, allocating a heap object with `new` is perfectly fine. Under the hood,
`new` calls `malloc`, which:

1. Acquires a lock on the heap allocator
2. Searches fragmented free memory for a suitable block
3. Possibly makes a system call to the OS for more memory
4. Returns a pointer

This works well on average, but the **latency is non-deterministic** — any single allocation
might take 50 ns in the best case, or spike to 10 µs+ if the allocator needs to call the OS.

In high-frequency trading, the matching engine processes incoming orders in **tens of
nanoseconds**. A single unexpected latency spike from a heap allocation can cause an order to
miss a price level. The rule in HFT hot paths is simple: **no heap allocations after startup**.

> **Hot path**: any code that executes millions of times per second in the critical trading loop.
> Every nanosecond counts here. Code that runs once at startup is fine to be slow.

---

## The Solution: Pre-allocate Everything at Startup

`MemoryPool<T, Capacity>` solves this by allocating all memory **once, at construction time**,
before trading begins. After that:

- `allocate()` hands you a pre-existing slot — no OS involvement, no heap fragmentation
- `deallocate()` returns the slot to the pool — again, no OS involvement
- Both operations are **O(1)** and complete in a handful of nanoseconds

The trade-off is a **fixed capacity**: you decide at compile time how many objects the pool
can hold. If you try to allocate when the pool is full, you get `nullptr`. This is intentional
— an explicit failure you can handle is far better than an unpredictable latency spike.

---

## How It Works: The Free List

Inside the pool is a **free list** — a classic data structure for managing a set of reusable
slots. Here's the key insight: instead of maintaining a separate list structure, the
`next_free` pointer is stored *inside each slot while it's unused*. That slot's storage is
sitting idle anyway, so we use it to hold the list linkage at zero extra cost.

At construction time, the slots form a chain:

```
free_head_ ──► [Slot 0]──►[Slot 1]──►[Slot 2]──►[Slot 3]──► END
               next=1      next=2      next=3      next=END
               (free)      (free)      (free)      (free)
```

### Allocating a Slot

To allocate, the pool pops the head off the free list:

```
Before:  free_head_ ──► [Slot 0] ──► [Slot 1] ──► [Slot 2]

Pop slot 0 atomically:
  1. Read free_head_ → 0
  2. Read slots[0].next_free → 1
  3. CAS: if free_head_ is still 0, set it to 1
  4. Construct your T in slots[0].storage via placement new

After:   free_head_ ──► [Slot 1] ──► [Slot 2]
         [Slot 0] → in_use, holds your T*
```

You receive a pointer directly into `slots[0].storage`. No copy, no heap — your object lives
right there.

### Deallocating a Slot

To deallocate, the pool pushes the slot back onto the front of the free list:

```
Before:  free_head_ ──► [Slot 1] ──► [Slot 2]
         [Slot 0] → in_use (your T)

Return slot 0:
  1. Destroy the T in slots[0].storage (if non-trivial)
  2. CAS: set free_head_ to 0, and slots[0].next_free to old head (1)

After:   free_head_ ──► [Slot 0] ──► [Slot 1] ──► [Slot 2]
         (slot 0 is free again, its storage will be reused next)
```

### What is CAS and Why Use It?

Both operations use a **CAS (Compare-And-Swap)** loop instead of a mutex. CAS is a single
CPU instruction that does: *"if the memory at this address still contains value X, atomically
replace it with Y; otherwise tell me it changed."*

Using CAS instead of a mutex means:

- No thread ever **blocks** waiting for a lock — threads just retry the CAS if another thread
  raced ahead
- No **priority inversion** — a low-priority OS thread can't accidentally freeze a high-priority
  trading thread by holding a lock
- The entire operation typically completes in 5–20 ns even under contention

This property is called **lock-free**: multiple threads can allocate and deallocate
simultaneously without any of them ever waiting for the others.

---

## Key Design Decisions

### Slots are 64-byte aligned

```cpp
struct alignas(64) Slot { ... };
```

64 bytes is the size of a **cache line** — the smallest unit of memory that the CPU cache
transfers between RAM and the processor. If two slots shared a cache line and two different
threads were working with those slots simultaneously, every write by one thread would
**invalidate** the other thread's cached copy of that line, even though they're touching
completely different data. This invisible slowdown is called **false sharing**, and it can cut
throughput by 5–10× on multi-core systems. Aligning each slot to its own cache line eliminates
the problem entirely.

### Storage is `alignof(T)` aligned

```cpp
alignas(alignof(T)) std::byte storage[sizeof(T)];
```

Constructing an object at an **unaligned** address is undefined behavior in C++ and causes a
CPU fault on many architectures. The raw byte array that holds your object must start at an
address the CPU considers valid for type `T`. The `alignas(alignof(T))` annotation guarantees
this at zero runtime cost.

`std::byte` (C++17) is used rather than `uint8_t` because this array is raw memory, not
arithmetic data. `std::byte` has no arithmetic operators, which signals intent clearly and
prevents accidental numeric operations on uninitialized storage.

### `~T()` is only called when necessary

```cpp
if constexpr (!std::is_trivially_destructible_v<T>) {
    ptr->~T();
}
```

For plain types like `int`, `double`, or simple POD structs, the compiler knows the destructor
does nothing — so this block compiles away to zero instructions. For types with real cleanup
work (file handles, reference counts, etc.), the destructor is called properly. This is a
compile-time decision, so there's no runtime overhead either way.

### The `in_use` flag per slot

Each slot carries a `bool in_use`. This enables two things:

1. **Double-free detection**: `deallocate()` asserts that `in_use == true` before freeing.
   Calling `deallocate()` twice on the same pointer would be caught immediately in debug builds.
2. **Safe pool destruction**: `~MemoryPool()` loops over all slots and calls `~T()` on any
   that are still marked in-use, preventing resource leaks if objects outlive their callers.

---

## Usage

```cpp
#include "memory_pool.hpp"

struct Order {
    int    id;
    double price;
    int    quantity;
};

// Declare the pool (typically as a class member or function-scope static).
// All 1024 Order slots are allocated HERE, not on every allocate() call.
HFT_sdk::MemoryPool<Order, 1024> pool;

// Allocate — O(1), no heap. Arguments are forwarded to Order's constructor.
Order* o = pool.allocate(42, 99.5, 100);

if (!o) {
    // Pool is full — handle this explicitly. There's no exception, just nullptr.
    // In a trading system, you might log a metric and reject the order.
    return;
}

// Use it normally — it's just a regular pointer to an Order.
process_order(o);

// Deallocate — O(1), no heap. Calls ~Order() if needed, returns slot to pool.
pool.deallocate(o);
// o is now dangling — don't use it after this point.

// Introspection (approximate — see Thread Safety section)
pool.capacity();   // always 1024
pool.allocated();  // how many slots are currently in use
pool.available();  // how many slots are free
```

### Ownership Pattern

Because `MemoryPool` is large (it holds all the pre-allocated storage) and non-copyable, own
it at a stable address — as a class member, a `static` local, or a global. Don't create it on
the stack inside a hot-path function.

```cpp
// Good — stable lifetime
class OrderManager {
    HFT_sdk::MemoryPool<Order, 500000> pool_;
    // ...
};

// Also good
static HFT_sdk::MemoryPool<Order, 500000> global_pool;
```

---

## Thread Safety

`allocate()` and `deallocate()` are **safe to call concurrently from multiple threads** — the
CAS loop handles all the coordination. You don't need an external lock around them.

The `capacity()`, `allocated()`, and `available()` accessors read atomic counters and are
**safe to call from any thread**, but treat them as approximate snapshots. If thread A calls
`allocated()` while thread B is mid-allocate, you might see a stale value. For monitoring and
metrics, this is fine. Don't make correctness decisions based on these values.

---

## Limitations and Gotchas

**Pool full returns `nullptr`, not an exception.**
Always check the return value of `allocate()`. When the pool is exhausted, you get back
`nullptr` — there's no `std::bad_alloc` or anything like that. In a trading system, exhausting
the pool usually means a bug in your lifecycle management (objects not being returned), but
the caller is responsible for handling it gracefully.

**The pool cannot be copied or moved.**
`MemoryPool` deletes its copy constructor and assignment operator. This is intentional — the
pool owns a large pre-allocated array, and moving it would invalidate all outstanding pointers
into it. Own it at a fixed address (member variable, `static`, etc.).

**No pointer validation in release builds.**
`deallocate()` only checks that the pointer belongs to this pool via `assert()`. In a Release
build, `assert()` compiles away. Calling `deallocate()` with a pointer that didn't come from
this pool is **undefined behavior**. Trust your calling code, not runtime validation, to get
this right.

**The ABA problem.**
The CAS free-list is theoretically susceptible to the ABA problem: thread A reads `head = 0`,
gets preempted; thread B allocates slot 0, deallocates it again; now slot 0 is back at the
head. Thread A resumes and its CAS succeeds even though the list has changed. In practice, for
HFT memory pools (bounded concurrency, typed pools, short-lived objects), ABA is not a real
concern — but it's worth knowing the theoretical limitation exists.

---

## Quick Reference

```
MemoryPool<T, N>
├── allocate(args...)   → T*      // construct T in-place, O(1) lock-free, nullptr if full
├── deallocate(T*)                // destruct T + return slot to pool, O(1) lock-free
├── capacity()          → size_t  // always N (compile-time constant)
├── allocated()         → size_t  // live objects right now (approximate)
└── available()         → size_t  // free slots right now (approximate)
```
