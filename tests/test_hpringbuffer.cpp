#include <gtest/gtest.h>
#include "HPRingBuffer.hpp"

TEST(HPRingBufferTest, SmokeTest) {
    HPRingBuffer<int, 4> buffer;
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.capacity(), 3);
}
