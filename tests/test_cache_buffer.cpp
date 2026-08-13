#include "minicyber/data/cache_buffer.h"

#include <gtest/gtest.h>
#include <thread>
#include <vector>

namespace {

using minicyber::data::CacheBuffer;

TEST(CacheBufferTest, InitialStateIsEmpty) {
  CacheBuffer<int> buf(4);
  EXPECT_TRUE(buf.Empty());
  EXPECT_EQ(buf.Size(), 0u);
  EXPECT_EQ(buf.Capacity(), 5u);  // internal vector is size + 1
}

TEST(CacheBufferTest, FillOneElement) {
  CacheBuffer<int> buf(4);
  buf.Fill(42);
  EXPECT_FALSE(buf.Empty());
  EXPECT_EQ(buf.Size(), 1u);
  EXPECT_EQ(buf.Head(), 1u);
  EXPECT_EQ(buf.Tail(), 1u);
  EXPECT_EQ(buf.Front(), 42);
  EXPECT_EQ(buf.Back(), 42);
  EXPECT_EQ(buf.at(buf.Head()), 42);
  EXPECT_EQ(buf[buf.Tail()], 42);
}

TEST(CacheBufferTest, FillUpToCapacityIsFull) {
  CacheBuffer<int> buf(4);
  for (int i = 0; i < 4; ++i) buf.Fill(i);
  EXPECT_TRUE(buf.Full());
  EXPECT_EQ(buf.Size(), 4u);
  EXPECT_EQ(buf.Front(), 0);
  EXPECT_EQ(buf.Back(), 3);
}

TEST(CacheBufferTest, OverwriteAdvancesHeadAndTail) {
  CacheBuffer<int> buf(4);
  for (int i = 0; i < 4; ++i) buf.Fill(i);
  // Buffer full: [0,1,2,3]. Push 4 -> overwrites 0.
  buf.Fill(4);
  EXPECT_TRUE(buf.Full());
  EXPECT_EQ(buf.Size(), 4u);  // size stays at capacity - 1
  EXPECT_EQ(buf.Front(), 1);
  EXPECT_EQ(buf.Back(), 4);
  EXPECT_EQ(buf.at(buf.Head()), 1);
  EXPECT_EQ(buf.at(buf.Tail()), 4);
}

TEST(CacheBufferTest, MinimumCapacityAlwaysRetainsNewestValue) {
  CacheBuffer<int> buf(1);
  buf.Fill(10);
  buf.Fill(20);
  buf.Fill(30);

  EXPECT_TRUE(buf.Full());
  EXPECT_EQ(buf.Size(), 1u);
  EXPECT_EQ(buf.Head(), buf.Tail());
  EXPECT_EQ(buf.Front(), 30);
  EXPECT_EQ(buf.Back(), 30);
}

TEST(CacheBufferTest, RandomAccessByAbsoluteIndex) {
  CacheBuffer<int> buf(4);
  for (int i = 0; i < 6; ++i) buf.Fill(i);  // 2 overwrites
  // Live window should be [2,3,4,5]
  EXPECT_EQ(buf.Size(), 4u);
  EXPECT_EQ(buf.at(buf.Head()), 2);
  EXPECT_EQ(buf.at(buf.Head() + 1), 3);
  EXPECT_EQ(buf.at(buf.Head() + 2), 4);
  EXPECT_EQ(buf.at(buf.Tail()), 5);
  EXPECT_EQ(buf[buf.Head()], 2);
}

TEST(CacheBufferTest, TailAndHeadAreMonotonic) {
  CacheBuffer<int> buf(3);
  buf.Fill(10);
  uint64_t head0 = buf.Head();
  uint64_t tail0 = buf.Tail();
  for (int i = 1; i <= 10; ++i) buf.Fill(i * 10);
  // Head and Tail should never wrap around (they're absolute indices).
  EXPECT_GT(buf.Tail(), tail0);
  EXPECT_GT(buf.Head(), head0);
  EXPECT_EQ(buf.Size(), 3u);  // capacity - 1
}

TEST(CacheBufferTest, CopyConstructorPreservesState) {
  CacheBuffer<int> buf(4);
  for (int i = 0; i < 4; ++i) buf.Fill(i);
  CacheBuffer<int> copy(buf);
  EXPECT_EQ(copy.Size(), buf.Size());
  EXPECT_EQ(copy.Capacity(), buf.Capacity());
  EXPECT_EQ(copy.Front(), buf.Front());
  EXPECT_EQ(copy.Back(), buf.Back());
  // Mutating copy should not affect source.
  copy.Fill(99);
  EXPECT_EQ(copy.Back(), 99);
  EXPECT_EQ(buf.Back(), 3);
}

TEST(CacheBufferTest, ConcurrentFillIsSafeUnderMutex) {
  // CacheBuffer itself only guarantees mutex-protected copy; concurrent Fill
  // requires external locking via Mutex(). This test exercises that path.
  CacheBuffer<int> buf(1000);
  auto& mtx = buf.Mutex();
  constexpr int kThreads = 4;
  constexpr int kPerThread = 500;
  std::vector<std::thread> ths;
  for (int t = 0; t < kThreads; ++t) {
    ths.emplace_back([&]() {
      for (int i = 0; i < kPerThread; ++i) {
        std::lock_guard<std::mutex> lg(mtx);
        buf.Fill(i);
      }
    });
  }
  for (auto& th : ths) th.join();
  EXPECT_EQ(buf.Size(), 1000u);  // exactly fills capacity - 1
  EXPECT_TRUE(buf.Full());
}

}  // namespace
