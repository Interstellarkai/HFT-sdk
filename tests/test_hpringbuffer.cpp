#include <gtest/gtest.h>

#include <atomic>
#include <numeric>
#include <thread>
#include <vector>

#include "HPRingBuffer.hpp"

// ── Basic Operations ────────────────────────────────────────────────────────

TEST(HPRingBufferTest, EmptyOnConstruction) {
  HPRingBuffer<int, 8> buffer;
  EXPECT_TRUE(buffer.empty());
  EXPECT_FALSE(buffer.full());
  EXPECT_EQ(buffer.size(), 0);
  EXPECT_EQ(buffer.capacity(), 7);  // Size - 1
}

TEST(HPRingBufferTest, TryPushAndTryPop) {
  HPRingBuffer<int, 4> buffer;
  EXPECT_TRUE(buffer.try_push(42));
  EXPECT_EQ(buffer.size(), 1);

  auto val = buffer.try_pop();
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, 42);
  EXPECT_TRUE(buffer.empty());
}

TEST(HPRingBufferTest, TryPushMoveSemantics) {
  HPRingBuffer<std::string, 4> buffer;
  std::string s = "hello";
  EXPECT_TRUE(buffer.try_push(std::move(s)));

  auto val = buffer.try_pop();
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, "hello");
}

TEST(HPRingBufferTest, TryPopFromEmptyReturnsNullopt) {
  HPRingBuffer<int, 4> buffer;
  auto val = buffer.try_pop();
  EXPECT_FALSE(val.has_value());
}

TEST(HPRingBufferTest, TryPushToFullReturnsFalse) {
  HPRingBuffer<int, 4> buffer;  // capacity = 3
  EXPECT_TRUE(buffer.try_push(1));
  EXPECT_TRUE(buffer.try_push(2));
  EXPECT_TRUE(buffer.try_push(3));
  EXPECT_TRUE(buffer.full());
  EXPECT_FALSE(buffer.try_push(4));  // full
}

// ── Capacity and Size ───────────────────────────────────────────────────────

TEST(HPRingBufferTest, CapacityIsStaticConstexpr) {
  static_assert(HPRingBuffer<int, 16>::capacity() == 15);
  static_assert(HPRingBuffer<int, 1024>::capacity() == 1023);
}

TEST(HPRingBufferTest, SizeTracksItems) {
  HPRingBuffer<int, 8> buffer;
  for (int i = 0; i < 7; ++i) {
    EXPECT_TRUE(buffer.try_push(i));
    EXPECT_EQ(buffer.size(), static_cast<std::size_t>(i + 1));
  }
  for (int i = 7; i > 0; --i) {
    [[maybe_unused]] auto val = buffer.try_pop();
    EXPECT_EQ(buffer.size(), static_cast<std::size_t>(i - 1));
  }
}

// ── Wraparound (monotonic sequences indexing correctly) ─────────────────────

TEST(HPRingBufferTest, WraparoundProducesCorrectValues) {
  HPRingBuffer<int, 4> buffer;  // capacity = 3
  // Push and pop more items than the buffer size to force wraparound
  for (int round = 0; round < 10; ++round) {
    for (int i = 0; i < 3; ++i) {
      EXPECT_TRUE(buffer.try_push(round * 3 + i));
    }
    for (int i = 0; i < 3; ++i) {
      auto val = buffer.try_pop();
      ASSERT_TRUE(val.has_value());
      EXPECT_EQ(*val, round * 3 + i);
    }
  }
}

// ── Blocking API ────────────────────────────────────────────────────────────

TEST(HPRingBufferTest, BlockingPushAndPop) {
  HPRingBuffer<int, 8, YieldWait> buffer;
  buffer.push(99);
  EXPECT_EQ(buffer.size(), 1);

  int val = buffer.pop();
  EXPECT_EQ(val, 99);
  EXPECT_TRUE(buffer.empty());
}

// ── Wait Strategy Variants ──────────────────────────────────────────────────

TEST(HPRingBufferTest, WorksWithBusySpinWait) {
  HPRingBuffer<int, 8, BusySpinWait> buffer;
  buffer.push(1);
  EXPECT_EQ(buffer.pop(), 1);
}

TEST(HPRingBufferTest, WorksWithYieldWait) {
  HPRingBuffer<int, 8, YieldWait> buffer;
  buffer.push(1);
  EXPECT_EQ(buffer.pop(), 1);
}

TEST(HPRingBufferTest, WorksWithBlockWait) {
  HPRingBuffer<int, 8, BlockWait> buffer;
  EXPECT_TRUE(buffer.try_push(1));
  auto val = buffer.try_pop();
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, 1);
}

// ── SPSC Cross-Thread Correctness ───────────────────────────────────────────

TEST(HPRingBufferTest, SPSCCrossThread) {
  constexpr std::size_t N = 100'000;
  HPRingBuffer<std::uint64_t, 1024, YieldWait> buffer;

  std::atomic<bool> done{false};

  // Producer thread
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < N; ++i) {
      buffer.push(i);
    }
    done.store(true, std::memory_order_release);
  });

  // Consumer thread
  std::vector<std::uint64_t> received;
  received.reserve(N);

  std::thread consumer([&] {
    while (received.size() < N) {
      auto val = buffer.try_pop();
      if (val.has_value()) {
        received.push_back(*val);
      } else {
        std::this_thread::yield();
      }
    }
  });

  producer.join();
  consumer.join();

  // Verify all items received in order
  ASSERT_EQ(received.size(), N);
  for (std::uint64_t i = 0; i < N; ++i) {
    EXPECT_EQ(received[i], i) << "Mismatch at index " << i;
  }
}

TEST(HPRingBufferTest, SPSCBlockingCrossThread) {
  constexpr std::size_t N = 50'000;
  HPRingBuffer<std::uint64_t, 256, YieldWait> buffer;

  // Producer: blocking push
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < N; ++i) {
      buffer.push(i);
    }
  });

  // Consumer: blocking pop
  std::vector<std::uint64_t> received;
  received.reserve(N);

  std::thread consumer([&] {
    for (std::size_t i = 0; i < N; ++i) {
      received.push_back(buffer.pop());
    }
  });

  producer.join();
  consumer.join();

  ASSERT_EQ(received.size(), N);
  for (std::uint64_t i = 0; i < N; ++i) {
    EXPECT_EQ(received[i], i) << "Mismatch at index " << i;
  }
}
