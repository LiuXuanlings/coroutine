#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
#include "minicyber/transport/shm/state.h"

using minicyber::transport::State;

// 构造时 ceiling_msg_size 正确设置
TEST(ShmStateTest, CeilingMsgSizeOnInit) {
  State s(1024);
  EXPECT_EQ(s.ceiling_msg_size(), 1024u);
}

// 引用计数从 0 开始
TEST(ShmStateTest, ReferenceCountStartsAtZero) {
  State s(1024);
  EXPECT_EQ(s.reference_counts(), 0u);
}

// 单次增减引用计数
TEST(ShmStateTest, IncrAndDecrReferenceCount) {
  State s(1024);
  s.IncreaseReferenceCounts();
  s.IncreaseReferenceCounts();
  EXPECT_EQ(s.reference_counts(), 2u);
  s.DecreaseReferenceCounts();
  EXPECT_EQ(s.reference_counts(), 1u);
}

// DecreaseReferenceCounts 不会跌到 0 以下
TEST(ShmStateTest, DecreaseNeverBelowZero) {
  State s(1024);
  s.DecreaseReferenceCounts();
  s.DecreaseReferenceCounts();
  EXPECT_EQ(s.reference_counts(), 0u);
}

// FetchAddSeq 单调递增，返回累加前的值
TEST(ShmStateTest, SeqMonotonic) {
  State s(1024);
  EXPECT_EQ(s.FetchAddSeq(1), 0u);
  EXPECT_EQ(s.FetchAddSeq(2), 1u);
  EXPECT_EQ(s.seq(), 3u);
  EXPECT_EQ(s.FetchAddSeq(0), 3u);
}

// need_remap 默认 false，可置位/清位
TEST(ShmStateTest, NeedRemapFlag) {
  State s(1024);
  EXPECT_FALSE(s.need_remap());
  s.set_need_remap(true);
  EXPECT_TRUE(s.need_remap());
  s.set_need_remap(false);
  EXPECT_FALSE(s.need_remap());
}

// 多线程并发增减引用计数，最终应为 0
TEST(ShmStateTest, ConcurrentReferenceCount) {
  State s(1024);
  const int NUM_THREADS = 8;
  const int OPS_PER_THREAD = 10000;
  std::vector<std::thread> threads;
  for (int i = 0; i < NUM_THREADS; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < OPS_PER_THREAD; ++j) {
        s.IncreaseReferenceCounts();
        s.DecreaseReferenceCounts();
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(s.reference_counts(), 0u);
}

// 多线程并发 FetchAddSeq：
//   - 最终 seq == 总调用次数（每次 diff=1）
//   - 所有返回值之和 == 0+1+...+(N-1) == N*(N-1)/2
TEST(ShmStateTest, ConcurrentSeq) {
  State s(1024);
  const int NUM_THREADS = 8;
  const int OPS_PER_THREAD = 10000;
  const uint64_t TOTAL = static_cast<uint64_t>(NUM_THREADS) * OPS_PER_THREAD;
  std::atomic<uint64_t> sum{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < NUM_THREADS; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < OPS_PER_THREAD; ++j) {
        sum += s.FetchAddSeq(1);
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(s.seq(), TOTAL);
  // 返回值是累加前的旧值，所有旧值集合恰为 {0,1,...,TOTAL-1}
  EXPECT_EQ(sum.load(), TOTAL * (TOTAL - 1) / 2);
}